// PrototypeRecomp Phase 2C — allocator / intern-table diagnostics.
//
// CRASH UNDER INVESTIGATION (run_gpu22): sub_82A02ED0 (Pure3D/ATG tracked
// grow/free front-end) executed `bctrl` with CTR=0 because [backend+12]
// (the allocator function-pointer slot of the evaluator's intern table,
// observed at 0xA080DB1C+12) read 0. The 0xffffffff7ea00000 host value is a
// stale host-stack artifact from the interceptor's printf path — the true
// guest call target is CTR=0.
//
// This file instruments the whole allocator stack and adds an mprotect
// write-watchpoint over the intern table's page so the FIRST write that
// corrupts the table is caught with full context:
//
//   sub_82A02ED0  tracked grow/free front-end (ring + canary)
//   sub_82243AD0  grow/free implementation     (ring + heap checks)
//   sub_82A01D90  intern-table insert          (name + state log)
//   sub_82A02ECC  gap-stub trap                (invalid target log)
//
// Enable the watchpoint with PR_EH_WATCH=1 (default on).
#include "state.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>

#include <execinfo.h>

extern "C" {
PPC_FUNC(__imp__sub_82A02ED0);   // tracked grow/free front-end (luaM_realloc_)
PPC_FUNC(__imp__sub_82243AD0);   // grow/free implementation
PPC_FUNC(__imp__sub_82A01D90);   // intern-table insert
PPC_FUNC(__imp__sub_82A01E80);   // luaS_newlstring (find-or-insert)
PPC_FUNC(__imp__sub_829FEB28);   // float-record resolver
PPC_FUNC(__imp__sub_82A07E58);   // Lua chunk loader wrapper (L, src, ?, name)
PPC_FUNC(__imp__sub_82A02ECC);   // 4-byte gap stub (invalid call target)
}

