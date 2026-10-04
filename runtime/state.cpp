// PrototypeRecomp Phase 2B runtime — state implementation: memory, objects,
// threads, boot.
#include "state.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace pr {

extern "C" void* pr_guest_base_for_crash_c() { return g_guest_base; }


uint8_t* g_guest_base = nullptr;
KernelState* g_kernel = nullptr;
static GuestMemory* g_memory = nullptr;

KernelState& K() { return *g_kernel; }

// =============================================================== GuestMemory

GuestMemory::GuestMemory() {
    RegisterRange("image", kImageBase, kImageBase + kImageSize);
    RegisterRange("lookup_table", kLookupTableBase,
                  kLookupTableBase + kLookupTableSize);
}

GuestMemory& GuestMemory::Get() { return *g_memory; }

bool GuestMemory::Init() {
    void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    g_guest_base = (uint8_t*)p;
    g_memory = new GuestMemory();
    return true;
}

void GuestMemory::RegisterRange(const char* name, uint32_t base, uint32_t end) {
    std::lock_guard<std::mutex> lock(mutex_);
    allocations[base] = {end - base, name};
}

bool GuestMemory::CheckNoOverlap(uint32_t addr, uint32_t size, const char* what) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = allocations.lower_bound(addr);
    // Check previous block for overlap.
    if (it != allocations.begin()) {
        auto prev = std::prev(it);
        uint32_t pb = prev->first, pe = pb + prev->second.first;
        if (addr < pe && addr + size > pb) {
            PRLOGE("MEMORY overlap: %s [%08X+%X) hits %s [%08X+%X)",
                   what, addr, size, prev->second.second.c_str(), pb, prev->second.first);
            return false;
        }
    }
    for (; it != allocations.end() && it->first < addr + size; ++it) {
        uint32_t b = it->first, e = b + it->second.first;
        if (addr < e && addr + size > b) {
            PRLOGE("MEMORY overlap: %s [%08X+%X) hits %s [%08X+%X)",
                   what, addr, size, it->second.second.c_str(), b, it->second.first);
            return false;
        }
    }
    allocations[addr] = {size, what};
    return true;
}

uint32_t GuestRegion::Alloc(uint32_t size, uint32_t alignment) {
    if (alignment < 8) alignment = 8;
    size = (size + 7u) & ~7u;
    // Best-fit from free list.
    for (auto it = frees.begin(); it != frees.end(); ++it) {
        if (it->first >= size) {
            uint32_t addr = it->second;
            uint32_t bsize = it->first;
            frees.erase(it);
            if (bsize - size >= 16) {
                frees.insert({bsize - size, addr + size});
            } else {
                size = bsize;
            }
            live[addr] = size;
            return addr;
        }
    }
    uint32_t addr = (next + alignment - 1) & ~(alignment - 1);
    // 64-bit bounds check: uint32 addition would silently wrap for huge
    // requests (e.g. 0xFFFC0000) and hand out bogus overlapping regions.
    if ((uint64_t)addr + (uint64_t)size > (uint64_t)end) return 0;
    next = addr + size;
    live[addr] = size;
    return addr;
}

void GuestRegion::Free(uint32_t addr) {
    auto it = live.find(addr);
    if (it == live.end()) return;
    uint32_t size = it->second;
    live.erase(it);
    // Coalesce with neighbors.
    uint32_t start = addr, end = addr + size;
    for (auto fit = frees.begin(); fit != frees.end();) {
        uint32_t fb = fit->second, fs = fit->first;
        if (fb + fs == start) { start = fb; fit = frees.erase(fit); }
        else if (end == fb) { end = fb + fs; fit = frees.erase(fit); }
        else ++fit;
    }
    frees.insert({end - start, start});
}

uint32_t GuestMemory::SystemHeapAlloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sys_heap_.Alloc(size, align);
}

void GuestMemory::SystemHeapFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    sys_heap_.Free(addr);
}

uint32_t GuestMemory::PoolAlloc(uint32_t size, uint32_t tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.Alloc(size, 8);
}

void GuestMemory::PoolFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.Free(addr);
}

