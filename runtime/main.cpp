// PrototypeRecomp Phase 2B runtime — XEX loader + title boot.
//
// Load pipeline:
//   default.xex -> XenonUtils Xex2LoadImage (decrypt + BASIC decompress +
//   relocations + PE sections) -> guest memory at 0x82000000 ->
//   PPC function lookup table -> import table walk (function thunks become
//   direct host calls in generated code; variable import slots receive
//   runtime kernel pointers) -> kernel variables -> main XThread ->
//   Execute(entry_point).
#include "state.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "xex.h"
#include "image.h"

extern "C" {
#include "aes.h"
}

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

// PPCFuncMappings is defined by the generated ppc_func_mapping.cpp at global
// scope.
extern PPCFuncMapping PPCFuncMappings[];

namespace pr {

void TraceHooksInit();
void StartThreadWatchdog();

// ------------------------------------------------------------ raw XEX bits
static uint32_t Raw32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

struct RawXex {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const uint8_t* payload = nullptr;     // encrypted/compressed body
    size_t payload_size = 0;
    std::vector<uint8_t> pristine;         // decrypted + decompressed image
    const Xex2SecurityInfo* security = nullptr;
    const Xex2OptFileFormatInfo* ffi = nullptr;
};

static bool LoadRawXex(const char* path, RawXex* out) {
    FILE* f = fopen(path, "rb");
    if (!f) { PRLOGE("cannot open %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, f) != (size_t)sz) {
        fclose(f);
        return false;
    }
    fclose(f);
    out->data = buf.data();
    out->size = buf.size();

    static std::vector<uint8_t> keep_alive = buf;  // own storage
    out->data = keep_alive.data();
    out->size = keep_alive.size();

    auto* hdr = (const Xex2Header*)out->data;
    if ((uint32_t)hdr->magic != 0x58455832u) {
        PRLOGE("not a XEX2 file");
        return false;
    }
    uint32_t sec_off = hdr->securityOffset;
    out->security = (const Xex2SecurityInfo*)(out->data + sec_off);
    out->ffi = (const Xex2OptFileFormatInfo*)getOptHeaderPtr(
        out->data, XEX_HEADER_FILE_FORMAT_INFO);

    out->payload = out->data + (uint32_t)hdr->headerSize;
    out->payload_size = out->size - (uint32_t)hdr->headerSize;

    // Pristine decrypt + decompress (own copy for import classification).
    const uint8_t* payload = out->payload;
    size_t payload_size = out->payload_size;
    std::vector<uint8_t> decrypted;
    if (out->ffi && (uint16_t)out->ffi->encryptionType == XEX_ENCRYPTION_NORMAL) {
        uint8_t key[16];
        memcpy(key, (const uint8_t*)out->security->aesKey, 16);
        AES_ctx ctx;
        AES_init_ctx_iv(&ctx, Xex2RetailKey, AESBlankIV);
        AES_CBC_decrypt_buffer(&ctx, key, 16);
        decrypted.resize(payload_size);
        memcpy(decrypted.data(), payload, payload_size);
        AES_init_ctx_iv(&ctx, key, AESBlankIV);
        AES_CBC_decrypt_buffer(&ctx, decrypted.data(), (uint32_t)payload_size);
        payload = decrypted.data();
    }
    out->pristine.resize((uint32_t)out->security->imageSize, 0);
    if (out->ffi && (uint16_t)out->ffi->compressionType == XEX_COMPRESSION_NONE) {
        memcpy(out->pristine.data(), payload, out->pristine.size());
    } else if (out->ffi &&
               (uint16_t)out->ffi->compressionType == XEX_COMPRESSION_BASIC) {
        auto* blocks = (const Xex2FileBasicCompressionBlock*)(out->ffi + 1);
        size_t numBlocks = (out->ffi->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;
        size_t dst = 0;
        const uint8_t* src = payload;
        for (size_t i = 0; i < numBlocks; i++) {
            size_t ds = blocks[i].dataSize, zs = blocks[i].zeroSize;
            if (dst + ds > out->pristine.size() ||
                src + ds > payload + payload_size) break;
            memcpy(out->pristine.data() + dst, src, ds);
            dst += ds; src += ds;
            memset(out->pristine.data() + dst, 0, zs);
            dst += zs;
        }
    } else {
        PRLOGE("unsupported XEX compression");
        return false;
    }
    PRLOG(Loader, "raw XEX ok: image %u bytes, load %08X",
          (uint32_t)out->security->imageSize,
          (uint32_t)out->security->loadAddress);
    return true;
}

// ------------------------------------------- variable import classification
// Function imports come as descriptor pairs (type-0 record + type-1 call
// stub). Unpaired type-0 descriptors are VARIABLE imports: the loader must
// write a real guest pointer into the thunk slot.
static void ClassifyImports(const RawXex& xex,
                            std::vector<XexVariableImport>* vars,
                            std::vector<XexFunctionImport>* funcs) {
    auto* imports = (const Xex2ImportHeader*)getOptHeaderPtr(
        xex.data, XEX_HEADER_IMPORT_LIBRARIES);
    if (!imports) return;
    const char* pStrTable = (const char*)(imports + 1);
    size_t padded = 0;
    std::vector<std::string> names;
    for (uint32_t i = 0; i < (uint32_t)imports->numImports; i++) {
        names.emplace_back(pStrTable + padded);
        padded += ((names.back().length() + 1) + 3) & ~3u;
    }
    auto* library =
        (const Xex2ImportLibrary*)((const char*)imports +
                                    sizeof(Xex2ImportHeader) +
                                    (uint32_t)imports->sizeOfStringTable);
    uint32_t load = (uint32_t)xex.security->loadAddress;
    for (uint32_t i = 0; i < (uint32_t)imports->numImports; i++) {
        auto* descriptors = (const Xex2ImportDescriptor*)(library + 1);
        uint32_t count = library->numberOfImports;
        const std::unordered_map<size_t, const char*>* table = nullptr;
        if (names[i] == "xam.xex") table = &XamExports;
        else if (names[i] == "xboxkrnl.exe") table = &XboxKernelExports;

        uint32_t im = 0;
        while (im < count) {
            uint32_t va = descriptors[im].firstThunk;
            uint32_t w = 0;
            if (va >= load && va - load + 4 <= xex.pristine.size()) {
                w = Raw32(xex.pristine.data() + (va - load));
            }
            uint32_t type = (w >> 24) & 0xFF;
            uint32_t ordinal = w & 0xFFFF;
            const char* nm = nullptr;
            if (table) {
                auto it = table->find(ordinal);
                if (it != table->end()) {
                    nm = it->second;
                    // XenonUtils export names carry the __imp__ prefix.
                    if (nm && strncmp(nm, "__imp__", 7) == 0) nm += 7;
                }
            }
            if (type == 0) {
                // Look ahead: is the next descriptor a type!=0 call stub with
                // the same ordinal? Then this is a function-import record.
                bool paired = false;
                if (im + 1 < count) {
                    uint32_t va2 = descriptors[im + 1].firstThunk;
                    uint32_t w2 = 0;
                    if (va2 >= load && va2 - load + 4 <= xex.pristine.size()) {
                        w2 = Raw32(xex.pristine.data() + (va2 - load));
                    }
                    if (((w2 >> 24) & 0xFF) != 0 && (w2 & 0xFFFF) == ordinal) {
                        paired = true;
                    }
                }
                if (paired) {
                    XexFunctionImport fi;
                    fi.thunk_va = descriptors[im + 1].firstThunk;
                    fi.ordinal = ordinal;
                    fi.name = nm ? nm : "";
                    fi.library = names[i];
                    funcs->push_back(fi);
                    im += 2;
                    continue;
                }
                // Unpaired type-0: variable import.
                XexVariableImport vi;
                vi.thunk_va = va;
                vi.ordinal = ordinal;
                vi.name = nm ? nm : "";
                vars->push_back(vi);
                im++;
            } else {
                // Stray non-zero thunk without record (unexpected).
                XexFunctionImport fi;
                fi.thunk_va = va;
                fi.ordinal = ordinal;
                fi.name = nm ? nm : "";
                fi.library = names[i];
                funcs->push_back(fi);
                im++;
            }
        }
        library = (const Xex2ImportLibrary*)(
            (const char*)(library + 1) + count * sizeof(Xex2ImportDescriptor));
    }
}

// ------------------------------------------------------------ guest helpers
static void WriteVariableSlot(const RawXex& xex, uint32_t thunk_va,
                              uint32_t value) {
    // Thunk slots live inside the image; write BE pointer value.
    StoreU32(thunk_va, value);
}

// ------------------------------------------------------------ boot


uint64_t GetHostTscHzWrap();

int BootTitle(const char* xex_path, int argc, char** argv) {
    LogInit();
    TraceHooksInit();
    // Calibrate the host TSC frequency NOW (before any guest thread runs)
    // so the first KeQueryPerformanceFrequency call cannot stall the game's
    // init sequencing (observed: a 100ms first-call sleep re-raced the
    // DriveThread handshake).
    (void)GetHostTscHzWrap();

    // 0. Guest memory.
    if (!GuestMemory::Init()) {
        PRLOGE("failed to map 4 GiB guest memory");
        return 1;
    }
    g_kernel = new KernelState();
    PRLOG(Loader, "guest memory base: %p (4 GiB)", (void*)g_guest_base);
    K().fs_root = getenv("PR_FS_ROOT") ? getenv("PR_FS_ROOT") : "";
    if (!K().fs_root.empty()) {
        PRLOG(Filesystem, "filesystem root (D:): %s", K().fs_root.c_str());
    } else {
        PRLOGW("no filesystem root set (PR_FS_ROOT) — disc absent; D:\\ "
               "opens will fail authentically");
    }

    // 1. Parse raw XEX.
    RawXex xex;
    if (!LoadRawXex(xex_path, &xex)) return 1;

    // 2. XenonUtils load: decrypt + decompress + relocations + sections.
    Image img = Xex2LoadImage(xex.data, xex.size);
    uint32_t load_addr = (uint32_t)xex.security->loadAddress;
    PRLOG(Loader, "image loaded: base=%08X size=%zu entry=%08X", img.base,
          img.size, img.entry_point);

    // 3. Copy image into guest memory.
    if (img.base != kImageBase) {
        PRLOGE("unexpected image base %08X", (uint32_t)img.base);
        return 1;
    }
    memcpy(g_guest_base + load_addr, img.data.get(), img.size);

    // 4. Function lookup table.
    {
        size_t n = 0;
        while (PPCFuncMappings[n].host != nullptr) n++;
        PRLOG(Loader, "registering %zu guest functions into lookup table at "
              "%08X", n, kLookupTableBase);
        for (size_t i = 0; i < n; i++) {
            uint32_t g = (uint32_t)PPCFuncMappings[i].guest;
            *(PPCFunc**)(g_guest_base + (uint64_t)kLookupTableBase +
                         (uint64_t)(g - PPC_CODE_BASE) * 2) =
                PPCFuncMappings[i].host;
        }
        // Guard: entry point must resolve.
        if (!LookupGuestFunc((uint32_t)img.entry_point)) {
            PRLOGE("entry point %08X missing from lookup table",
                   (uint32_t)img.entry_point);
            return 1;
        }
    }

    // 5. Import classification.
    std::vector<XexVariableImport> vars;
    std::vector<XexFunctionImport> funcs;
    ClassifyImports(xex, &vars, &funcs);
    PRLOG(Loader, "imports: %zu functions, %zu variables", funcs.size(),
          vars.size());

    // 6. Kernel variables + XEX header copy.
    KernelState& k = K();
    {
        // XEX2 header copy (raw, unmutated).
        auto* xhdr = (const Xex2Header*)xex.data;
        k.var_xex_header = GuestMemory::Get().SystemHeapAlloc(
            (uint32_t)xhdr->headerSize, 8);
        memcpy(HostFromGuest(k.var_xex_header), xex.data,
               (uint32_t)xhdr->headerSize);

        // Module struct chain: slot -> PP -> module -> +0x58 -> xex header.
        k.var_module_struct = GuestMemory::Get().SystemHeapAlloc(0x100, 8);
        GuestMemset(k.var_module_struct, 0, 0x100);
        StoreU32(k.var_module_struct + 0x58, k.var_xex_header);
        k.var_module_handle_pp = GuestMemory::Get().SystemHeapAlloc(8, 8);
        StoreU32(k.var_module_handle_pp, k.var_module_struct);

        // Process info block (KTHREAD+0x84).
        k.var_process_info_block = GuestMemory::Get().SystemHeapAlloc(0x100, 8);
        GuestMemset(k.var_process_info_block, 0, 0x100);
        StoreU32(k.var_process_info_block + 0x00, 3);  // process type marker

        // KeTimeStampBundle: 6 * u64.
        k.var_ketsb = GuestMemory::Get().SystemHeapAlloc(0x30, 8);
        GuestMemset(k.var_ketsb, 0, 0x30);

        // XboxHardwareInfo.
        k.var_hardware_info = GuestMemory::Get().SystemHeapAlloc(0x10, 8);
        StoreU32(k.var_hardware_info + 0x00, 0);       // flags
        StoreU32(k.var_hardware_info + 0x04, 0x20000000);  // 512MB
        StoreU32(k.var_hardware_info + 0x08, 1);       // cpu count-ish

        // ExThreadObjectType: OBJECT_TYPE-ish struct.
        k.var_thread_object_type = GuestMemory::Get().SystemHeapAlloc(0x20, 8);
        GuestMemset(k.var_thread_object_type, 0, 0x20);
        StoreU32(k.var_thread_object_type + 0x00, k.var_thread_object_type);

        // ExLoadedCommandLine: "default.xex".
        k.var_command_line = GuestMemory::Get().SystemHeapAlloc(0x104, 8);
        GuestWriteAnsiString(k.var_command_line, "default.xex");

        // XboxKrnlVersion.
        k.var_krnl_version = GuestMemory::Get().SystemHeapAlloc(8, 8);
        StoreU32(k.var_krnl_version, 7978);  // 0.0.7978.32
        StoreU32(k.var_krnl_version + 4, 32);

        // VdGpuClockInMHz.
        k.var_gpu_clock_mhz = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_gpu_clock_mhz, 500);

        // VdHSIOCalibrationLock: ULONG.
        k.var_hsio_lock = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_hsio_lock, 0);

        // VdGlobalDevice / VdGlobalXamDevice / debug/cert monitors: null
        // pointers (created when the game creates its D3D device).
        k.var_vd_global_device = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_vd_global_device, 0);
        k.var_vd_global_xam_device = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_vd_global_xam_device, 0);
        k.var_debug_monitor = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_debug_monitor, 0);
        k.var_cert_monitor = GuestMemory::Get().SystemHeapAlloc(4, 8);
        StoreU32(k.var_cert_monitor, 0);
    }

    // Write variable import slots.
    for (auto& v : vars) {
        uint32_t value = 0;
        if (v.name == "KeTimeStampBundle") value = k.var_ketsb;
        else if (v.name == "XexExecutableModuleHandle") value = k.var_module_handle_pp;
        else if (v.name == "XboxHardwareInfo") value = k.var_hardware_info;
        else if (v.name == "KeDebugMonitorData") value = k.var_debug_monitor;
        else if (v.name == "ExThreadObjectType") value = k.var_thread_object_type;
        else if (v.name == "ExLoadedCommandLine") value = k.var_command_line;
        else if (v.name == "KeCertMonitorData") value = k.var_cert_monitor;
        else if (v.name == "VdGlobalDevice") value = k.var_vd_global_device;
        else if (v.name == "VdGlobalXamDevice") value = k.var_vd_global_xam_device;
        else if (v.name == "XboxKrnlVersion") value = k.var_krnl_version;
        else if (v.name == "VdGpuClockInMHz") value = k.var_gpu_clock_mhz;
        else if (v.name == "VdHSIOCalibrationLock") value = k.var_hsio_lock;
        else {
            PRLOGW("unmapped variable import ord=%u (%s) — writing 0",
                   v.ordinal, v.name.c_str());
        }
        WriteVariableSlot(xex, v.thunk_va, value);
        PRLOG(Loader, "variable import %-30s slot %08X = %08X", v.name.c_str(),
              v.thunk_va, value);
    }

    // 7. Module info from XEX opt headers: entry, stack size, TLS.
    {
        k.entry_point = (uint32_t)img.entry_point;
        const void* ssh = getOptHeaderPtr(xex.data, XEX_HEADER_DEFAULT_STACK_SIZE);
        k.default_stack_size = ssh
            ? (uint32_t)*reinterpret_cast<const be<uint32_t>*>(ssh)
            : 256 * 1024;
        // TLS info: opt header (key low byte != 0/1) is offset-based.
        const void* tls_ptr = getOptHeaderPtr(xex.data, XEX_HEADER_TLS_INFO);
        if (tls_ptr) {
            // Struct: { u32 slot_count, u32 raw_data_address (RVA),
            //           u32 data_size, u32 raw_data_size } all BE.
            auto* p = (const uint8_t*)tls_ptr;
            k.tls_slot_count = Raw32(p + 0);
            k.tls_raw_data_address = Raw32(p + 4);
            k.tls_data_size = Raw32(p + 8);
            k.tls_raw_data_size = Raw32(p + 12);
            if (k.tls_raw_data_address && k.tls_raw_data_address < kImageSize) {
                k.tls_raw_data_address += kImageBase;
            }
        }
        PRLOG(Loader, "entry=%08X default_stack=%u tls(slots=%u data=%u raw=%u@%08X)",
              k.entry_point, k.default_stack_size, k.tls_slot_count,
              k.tls_data_size, k.tls_raw_data_size, k.tls_raw_data_address);
    }

    // 8. Default symbolic link: D: -> \Device\Cdrom0 (XAM normally sets this;
    // the game's own STFS setup also registers it).
    {
        std::lock_guard<std::mutex> lk(k.symlink_mutex);
        k.symbolic_links["D:"] = "\\Device\\Cdrom0";
        k.symbolic_links["game:"] = "\\Device\\Cdrom0";
    }

    // 9. Main thread.
    auto* main = new GuestThread();
    main->name = "Main XThread";
    main->launch.entry = k.entry_point;
    main->launch.arg = 0;
    main->launch.xapi_startup = 0;
    main->launch.creation_flags = 0;   // launched running
    main->launch.stack_size = k.default_stack_size;
    uint32_t main_handle = k.objects.NewHandle(main);
    if (!main->Create(k.default_stack_size)) {
        PRLOGE("main thread create failed");
        return 1;
    }
    k.main_thread = main;
    PRLOG(Loader, "main thread: handle=%08X id=%u stack=%08X-%08X entry=%08X",
          main_handle, main->thread_id, main->stack_limit, main->stack_base,
          main->launch.entry);

    // 10. Execute (main thread runs on the host main thread).
    StartThreadWatchdog();
    StartXenosCommandProcessor();
    PRLOG(Loader, "=== launching title ===");
    int64_t boot_r3 = 1;  // main thread entry arg: r3=1 observed on real HW
    main->launch.arg = 1;
    int exit_code = 0;
    try {
        // Bind this host thread to the main GuestThread (imports, interrupt
        // dispatch and crash diagnostics all resolve the current guest
        // thread through the host-TLS pointer; spawned threads set it in
        // GuestThread::Run(), the main thread runs inline).
        GuestThread::SetCurrentForHostThread(main);
        uint64_t args[1] = {1};
        exit_code = RunGuestFunction(main, k.entry_point, args, 1);
        PRLOG(Loader, "=== title returned: %d (r3=%08X) ===", exit_code,
              (uint32_t)main->ctx->r3.u32);
    } catch (const GuestUnwind& uw) {
        PRLOG(Loader, "=== title unwound to %08X ===", uw.target_pc);
    }

    // Give worker threads a moment to log their state.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return exit_code & 0xFF;
}

}  // namespace pr

int main(int argc, char** argv) {
    const char* xex = argc > 1 ? argv[1] : "default.xex";
    return pr::BootTitle(xex, argc, argv);
}