namespace pr {
namespace ehdiag {

// ---------------------------------------------------------------- config
static bool g_watch_enabled = true;      // PR_EH_WATCH=0 disables
static bool g_watch_installed = false;

// Game heap bounds (from MmAllocatePhysicalMemoryEx log of the failing run;
// refreshed dynamically from 82243AD0 results).
static uint32_t g_heap_lo = 0xA0010000;
static uint32_t g_heap_hi = 0xBF8F0000;

// ---------------------------------------------------------------- rings
struct GrowRec {                     // sub_82A02ED0
    uint32_t seq, tid, lr;
    uint32_t registry, table, fn12, ctx16, ctr68;
    uint32_t r4, r5, r6;
};
static GrowRec g_grow[96];
static std::atomic<uint32_t> g_grow_n{0};

struct AllocRec {                    // sub_82243AD0
    uint32_t seq, tid, lr;
    uint32_t old_ptr, old_size, delta, ret;
};
static AllocRec g_alloc[192];
static std::atomic<uint32_t> g_alloc_n{0};

struct InsertRec {                   // sub_82A01D90
    uint32_t seq, tid, lr;
    uint32_t registry, table, name_ptr, len, hash;
    uint32_t buckets, count, nbuckets, fn12, ctr68;
};
static InsertRec g_ins[64];
static std::atomic<uint32_t> g_ins_n{0};

// ---------------------------------------------------------------- watchpoint
// The watched table (set on first sub_82A02ED0 call).
static uint32_t g_table = 0;               // guest address of intern table
static uint64_t g_watch_page_host = 0;     // host address of watched page
static uint32_t g_watch_page_guest = 0;
static std::atomic<bool> g_watch_armed{false};
static std::atomic<bool> g_stepping{false};
static __thread uint32_t t_step_target = 0;    // guest addr being written

struct WatchRec {
    uint64_t rip;
    uint32_t guest_addr, tid;
    uint32_t newval;
};
static WatchRec g_watch_ring[256];
static std::atomic<uint32_t> g_watch_n{0};
static std::atomic<uint64_t> g_watch_faults{0};

struct sigaction g_prev_segv;
struct sigaction g_prev_trap;

static uint32_t Tid() {
    GuestThread* t = GuestThread::GetCurrent();
    return t ? t->thread_id : 0;
}

inline uint32_t Rd32(uint32_t a) { return LoadU32(a); }

static bool ValidCode(uint32_t a) {
    return a >= 0x82230000 && a < 0x82BA880C && ((a & 3) == 0);
}

// ------------------------------------------------------------ watchpoint core
static void WatchSigsegv(int sig, siginfo_t* info, void* ctx_) {
    uintptr_t fa = (uintptr_t)(info ? info->si_addr : 0);
    if (g_watch_armed.load(std::memory_order_relaxed) &&
        fa >= g_watch_page_host && fa < g_watch_page_host + 0x1000) {
        ucontext_t* uc = (ucontext_t*)ctx_;
        g_watch_faults.fetch_add(1, std::memory_order_relaxed);
        if (!g_stepping.exchange(true)) {
            // Unprotect, set trap flag; the instruction executes, SIGTRAP
            // re-protects. Remember the target for value capture.
            t_step_target = (uint32_t)(fa - g_watch_page_host + g_watch_page_guest);
            mprotect((void*)g_watch_page_host, 0x1000, PROT_READ | PROT_WRITE);
            uc->uc_mcontext.gregs[REG_EFL] |= 0x100;   // TF
            return;
        }
        // Already stepping (rare cross-thread race): let it pass silently.
        mprotect((void*)g_watch_page_host, 0x1000, PROT_READ | PROT_WRITE);
        return;
    }
    // Not ours: chain to the generic crash handler.
    if (g_prev_segv.sa_sigaction)
        g_prev_segv.sa_sigaction(sig, info, ctx_);
    else
        _exit(139);
}

static void WatchSigtrap(int sig, siginfo_t* info, void* ctx_) {
    ucontext_t* uc = (ucontext_t*)ctx_;
    bool tf = uc->uc_mcontext.gregs[REG_EFL] & 0x100;
    if (g_stepping.load(std::memory_order_relaxed) && tf) {
        uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
        mprotect((void*)g_watch_page_host, 0x1000, PROT_READ);
        g_stepping.store(false, std::memory_order_relaxed);
        // Capture the freshly written value at the remembered target.
        uint32_t tgt = t_step_target;
        uint32_t val = 0;
        if (tgt) {
            uint32_t off = tgt - g_watch_page_guest;
            memcpy(&val, (void*)(g_watch_page_host + off), 4);
            val = __builtin_bswap32(val);   // guest is big-endian
        }
        uint32_t i = g_watch_n.fetch_add(1, std::memory_order_relaxed);
        if (i < 256) {
            g_watch_ring[i].rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
            g_watch_ring[i].guest_addr = tgt;
            g_watch_ring[i].tid = Tid();
            g_watch_ring[i].newval = val;
        }
        t_step_target = 0;
        return;
    }
    if (g_prev_trap.sa_sigaction)
        g_prev_trap.sa_sigaction(sig, info, ctx_);
}

static void ArmWatchpoint(uint32_t table_guest) {
    if (!g_watch_enabled || g_watch_installed) return;
    g_table = table_guest;
    g_watch_page_guest = table_guest & ~0xFFFu;
    g_watch_page_host = (uint64_t)(uintptr_t)HostFromGuest(g_watch_page_guest);
    // Save previous handlers, install ours.
    struct sigaction sa{};
    sa.sa_sigaction = WatchSigsegv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    struct sigaction st{};
    st.sa_sigaction = WatchSigtrap;
    st.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&st.sa_mask);
    sigaction(SIGTRAP, &st, &g_prev_trap);
    mprotect((void*)g_watch_page_host, 0x1000, PROT_READ);
    g_watch_armed.store(true, std::memory_order_relaxed);
    g_watch_installed = true;
    PRLOG(Exception,
          "EH-WATCH armed: table=%08X page=%08X (guest) host=%p — writes to "
          "the table page now trapped",
          table_guest, g_watch_page_guest, (void*)g_watch_page_host);
}

static void DisarmWatchpoint() {
    if (g_watch_installed) {
        g_watch_armed.store(false, std::memory_order_relaxed);
        mprotect((void*)g_watch_page_host, 0x1000, PROT_READ | PROT_WRITE);
    }
}

// ---------------------------------------------------------------- dumps
static void SymOf(uint64_t rip, char* out, size_t n) {
    Dl_info dli{};
    if (dladdr((void*)rip, &dli) && dli.dli_sname) {
        snprintf(out, n, "%s+%lX", dli.dli_sname,
                 (unsigned long)((char*)rip - (char*)dli.dli_saddr));
    } else {
        snprintf(out, n, "%p", (void*)rip);
    }
}

static void DumpAll(const char* why) {
    DisarmWatchpoint();
    char sbuf[128];
    fprintf(stdout, "\n=== EHDIAG DUMP: %s ===\n", why);
    {   // Guest stack walk: r1 -> [r1]=caller r1, [r1+8]=saved LR.
        // This is the definitive guest call chain (no host-frame ambiguity).
        GuestThread* gt = GuestThread::GetCurrent();
        if (gt && gt->ctx) {
            uint32_t r1 = gt->ctx->r1.u32;
            fprintf(stdout, "-- guest stack walk (r1 chain) --\n");
            for (int i = 0; i < 24 && r1 >= 0x70000000 && r1 < 0x7F000000; i++) {
                uint32_t next = LoadU32(r1);
                uint32_t lr = LoadU32(r1 + 8);
                fprintf(stdout, "  frame%d r1=%08X ret=%08X %s\n", i, r1, lr,
                        GuestFuncName(lr));
                if (next <= r1 || next >= r1 + 0x10000) break;
                r1 = next;
            }
        }
    }
    {   // Host backtrace: which generated PPC function is on the stack?
        void* bt[24];
        int n = backtrace(bt, 24);
        for (int i = 0; i < n && i < 20; i++) {
            SymOf((uint64_t)bt[i], sbuf, sizeof(sbuf));
            fprintf(stdout, "  bt[%d] %s\n", i, sbuf);
        }
    }
    // Intern table state.
    if (g_table) {
        fprintf(stdout, "table @%08X:", g_table);
        for (int o = 0; o <= 68; o += 4)
            fprintf(stdout, " +%d=%08X", o, Rd32(g_table + o));
        fprintf(stdout, "\n");
    }
    // Grow ring (newest last).
    uint32_t gn = g_grow_n.load();
    fprintf(stdout, "-- grow/free front-end calls: %u total, last %u --\n",
            gn, gn < 96 ? gn : 96u);
    for (uint32_t k = gn > 96 ? gn - 96 : 0; k < gn; k++) {
        GrowRec& r = g_grow[k % 96];
        fprintf(stdout,
                "  #%u tid=%u lr=%08X reg=%08X tbl=%08X fn12=%08X ctx16=%08X "
                "ctr68=%08X args(old=%08X size=%08X delta=%08X)\n",
                r.seq, r.tid, r.lr, r.registry, r.table, r.fn12, r.ctx16,
                r.ctr68, r.r4, r.r5, r.r6);
    }
    // Alloc ring.
    uint32_t an = g_alloc_n.load();
    fprintf(stdout, "-- allocator impl calls: %u total, last %u --\n", an,
            an < 192 ? an : 192u);
    for (uint32_t k = an > 192 ? an - 192 : 0; k < an; k++) {
        AllocRec& r = g_alloc[k % 192];
        SymOf(0, sbuf, 0);  // no-op keeps compiler happy
        fprintf(stdout,
                "  #%u tid=%u lr=%08X grow(old=%08X size=%08X delta=%08X) "
                "-> %08X\n",
                r.seq, r.tid, r.lr, r.old_ptr, r.old_size, r.delta, r.ret);
    }
    // Insert ring.
    uint32_t in = g_ins_n.load();
    fprintf(stdout, "-- inserts: %u total, last %u --\n", in,
            in < 64 ? in : 64u);
    for (uint32_t k = in > 64 ? in - 64 : 0; k < in; k++) {
        InsertRec& r = g_ins[k % 64];
        char name[40] = "?";
        if (r.name_ptr && r.len < 64) {
            for (uint32_t c = 0; c < r.len && c < 34; c++) {
                uint8_t ch = LoadU8(r.name_ptr + c);
                name[c] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            name[r.len < 34 ? r.len : 34] = 0;
        }
        fprintf(stdout,
                "  #%u tid=%u lr=%08X reg=%08X tbl=%08X name(%u)=\"%s\" "
                "hash=%08X {buckets=%08X cnt=%u nbkt=%u fn12=%08X ctr68=%08X}\n",
                r.seq, r.tid, r.lr, r.registry, r.table, r.len, name,
                r.hash, r.buckets, r.count, r.nbuckets, r.fn12, r.ctr68);
    }
    // Watchpoint ring.
    uint32_t wn = g_watch_n.load();
    fprintf(stdout, "-- watchpoint: %llu faults, %u captured writes --\n",
            (unsigned long long)g_watch_faults.load(), wn < 256 ? wn : 256u);
    for (uint32_t k = wn > 256 ? wn - 256 : 0; k < wn && k < 4096; k++) {
        WatchRec& r = g_watch_ring[k % 256];
        SymOf(r.rip, sbuf, sizeof(sbuf));
        uint32_t off = r.guest_addr - g_watch_page_guest;
        const char* rel = "";
        if (g_table && r.guest_addr >= g_table && r.guest_addr < g_table + 76)
            rel = "  <<<< TABLE WRITE";
        fprintf(stdout,
                "  #%u tid=%u guest=%08X (page+%03X) newval=%08X by %s%s\n",
                k, r.tid, r.guest_addr, off, r.newval, sbuf, rel);
    }
    fflush(stdout);
}

// Canary check executed on every front-end call (outside signal context).
static bool g_dumped = false;
static void CheckAndMaybeDump(uint32_t registry, uint32_t table) {
    if (g_dumped) return;
    uint32_t fn12 = Rd32(table + 12);
    if (ValidCode(fn12)) return;
    g_dumped = true;
    DumpAll("INVALID ALLOCATOR SLOT [table+12] DETECTED");
    _exit(99);
}

}  // namespace ehdiag

void EhTraceInit() {
    if (getenv("PR_EH_WATCH") && !strcmp(getenv("PR_EH_WATCH"), "0"))
        ehdiag::g_watch_enabled = false;
    PRLOG(Exception, "ehdiag online (watchpoint %s)",
          ehdiag::g_watch_enabled ? "enabled" : "disabled");
}

}  // namespace pr