uint32_t GuestMemory::PhysicalAlloc(uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    return physical_.Alloc((size + 0xFFFFu) & ~0xFFFFu, 0x10000);
}

void GuestMemory::PhysicalFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    physical_.Free(addr & ~0xFFFFu);
}

uint32_t GuestMemory::PhysicalAvailable() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t avail = physical_.end > physical_.next
                         ? physical_.end - physical_.next : 0;
    for (auto& [sz, a] : physical_.frees) {
        (void)a;
        avail += sz;
    }
    return avail;
}

uint32_t GuestMemory::StackAlloc(uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 16KB alignment + two guard pages top and bottom (Xenia model).
    uint32_t actual = (size + 0x3FFFu) & ~0x3FFFu;
    uint32_t a = stacks_.Alloc(actual + 0x2000, 0x4000);
    return a;
}

void GuestMemory::StackFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    stacks_.Free(addr);
}

uint32_t GuestMemory::VirtualAlloc(uint32_t* base_io, uint32_t size,
                                   uint32_t alloc_type, uint32_t protect) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t base_in = *base_io;
    size = (size + 0xFFFu) & ~0xFFFu;
    if (base_in != 0) {
        // Fixed address request: accept if in title heap or untouched region.
        if (title_heap_.Contains(base_in)) {
            title_heap_.live[base_in] = size;
            return X_STATUS_SUCCESS;
        }
        // Treat as reserved elsewhere: only track it.
        return X_STATUS_SUCCESS;
    }
    uint32_t a = title_heap_.Alloc(size, 0x1000);
    if (!a) return X_STATUS_NO_MEMORY;
    *base_io = a;
    return X_STATUS_SUCCESS;
}

uint32_t GuestMemory::VirtualFree(uint32_t base, uint32_t* size_io,
                                  uint32_t free_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    title_heap_.Free(base);
    if (size_io) *size_io = 0;
    return X_STATUS_SUCCESS;
}

uint32_t GuestMemory::QueryAddressProtect(uint32_t addr) const {
    if (addr >= kImageBase && addr < kImageBase + kImageSize) {
        // 0x40 = PAGE_READWRITE, 0x10 = PAGE_EXECUTE (Xbox values approximated).
        if (addr >= PPC_CODE_BASE && addr < PPC_CODE_BASE + PPC_CODE_SIZE)
            return 0x50;  // EXECUTE|READ
        return 0x04;      // READ
    }
    return 0x40;         // READWRITE by default for allocated regions
}

uint64_t GuestMemory::QueryPhysicalAddress(uint32_t guest) const {
    // Physical = guest - 0x80000000 for the 512MB window (Xenia style).
    return (uint64_t)guest;
}

void GuestMemory::DumpMap() {
    std::lock_guard<std::mutex> lock(mutex_);
    PRLOG(Memory, "=== guest memory map ===");
    for (auto& [addr, sz_name] : allocations) {
        PRLOG(Memory, "  %08X - %08X  %s (%u bytes)", addr, addr + sz_name.first,
              sz_name.second.c_str(), sz_name.first);
    }
    PRLOG(Memory, "  sysheap cursor=%08X pool cursor=%08X stacks cursor=%08X "
          "titleheap cursor=%08X physical cursor=%08X",
          sys_heap_.next, pool_.next, stacks_.next, title_heap_.next, physical_.next);
}

// =============================================================== ObjectTable

uint32_t ObjectTable::NewHandle(KernelObject* obj) {
    std::lock_guard<std::mutex> lock(mutex);
    uint32_t h = next_handle_;
    next_handle_ += 4;
    if (next_handle_ < 0xF8000000u) next_handle_ = 0xF8000008u;
    map_[h] = obj;
    obj->handle = h;
    return h;
}

KernelObject* ObjectTable::Lookup(uint32_t handle) {
    if (handle == kCurrentThread) {
        GuestThread* t = GuestThread::GetCurrent();
        if (t) return t;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto it = map_.find(handle);
    return it == map_.end() ? nullptr : it->second;
}

void ObjectTable::Release(uint32_t handle) {
    KernelObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = map_.find(handle);
        if (it == map_.end()) return;
        obj = it->second;
        map_.erase(it);
    }
    if (obj) Deref(obj);
}

