// PrototypeRecomp Phase 2B runtime — kernel state, object manager, threads,
// guest memory management.
#pragma once

#include "guest.h"
#include "logging.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pr {

class GuestThread;
struct KernelState;

// ---------------------------------------------------------------- memory
// Simple region allocators over the 4 GiB guest space. Each region is a
// bump allocator with a free list of blocks. Xbox semantics (reserve/commit,
// top-down pools, physical allocations) are layered on top.
struct GuestRegion {
    uint32_t base = 0, end = 0;
    uint32_t next = 0;                    // bump cursor
    std::map<uint32_t, uint32_t> live;    // addr -> size (sorted)
    std::multimap<uint32_t, uint32_t> frees;  // size -> addr

    GuestRegion(uint32_t b, uint32_t e) : base(b), end(e), next(b) {}
    uint32_t Alloc(uint32_t size, uint32_t alignment);
    void Free(uint32_t addr);
    bool Contains(uint32_t a) const { return a >= base && a < end; }
};

class GuestMemory {
public:
    static bool Init();          // mmap 4 GiB, zero image region
    static GuestMemory& Get();

    uint32_t SystemHeapAlloc(uint32_t size, uint32_t align = 8);  // kernel structs
    void SystemHeapFree(uint32_t addr);

    uint32_t PoolAlloc(uint32_t size, uint32_t tag);              // ExAllocatePool
    void PoolFree(uint32_t addr);

    uint32_t PhysicalAlloc(uint32_t size);                        // Mm physical
    void PhysicalFree(uint32_t addr);
    uint32_t PhysicalAvailable();

    uint32_t StackAlloc(uint32_t size);                           // thread stacks
    void StackFree(uint32_t addr);

    // NtAllocateVirtualMemory: NULL base -> bottom-up title heap;
    // nonzero base -> try exact placement (with 64KB granularity).
    uint32_t VirtualAlloc(uint32_t* base_io, uint32_t size, uint32_t alloc_type,
                          uint32_t protect);
    uint32_t VirtualFree(uint32_t base, uint32_t* size_io, uint32_t free_type);

    uint32_t QueryAddressProtect(uint32_t addr) const;
    uint64_t QueryPhysicalAddress(uint32_t guest) const;

    void RegisterRange(const char* name, uint32_t base, uint32_t end);
    void DumpMap();

    // All allocations tracked for overlap assertions: addr -> (size, name)
    std::map<uint32_t, std::pair<uint32_t, std::string>> allocations;
    bool CheckNoOverlap(uint32_t addr, uint32_t size, const char* what);

private:
    GuestMemory();
    GuestRegion sys_heap_{kKernelStructBase, kKernelStructEnd};
    GuestRegion pool_{kSystemPoolBase, kSystemPoolEnd};
    GuestRegion physical_{kPhysicalArenaBase, kPhysicalArenaEnd};
    GuestRegion stacks_{kStackRegionBase, kStackRegionEnd};
    GuestRegion title_heap_{kTitleHeapBase, kTitleHeapEnd};
    std::mutex mutex_;
};

// ---------------------------------------------------------------- objects
enum class ObjType : uint8_t {
    Thread = 6,     // KTHREAD type (Xenia)
    Event = 0,
    Semaphore = 0,
    Mutant = 0,
    Timer = 0,
    File = 0,
    Device = 0,
    SymbolicLink = 0,
    Section = 0,
    IoCompletion = 0,
};

// DISPATCH_HEADER — every kernel object starts with this (8 bytes).
constexpr uint8_t kObjTypeEvent      = 0;
constexpr uint8_t kObjTypeSemaphore  = 3;   // per real Xbox: NotificationTimer=1? Xenia: SemaphoreObject?
constexpr uint8_t kObjTypeMutant     = 2;
constexpr uint8_t kObjTypeTimer      = 1;
constexpr uint8_t kObjTypeThread     = 6;
constexpr uint8_t kObjTypeFile       = 7;   // runtime-local choice (opaque)

struct KernelObject {
    uint32_t guest_addr = 0;    // guest address of object body
    uint32_t size = 0;
    uint8_t  type = 0;          // dispatch header type value
    int32_t  refcount = 1;
    uint32_t handle = 0;
    virtual ~KernelObject() = default;
};

class ObjectTable {
public:
    uint32_t NewHandle(KernelObject* obj);          // returns guest handle
    KernelObject* Lookup(uint32_t handle);
    void Release(uint32_t handle);                  // remove + deref
    void Retain(KernelObject* obj);
    void Deref(KernelObject* obj);

    // Pseudo-handles.
    static constexpr uint32_t kCurrentThread = 0xFFFF0001;