// ===========================================================================
// Guest-function overrides (weak alias replaced by strong definition).
// ===========================================================================
using namespace pr::ehdiag;
using pr::LoadU8;
using pr::LoadU32;

// Tracked grow/free front-end: r3=registry, r4=old, r5=old_size, r6=delta.
// backend = [registry+16]; call [backend+12]([backend+16], r4, r5, r6).
PPC_FUNC(sub_82A02ED0) {
    uint32_t registry = ctx.r3.u32;
    uint32_t old = ctx.r4.u32, oldsz = ctx.r5.u32, delta = ctx.r6.u32;
    uint32_t lr = (uint32_t)ctx.lr;
    uint32_t tid = Tid();
    uint32_t table = 0, fn12 = 0, ctx16 = 0, ctr68 = 0;
    bool ok = false;
    if (registry >= g_heap_lo && registry < g_heap_hi) {
        table = Rd32(registry + 16);
        if (table >= g_heap_lo && table < g_heap_hi) {
            fn12 = Rd32(table + 12);
            ctx16 = Rd32(table + 16);
            ctr68 = Rd32(table + 68);
            ok = true;
        }
    }
    if (!g_watch_installed && ok && ValidCode(fn12)) {
        // Arm the watchpoint on the first healthy call.
        ArmWatchpoint(table);
    }
    uint32_t n = g_grow_n.fetch_add(1) + 1;
    if (n <= 96 || (n % 64) == 0) {
        GrowRec& r = g_grow[n % 96];
        r.seq = n; r.tid = tid; r.lr = lr;
        r.registry = registry; r.table = table; r.fn12 = fn12;
        r.ctx16 = ctx16; r.ctr68 = ctr68;
        r.r4 = old; r.r5 = oldsz; r.r6 = delta;
        PRLOG(Exception,
              "grow#%u tid=%u lr=%08X reg=%08X tbl=%08X fn12=%08X "
              "old=%08X size=%08X delta=%08X",
              n, tid, lr, registry, table, fn12, old, oldsz, delta);
    }
    if (!ok && !g_dumped) {
        // THE CRASH CONDITION: registry or [registry+16] is invalid. The
        // real body would read [table+12] through a null/garbage pointer,
        // load CTR=0 and fault in the lookup table. Dump everything now.
        g_dumped = true;
        DumpAll("GROW ENTRY WITH INVALID REGISTRY/TABLE (CTR would load 0)");
        _exit(99);
    }
    if (ok) CheckAndMaybeDump(registry, table);
    __imp__sub_82A02ED0(ctx, base);
}