void ObjectTable::Retain(KernelObject* obj) {
    if (!obj) return;
    // Refcount is guarded by the table mutex conceptually; use atomics.
    obj->refcount++;  // simple; races acceptable for Phase 2B single process
}

void ObjectTable::Deref(KernelObject* obj) {
    if (!obj) return;
    if (--obj->refcount <= 0) delete obj;
}

// =============================================================== GuestThread

thread_local GuestThread* t_current_thread = nullptr;

GuestThread* GuestThread::GetCurrent() { return t_current_thread; }

GuestThread::~GuestThread() {
    if (ctx) {
        ctx->~PPCContext();
        free(ctx);
    }
}

bool GuestThread::Create(uint32_t stack_size) {
    if (stack_size == 0) stack_size = K().default_stack_size;
    if (stack_size == 0) stack_size = 256 * 1024;
    stack_size = std::max<uint32_t>(0x4000, (stack_size + 0xFFF) & 0xFFFFF000);

    // Stack: guard pages handled implicitly by region layout.
    uint32_t alloc = GuestMemory::Get().StackAlloc(stack_size);
    if (!alloc) { PRLOGE("THREAD stack alloc failed (%u bytes)", stack_size); return false; }
    stack_limit = alloc + 0x1000;            // bottom guard page
    stack_base = stack_limit + stack_size;   // top

    // TLS block.
    uint32_t tls_size = K().tls_slot_count * 4 + K().tls_data_size + 64;
    tls = GuestMemory::Get().SystemHeapAlloc(tls_size, 8);
    GuestMemset(tls, 0, tls_size);
    if (K().tls_data_size && K().tls_raw_data_address) {
        // Copy initializers (they are in BE already).
        GuestMemcpy(tls, K().tls_raw_data_address, K().tls_raw_data_size);
    }

    // PCR (0x2D8).
    pcr = GuestMemory::Get().SystemHeapAlloc(0x2D8, 8);
    GuestMemset(pcr, 0, 0x2D8);
    StoreU32(pcr + 0x000, tls);           // tls_ptr
    StoreU32(pcr + 0x030, pcr);           // pcr_ptr
    StoreU32(pcr + 0x070, stack_base);    // stack_base_ptr
    StoreU32(pcr + 0x074, stack_limit);   // stack_end_ptr
    StoreU32(pcr + 0x100, 0);             // current_thread (patched below)
    StoreU8 (pcr + 0x10C, 0);             // current_cpu
    StoreU32(pcr + 0x150, 0);             // dpc_active

    // Thread scratch (4 * 16B, used by interrupts/APCs).
    scratch = GuestMemory::Get().SystemHeapAlloc(64, 8);

    // KTHREAD (0xAB0).
    kthread = GuestMemory::Get().SystemHeapAlloc(0xAB0, 8);
    GuestMemset(kthread, 0, 0xAB0);
    guest_addr = kthread;
    size = 0xAB0;
    type = kObjTypeThread;
    thread_id = K().next_thread_id++;
    StoreU32(pcr + 0x100, kthread);       // current_thread -> KTHREAD

    InitializeGuestObject();

    // Host PPCContext (64-byte aligned, zeroed).
    ctx = (PPCContext*)aligned_alloc(0x40, sizeof(PPCContext));
    new (ctx) PPCContext();
    memset(ctx, 0, sizeof(PPCContext));
    // PPC FPSCR starts with all FP exceptions DISABLED (masked). The PPC
    // register file never traps on invalid/denormal/overflow operations —
    // they quietly produce NaNs/INF. XenonRecomp mirrors the guest FPSCR
    // onto the host MXCSR via fpscr.setcsr; with a zero-initialized csr the
    // host would run with ALL exceptions UNMASKED and the first NaN math
    // would raise SIGFPE. Initialize to the masked default (0x1F80).
    ctx->fpscr.csr = 0x1F80;
    ctx->fpscr.setcsr(0x1F80);
    ctx->r1.u64 = stack_base;
    ctx->r13.u64 = pcr;
    ctx->msr = 0x200A000;

    {
        std::lock_guard<std::mutex> lk(K().threads_mutex);
        K().threads.push_back(this);
    }
    PRLOG(Thread, "XThread%08X (%u) Stack: %08X-%08X pcr=%08X tls=%08X entry=%08X",
          handle ? handle : 0, thread_id, stack_limit, stack_base, pcr, tls,
          launch.entry);
    return true;
}

