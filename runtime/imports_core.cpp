// PrototypeRecomp Phase 2B runtime — core kernel imports: memory, threads,
// synchronization, object manager.
#include "state.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <unistd.h>

#include "args.h"

using namespace pr;

// Import boilerplate: guest-visible function with the exact generated-code
// ABI (void __imp__Name(PPCContext&, uint8_t*)). Arguments arrive in
// ctx.r3..ctx.r10; extra arguments at [r1+0x54+8i]; result in ctx.r3.
#define IMPORT(name) \
    void __imp__##name(PPCContext& ctx, uint8_t* base)

#define RET(v) do { ctx.r3.u64 = (uint64_t)(uint32_t)(v); return; } while (0)

// Forward declarations (defined at bottom of file).
GuestEvent* RegisterInPlaceEvent(uint32_t addr, bool manual, bool init);
GuestEvent* LookupInPlaceEvent(uint32_t addr);
GuestSemaphore* RegisterInPlaceSemaphore(uint32_t addr, int32_t init, int32_t limit);
GuestSemaphore* LookupInPlaceSemaphore(uint32_t addr);
void FindThreadByKThread(uint32_t kthread, GuestThread** out);
KernelObject* ResolveGuestObjectPtr(uint32_t guest_ptr);

// =============================================================== memory

IMPORT(ExAllocatePoolTypeWithTag) {
    uint32_t size = ARG(0);
    uint32_t tag = ARG(1);
    uint32_t zero = ARG(2);
    if (size == 0) size = 8;
    uint32_t alignment = size < 4096 ? 8 : 4096;
    uint32_t asize = size < 4096 ? (size + 7u) & ~7u : (size + 0xFFFu) & ~0xFFFu;
    uint32_t addr = GuestMemory::Get().PoolAlloc(asize, tag);
    if (addr && zero == 1) GuestMemset(addr, 0, asize);
    PRLOG(Memory, "ExAllocatePool(size=%u tag=%c%c%c%c zero=%u) = %08X",
          size, (char)(tag >> 24), (char)(tag >> 16), (char)(tag >> 8),
          (char)tag, zero, addr);
    RET(addr);
}

IMPORT(ExFreePool) {
    uint32_t addr = ARG(0);
    if (addr) GuestMemory::Get().PoolFree(addr);
}

IMPORT(NtAllocateVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, alloc_type, protect, debug)
    uint32_t base_ptr = ARG(0);
    uint32_t size_ptr = ARG(1);
    uint32_t alloc_type = ARG(2);
    uint32_t protect = ARG(3);
    uint32_t base_in = LoadU32(base_ptr);
    uint32_t size_in = LoadU32(size_ptr);
    uint32_t out_base = base_in;
    uint32_t status = GuestMemory::Get().VirtualAlloc(&out_base, size_in,
                                                      alloc_type, protect);
    if (status == X_STATUS_SUCCESS) {
        StoreU32(base_ptr, out_base);
        StoreU32(size_ptr, (size_in + 0xFFFu) & ~0xFFFu);
    }
    PRLOG(Memory, "NtAllocateVirtualMemory(base=%08X size=%u type=%08X prot=%08X) "
          "= %08X -> %08X", base_in, size_in, alloc_type, protect, status,
          out_base);
    RET(status);
}

IMPORT(NtFreeVirtualMemory) {
    uint32_t base_ptr = ARG(0);
    uint32_t size_ptr = ARG(1);
    uint32_t free_type = ARG(2);
    uint32_t base_addr = LoadU32(base_ptr);
    uint32_t size = LoadU32(size_ptr);
    uint32_t status = GuestMemory::Get().VirtualFree(base_addr, &size, free_type);
    StoreU32(size_ptr, 0);
    PRLOG(Memory, "NtFreeVirtualMemory(base=%08X type=%08X) = %08X", base_addr,
          free_type, status);
    RET(status);
}

IMPORT(NtQueryVirtualMemory) {
    uint32_t addr = ARG(0);
    uint32_t mb_ptr = ARG(1);  // MEMORY_BASIC_INFORMATION-ish (size in r3)
    // Xbox: returns protection of address. Xenia returns a small struct.
    if (mb_ptr) {
        uint32_t protect = GuestMemory::Get().QueryAddressProtect(addr);
        StoreU32(mb_ptr + 0x00, addr & ~0xFFFFu);   // AllocationBase
        StoreU32(mb_ptr + 0x04, 0x10000);           // RegionSize page
        StoreU32(mb_ptr + 0x08, protect);           // Protect
        StoreU32(mb_ptr + 0x0C, 0x1000);            // Type (private)
    }
    RET(X_STATUS_SUCCESS);
}

IMPORT(MmAllocatePhysicalMemoryEx) {
    // (flags, region_size, protect_bits, min_addr, max_addr, alignment)
    // Real-hardware semantics: the kernel reserves physical memory in the
    // requested range, clamping oversized requests to whatever is available
    // (the game derives the actual pool size from MmQueryStatistics and
    // address arithmetic — there is no size output parameter).
    uint32_t flags = ARG(0);
    uint32_t size = ARG(1);
    uint32_t protect = ARG(2);
    uint32_t min_addr = ARG(3);
    uint32_t max_addr = ARG(4);
    uint32_t alignment = ARG(5);
    if (!(protect & 0xFF)) {
        PRLOGW("MmAllocatePhysicalMemoryEx: bad protect %08X", protect);
        RET(0);
    }
    // Page size from protect flags (XDK: X_MEM_LARGE_PAGES / 16MB pages).
    uint32_t page_size = 4096;
    if (protect & 0x20000000u) page_size = 65536;        // X_MEM_LARGE_PAGES
    else if (protect & 0x80000000u) page_size = 16 << 20; // X_MEM_16MB_PAGES
    size = (size + page_size - 1) & ~(page_size - 1);
    if (size == 0) size = page_size;

    uint32_t addr = GuestMemory::Get().PhysicalAlloc(size);
    bool clamped = false;
    if (addr == 0 && size > 0x10000) {
        // Clamp: retry with the largest remaining arena space.
        uint32_t avail = GuestMemory::Get().PhysicalAvailable();
        if (avail >= 0x10000) {
            avail &= ~(page_size - 1);
            addr = GuestMemory::Get().PhysicalAlloc(avail);
            clamped = addr != 0;
            size = avail;
        }
    }
    PRLOG(Memory, "MmAllocatePhysicalMemoryEx(flags=%u size=%u prot=%08X "
          "range=[%08X,%08X] align=%u) = %08X%s", flags, size, protect, min_addr,
          max_addr, alignment, addr, clamped ? " (clamped)" : "");
    RET(addr);
}

IMPORT(MmFreePhysicalMemory) {
    uint32_t type = ARG(0);
    uint32_t addr = ARG(1);
    if (addr) GuestMemory::Get().PhysicalFree(addr);
}