// Grow/free implementation: r3=ctx, r4=old, r5=old_size, r6=delta.
// delta==0 -> free; delta!=0 -> alloc (+copy+free if old!=0).
PPC_FUNC(sub_82243AD0) {
    uint32_t old = ctx.r4.u32, oldsz = ctx.r5.u32, delta = ctx.r6.u32;
    uint32_t lr = (uint32_t)ctx.lr;
    uint32_t tid = Tid();
    __imp__sub_82243AD0(ctx, base);
    uint32_t ret = ctx.r3.u32;
    if (ret && (ret < g_heap_lo || ret >= g_heap_hi)) {
        // Refresh heap bounds from a plausible large allocation result.
        if (ret >= 0xA0000000 && ret < 0xC0000000) {
            PRLOG(Exception,
                  "alloc outside tracked heap: ret=%08X (old=%08X size=%08X "
                  "delta=%08X) tid=%u lr=%08X — updating bounds",
                  ret, old, oldsz, delta, tid, lr);
            if (ret < g_heap_lo) g_heap_lo = ret & ~0xFFFFu;
        }
    }
    uint32_t n = g_alloc_n.fetch_add(1) + 1;
    if (n <= 192 || (n % 128) == 0) {
        AllocRec& r = g_alloc[n % 192];
        r.seq = n; r.tid = tid; r.lr = lr;
        r.old_ptr = old; r.old_size = oldsz; r.delta = delta; r.ret = ret;
        if (n <= 24) {
            PRLOG(Exception,
                  "alloc#%u tid=%u lr=%08X grow(old=%08X size=%08X "
                  "delta=%08X) -> %08X",
                  n, tid, lr, old, oldsz, delta, ret);
        }
    }
}