void GuestThread::InitializeGuestObject() {
    // Per Xenia XThread::InitializeGuestObject.
    StoreU8(kthread + 0x00, kObjTypeThread);        // type
    StoreU8(kthread + 0x01, 0);                     // signal_state
    StoreU16(kthread + 0x02, 0x1000);               // size
    StoreU32(kthread + 0x010, kthread + 0x010);
    StoreU32(kthread + 0x014, kthread + 0x010);
    StoreU32(kthread + 0x040, kthread + 0x018 + 8);
    StoreU32(kthread + 0x044, kthread + 0x018 + 8);
    StoreU32(kthread + 0x048, kthread);
    StoreU32(kthread + 0x04C, kthread + 0x018);
    StoreU16(kthread + 0x054, 0x102);
    StoreU16(kthread + 0x056, 1);
    StoreU32(kthread + 0x05C, stack_base);
    StoreU32(kthread + 0x060, stack_limit);
    StoreU32(kthread + 0x068, tls);
    StoreU8 (kthread + 0x06C, 0);
    StoreU32(kthread + 0x074, kthread + 0x074);
    StoreU32(kthread + 0x078, kthread + 0x074);
    StoreU32(kthread + 0x07C, kthread + 0x07C);
    StoreU32(kthread + 0x080, kthread + 0x07C);
    StoreU32(kthread + 0x084, K().var_process_info_block);
    StoreU8 (kthread + 0x08B, 1);
    StoreU32(kthread + 0x09C, 0xFDFFD7FFu);
    StoreU32(kthread + 0x0D0, stack_base);
    // create_time (FILETIME-ish 100ns units).
    StoreU64(kthread + 0x130, (uint64_t)time(nullptr) * 10000000ull + 116444736000000000ull);
    StoreU32(kthread + 0x144, kthread + 0x144);
    StoreU32(kthread + 0x148, kthread + 0x144);
    StoreU32(kthread + 0x14C, thread_id);
    StoreU32(kthread + 0x150, launch.entry);
    StoreU32(kthread + 0x154, kthread + 0x154);
    StoreU32(kthread + 0x158, kthread + 0x154);
    StoreU32(kthread + 0x160, 0);       // last_error
    StoreU32(kthread + 0x16C, launch.creation_flags);
    StoreU32(kthread + 0x17C, 1);
    StoreU8 (kthread + 0xBC, (launch.creation_flags & kCreateSuspended) ? 1 : 0);
}

void GuestThread::Run() {
    t_current_thread = this;

    // Xenia quirk-compat: brief yield at thread start.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // Suspend wait if created suspended.
    if (launch.creation_flags & kCreateSuspended) {
        std::unique_lock<std::mutex> lk(suspend_mtx);
        suspend_cv.wait(lk, [&] { return suspend_count == 0; });
    }

    PRLOG(Thread, "thread %u (entry %08X) executing", thread_id, launch.entry);

    uint64_t args[2];
    uint32_t address;
    if (launch.xapi_startup) {
        address = launch.xapi_startup;
        args[0] = launch.entry;
        args[1] = launch.arg;
        RunGuestFunction(this, address, args, 2);
    } else {
        address = launch.entry;
        args[0] = launch.arg;
        RunGuestFunction(this, address, args, 1);
    }
    finished = true;
    // Thread signaled (Xenia: signal_state=1 on exit).
    StoreU8(kthread + 0x01, 1);
    StoreU32(kthread + 0x140, exit_code);
}

int GuestThread::Resume(int* prev) {
    int old = suspend_count.fetch_sub(1);
    if (old < 1) {
        // Wasn't suspended.
        suspend_count.store(0);
        if (prev) *prev = old < 0 ? 0 : old;
        return X_STATUS_SUCCESS;
    }
    if (prev) *prev = old;
    suspend_cv.notify_all();
    return X_STATUS_SUCCESS;
}

