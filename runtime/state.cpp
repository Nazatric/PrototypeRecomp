// PrototypeRecomp Phase 2B runtime — state implementation: memory, objects,
// threads, boot.
#include "state.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

// Generated function mapping table (global scope, from ppc_func_mapping.cpp).
extern PPCFuncMapping PPCFuncMappings[];

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

void GuestThread::SetCurrentForHostThread(GuestThread* t) {
    t_current_thread = t;
}

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
// Enhanced Phase 2B: walks the PPC frame chain (r1 -> [r1]=caller r1,
// [r1+8]=saved LR) and dumps the job-system globals around 0x82DF5148.
static void DumpGuestStackChain(GuestThread* t, int max_frames) {
    uint32_t r1 = (uint32_t)t->ctx->r1.u32;
    uint32_t lr = (uint32_t)t->ctx->lr;
    PRLOG(Thread, "  stack chain tid=%u:", t->thread_id);
    for (int i = 0; i < max_frames && r1 >= 0x70000000 && r1 < 0x80000000; i++) {
        uint32_t next = LoadU32(r1);
        uint32_t saved_lr = LoadU32(r1 + 8);
        PRLOG(Thread, "    frame%d r1=%08X ret=%08X", i, r1,
              i == 0 ? lr : saved_lr);
        if (next <= r1 || next - r1 > 0x40000) break;   // broken chain
        r1 = next;
    }
}

// Heuristic: scan the thread stack region for words that look like .text
// return addresses; symbolizes the outer callers even when frames don't
// save LR at [r1+8] (MW prologues store it at varying offsets).
static void DumpStackRetAddrs(GuestThread* t, int max_hits) {
    uint32_t r1 = (uint32_t)t->ctx->r1.u32;
    uint32_t base = t->stack_base;
    if (r1 < t->stack_limit || r1 >= base) return;
    PRLOG(Thread, "  ret-addr scan tid=%u (r1=%08X..base=%08X):", t->thread_id, r1, base);
    int hits = 0;
    for (uint32_t a = (r1 + 3) & ~3u; a + 4 <= base && hits < max_hits; a += 4) {
        uint32_t v = LoadU32(a);
        if (v >= 0x82230000 && v < 0x82BA880C) {
            PRLOG(Thread, "    [%08X] = %08X (%s)", a, v, GuestFuncName(v));
            hits++;
        }
    }
}

// Symbolize a guest .text address: "sub_XXXXXXXX+off" via binary search over
// the generated PPCFuncMappings table (sorted ascending by guest address).
const char* GuestFuncName(uint32_t addr) {
    static size_t count = 0;
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        size_t n = 0;
        while (PPCFuncMappings[n].host != nullptr) n++;
        count = n;
    });
    if (count == 0) return "?";
    size_t lo = 0, hi = count;
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if ((uint32_t)PPCFuncMappings[mid].guest <= addr) lo = mid; else hi = mid;
    }
    static thread_local char buf[2][32];
    static thread_local int sel = 0;
    sel ^= 1;
    char* p = buf[sel];
    uint32_t start = (uint32_t)PPCFuncMappings[lo].guest;
    if (start == addr) snprintf(p, 32, "sub_%08X", start);
    else snprintf(p, 32, "sub_%08X+%u", start, addr - start);
    return p;
}

static void DumpJobSystemGlobals() {
    // Job-system region around 0x82DF5148 (sub_82ADDA60 initialization).
    struct { uint32_t addr; const char* name; } globals[] = {
        {0x82DF5118, "jobsys CS"},
        {0x82DF5145, "active flag"},
        {0x82DF5148, "dispatch param"},
        {0x82DF514C, "limit 0x8000?"},
        {0x82DF5150, "worker count"},
        {0x82DF5154, "default submit obj"},
    };
    PRLOG(Thread, "  job-system globals:");
    for (auto& g : globals) {
        PRLOG(Thread, "    [%08X] %-18s = %08X", g.addr, g.name,
              LoadU32(g.addr & ~3u));
    }
    // Worker registration array: 8 entries x 284 bytes at 0x82DF4838+20.
    uint32_t count = LoadU32(0x82DF5150);
    for (uint32_t i = 0; i < count && i < 8; i++) {
        uint32_t base = 0x82DF4838 + 20 + i * 284;
        uint32_t job_obj = LoadU32(base);
        char name[17] = {0};
        for (int k = 0; k < 16; k++) name[k] = (char)LoadU8(0x82DF4838 + i * 284 + k);
        PRLOG(Thread, "    worker[%u] name='%s' obj=%08X flag=%02X", i, name,
              job_obj, LoadU8(base + 4));
    }
}