// Intern-table insert: r3=registry, r4=name, r5=len, r6=hash.
PPC_FUNC(sub_82A01D90) {
    uint32_t registry = ctx.r3.u32;
    uint32_t name = ctx.r4.u32, len = ctx.r5.u32, hash = ctx.r6.u32;
    uint32_t lr = (uint32_t)ctx.lr;
    uint32_t tid = Tid();
    uint32_t table = 0, buckets = 0, count = 0, nbkt = 0, fn12 = 0, ctr68 = 0;
    if (registry >= g_heap_lo && registry < g_heap_hi) {
        table = Rd32(registry + 16);
        if (table >= g_heap_lo && table < g_heap_hi) {
            buckets = Rd32(table + 0);
            count = Rd32(table + 4);
            nbkt = Rd32(table + 8);
            fn12 = Rd32(table + 12);
            ctr68 = Rd32(table + 68);
        }
    }
    uint32_t n = g_ins_n.fetch_add(1) + 1;
    if (n <= 200) {
        InsertRec& r = g_ins[n % 64];
        r.seq = n; r.tid = tid; r.lr = lr;
        r.registry = registry; r.table = table; r.name_ptr = name;
        r.len = len; r.hash = hash;
        r.buckets = buckets; r.count = count; r.nbuckets = nbkt;
        r.fn12 = fn12; r.ctr68 = ctr68;
        char nm[36] = "?";
        if (name && len < 64) {
            for (uint32_t c = 0; c < len && c < 34; c++) {
                uint8_t ch = LoadU8(name + c);
                nm[c] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            nm[len < 34 ? len : 34] = 0;
        }
        PRLOG(Exception,
              "insert#%u tid=%u lr=%08X reg=%08X name(%u)=\"%s\" hash=%08X "
              "{cnt=%u nbkt=%u fn12=%08X ctr68=%08X}",
              n, tid, lr, registry, len, nm, hash, count, nbkt, fn12, ctr68);
    }
    if (registry < g_heap_lo || registry >= g_heap_hi) {
        g_dumped = true;
        DumpAll("INSERT ENTRY WITH INVALID REGISTRY");
        _exit(99);
    }
    __imp__sub_82A01D90(ctx, base);
    // Post-insert canary.
    if (table && !g_dumped) {
        uint32_t f2 = Rd32(table + 12);
        if (!ValidCode(f2)) {
            g_dumped = true;
            DumpAll("TABLE CORRUPTED AFTER INSERT");
            _exit(99);
        }
    }
}

// Gap stub: an invalid 4-byte gap called as a function (observed as
// __CxxFrameHandler-4). Log and return cleanly.
PPC_FUNC(sub_82A02ECC) {
    PRLOG(Exception,
          "GAP-STUB CALLED: 0x82A02ECC (invalid target!) lr=%08X r3=%08X "
          "r4=%08X tid=%u",
          (uint32_t)ctx.lr, (uint32_t)ctx.r3.u32, (uint32_t)ctx.r4.u32, Tid());
}

// luaS_newlstring entry: r3=L, r4=str, r5=len. The guest LR identifies the
// exact call site — no host-frame ambiguity.
PPC_FUNC(sub_82A01E80) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    if (n > 160 || r3 == 0 || r4 == 0) {
        PRLOG(Exception,
              "newlstr#%u tid=%u ENTRY lr=%08X L=%08X str=%08X len=%u%s",
              n, Tid(), (uint32_t)ctx.lr, r3, r4, r5,
              (r3 == 0 || r4 == 0) ? "  <<<< NULL ARG" : "");
    }
    if (r3 == 0) {
        // The true caller: host return addresses are exact. Dump now.
        g_dumped = true;
        DumpAll("newlstring ENTERED WITH L=NULL — true caller backtrace");
        _exit(99);
    }
    __imp__sub_82A01E80(ctx, base);
}