IMPORT(MmQueryStatistics) {
    // X_MM_QUERY_STATISTICS_RESULT (104 bytes) — exact Xenia layout:
    // +0x00 size, +0x04 total_physical_pages, +0x08 kernel_pages,
    // +0x0C title.available_pages, +0x10 title.total_virtual_memory_bytes,
    // +0x14 title.reserved_virtual_memory_bytes, +0x18 title.physical_pages,
    // +0x1C..0x30 title pool/stack/image/heap/virtual/page_table/cache pages,
    // +0x34 system.* (11 u32s), +0x60 highest_physical_page.
    uint32_t stats_ptr = ARG(0);
    uint32_t len = LoadU32(stats_ptr);
    if (len != 104) {
        PRLOGW("MmQueryStatistics: unexpected size %u", len);
        RET(0xC0000023u);  // STATUS_BUFFER_TOO_SMALL-ish
    }
    GuestMemset(stats_ptr, 0, 104);
    StoreU32(stats_ptr + 0x00, 104);
    StoreU32(stats_ptr + 0x04, 0x20000);  // total_physical_pages: 512MB/4KB
    StoreU32(stats_ptr + 0x08, 0x300);    // kernel_pages
    StoreU32(stats_ptr + 0x0C, 0x1FF00);  // title.available_pages
    StoreU32(stats_ptr + 0x10, 0x2FFF0000);   // title.total_virtual_memory_bytes
    StoreU32(stats_ptr + 0x14, 0x160000);     // title.reserved_virtual_memory_bytes
    StoreU32(stats_ptr + 0x18, 0x1000);       // title.physical_pages
    StoreU32(stats_ptr + 0x1C, 0x10);         // title.pool_pages
    StoreU32(stats_ptr + 0x20, 0x100);        // title.stack_pages
    StoreU32(stats_ptr + 0x24, 0x100);        // title.image_pages
    StoreU32(stats_ptr + 0x28, 0x100);        // title.heap_pages
    StoreU32(stats_ptr + 0x2C, 0x100);        // title.virtual_pages
    StoreU32(stats_ptr + 0x30, 0x100);        // title.page_table_pages
    StoreU32(stats_ptr + 0x5C, 0x100);        // title.cache_pages (offset check)
    StoreU32(stats_ptr + 0x60, 0x1FFFF);      // highest_physical_page
    PRLOG(Memory, "MmQueryStatistics(len=%u) -> 512MB physical", len);
    RET(X_STATUS_SUCCESS);
}

IMPORT(MmGetPhysicalAddress) {
    uint32_t addr = ARG(0);
    // Xbox: physical = virtual - 0x80000000 in the 512MB window. For our
    // single-space layout the identity mapping is reported (GPU work is
    // Phase 2C).
    RET(addr);
}

IMPORT(MmSetAddressProtect) {
    uint32_t addr = ARG(0);
    uint32_t size = ARG(1);
    uint32_t protect = ARG(2);
    RET(X_STATUS_SUCCESS);
}

IMPORT(MmQueryAddressProtect) {
    RET(GuestMemory::Get().QueryAddressProtect(ARG(0)));
}

IMPORT(MmCreateKernelStack) {
    // (size, flags?) -> kernel stack base (used by XAPI for fiber/stack swap)
    uint32_t size = ARG(0);
    uint32_t flags = ARG(1);
    uint32_t alloc = GuestMemory::Get().StackAlloc(size ? size : 0x10000);
    uint32_t top = alloc + 0x1000 + (size ? size : 0x10000);
    PRLOG(Memory, "MmCreateKernelStack(size=%u flags=%08X) = %08X", size, flags,
          top);
    RET(top);
}

IMPORT(MmDeleteKernelStack) {
    uint32_t stack_base = ARG(0);
    uint32_t kthread = ARG(1);
    RET(X_STATUS_SUCCESS);
}

// =============================================================== threads

static void SpawnGuestThread(PPCContext& ctx, uint32_t handle_ptr,
                             uint32_t stack_size, uint32_t thread_id_ptr,
                             uint32_t xapi_startup, uint32_t start_address,
                             uint32_t start_context, uint32_t creation_flags) {
    auto* t = new GuestThread();
    t->launch.entry = start_address;
    t->launch.arg = start_context;
    t->launch.xapi_startup = xapi_startup;
    t->launch.creation_flags = creation_flags;
    t->launch.stack_size = stack_size;
    uint32_t handle = K().objects.NewHandle(t);
    if (!t->Create(stack_size)) {
        delete t;
        if (handle_ptr) StoreU32(handle_ptr, 0);
        RET(X_STATUS_NO_MEMORY);
    }
    t->host = std::thread([t]() { t->Run(); });
    t->host.detach();
    if (creation_flags & kCreateFlag0x80) {
        if (handle_ptr) StoreU32(handle_ptr, t->kthread);
    } else {
        if (handle_ptr) StoreU32(handle_ptr, handle);
    }
    if (thread_id_ptr) StoreU32(thread_id_ptr, t->thread_id);
    RET(X_STATUS_SUCCESS);
}

IMPORT(ExCreateThread) {
    // (handle_ptr, stack_size, thread_id_ptr, xapi_startup, start_address,
    //  start_context, creation_flags)
    SpawnGuestThread(ctx, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5),
                     ARG(6));
}

IMPORT(ExTerminateThread) {
    uint32_t exit_code = ARG(0);
    GuestThread* t = GuestThread::GetCurrent();
    if (!t) {
        PRLOGE("ExTerminateThread on host thread");
        TerminateTitle(exit_code, "ExTerminateThread on main/host thread");
    }
    t->Exit(exit_code);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtResumeThread) {
    uint32_t handle = ARG(0);
    uint32_t prev_ptr = ARG(1);
    int prev = 0;
    auto* obj = K().objects.Lookup(handle);
    uint32_t status = X_STATUS_INVALID_HANDLE;
    if (auto* t = dynamic_cast<GuestThread*>(obj)) {
        status = t->Resume(&prev);
    } else if (obj) {
        // Xenia: non-thread object -> invalid handle? Threads only.
        status = X_STATUS_INVALID_HANDLE;
    }
    if (prev_ptr) StoreU32(prev_ptr, prev);
    PRLOG(Thread, "NtResumeThread(%08X) = %08X prev=%d", handle, status, prev);
    RET(status);
}

IMPORT(KeResumeThread) {
    uint32_t thread_ptr = ARG(0);  // KTHREAD pointer
    uint32_t status = X_STATUS_INVALID_HANDLE;
    GuestThread* t = nullptr;
    FindThreadByKThread(thread_ptr, &t);
    if (t) {
        t->Resume();
        status = X_STATUS_SUCCESS;
    }
    RET(status);
}

IMPORT(KeSetBasePriorityThread) {
    uint32_t thread_ptr = ARG(0);  // KTHREAD (deref'd from handle earlier)
    int32_t priority = (int32_t)ARG(1);
    GuestThread* t = nullptr;
    FindThreadByKThread(thread_ptr, &t);
    if (t && t->host.joinable() == false) {
        // detached thread: adjust priority via native handle if available.
    }
    PRLOG(Thread, "KeSetBasePriorityThread(%08X, %d)", thread_ptr, priority);
    RET(0);
}

IMPORT(KeSetAffinityThread) {
    uint32_t thread_ptr = ARG(0);
    uint32_t affinity = ARG(1);
    RET(affinity);  // previous affinity
}