int GuestThread::Suspend(int* prev) {
    int old = suspend_count.fetch_add(1);
    if (prev) *prev = old;
    return X_STATUS_SUCCESS;
}

void GuestThread::Join() {
    if (host.joinable()) host.join();
}

void GuestThread::Exit(uint32_t code) {
    exit_code = code;
    finished = true;
    StoreU8(kthread + 0x01, 1);
    throw GuestUnwind(0);  // unwind to thread boundary
}

// =============================================================== helpers

std::string GuestAnsiString(uint32_t addr, uint32_t max_len) {
    std::string s;
    s.reserve(64);
    for (uint32_t i = 0; i < max_len; i++) {
        char c = (char)LoadU8(addr + i);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void GuestWriteAnsiString(uint32_t addr, const std::string& s) {
    GuestWrite(addr, s.data(), (uint32_t)s.size() + 1);
}

int RunGuestFunction(GuestThread* t, uint32_t address,
                     const uint64_t* args, size_t arg_count) {
    PPCFunc* fn = LookupGuestFunc(address);
    if (!fn) {
        PRLOGE("guest function %08X not found in lookup table", address);
        return 0xDEADBABE;
    }
    PPCContext& ctx = *t->ctx;
    uint8_t* base = g_guest_base;

    // Args r3..r10 (non-contiguous in PPCContext!); extras at r1+0x54+8i
    // (Xenia Processor::Execute model: r1 pre-padded by 176 bytes; stack args
    // written at padded r1 + 0x54).
    ctx.r1.u64 = t->stack_base - 176;
    static constexpr PPCRegister PPCContext::* kArgRegs[8] = {
        &PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6,
        &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10};
    for (size_t i = 0; i < arg_count && i < 8; i++) {
        (ctx.*kArgRegs[i]).u64 = args[i];
    }
    for (size_t i = 8; i < arg_count; i++) {
        StoreU32(ctx.r1.u32 + 0x54 + (i - 8) * 8, (uint32_t)args[i]);
    }
    uint64_t prev_lr = ctx.lr;
    ctx.lr = 0xBCBCBCBC;

    int result;
    try {
        fn(ctx, base);
        result = (int)(uint32_t)ctx.r3.u32;
    } catch (const GuestUnwind& gw) {
        if (gw.target_pc != 0) {
            // RtlUnwind-style transfer: re-dispatch at target with carried regs.
            // (gw.r holds raw r3..r10 values in order.)
            static constexpr PPCRegister PPCContext::* kRegs[8] = {
                &PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6,
                &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10};
            for (int i = 0; i < 8; i++) (ctx.*kRegs[i]).u64 = gw.r[i];
            try {
                PPCFunc* fn2 = LookupGuestFunc(gw.target_pc);
                if (fn2) fn2(ctx, base);
                else PRLOGE("unwind target %08X has no function", gw.target_pc);
            } catch (const GuestUnwind&) {
                // nested unwind during unwind: give up cleanly
            }
        }
        result = (int)(uint32_t)ctx.r3.u32;
    }
    ctx.lr = prev_lr;
    return result;
}

// Watchdog: periodic guest-thread status dump (guest LR = last call return).
void StartThreadWatchdog() {
    std::thread([]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::lock_guard<std::mutex> lk(K().threads_mutex);
            PRLOG(Thread, "=== thread status (%zu live) ===", K().threads.size());
            for (auto* t : K().threads) {
                const char* state = t->finished.load() ? "done"
                    : t->suspend_count.load() > 0 ? "suspended" : "running";
                if (t->ctx) {
                    PRLOG(Thread, "  tid=%-3u entry=%08X lr=%08X r1=%08X %s",
                          t->thread_id, t->launch.entry,
                          (uint32_t)t->ctx->lr, (uint32_t)t->ctx->r1.u32, state);
                }
            }
        }
    }).detach();
}

[[noreturn]] void TerminateTitle(uint32_t code, const char* reason) {
    PRLOGE("TERMINATE TITLE: code=%08X reason=%s", code, reason);
    if (K().terminate_notification_fn) {
        // Best effort: notify the title (no reentrancy here).
    }
    _exit((int)(code & 0x7F));
}

}  // namespace pr