// Float-record resolver entry: r3=registry, r4=record.
PPC_FUNC(sub_829FEB28) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    if (n <= 200 || ctx.r3.u32 == 0) {
        PRLOG(Exception,
              "resolver#%u tid=%u ENTRY lr=%08X reg=%08X rec=%08X%s", n, Tid(),
              (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32,
              ctx.r3.u32 == 0 ? "  <<<< NULL REGISTRY" : "");
    }
    __imp__sub_829FEB28(ctx, base);
}

// Lua chunk loader wrapper: r3=L, r4=source, r5=?, r6=chunkname.
// Dumps the script source so parse failures are directly visible.
PPC_FUNC(sub_82A07E58) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t src = ctx.r4.u32, name = ctx.r6.u32;
    if (n <= 200) {
        char nm[40] = "?";
        if (name) {
            for (int i = 0; i < 34; i++) {
                uint8_t ch = LoadU8(name + i);
                if (!ch) { nm[i] = 0; break; }
                nm[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            nm[34] = 0;
        }
        char dump[1600];
        size_t o = 0;
        if (src) {
            // The source is a ZIO-ish object: dump words around it and the
            // bytes that follow (the actual script text usually sits right
            // after the counter fields).
            o += snprintf(dump + o, sizeof(dump) - o,
                          "  source@%08X words:", src);
            for (int i = -4; i <= 12; i++) {
                o += snprintf(dump + o, sizeof(dump) - o, " [+%d]=%08X", i * 4,
                              LoadU32(src + i * 4));
            }
            o += snprintf(dump + o, sizeof(dump) - o, "\n  text+0..600: ");
            for (int i = 0; i < 600; i++) {
                uint8_t ch = LoadU8(src + i);
                char c = (ch >= 32 && ch < 127) ? (char)ch
                                                : (ch == 10 ? 'N' : '.');
                if (o < sizeof(dump) - 8) dump[o++] = c;
            }
            dump[o] = 0;
        } else {
            o = snprintf(dump, sizeof(dump), "  source=NULL");
        }
        PRLOG(Exception, "loadchunk#%u tid=%u lr=%08X L=%08X name=\"%s\" "
              "r4(src)=%08X r5=%08X r6=%08X\n%s",
              n, Tid(), (uint32_t)ctx.lr, ctx.r3.u32, nm, ctx.r4.u32,
              ctx.r5.u32, ctx.r6.u32, dump);
    }
    __imp__sub_82A07E58(ctx, base);
}

// Lua lexer token fetch (luaX_next-ish): r3 = lexer state (ls), r4 = seminfo.
// [ls+40] = ZIO {n, p, reader, data}; [ls+48] = current char. Logs each token.
extern "C" {
PPC_FUNC(__imp__sub_82A03588);
PPC_FUNC(__imp__sub_82A03930);
}
PPC_FUNC(sub_82A03588) {
    __imp__sub_82A03588(ctx, base);
}
PPC_FUNC(sub_82A03930) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ls = ctx.r3.u32;
    uint32_t zio = ls ? LoadU32(ls + 40) : 0;
    uint32_t zn = zio ? LoadU32(zio + 0) : 0;
    uint32_t zp = zio ? LoadU32(zio + 4) : 0;
    uint8_t cur = ls ? LoadU8(ls + 48) : 0;
    __imp__sub_82A03930(ctx, base);
    uint32_t tok = ctx.r3.u32;
    if (n <= 400) {   // the persist script parse
        char peek[24] = "";
        for (int i = 0; i < 10 && zp; i++) {
            uint8_t c = LoadU8(zp + i);
            peek[i] = (c >= 32 && c < 127) ? (char)c : (c == 10 ? 'N' : '.');
        }
        PRLOG(Exception,
              "lex#%u tid=%u ls=%08X tok=%d zn=%u zp=%08X cur=%02X "
              "peek=\"%s\"",
              n, Tid(), ls, (int)(int32_t)tok, zn, zp, cur, peek);
    }
}