// Dump ATG core::DriveThread object state (layout derived from sub_82AE6210).
static void DumpDriveThread(uint32_t obj) {
    if (!obj || obj < 0x82000000) return;
    PRLOG(Thread, "  DriveThread obj=%08X vtbl=%08X", obj, LoadU32(obj));
    struct { uint32_t off; const char* name; } fields[] = {
        {0x0C, "queue obj"},   {0x54, "list head"},  {0x58, "list last"},
        {0x68, "cur request"}, {0x70, "flag112"},    {0x74, "cur req ptr"},
        {0x79, "busy121"},     {0xD0, "f208"},
    };
    for (auto& f : fields) {
        PRLOG(Thread, "    +%03X %-12s = %08X", f.off, f.name, LoadU32(obj + f.off));
    }
    // Follow the linked list at +0x54 (items with +0 = next?).
    uint32_t item = LoadU32(obj + 0x54);
    for (int i = 0; i < 8 && item >= 0x82000000 && item < 0xC0000000; i++) {
        PRLOG(Thread, "    listItem[%d] = %08X next=%08X vtbl=%08X", i, item,
              LoadU32(item), LoadU32(item + 4));
        uint32_t nx = LoadU32(item);
        if (nx == item) break;
        item = nx;
    }
}

// Dump the REAL ATG thread objects (arg passed to the 0x82236A90
// trampoline): handles at +220/+248, request lists at +224..232, pending
// count at +256, current request at +260.
static void DumpATGThreadObject(uint32_t obj, const char* tag) {
    if (!obj || obj < 0x82000000 || obj >= 0xC0000000) return;
    PRLOG(Thread, "  ATGobj %s %08X vtbl=%08X sem220=%08X sem248=%08X "
          "lists=[%08X %08X %08X] pending=%u cur=%08X",
          tag, obj, LoadU32(obj), LoadU32(obj + 220), LoadU32(obj + 248),
          LoadU32(obj + 224), LoadU32(obj + 228), LoadU32(obj + 232),
          LoadU32(obj + 256), LoadU32(obj + 260));
}

// Dump every semaphore's current count (deadlock diagnosis).
static void DumpSemaphoreCounts() {
    std::lock_guard<std::mutex> lk(K().objects.mutex);
    PRLOG(Thread, "  semaphores (handle obj count):");
    for (auto& [h, obj] : K().objects.map_impl()) {
        if (auto* s = dynamic_cast<GuestSemaphore*>(obj)) {
            std::lock_guard<std::mutex> sl(s->mtx);
            PRLOG(Thread, "    %08X %08X count=%d", h, s->guest_addr, s->count);
        }
    }
}