IMPORT(KeSetCurrentStackPointers) {
    // (stack_base, stack_limit) — updates current thread stack bounds.
    uint32_t stack_base = ARG(0);
    uint32_t stack_limit = ARG(1);
    GuestThread* t = GuestThread::GetCurrent();
    if (t) {
        t->stack_base = stack_base;
        t->stack_limit = stack_limit;
        StoreU32(t->kthread + 0x5C, stack_base);
        StoreU32(t->kthread + 0x60, stack_limit);
        // r1 assumed set by caller (XapiThreadStartup switches stack).
    }
    RET(0);
}

IMPORT(KeGetCurrentProcessType) {
    RET(1);  // 1 = user process
}

IMPORT(NtTerminateThread) {
    uint32_t exit_code = ARG(0);
    GuestThread* t = GuestThread::GetCurrent();
    if (t) t->Exit(exit_code);
    RET(X_STATUS_SUCCESS);
}

// =============================================================== TLS

IMPORT(KeTlsAlloc) {
    std::lock_guard<std::mutex> lock(K().tls_slot_mutex);
    for (uint32_t i = 0; i < 64; i++) {
        if (!(K().tls_slots_used & (1ull << i))) {
            K().tls_slots_used |= (1ull << i);
            RET(i);
        }
    }
    RET(0xFFFFFFFF);
}

IMPORT(KeTlsFree) {
    uint32_t slot = ARG(0);
    std::lock_guard<std::mutex> lock(K().tls_slot_mutex);
    if (slot < 64 && (K().tls_slots_used & (1ull << slot))) {
        K().tls_slots_used &= ~(1ull << slot);
        RET(1);
    }
    RET(0);
}

IMPORT(KeTlsGetValue) {
    uint32_t slot = ARG(0);
    GuestThread* t = GuestThread::GetCurrent();
    RET(t ? LoadU32(t->tls + slot * 4) : 0);
}

IMPORT(KeTlsSetValue) {
    uint32_t slot = ARG(0);
    uint32_t value = ARG(1);
    GuestThread* t = GuestThread::GetCurrent();
    if (t) StoreU32(t->tls + slot * 4, value);
    RET(1);
}

// =============================================================== sync: events

static GuestEvent* CreateGuestEvent(bool manual_reset, bool initial_state) {
    auto* ev = new GuestEvent();
    ev->manual_reset = manual_reset;
    ev->signaled = initial_state;
    ev->type = kObjTypeEvent;
    // Guest-visible body: DISPATCH_HEADER (8 bytes: type, signal_state,
    // size, pointer).
    ev->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x18, 8);
    ev->size = 0x18;
    GuestMemset(ev->guest_addr, 0, 0x18);
    StoreU8(ev->guest_addr + 0x00, kObjTypeEvent);
    StoreU8(ev->guest_addr + 0x01, initial_state ? 1 : 0);
    StoreU16(ev->guest_addr + 0x02, 0x18);
    return ev;
}

IMPORT(NtCreateEvent) {
    // (handle_ptr, obj_attributes, manual_reset?, initial_state)
    uint32_t handle_ptr = ARG(0);
    uint32_t obj_attrs = ARG(1);
    uint32_t event_type = ARG(2);  // 0 = notification (manual), 1 = sync (auto)
    uint32_t initial_state = ARG(3);
    auto* ev = CreateGuestEvent(event_type == 0, initial_state != 0);
    uint32_t handle = K().objects.NewHandle(ev);
    if (handle_ptr) StoreU32(handle_ptr, handle);
    PRLOG(Sync, "NtCreateEvent(type=%u init=%u) = handle %08X obj %08X",
          event_type, initial_state, handle, ev->guest_addr);
    RET(X_STATUS_SUCCESS);
}

IMPORT(KeInitializeEvent) {
    // (event_ptr guest KEVENT, type, state) — in-place initialization!
    uint32_t ev_ptr = ARG(0);
    uint32_t type = ARG(1);   // 0=notification 1=synchronization
    uint32_t state = ARG(2);
    // Register the guest KEVENT as a kernel object keyed by address.
    // We keep a side table: guest_addr -> GuestEvent.
    RegisterInPlaceEvent(ev_ptr, type == 0, state != 0);
    StoreU8(ev_ptr + 0x00, kObjTypeEvent);
    StoreU8(ev_ptr + 0x01, state ? 1 : 0);
    StoreU16(ev_ptr + 0x02, 0x18);
    RET(0);
}