    std::mutex mutex;
    // Read-only view for iteration (caller must hold mutex).
    const std::unordered_map<uint32_t, KernelObject*>& map_impl() const {
        return map_;
    }
private:
    uint32_t next_handle_ = 0xF8000008u;
    std::unordered_map<uint32_t, KernelObject*> map_;
};

// ---------------------------------------------------------------- sync
// Event/Semaphore/Mutant guest-visible semantics with host primitives.
struct GuestEvent : KernelObject {
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled = false;
    bool manual_reset = true;
    int waiting = 0;
};

struct GuestSemaphore : KernelObject {
    std::mutex mtx;
    std::condition_variable cv;
    int32_t count = 0;
    int32_t limit = 0x7FFFFFFF;
};

struct GuestMutant : KernelObject {
    std::mutex mtx;
    std::condition_variable cv;
    uint32_t owner = 0;      // guest KTHREAD addr
    int32_t recursion = 0;
    bool abandoned = false;
};

struct GuestTimer : KernelObject {
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled = false;
    uint64_t due_time_100ns = 0;
    bool absolute = false;
    uint32_t period_ms = 0;
};

// ---------------------------------------------------------------- threads
enum {
    kCreateSuspended = 0x1,
    kCreateFlag0x80  = 0x80,   // ExCreateThread: return object ptr, not handle
};

struct ThreadLaunch {
    uint32_t entry = 0;
    uint32_t arg = 0;
    uint32_t xapi_startup = 0;
    uint32_t creation_flags = 0;
    uint32_t stack_size = 0;
};

class GuestThread : public KernelObject {
public:
    // -------------------------------------------------- guest-visible state
    uint32_t kthread = 0;      // guest KTHREAD address (0xAB0 bytes)
    uint32_t pcr = 0;          // guest PCR address (0x2D8 bytes)
    uint32_t tls = 0;          // guest TLS block address
    uint32_t stack_base = 0;   // high address
    uint32_t stack_limit = 0;  // low address
    uint32_t scratch = 0;
    uint32_t thread_id = 0;

    // -------------------------------------------------- host side
    PPCContext* ctx = nullptr;         // aligned host context (per thread)
    std::thread host;
    std::atomic<bool> suspended{false};
    std::atomic<bool> finished{false};
    std::atomic<int>  suspend_count{0};
    std::atomic<uint32_t> exit_code{0};
    ThreadLaunch launch;
    std::string name;

    // Suspension machinery.
    std::mutex suspend_mtx;
    std::condition_variable suspend_cv;

    // Held while the thread runs guest code.
    std::mutex state_mtx;

    ~GuestThread() override;

    bool Create(uint32_t stack_size);  // allocate stack/TLS/PCR/KTHREAD
    void Run();                        // host thread body
    void Exit(uint32_t code);
    int Resume(int* prev_count = nullptr);
    int Suspend(int* prev_count = nullptr);
    void Join();

    // Write KTHREAD guest fields per Xenia layout.
    void InitializeGuestObject();
    void SetLastError(uint32_t err) { StoreU32(kthread + 0x160, err); }
    uint32_t GetLastError() { return LoadU32(kthread + 0x160); }

    static GuestThread* GetCurrent();     // host-TLS lookup
    static void SetCurrentForHostThread(GuestThread* t);  // bind CP/DPC host threads
};

// ---------------------------------------------------------------- loader
struct XexVariableImport {
    uint32_t thunk_va;
    uint32_t ordinal;
    std::string name;
};

struct XexFunctionImport {
    uint32_t thunk_va;
    uint32_t ordinal;
    std::string name;
    std::string library;
};

// ---------------------------------------------------------------- kernel
struct KernelState {
    ObjectTable objects;

    // Variable-import guest allocations (filled during load).
    uint32_t var_ketsb = 0;            // KeTimeStampBundle (6 * u64)
    uint32_t var_module_handle_pp = 0; // XexExecutableModuleHandle slot target
    uint32_t var_module_struct = 0;    // module struct; +0x58 -> xex header
    uint32_t var_xex_header = 0;       // XEX2 header copy in guest memory
    uint32_t var_hardware_info = 0;    // XBOX_HARDWARE_INFO
    uint32_t var_debug_monitor = 0;    // KeDebugMonitorData
    uint32_t var_thread_object_type = 0; // ExThreadObjectType
    uint32_t var_command_line = 0;     // ExLoadedCommandLine
    uint32_t var_cert_monitor = 0;     // KeCertMonitorData
    uint32_t var_vd_global_device = 0; // VdGlobalDevice
    uint32_t var_vd_global_xam_device = 0;
    uint32_t var_krnl_version = 0;     // XboxKrnlVersion
    uint32_t var_gpu_clock_mhz = 0;    // VdGpuClockInMHz
    uint32_t var_hsio_lock = 0;        // VdHSIOCalibrationLock
    uint32_t var_process_info_block = 0; // KPROCESS/PEB-ish (+0x84 of KTHREAD)