// Dump GPU ring state: D3D device pull fields, ring buffer contents head,
// and the read-pointer writeback (see VdInitializeRingBuffer / sub_82A7A2C0).
static void DumpGpuRingState() {
    uint32_t dev = K().vd_interrupt_callback_arg;
    if (!dev) dev = 0xA64B0080;   // fallback: previously observed device
    PRLOG(Thread, "  GPU ring: dev=%08X", dev);
    uint32_t mirror_ptr = LoadU32(dev + 10896);
    PRLOG(Thread, "    dev+10896(mirror ptr)=%08X *mirror=%08X dev+10908(wptr)=%08X "
          "dev+10956(cursor)=%08X dev+13232=%08X", mirror_ptr,
          (mirror_ptr >= 0x82000000 && mirror_ptr < 0xC0000000)
              ? LoadU32(mirror_ptr) : 0,
          LoadU32(dev + 10908), LoadU32(dev + 10956), LoadU32(dev + 13232));
    // Interrupt object (dev+10900): +16 = ISR fn, +20 = arg; +4 = flip-pending
    // word the GPU WAIT_REG_MEM packets poll; +0 = flag bits.
    uint32_t intobj = LoadU32(dev + 10900);
    if (intobj >= 0x82000000 && intobj < 0xC0000000) {
        PRLOG(Thread, "    intobj(dev+10900)=%08X +0=%08X +4=%08X +8=%08X "
              "+12=%08X +16=%08X +20=%08X", intobj, LoadU32(intobj),
              LoadU32(intobj + 4), LoadU32(intobj + 8), LoadU32(intobj + 12),
              LoadU32(intobj + 16), LoadU32(intobj + 20));
        uint32_t isr = LoadU32(intobj + 16);
        if (isr >= 0x82000000 && isr < 0x82E60000) {
            PRLOG(Thread, "      ISR symbol: %s", GuestFuncName(isr));
        }
    }
    // Vblank DPC state (sub_82A696D8 fields).
    PRLOG(Thread, "    vblank DPC: frame_ctr[16548]=%08X processed[16568]=%08X "
          "cur[16700]=%08X target[16704]=%08X cb[16572]=%08X cb2[16576]=%08X "
          "pending11012=%08X",
          LoadU32(dev + 16548), LoadU32(dev + 16568), LoadU32(dev + 16700),
          LoadU32(dev + 16704), LoadU32(dev + 16572), LoadU32(dev + 16576),
          LoadU32(dev + 11012));
    // Insert-buffer pool state.
    PRLOG(Thread, "    ibuf: 48=%08X 52=%08X 56=%08X 14908=%08X 14916=%08X "
          "14924=%08X 14928=%08X",
          LoadU32(dev + 48), LoadU32(dev + 52), LoadU32(dev + 56),
          LoadU32(dev + 14908), LoadU32(dev + 14916), LoadU32(dev + 14924),
          LoadU32(dev + 14928));
    uint32_t ring = K().vd_ring_buffer_ptr;
    uint32_t wb = K().vd_rptr_writeback_ptr;
    uint32_t packets = 0, ibs = 0, waits = 0, ints = 0;
    uint64_t frames = XenosGpuStats(&packets, &ibs, &waits, &ints);
    if (ring) {
        PRLOG(Thread, "    ring(PA)=%08X ring(VA)=%08X log2=%u", ring,
              (ring < 0x20000000u) ? (ring | 0xA0000000u) : ring,
              K().vd_ring_buffer_size_log2);
        if (wb) {
            PRLOG(Thread, "    wb(PA)=%08X raw=%08X  wb(VA)=%08X val=%08X", wb,
                  LoadU32(wb),
                  (wb < 0x20000000u) ? (wb | 0xA0000000u) : wb,
                  LoadU32((wb < 0x20000000u) ? (wb | 0xA0000000u) : wb));
        }
        PRLOG(Thread, "    gpu stats: frames=%llu packets=%llu ibs=%llu "
              "blocked_waits=%llu interrupts=%llu",
              (unsigned long long)frames, (unsigned long long)packets,
              (unsigned long long)ibs, (unsigned long long)waits,
              (unsigned long long)ints);
        uint32_t ring_va = (ring < 0x20000000u) ? (ring | 0xA0000000u) : ring;
        // First 24 words of the ring.
        for (int i = 0; i < 24; i++) {
            PRLOG(Thread, "      ring[%d] = %08X", i, LoadU32(ring_va + i * 4));
        }
        // Follow the first PM4_INDIRECT_BUFFER in the ring: payload is
        // {gpu_addr, size}. GPU addr = physical; guest VA = PA | 0xA0000000.
        for (int i = 0; i < 24; i++) {
            uint32_t p = LoadU32(ring_va + i * 4);
            if ((p >> 30) == 3 && ((p >> 8) & 0x7F) == 0x3F) {  // PM4_INDIRECT_BUFFER
                uint32_t gaddr = LoadU32(ring_va + (i + 1) * 4);
                uint32_t gsize = LoadU32(ring_va + (i + 2) * 4);
                uint32_t ib_va = (gaddr & 0x1FFFFFFFu) | 0xA0000000u;
                PRLOG(Thread, "      IB@ring[%d] gpu=%08X size=%08X guestVA=%08X",
                      i, gaddr, gsize, ib_va);
                for (int k = 0; k < 12 && k < (int)gsize; k++) {
                    PRLOG(Thread, "        ib[%d] = %08X", k, LoadU32(ib_va + k * 4));
                }
                break;
            }
        }
    }
}