IMPORT(NtSetEvent) {
    uint32_t handle = ARG(0);
    uint32_t prev_ptr = ARG(1);
    auto* obj = K().objects.Lookup(handle);
    if (auto* ev = dynamic_cast<GuestEvent*>(obj)) {
        std::lock_guard<std::mutex> lk(ev->mtx);
        bool prev = ev->signaled;
        ev->signaled = true;
        StoreU8(ev->guest_addr + 0x01, 1);
        ev->cv.notify_all();
        if (prev_ptr) StoreU32(prev_ptr, prev ? 1 : 0);
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

IMPORT(KeSetEvent) {
    // (KEVENT ptr, increment, wait)
    uint32_t ev_ptr = ARG(0);
    uint32_t increment = ARG(1);
    uint32_t wait = ARG(2);
    GuestEvent* ev = LookupInPlaceEvent(ev_ptr);
    if (!ev) { ev = RegisterInPlaceEvent(ev_ptr, true, false); }
    if (ev) {
        std::lock_guard<std::mutex> lk(ev->mtx);
        ev->signaled = true;
        StoreU8(ev_ptr + 0x01, 1);
        ev->cv.notify_all();
    }
    RET(0);
}

IMPORT(NtClearEvent) {
    uint32_t handle = ARG(0);
    auto* obj = K().objects.Lookup(handle);
    if (auto* ev = dynamic_cast<GuestEvent*>(obj)) {
        std::lock_guard<std::mutex> lk(ev->mtx);
        ev->signaled = false;
        StoreU8(ev->guest_addr + 0x01, 0);
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

IMPORT(KeResetEvent) {
    uint32_t ev_ptr = ARG(0);
    GuestEvent* ev = LookupInPlaceEvent(ev_ptr);
    if (ev) {
        std::lock_guard<std::mutex> lk(ev->mtx);
        ev->signaled = false;
        StoreU8(ev_ptr + 0x01, 0);
    }
    RET(0);
}

// =============================================================== semaphores

static GuestSemaphore* CreateGuestSemaphore(int32_t initial, int32_t limit) {
    auto* sem = new GuestSemaphore();
    sem->count = initial;
    sem->limit = limit;
    sem->type = kObjTypeSemaphore;
    sem->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x18, 8);
    sem->size = 0x18;
    GuestMemset(sem->guest_addr, 0, 0x18);
    StoreU8(sem->guest_addr + 0x00, kObjTypeSemaphore);
    StoreU8(sem->guest_addr + 0x01, initial > 0 ? 1 : 0);
    return sem;
}

IMPORT(NtCreateSemaphore) {
    // (handle_ptr, attrs, initial, limit)
    uint32_t handle_ptr = ARG(0);
    uint32_t attrs = ARG(1);
    uint32_t initial = ARG(2);
    uint32_t limit = ARG(3);
    auto* sem = CreateGuestSemaphore((int32_t)initial, (int32_t)limit);
    uint32_t handle = K().objects.NewHandle(sem);
    if (handle_ptr) StoreU32(handle_ptr, handle);
    PRLOG(Sync, "NtCreateSemaphore(init=%u limit=%u) = handle %08X obj %08X",
          initial, limit, handle, sem->guest_addr);
    RET(X_STATUS_SUCCESS);
}

IMPORT(KeInitializeSemaphore) {
    // (sem_ptr guest KSEMAPHORE, limit, initial)? Xbox: (Limit, Initial)?? 
    // Xenia: (lpvoid sem, dword limit, dword initial)
    uint32_t sem_ptr = ARG(0);
    int32_t limit = (int32_t)ARG(1);
    int32_t initial = (int32_t)ARG(2);
    RegisterInPlaceSemaphore(sem_ptr, initial, limit);
    StoreU8(sem_ptr + 0x00, kObjTypeSemaphore);
    StoreU8(sem_ptr + 0x01, initial > 0 ? 1 : 0);
    RET(0);
}

IMPORT(KeReleaseSemaphore) {
    // (sem_ptr, adjustment, new_count_ptr, wait)
    uint32_t sem_ptr = ARG(0);
    uint32_t adjustment = ARG(1);
    uint32_t new_count_ptr = ARG(2);
    uint32_t wait = ARG(3);
    GuestSemaphore* sem = LookupInPlaceSemaphore(sem_ptr);
    if (!sem) sem = RegisterInPlaceSemaphore(sem_ptr, 0, 0x7FFFFFFF);
    int32_t prev = 0;
    if (sem) {
        std::lock_guard<std::mutex> lk(sem->mtx);
        prev = sem->count;
        sem->count = std::min<int32_t>(sem->count + (int32_t)adjustment,
                                       sem->limit);
        StoreU8(sem_ptr + 0x01, sem->count > 0 ? 1 : 0);
        sem->cv.notify_all();
    }
    if (new_count_ptr) StoreU32(new_count_ptr, (uint32_t)prev);
    RET(0);
}

IMPORT(NtReleaseSemaphore) {
    uint32_t handle = ARG(0);
    uint32_t count = ARG(1);
    uint32_t prev_ptr = ARG(2);
    auto* obj = K().objects.Lookup(handle);
    if (auto* sem = dynamic_cast<GuestSemaphore*>(obj)) {
        std::lock_guard<std::mutex> lk(sem->mtx);
        int32_t prev = sem->count;
        sem->count = std::min<int32_t>(sem->count + (int32_t)count, sem->limit);
        StoreU8(sem->guest_addr + 0x01, sem->count > 0 ? 1 : 0);
        if (prev_ptr) StoreU32(prev_ptr, (uint32_t)prev);
        sem->cv.notify_all();
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

// =============================================================== mutants

IMPORT(NtCreateMutant) {
    uint32_t handle_ptr = ARG(0);
    uint32_t attrs = ARG(1);
    uint32_t initial_owned = ARG(2);
    auto* mut = new GuestMutant();
    mut->type = kObjTypeMutant;
    mut->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x18, 8);
    mut->size = 0x18;
    GuestMemset(mut->guest_addr, 0, 0x18);
    StoreU8(mut->guest_addr + 0x00, kObjTypeMutant);
    if (initial_owned) {
        GuestThread* t = GuestThread::GetCurrent();
        if (t) {
            mut->owner = t->kthread;
            mut->recursion = 1;
        }
    }
    uint32_t handle = K().objects.NewHandle(mut);
    if (handle_ptr) StoreU32(handle_ptr, handle);
    PRLOG(Sync, "NtCreateMutant(owned=%u) = handle %08X obj %08X",
          initial_owned, handle, mut->guest_addr);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtReleaseMutant) {
    uint32_t handle = ARG(0);
    uint32_t prev_count_ptr = ARG(1);
    auto* obj = K().objects.Lookup(handle);
    if (auto* mut = dynamic_cast<GuestMutant*>(obj)) {
        std::lock_guard<std::mutex> lk(mut->mtx);
        if (prev_count_ptr) StoreU32(prev_count_ptr, (uint32_t)mut->recursion);
        if (mut->recursion > 0) mut->recursion--;
        if (mut->recursion == 0) {
            mut->owner = 0;
            StoreU8(mut->guest_addr + 0x01, 1);  // signaled when unowned
            mut->cv.notify_all();
        }
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

// =============================================================== waits

static bool ObjectSignaled(KernelObject* obj) {
    if (auto* t = dynamic_cast<GuestThread*>(obj)) return t->finished.load();
    if (auto* ev = dynamic_cast<GuestEvent*>(obj)) return ev->signaled;
    if (auto* sem = dynamic_cast<GuestSemaphore*>(obj)) return sem->count > 0;
    if (auto* mut = dynamic_cast<GuestMutant*>(obj)) return mut->owner == 0;
    if (auto* tim = dynamic_cast<GuestTimer*>(obj)) return tim->signaled;
    return false;
}

static int WaitSingleObjectKernel(KernelObject* obj, uint64_t timeout_100ns,
                                  bool have_timeout) {
    using namespace std::chrono;
    if (auto* t = dynamic_cast<GuestThread*>(obj)) {
        if (t->finished.load()) return 0;
        // Thread wait: poll with small sleeps (host join may deadlock the
        // object-table design); 5ms quantum.
        steady_clock::time_point deadline =
            steady_clock::now() + nanoseconds(timeout_100ns / 100);
        while (!t->finished.load()) {
            if (have_timeout && timeout_100ns != 0xFFFFFFFFFFFFFFFFull) {
                if (timeout_100ns == 0) return 0x102;  // TIMEOUT
                if (steady_clock::now() >= deadline) return 0x102;
            }
            std::this_thread::sleep_for(milliseconds(5));
        }
        return 0;
    }
    if (auto* ev = dynamic_cast<GuestEvent*>(obj)) {
        std::unique_lock<std::mutex> lk(ev->mtx);
        if (!ev->signaled) {
            if (have_timeout && timeout_100ns == 0) return 0x102;
            if (have_timeout && timeout_100ns != 0xFFFFFFFFFFFFFFFFull) {
                if (ev->cv.wait_for(lk, nanoseconds(timeout_100ns / 100),
                                    [&] { return ev->signaled; }))
                    goto acquired_ev;
                return 0x102;
            }
            ev->cv.wait(lk, [&] { return ev->signaled; });
        }
    acquired_ev:
        if (!ev->manual_reset) {
            ev->signaled = false;
            StoreU8(ev->guest_addr + 0x01, 0);
        }
        return 0;
    }
    if (auto* sem = dynamic_cast<GuestSemaphore*>(obj)) {
        std::unique_lock<std::mutex> lk(sem->mtx);
        if (sem->count <= 0) {
            if (have_timeout && timeout_100ns == 0) return 0x102;
            if (have_timeout && timeout_100ns != 0xFFFFFFFFFFFFFFFFull) {
                if (sem->cv.wait_for(lk, nanoseconds(timeout_100ns / 100),
                                     [&] { return sem->count > 0; }))
                    goto acquired_sem;
                return 0x102;
            }
            sem->cv.wait(lk, [&] { return sem->count > 0; });
        }
    acquired_sem:
        sem->count--;
        StoreU8(sem->guest_addr + 0x01, sem->count > 0 ? 1 : 0);
        return 0;
    }
    if (auto* mut = dynamic_cast<GuestMutant*>(obj)) {
        GuestThread* cur = GuestThread::GetCurrent();
        std::unique_lock<std::mutex> lk(mut->mtx);
        if (mut->owner != 0 && cur && mut->owner != cur->kthread) {
            if (have_timeout && timeout_100ns == 0) return 0x102;
            if (have_timeout && timeout_100ns != 0xFFFFFFFFFFFFFFFFull) {
                if (mut->cv.wait_for(lk, nanoseconds(timeout_100ns / 100),
                                     [&] { return mut->owner == 0; }))
                    goto acquired_mut;
                return 0x102;
            }
            mut->cv.wait(lk, [&] { return mut->owner == 0; });
        }
    acquired_mut:
        if (cur && mut->owner == cur->kthread) mut->recursion++;
        else { mut->owner = cur ? cur->kthread : 0; mut->recursion = 1; }
        StoreU8(mut->guest_addr + 0x01, 0);
        return 0;
    }
    if (auto* tim = dynamic_cast<GuestTimer*>(obj)) {
        if (!tim->signaled) {
            if (have_timeout && timeout_100ns != 0xFFFFFFFFFFFFFFFFull) {
                return 0x102;
            }
        } else {
            tim->signaled = false;
        }
        return 0;
    }
    return (int)X_STATUS_INVALID_HANDLE;
}

IMPORT(KeWaitForSingleObject) {
    // (object_ptr, wait_reason, processor_mode, alertable, timeout_ptr)
    uint32_t obj_ptr = ARG(0);
    uint32_t wait_reason = ARG(1);
    uint32_t proc_mode = ARG(2);
    uint32_t alertable = ARG(3);
    uint32_t timeout_ptr = ARG(4);
    uint64_t timeout = 0;
    bool have_timeout = timeout_ptr != 0;
    if (timeout_ptr) timeout = LoadU64(timeout_ptr);

    // The object_ptr is a GUEST object address (KEVENT/KSEMAPHORE/KTHREAD).
    KernelObject* obj = ResolveGuestObjectPtr(obj_ptr);
    if (!obj) {
        // Lazily adopt in-place guest objects by their DISPATCH_HEADER type
        // byte (games hand-initialize D3D pool events without imports).
        uint8_t hdr_type = LoadU8(obj_ptr);
        static std::set<uint32_t> reported;
        if (reported.insert(obj_ptr).second) {
            LogLineOnce(LogCategory::kWarn,
                        "KeWaitForSingleObject: unknown object %08X "
                        "(hdr type=%u lr=%08X thread=%u) — adopting as event",
                        obj_ptr, hdr_type, (uint32_t)ctx.lr,
                        GuestThread::GetCurrent()
                            ? GuestThread::GetCurrent()->thread_id : 0);
        }
        if (hdr_type > 8) {
            // Not a plausible dispatch header: refuse.
            RET(X_STATUS_INVALID_HANDLE);
        }
        // type 0 = NotificationEvent; honor the guest signal_state at +1.
        obj = RegisterInPlaceEvent(obj_ptr, true, LoadU8(obj_ptr + 1) != 0);
    }
    int r = WaitSingleObjectKernel(obj, timeout, have_timeout);
    if (r == 0x102 && have_timeout && timeout == 0) {
        // zero timeout: not an error, STATUS_TIMEOUT reported to caller.
    }
    RET((uint32_t)r);
}

IMPORT(NtWaitForSingleObjectEx) {
    // (handle, alertable, timeout_ptr)
    uint32_t handle = ARG(0);
    uint32_t alertable = ARG(1);
    uint32_t timeout_ptr = ARG(2);
    uint64_t timeout = 0;
    bool have_timeout = timeout_ptr != 0;
    if (timeout_ptr) timeout = LoadU64(timeout_ptr);
    KernelObject* obj = K().objects.Lookup(handle);
    if (!obj) {
        PRLOGW("NtWaitForSingleObjectEx: bad handle %08X", handle);
        RET(X_STATUS_INVALID_HANDLE);
    }
    RET((uint32_t)WaitSingleObjectKernel(obj, timeout, have_timeout));
}

IMPORT(NtWaitForMultipleObjectsEx) {
    // (count, handles_ptr, wait_type, wait_mode, alertable, timeout_ptr)
    uint32_t count = ARG(0);
    uint32_t handles_ptr = ARG(1);
    uint32_t wait_type = ARG(2);   // 0 = wait all, 1 = wait any
    uint32_t wait_mode = ARG(3);
    uint32_t alertable = ARG(4);
    uint32_t timeout_ptr = ARG(5);
    uint64_t timeout = 0;
    bool have_timeout = timeout_ptr != 0;
    if (timeout_ptr) timeout = LoadU64(timeout_ptr);

    std::vector<KernelObject*> objs;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t h = LoadU32(handles_ptr + i * 4);
        KernelObject* o = K().objects.Lookup(h);
        if (!o) { RET(X_STATUS_INVALID_HANDLE); }
        objs.push_back(o);
    }
    using namespace std::chrono;
    steady_clock::time_point deadline =
        steady_clock::now() + nanoseconds(have_timeout ? (int64_t)(timeout / 100) : -1);

    if (wait_type == 0) {
        // Wait all: poll loop (rare in games).
        for (;;) {
            bool all = true;
            for (auto* o : objs) if (!ObjectSignaled(o)) { all = false; break; }
            if (all) {
                for (auto* o : objs) {
                    // Consume auto-reset semantics.
                    WaitSingleObjectKernel(o, 0, false);
                }
                RET(X_STATUS_WAIT_0);
            }
            if (have_timeout && timeout != 0xFFFFFFFFFFFFFFFFull) {
                if (timeout == 0 || steady_clock::now() >= deadline) RET(0x102);
            }
            std::this_thread::sleep_for(milliseconds(2));
        }
    } else {
        // Wait any: race via poll (correctness over efficiency for Phase 2B).
        for (;;) {
            for (uint32_t i = 0; i < objs.size(); i++) {
                if (ObjectSignaled(objs[i])) {
                    WaitSingleObjectKernel(objs[i], 0, false);
                    RET(X_STATUS_WAIT_0 + i);
                }
            }
            if (have_timeout && timeout != 0xFFFFFFFFFFFFFFFFull) {
                if (timeout == 0 || steady_clock::now() >= deadline) RET(0x102);
            }
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
}

IMPORT(KeWaitForMultipleObjects) {
    // (count, objects_ptr, wait_type, wait_reason, wait_mode, alertable,
    //  timeout_ptr) — objects are GUEST OBJECT POINTERS.
    uint32_t count = ARG(0);
    uint32_t objects_ptr = ARG(1);
    uint32_t wait_type = ARG(2);
    uint32_t timeout_ptr = ARG(6);
    uint64_t timeout = 0;
    bool have_timeout = timeout_ptr != 0;
    if (timeout_ptr) timeout = LoadU64(timeout_ptr);
    extern KernelObject* ResolveGuestObjectPtr(uint32_t guest_ptr);
    std::vector<KernelObject*> objs;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = LoadU32(objects_ptr + i * 4);
        KernelObject* o = ResolveGuestObjectPtr(p);
        if (!o) { RET(X_STATUS_INVALID_HANDLE); }
        objs.push_back(o);
    }
    if (wait_type == 0) {
        for (;;) {
            bool all = true;
            for (auto* o : objs) if (!ObjectSignaled(o)) { all = false; break; }
            if (all) {
                for (auto* o : objs) WaitSingleObjectKernel(o, 0, false);
                RET(X_STATUS_WAIT_0);
            }
            if (have_timeout && timeout != 0xFFFFFFFFFFFFFFFFull) {
                if (timeout == 0) RET(0x102);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } else {
        for (;;) {
            for (uint32_t i = 0; i < objs.size(); i++) {
                if (ObjectSignaled(objs[i])) {
                    WaitSingleObjectKernel(objs[i], 0, false);
                    RET(X_STATUS_WAIT_0 + i);
                }
            }
            if (have_timeout && timeout != 0xFFFFFFFFFFFFFFFFull) {
                if (timeout == 0) RET(0x102);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

IMPORT(KeDelayExecutionThread) {
    // (alertable, interval_ptr) — r3=alertable r4=PLARGE_INTEGER interval
    uint32_t alertable = ARG(0);
    uint32_t interval_ptr = ARG(1);
    if (interval_ptr) {
        uint64_t interval = LoadU64(interval_ptr);
        // Negative = relative (100ns units). Positive = absolute.
        int64_t rel = (int64_t)interval;
        if (rel < 0) {
            rel = -rel;
            auto ns = std::chrono::nanoseconds(rel / 100);
            std::this_thread::sleep_for(ns);
        } else if (rel > 0) {
            // Absolute FILETIME.
            uint64_t now = (uint64_t)time(nullptr) * 10000000ull +
                           116444736000000000ull;
            if (rel > now) {
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds((rel - now) / 100));
            }
        }
    }
    RET(X_STATUS_SUCCESS);
}

// =============================================================== critical sections

// X_RTL_CRITICAL_SECTION (28 bytes):
// +0x00 DISPATCH_HEADER { type:u8, signal_state:u8, absolute:u16 }
// +0x08 lock_count:i32
// +0x0C recursion_count:u32
// +0x10 owning_thread:u32
// +0x14 h semaphore ptr
struct GuestCS {
    std::mutex mtx;
    std::condition_variable cv;
    int host_lock_count = 0;    // == lock_count+1 when locked
    uint32_t owner = 0;
    int recursion = 0;
    std::deque<uint32_t> waiters;
};
static std::mutex g_cs_map_mutex;
static std::unordered_map<uint32_t, GuestCS*> g_cs_map;

static GuestCS* GetCS(uint32_t cs_ptr) {
    std::lock_guard<std::mutex> lk(g_cs_map_mutex);
    auto it = g_cs_map.find(cs_ptr);
    return it == g_cs_map.end() ? nullptr : it->second;
}

static GuestCS* MakeCS(uint32_t cs_ptr) {
    std::lock_guard<std::mutex> lk(g_cs_map_mutex);
    auto it = g_cs_map.find(cs_ptr);
    if (it != g_cs_map.end()) return it->second;
    auto* cs = new GuestCS();
    g_cs_map[cs_ptr] = cs;
    return cs;
}

static void CSInit(uint32_t cs_ptr, uint32_t spin_count) {
    uint32_t spin_div = (spin_count + 255) >> 8;
    if (spin_div > 255) spin_div = 255;
    StoreU8(cs_ptr + 0x00, 1);                   // type: auto-reset event
    StoreU16(cs_ptr + 0x02, (uint16_t)spin_div); // absolute (spin count / 256)
    StoreU8(cs_ptr + 0x01, 0);                   // signal_state
    StoreU32(cs_ptr + 0x08, 0xFFFFFFFF);         // lock_count = -1
    StoreU32(cs_ptr + 0x0C, 0);                  // recursion_count
    StoreU32(cs_ptr + 0x10, 0);                  // owning_thread
    StoreU32(cs_ptr + 0x14, 0);                  // semaphore ptr (unused)
    MakeCS(cs_ptr);
}

IMPORT(RtlInitializeCriticalSection) {
    CSInit(ARG(0), 0);
}

IMPORT(RtlInitializeCriticalSectionAndSpinCount) {
    CSInit(ARG(0), ARG(1));
}

IMPORT(RtlEnterCriticalSection) {
    uint32_t cs_ptr = ARG(0);
    GuestThread* cur = GuestThread::GetCurrent();
    uint32_t cur_kthread = cur ? cur->kthread : 0;
    GuestCS* cs = MakeCS(cs_ptr);
    {
        std::unique_lock<std::mutex> lk(cs->mtx);
        if (cs->owner == cur_kthread && cur_kthread != 0) {
            cs->recursion++;
            StoreU32(cs_ptr + 0x0C, cs->recursion);
            // lock_count++ mirrors Xenia
            uint32_t lc = LoadU32(cs_ptr + 0x08);
            StoreU32(cs_ptr + 0x08, lc + 1);
            return;
        }
        cs->waiters.push_back(cur_kthread);
        cs->cv.wait(lk, [&] {
            return cs->owner == 0 &&
                   (!cs->waiters.empty() && cs->waiters.front() == cur_kthread);
        });
        cs->waiters.pop_front();
        cs->owner = cur_kthread;
        cs->recursion = 1;
    }
    StoreU32(cs_ptr + 0x08, 0);
    StoreU32(cs_ptr + 0x0C, 1);
    StoreU32(cs_ptr + 0x10, cur_kthread);
}

IMPORT(RtlTryEnterCriticalSection) {
    uint32_t cs_ptr = ARG(0);
    GuestThread* cur = GuestThread::GetCurrent();
    uint32_t cur_kthread = cur ? cur->kthread : 0;
    GuestCS* cs = MakeCS(cs_ptr);
    {
        std::lock_guard<std::mutex> lk(cs->mtx);
        if (cs->owner == 0) {
            cs->owner = cur_kthread;
            cs->recursion = 1;
            StoreU32(cs_ptr + 0x08, 0);
            StoreU32(cs_ptr + 0x0C, 1);
            StoreU32(cs_ptr + 0x10, cur_kthread);
            RET(1);
        }
        if (cs->owner == cur_kthread && cur_kthread != 0) {
            cs->recursion++;
            StoreU32(cs_ptr + 0x0C, cs->recursion);
            RET(1);
        }
    }
    RET(0);
}

IMPORT(RtlLeaveCriticalSection) {
    uint32_t cs_ptr = ARG(0);
    GuestThread* cur = GuestThread::GetCurrent();
    uint32_t cur_kthread = cur ? cur->kthread : 0;
    GuestCS* cs = GetCS(cs_ptr);
    if (!cs) { PRLOGW("RtlLeaveCriticalSection on uninit CS %08X", cs_ptr); return; }
    {
        std::lock_guard<std::mutex> lk(cs->mtx);
        if (cs->owner != cur_kthread) {
            PRLOGW("CS %08X leave by non-owner %08X (owner %08X)", cs_ptr,
                   cur_kthread, cs->owner);
        }
        if (--cs->recursion > 0) {
            StoreU32(cs_ptr + 0x0C, cs->recursion);
            uint32_t lc = LoadU32(cs_ptr + 0x08);
            StoreU32(cs_ptr + 0x08, lc + 1);
            return;
        }
        cs->owner = 0;
        cs->recursion = 0;
        cs->cv.notify_one();
    }
    StoreU32(cs_ptr + 0x08, 0xFFFFFFFF);
    StoreU32(cs_ptr + 0x0C, 0);
    StoreU32(cs_ptr + 0x10, 0);
    StoreU8(cs_ptr + 0x01, 1);  // signaled (Xenia semantics on waiter wake)
}

// =============================================================== spinlocks

static std::mutex g_spinlock_mutex;

IMPORT(KfAcquireSpinLock) {
    uint32_t lock_ptr = ARG(0);
    g_spinlock_mutex.lock();
    // Old IRQL in r3+cr? Xbox returns old IRQL. Keep 0.
    RET(0);  // DISPATCH_LEVEL on Xbox; games rarely inspect.
}

IMPORT(KfReleaseSpinLock) {
    uint32_t old_irql = ARG(0);
    g_spinlock_mutex.unlock();
    RET(old_irql);
}

IMPORT(KeAcquireSpinLockAtRaisedIrql) {
    uint32_t lock_ptr = ARG(0);
    g_spinlock_mutex.lock();
}

IMPORT(KeReleaseSpinLockFromRaisedIrql) {
    g_spinlock_mutex.unlock();
}

IMPORT(KeRaiseIrqlToDpcLevel) {
    RET(2);  // DISPATCH_LEVEL
}

IMPORT(KfLowerIrql) {
    // (old_irql) — no-op.
    RET(0);
}

IMPORT(KeEnterCriticalRegion) { }
IMPORT(KeLeaveCriticalRegion) { }
IMPORT(KiApcNormalRoutineNop) { }
IMPORT(KeLockL2) { RET(0); }
IMPORT(KeUnlockL2) { RET(0); }

// =============================================================== timers

IMPORT(NtCreateTimer) {
    uint32_t handle_ptr = ARG(0);
    uint32_t attrs = ARG(1);
    uint32_t timer_type = ARG(2);
    auto* tim = new GuestTimer();
    tim->type = kObjTypeTimer;
    tim->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x18, 8);
    tim->size = 0x18;
    GuestMemset(tim->guest_addr, 0, 0x18);
    StoreU8(tim->guest_addr + 0x00, kObjTypeTimer);
    uint32_t handle = K().objects.NewHandle(tim);
    if (handle_ptr) StoreU32(handle_ptr, handle);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtSetTimerEx) {
    // (timer_handle, due_time_ptr, routine?, context?) — simplified.
    uint32_t handle = ARG(0);
    uint32_t due_time_ptr = ARG(1);
    uint32_t routine = ARG(2);
    uint32_t context = ARG(3);
    auto* obj = K().objects.Lookup(handle);
    if (auto* tim = dynamic_cast<GuestTimer*>(obj)) {
        std::lock_guard<std::mutex> lk(tim->mtx);
        tim->due_time_100ns = due_time_ptr ? LoadU64(due_time_ptr) : 0;
        tim->absolute = (int64_t)tim->due_time_100ns > 0;
        // Phase 2B: no async callback dispatch; timer signals immediately
        // after the relative interval via waiter-side checks.
        tim->signaled = false;
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

IMPORT(NtCancelTimer) {
    uint32_t handle = ARG(0);
    uint32_t current_state_ptr = ARG(1);
    auto* obj = K().objects.Lookup(handle);
    if (auto* tim = dynamic_cast<GuestTimer*>(obj)) {
        std::lock_guard<std::mutex> lk(tim->mtx);
        if (current_state_ptr) StoreU32(current_state_ptr, tim->signaled ? 1 : 0);
        tim->due_time_100ns = 0;
        RET(X_STATUS_SUCCESS);
    }
    RET(X_STATUS_INVALID_HANDLE);
}

// =============================================================== objects

void FindThreadByKThread(uint32_t kthread, GuestThread** out) {
    *out = nullptr;
    std::lock_guard<std::mutex> lock(K().objects.mutex);
    for (auto& [h, obj] : K().objects.map_impl()) {
        (void)h;
        if (auto* t = dynamic_cast<GuestThread*>(obj)) {
            if (t->kthread == kthread) { *out = t; return; }
        }
    }
}

// In-place event/semaphore registries (guest objects initialized by
// KeInitializeEvent / KeInitializeSemaphore in guest memory).
static std::mutex g_inplace_mutex;
static std::unordered_map<uint32_t, GuestEvent*> g_inplace_events;
static std::unordered_map<uint32_t, GuestSemaphore*> g_inplace_sems;

GuestEvent* RegisterInPlaceEvent(uint32_t addr, bool manual, bool init) {
    std::lock_guard<std::mutex> lk(g_inplace_mutex);
    auto it = g_inplace_events.find(addr);
    if (it != g_inplace_events.end()) {
        it->second->manual_reset = manual;
        it->second->signaled = init;
        return it->second;
    }
    auto* ev = new GuestEvent();
    ev->manual_reset = manual;
    ev->signaled = init;
    ev->guest_addr = addr;
    g_inplace_events[addr] = ev;
    return ev;
}

GuestEvent* LookupInPlaceEvent(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_inplace_mutex);
    auto it = g_inplace_events.find(addr);
    return it == g_inplace_events.end() ? nullptr : it->second;
}

GuestSemaphore* RegisterInPlaceSemaphore(uint32_t addr, int32_t init,
                                         int32_t limit) {
    std::lock_guard<std::mutex> lk(g_inplace_mutex);
    auto it = g_inplace_sems.find(addr);
    if (it != g_inplace_sems.end()) {
        it->second->count = init;
        it->second->limit = limit;
        return it->second;
    }
    auto* sem = new GuestSemaphore();
    sem->count = init;
    sem->limit = limit;
    sem->guest_addr = addr;
    g_inplace_sems[addr] = sem;
    return sem;
}

GuestSemaphore* LookupInPlaceSemaphore(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_inplace_mutex);
    auto it = g_inplace_sems.find(addr);
    return it == g_inplace_sems.end() ? nullptr : it->second;
}

KernelObject* ResolveGuestObjectPtr(uint32_t guest_ptr) {
    // Handle-table objects by guest address.
    {
        std::lock_guard<std::mutex> lock(K().objects.mutex);
        for (auto& [h, obj] : K().objects.map_impl()) {
            (void)h;
            if (obj->guest_addr == guest_ptr) return obj;
        }
    }
    if (GuestEvent* ev = LookupInPlaceEvent(guest_ptr)) return ev;
    if (GuestSemaphore* sem = LookupInPlaceSemaphore(guest_ptr)) return sem;
    return nullptr;
}

IMPORT(ObReferenceObjectByHandle) {
    // (handle, object_type, object_ptr_out)
    uint32_t handle = ARG(0);
    uint32_t obj_type = ARG(1);
    uint32_t obj_out = ARG(2);
    KernelObject* obj = K().objects.Lookup(handle);
    if (!obj) {
        if (obj_out) StoreU32(obj_out, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    K().objects.Retain(obj);
    if (obj_out) StoreU32(obj_out, obj->guest_addr);
    PRLOG(Object, "ObReferenceObjectByHandle(%08X type=%08X) = obj %08X",
          handle, obj_type, obj->guest_addr);
    RET(X_STATUS_SUCCESS);
}

IMPORT(ObReferenceObject) {
    uint32_t obj_ptr = ARG(0);
    extern KernelObject* ResolveGuestObjectPtr(uint32_t guest_ptr);
    KernelObject* obj = ResolveGuestObjectPtr(obj_ptr);
    if (obj) K().objects.Retain(obj);
    RET(X_STATUS_SUCCESS);
}

IMPORT(ObDereferenceObject) {
    uint32_t obj_ptr = ARG(0);
    extern KernelObject* ResolveGuestObjectPtr(uint32_t guest_ptr);
    KernelObject* obj = ResolveGuestObjectPtr(obj_ptr);
    if (obj) K().objects.Deref(obj);
    RET(X_STATUS_SUCCESS);
}

IMPORT(ObIsTitleObject) {
    uint32_t obj_ptr = ARG(0);
    RET(1);
}

IMPORT(NtClose) {
    uint32_t handle = ARG(0);
    // Never close the current-thread pseudo-handle object.
    if (handle == ObjectTable::kCurrentThread) RET(X_STATUS_SUCCESS);
    K().objects.Release(handle);
    PRLOG(Object, "NtClose(%08X)", handle);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtDuplicateObject) {
    uint32_t src_handle = ARG(0);
    uint32_t dst_ptr = ARG(1);
    uint32_t options = ARG(2);
    KernelObject* obj = K().objects.Lookup(src_handle);
    if (!obj) {
        if (dst_ptr) StoreU32(dst_ptr, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    if (options & 1 /*DUPLICATE_CLOSE_SOURCE*/) {
        K().objects.Release(src_handle);
    }
    uint32_t newh = K().objects.NewHandle(obj);
    K().objects.Retain(obj);
    if (dst_ptr) StoreU32(dst_ptr, newh);
    RET(X_STATUS_SUCCESS);
}

IMPORT(ObCreateSymbolicLink) {
    // (ANSI_STRING* link, ANSI_STRING* target)
    uint32_t link_str = ARG(0);
    uint32_t target_str = ARG(1);
    // ANSI_STRING: { u16 len, u16 maxlen, u32 buffer }
    uint16_t llen = LoadU16(link_str);
    uint32_t lbuf = LoadU32(link_str + 4);
    uint16_t tlen = LoadU16(target_str);
    uint32_t tbuf = LoadU32(target_str + 4);
    std::string link(GuestAnsiString(lbuf, llen));
    std::string target(GuestAnsiString(tbuf, tlen));
    {
        std::lock_guard<std::mutex> lk(K().symlink_mutex);
        K().symbolic_links[link] = target;
    }
    PRLOG(Filesystem, "ObCreateSymbolicLink('%s' -> '%s')", link.c_str(),
          target.c_str());
    RET(X_STATUS_SUCCESS);
}

IMPORT(ObDeleteSymbolicLink) {
    uint32_t link_str = ARG(0);
    uint16_t llen = LoadU16(link_str);
    uint32_t lbuf = LoadU32(link_str + 4);
    std::string link(GuestAnsiString(lbuf, llen));
    {
        std::lock_guard<std::mutex> lk(K().symlink_mutex);
        K().symbolic_links.erase(link);
    }
    RET(X_STATUS_SUCCESS);
}

// =============================================================== misc

IMPORT(ExRegisterTitleTerminateNotification) {
    uint32_t fn = ARG(0);
    K().terminate_notification_fn = fn;
    PRLOG(Thread, "ExRegisterTitleTerminateNotification(%08X)", fn);
    RET(0);
}

IMPORT(KeQuerySystemTime) {
    uint32_t time_ptr = ARG(0);
    StoreU64(time_ptr, (uint64_t)time(nullptr) * 10000000ull +
              116444736000000000ull);
}

IMPORT(KeQueryPerformanceFrequency) {
    uint32_t freq_ptr = ARG(0);
    if (freq_ptr) StoreU64(freq_ptr, 1000000000ull / 1000ull);  // 1MHz QPC on Xbox
    RET(0);  // counter value 0
}

IMPORT(KeBugCheck) {
    PRLOGE("KeBugCheck called from guest");
    TerminateTitle(0xE691BEEF, "KeBugCheck");
}

IMPORT(KeBugCheckEx) {
    uint32_t code = ARG(0);
    uint32_t p1 = ARG(1), p2 = ARG(2), p3 = ARG(3), p4 = ARG(4);
    PRLOGE("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", code, p1, p2, p3, p4);
    TerminateTitle(code, "KeBugCheckEx");
}

IMPORT(HalReturnToFirmware) {
    uint32_t what = ARG(0);
    PRLOGE("HalReturnToFirmware(%u) — title requests reboot", what);
    _exit(0);
}

IMPORT(ExGetXConfigSetting) {
    // (category:u16, setting:u16, buffer, buffer_size:u16, required_size:u16*)
    // Semantics per Xenia xboxkrnl_xconfig.cc / free60 XConfig docs.
    uint32_t category = ARG(0) & 0xFFFF;
    uint32_t setting = ARG(1) & 0xFFFF;
    uint32_t buffer = ARG(2);
    uint32_t buffer_size = ARG(3) & 0xFFFF;
    uint32_t required_ptr = ARG(4);
    uint32_t setting_size = 0;
    uint32_t value = 0;
    uint32_t status = X_STATUS_SUCCESS;

    switch (category) {
    case 0x0002:  // XCONFIG_SECURED_CATEGORY
        if (setting == 0x0002) {  // XCONFIG_SECURED_AV_REGION
            setting_size = 4;
            value = 0x00001000;  // NTSC/U (USA/Canada)
        } else {
            status = 0xC00000EFu;  // INVALID_PARAMETER_2
        }
        break;
    case 0x0003:  // XCONFIG_USER_CATEGORY
        setting_size = 4;
        switch (setting) {
        case 0x0001: case 0x0002: case 0x0003: case 0x0004:
        case 0x0005: case 0x0006: case 0x0007:
            value = 0;   // time zone data
            break;
        case 0x0009:     // XCONFIG_USER_LANGUAGE
            value = 1;   // English
            break;
        case 0x000A:     // XCONFIG_USER_VIDEO_FLAGS
            value = 0x00040000;
            break;
        case 0x000C:     // XCONFIG_USER_RETAIL_FLAGS
            value = 0;
            break;
        case 0x000E:     // XCONFIG_USER_COUNTRY (1 byte)
            setting_size = 1;
            value = 0xFD;
            break;
        default:
            status = 0xC00000EFu;
            setting_size = 0;
        }
        break;
    default:
        status = 0xC00000EEu;  // INVALID_PARAMETER_1
        break;
    }

    if (status == X_STATUS_SUCCESS) {
        if (buffer) {
            if (buffer_size < setting_size) {
                RET(0xC0000023u);  // BUFFER_TOO_SMALL
            }
            if (setting_size == 1) StoreU8(buffer, (uint8_t)value);
            else if (setting_size == 4) StoreU32(buffer, value);
        }
        if (required_ptr) StoreU16(required_ptr, (uint16_t)setting_size);
    }
    PRLOG(Import, "ExGetXConfigSetting(cat=%u set=%u) = %08X val=%08X",
          category, setting, status, value);
    RET(status);
}