// Token finalizer (called for EVERY scanned name/keyword): r3 = lexer state.
extern "C" {
PPC_FUNC(__imp__sub_82A032A8);
PPC_FUNC(__imp__sub_82A04128);
}
PPC_FUNC(sub_82A032A8) {
    __imp__sub_82A032A8(ctx, base);
}
// The REAL lexer (llex): r3 = ls, r4 = seminfo. [ls+36] = ZIO {n, p}.
// Logs every token return with the stream state.
PPC_FUNC(sub_82A04128) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ls = ctx.r3.u32;
    uint32_t zio = ls ? LoadU32(ls + 36) : 0;
    uint32_t zn = zio ? LoadU32(zio + 0) : 0;
    uint32_t zp = zio ? LoadU32(zio + 4) : 0;
    uint8_t cur = ls ? LoadU8(ls + 0) : 0;   // ls->current
    __imp__sub_82A04128(ctx, base);
    uint32_t tok = ctx.r3.u32;
    if (n <= 300) {
        char peek[24] = "";
        for (int i = 0; i < 10 && zp; i++) {
            uint8_t c = LoadU8(zp + i);
            peek[i] = (c >= 32 && c < 127) ? (char)c : (c == 10 ? 'N' : '.');
        }
        PRLOG(Exception,
              "lex#%u tid=%u ls=%08X tok=%d(0x%X) zn=%u zp=%08X cur=%02X "
              "peek=\"%s\"",
              n, Tid(), ls, (int)(int32_t)tok, tok, zn, zp, cur, peek);
    }
}