// Crash-time guest dump: current thread context + frame chain.
extern "C" int pr_crash_dump_guest(const char* tag) {
    GuestThread* t = GuestThread::GetCurrent();
    if (!t || !t->ctx) {
        fprintf(stdout, "[%s] no guest thread context on this host thread\n",
                tag);
        return 0;
    }
    PPCContext& ctx = *t->ctx;
    fprintf(stdout,
            "[%s] guest tid=%u r1=%08X lr=%08X ctr=%08X r3=%08X r4=%08X "
            "r8=%08X r11=%08X r12=%08X\n",
            tag, t->thread_id, (uint32_t)ctx.r1.u32, (uint32_t)ctx.lr,
            (uint32_t)ctx.ctr.u32, (uint32_t)ctx.r3.u32, (uint32_t)ctx.r4.u32,
            (uint32_t)ctx.r8.u32, (uint32_t)ctx.r11.u32,
            (uint32_t)ctx.r12.u32);
    fflush(stdout);
    uint32_t r1 = (uint32_t)ctx.r1.u32;
    for (int i = 0; i < 24 && r1 >= 0x70000000 && r1 < 0x80000000; i++) {
        uint32_t next = LoadU32(r1);
        uint32_t lr = LoadU32(r1 + 8);
        fprintf(stdout, "[%s]   frame%d r1=%08X ret=%08X %s\n", tag, i, r1,
                lr, GuestFuncName(lr));
        fflush(stdout);
        if (next <= r1 || next >= r1 + 0x10000) break;
        r1 = next;
    }
    return 0;
}

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
            // Deep-dive the main thread + job globals every 4th dump.
            static int dump_n = 0;
            if (++dump_n % 4 == 1 && K().main_thread && K().main_thread->ctx) {
                DumpGuestStackChain(K().main_thread, 24);
                DumpStackRetAddrs(K().main_thread, 40);
                // Also dump the first generic worker's wait chain.
                for (auto* t : K().threads) {
                    if (t != K().main_thread && t->ctx && !t->finished.load() &&
                        t->launch.entry == 0x82236A90) {
                        DumpStackRetAddrs(t, 14);
                        break;
                    }
                }
                // Dump the GPU pusher thread (tid with entry via 0x82236A90
                // whose real entry is sub_822646E8, obj 0x82D83A90).
                for (auto* t : K().threads) {
                    if (t->ctx && !t->finished.load() &&
                        t->launch.arg == 0x82D83A90) {
                        DumpStackRetAddrs(t, 16);
                        break;
                    }
                }
                DumpJobSystemGlobals();
                DumpSemaphoreCounts();
                // Known ATG thread objects: DriveThread (thread arg of tid=8),
                // the IO thread object, first pool worker.
                DumpATGThreadObject(LoadU32(0xA19AEEB8 + 12) == 0 ? 0 : 0xA19AEEA0, "drivethread");
                DumpATGThreadObject(0xA19AEEB8, "threadobj8");
                DumpATGThreadObject(0xA19AF3D0, "worker9");
                DumpATGThreadObject(0x82D83A90, "iothread");
                DumpGpuRingState();
                uint32_t count = LoadU32(0x82DF5150);
                for (uint32_t i = 0; i < count && i < 8; i++) {
                    DumpDriveThread(LoadU32(0x82DF4838 + 20 + i * 284));
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