    // Executable module info.
    uint32_t entry_point = 0;
    uint32_t default_stack_size = 0;
    uint32_t tls_slot_count = 0;
    uint32_t tls_data_size = 0;
    uint32_t tls_raw_data_address = 0; // image VA of initializers
    uint32_t tls_raw_data_size = 0;

    // Main thread.
    GuestThread* main_thread = nullptr;

    // All live guest threads (for the watchdog status dump).
    std::mutex threads_mutex;
    std::vector<GuestThread*> threads;

    // Title terminate notification (ExRegisterTitleTerminateNotification).
    uint32_t terminate_notification_fn = 0;

    // Termination.
    std::atomic<bool> terminating{false};

    // TLS slots allocated by KeTlsAlloc (guest-global).
    std::mutex tls_slot_mutex;
    uint32_t tls_slots_used = 0;       // bitmask in 64-slot space (bit per slot)

    // Symbolic links (ObCreateSymbolicLink) e.g. "D:" -> \Device\Cdrom0.
    std::map<std::string, std::string> symbolic_links;
    std::mutex symlink_mutex;

    // Devices (IoCreateDevice) mapped by name.
    struct DeviceObject : KernelObject {
        std::string name;
        uint32_t driver_object = 0; // guest DRIVER_OBJECT-ish
    };
    std::map<std::string, DeviceObject*> devices;
    std::mutex device_mutex;

    // XAudio render driver client state (Phase 2B: registered + logged).
    std::mutex audio_mutex;
    uint32_t audio_client_callback = 0;
    uint32_t audio_client_priority = 0;
    bool audio_client_registered = false;

    // Graphics state (Vd*).
    uint32_t vd_ring_buffer_ptr = 0;      // guest ptr (physical)
    uint32_t vd_ring_buffer_size_log2 = 0;
    uint32_t vd_rptr_writeback_ptr = 0;
    uint32_t vd_rptr_writeback_block_log2 = 0;
    uint32_t vd_interrupt_callback = 0;
    uint32_t vd_interrupt_callback_arg = 0;
    uint64_t vd_swap_count = 0;
    uint32_t vd_frontbuffer_ptr = 0;

    // XNotify listeners.
    std::mutex notify_mutex;
    uint32_t notify_listener_next_handle = 0xF9000000u;

    // Session handles (XamSessionCreateHandle -> dummy objects).
    ObjectTable session_objects;

    // Execution counters.
    std::atomic<uint64_t> import_calls{0};

    // Thread id allocator.
    std::atomic<uint32_t> next_thread_id{7};

    // Filesystem root: D:\ maps here (extracted game root). Empty = no disc.
    std::string fs_root;
};

extern KernelState* g_kernel;
KernelState& K();

// ---------------------------------------------------------------- control
// Thread execution entry: calls guest function via lookup table. Never
// returns for main thread until title exit.
int RunGuestFunction(GuestThread* t, uint32_t address,
                     const uint64_t* args, size_t arg_count);

// Guest exception used to unwind host frames back to the thread boundary
// (RtlUnwind / thread exit / longjmp-style transfers).
struct GuestUnwind {
    uint32_t target_pc = 0;
    uint64_t r[12] = {};
    explicit GuestUnwind(uint32_t pc) : target_pc(pc) {}
};

// Terminate the whole process from guest (KeBugCheckEx etc).
[[noreturn]] void TerminateTitle(uint32_t code, const char* reason);

// Boot: load XEX, set up everything, run main thread. Returns exit code.
int BootTitle(const char* xex_path, int argc, char** argv);

// Minimal Xenos command-processor front-end (gpu_command_processor.cpp):
// consumes real PM4 packets from the game's ring, advances the read-pointer
// writeback, and wakes the game's DPC interrupt threads.
void StartXenosCommandProcessor();
void StopXenosCommandProcessor();

// Reflect kernel ring setup into the Xenos register file (Vd imports).
void XenosSetRingRegs(uint32_t rb_base_pa, uint32_t rptr_wb_pa);

// GPU statistics for the watchdog.
uint64_t XenosGpuStats(uint32_t* packets, uint32_t* ibs, uint32_t* waits,
                       uint32_t* ints);

}  // namespace pr