// Parser simpleexp: errors "unexpected symbol" when token != '(' / TK_NAME.
extern "C" {
PPC_FUNC(__imp__sub_82A06080);
}
PPC_FUNC(sub_82A06080) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ps = ctx.r3.u32;
    uint32_t tok = ps ? LoadU32(ps + 12) : 0;
    // seminfo (the TString) is likely at [ps+16]; text at +16 of TString.
    uint32_t semi = ps ? LoadU32(ps + 16) : 0;
    char txt[20] = "";
    if (semi && tok >= 285 && tok <= 300) {
        for (int i = 0; i < 15; i++) {
            uint8_t c = LoadU8(semi + 16 + i);
            if (!c) break;
            txt[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
    }
    PRLOG(Exception, "simpleexp#%u tid=%u ps=%08X tok=%d semi=%08X txt=\"%s\"",
          n, Tid(), ps, (int)(int32_t)tok, semi, txt);
    if (tok == 262 && !g_dumped) {
        g_dumped = true;
        DumpAll("simpleexp entered with TK_END — parser backtrace");
    }
    __imp__sub_82A06080(ctx, base);
}

// luaX_next: advances the parser's current token. Log the token stream.
extern "C" {
PPC_FUNC(__imp__sub_82A036C8);
}
PPC_FUNC(sub_82A036C8) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ls = ctx.r3.u32;
    __imp__sub_82A036C8(ctx, base);
    uint32_t tok = ls ? LoadU32(ls + 12) : 0;
    uint32_t semi = ls ? LoadU32(ls + 16) : 0;
    char txt[20] = "";
    if (semi && tok == 285) {
        for (int i = 0; i < 15; i++) {
            uint8_t c = LoadU8(semi + 16 + i);
            if (!c) break;
            txt[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
    }
    PRLOG(Exception, "next#%u tid=%u ls=%08X tok=%d(0x%X) txt=\"%s\"",
          n, Tid(), ls, (int)(int32_t)tok, tok, txt);
}

// luaX_next (82A04880) and luaX_lookahead (82A048F8): the parser's token
// consumption. Log the current token before/after each.
extern "C" {
PPC_FUNC(__imp__sub_82A04880);
PPC_FUNC(__imp__sub_82A048F8);
}
PPC_FUNC(sub_82A04880) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ls = ctx.r3.u32;
    uint32_t before = ls ? LoadU32(ls + 12) : 0;
    __imp__sub_82A04880(ctx, base);
    uint32_t after = ls ? LoadU32(ls + 12) : 0;
    PRLOG(Exception, "next#%u tok %d -> %d", n, (int)(int32_t)before,
          (int)(int32_t)after);
}
PPC_FUNC(sub_82A048F8) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t n = s_n.fetch_add(1) + 1;
    uint32_t ls = ctx.r3.u32;
    __imp__sub_82A048F8(ctx, base);
    uint32_t la = ls ? LoadU32(ls + 20) : 0;
    PRLOG(Exception, "look#%u lookahead=%d", n, (int)(int32_t)la);
}
