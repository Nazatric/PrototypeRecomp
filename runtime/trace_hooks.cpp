// PrototypeRecomp Phase 2B runtime — guest call tracer.
// Every generated guest function has a WEAK alias symbol (sub_XXXX) pointing
// at the __imp__ implementation. Defining a STRONG symbol with the same name
// overrides it at link time, letting us trace specific call sites without
// touching generated code. Used only for diagnostics; controlled by
// PR_TRACE_HOOKS env variable.
#include "state.h"

#include <unistd.h>
#include <atomic>

namespace pr {

void EhTraceInit();  // eh_trace.cpp: allocator/intern-table diagnostics.
extern bool g_trace_hooks;
bool g_trace_hooks = false;
}

extern "C" {
void __imp__sub_82A5E810(PPCContext&, uint8_t*);
void __imp__sub_82A5BA00(PPCContext&, uint8_t*);
void __imp__sub_82A52A18(PPCContext&, uint8_t*);
void __imp__sub_82A4BBD8(PPCContext&, uint8_t*);
void __imp__sub_82A5E6C8(PPCContext&, uint8_t*);
void __imp__sub_82A5E5E8(PPCContext&, uint8_t*);
void __imp__sub_82230000(PPCContext&, uint8_t*);
void __imp__sub_82A528D0(PPCContext&, uint8_t*);
void __imp__sub_82A5E868(PPCContext&, uint8_t*);
void __imp__sub_82A533A0(PPCContext&, uint8_t*);
void __imp__sub_82A5B9F0(PPCContext&, uint8_t*);
// Job-system / worker-creation chain (Phase 2B job-system investigation).
void __imp__sub_82ADDD88(PPCContext&, uint8_t*);
void __imp__sub_82ADDE88(PPCContext&, uint8_t*);
void __imp__sub_82ADDD18(PPCContext&, uint8_t*);
void __imp__sub_82AE5760(PPCContext&, uint8_t*);
void __imp__sub_82AE5630(PPCContext&, uint8_t*);
void __imp__sub_82AEA980(PPCContext&, uint8_t*);
void __imp__sub_82AEAA00(PPCContext&, uint8_t*);
void __imp__sub_82AEB180(PPCContext&, uint8_t*);
void __imp__sub_82AEA010(PPCContext&, uint8_t*);
void __imp__sub_82A5F100(PPCContext&, uint8_t*);
void __imp__sub_82A60B20(PPCContext&, uint8_t*);
void __imp__sub_82B4AAF8(PPCContext&, uint8_t*);
void __imp__sub_82ADE0C8(PPCContext&, uint8_t*);
void __imp__sub_82ADE2C8(PPCContext&, uint8_t*);
void __imp__sub_82AD27E0(PPCContext&, uint8_t*);
void __imp__sub_82ADE278(PPCContext&, uint8_t*);
// Submission path internals.
void __imp__sub_82AEA108(PPCContext&, uint8_t*);
void __imp__sub_82AE3330(PPCContext&, uint8_t*);
void __imp__sub_82AE4020(PPCContext&, uint8_t*);
void __imp__sub_82AE5FA8(PPCContext&, uint8_t*);
void __imp__sub_82AEB318(PPCContext&, uint8_t*);
void __imp__sub_82AE2948(PPCContext&, uint8_t*);
// Main-thread async-wait chain (Phase 2C content-pipeline investigation):
// the boot state machine pumps these while waiting on an async operation.
void __imp__sub_82B84420(PPCContext&, uint8_t*);
void __imp__sub_82238E58(PPCContext&, uint8_t*);
void __imp__sub_82AF0A58(PPCContext&, uint8_t*);
void __imp__sub_823F5768(PPCContext&, uint8_t*);
void __imp__sub_8239D438(PPCContext&, uint8_t*);
void __imp__sub_82365288(PPCContext&, uint8_t*);
void __imp__sub_823F3BB8(PPCContext&, uint8_t*);
void __imp__sub_823F3C40(PPCContext&, uint8_t*);
void __imp__sub_823F3D88(PPCContext&, uint8_t*);
void __imp__sub_823F3ED8(PPCContext&, uint8_t*);
void __imp__sub_82239F58(PPCContext&, uint8_t*);
void __imp__sub_822509C8(PPCContext&, uint8_t*);
void __imp__sub_82267718(PPCContext&, uint8_t*);
void __imp__sub_82267C10(PPCContext&, uint8_t*);
void __imp__sub_8226E260(PPCContext&, uint8_t*);
void __imp__sub_82230B58(PPCContext&, uint8_t*);
void __imp__sub_82230DE0(PPCContext&, uint8_t*);
void __imp__sub_827E1E58(PPCContext&, uint8_t*);
void __imp__sub_827E1F80(PPCContext&, uint8_t*);
// Display-subsystem init chain (tid26 = sub_8226DD70 entry): where does the
// display init stall? All hooked HOT (they may sit in spin loops).

// ATG worker state machine (sub_827E7420 sets, sub_827E70A0 gets).
void __imp__sub_827E7420(PPCContext&, uint8_t*);
void __imp__sub_827E70A0(PPCContext&, uint8_t*);
void __imp__sub_827E55E0(PPCContext&, uint8_t*);
void __imp__sub_827E5420(PPCContext&, uint8_t*);
void __imp__sub_82861B78(PPCContext&, uint8_t*);
void __imp__sub_82861D78(PPCContext&, uint8_t*);
void __imp__sub_82844850(PPCContext&, uint8_t*);
void __imp__sub_82A8E7A0(PPCContext&, uint8_t*);

// RCF archive mount chain (Phase 2D content-pipeline investigation):
// vtable method 8239EF58 -> 8239D908 -> 82A58CA8(8, paths[5], 5) ->
// per-file 82A58B80(8, path) -> 82A588C8 / 82A56CC8.
void __imp__sub_8239EF58(PPCContext&, uint8_t*);
void __imp__sub_8227C0B0(PPCContext&, uint8_t*);
void __imp__sub_82A58CA8(PPCContext&, uint8_t*);
void __imp__sub_82A58B80(PPCContext&, uint8_t*);
void __imp__sub_82A56CC8(PPCContext&, uint8_t*);
void __imp__sub_82A588C8(PPCContext&, uint8_t*);
void __imp__sub_823CE4A8(PPCContext&, uint8_t*);
void __imp__sub_82A54028(PPCContext&, uint8_t*);
void __imp__sub_8232C6B8(PPCContext&, uint8_t*);
// ATG thread-framework: work submit (KeSetEvent(param+32)) + dispatcher.
void __imp__sub_82A87608(PPCContext&, uint8_t*);
void __imp__sub_82A87810(PPCContext&, uint8_t*);
void __imp__sub_82A880D0(PPCContext&, uint8_t*);
void __imp__sub_82A69E70(PPCContext&, uint8_t*);
void __imp__sub_82A69E78(PPCContext&, uint8_t*);
void __imp__sub_82A876B0(PPCContext&, uint8_t*);
void __imp__sub_82A874C0(PPCContext&, uint8_t*);
void __imp__sub_82A87460(PPCContext&, uint8_t*);
void __imp__sub_8239D138(PPCContext&, uint8_t*);
void __imp__sub_8226DD70(PPCContext&, uint8_t*);
void __imp__sub_8226DA40(PPCContext&, uint8_t*);
void __imp__sub_82807120(PPCContext&, uint8_t*);
void __imp__sub_8280C460(PPCContext&, uint8_t*);
void __imp__sub_827F9428(PPCContext&, uint8_t*);
void __imp__sub_82807710(PPCContext&, uint8_t*);
void __imp__sub_827F98F0(PPCContext&, uint8_t*);
void __imp__sub_82AD2D18(PPCContext&, uint8_t*);
void __imp__sub_822309C8(PPCContext&, uint8_t*);
void __imp__sub_82235C60(PPCContext&, uint8_t*);
void __imp__sub_82806900(PPCContext&, uint8_t*);
void __imp__sub_82807C30(PPCContext&, uint8_t*);
void __imp__sub_8280AD80(PPCContext&, uint8_t*);
void __imp__sub_82807668(PPCContext&, uint8_t*);
void __imp__sub_82806A08(PPCContext&, uint8_t*);
void __imp__sub_82267FB0(PPCContext&, uint8_t*);
void __imp__sub_8239D908(PPCContext&, uint8_t*);
void __imp__sub_82A87608(PPCContext&, uint8_t*);
}

#define TRACE_HOOK(name)                                                    \
    void name(PPCContext& ctx, uint8_t* base) {                             \
        if (::pr::g_trace_hooks)                                            \
            ::pr::LogLine(::pr::LogCategory::kTrace, ">> %s", #name);       \
        __imp__##name(ctx, base);                                           \
        if (::pr::g_trace_hooks)                                            \
            ::pr::LogLine(::pr::LogCategory::kTrace, "<< %s r3=%08X", #name,\
                          ctx.r3.u32);                                      \
    }

// Detailed hook: logs arguments r3..r10 and guest memory strings.
#define TRACE_HOOK_ARGS(name, fmt)                                          \
    void name(PPCContext& ctx, uint8_t* base) {                             \
        if (::pr::g_trace_hooks)                                            \
            ::pr::LogLine(::pr::LogCategory::kTrace,                        \
                          ">> %s(" fmt ") [lr=%08X]", #name,                 \
                          ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,   \
                          ctx.r7.u32, ctx.r8.u32, (uint32_t)ctx.lr);        \
        __imp__##name(ctx, base);                                           \
        if (::pr::g_trace_hooks)                                            \
            ::pr::LogLine(::pr::LogCategory::kTrace, "<< %s r3=%08X", #name,\
                          ctx.r3.u32);                                      \
    }
TRACE_HOOK(sub_82A5E810)
TRACE_HOOK(sub_82A5BA00)
TRACE_HOOK(sub_82A52A18)
TRACE_HOOK(sub_82A4BBD8)
TRACE_HOOK(sub_82A5E6C8)
TRACE_HOOK(sub_82A5E5E8)
TRACE_HOOK(sub_82230000)
TRACE_HOOK(sub_82A528D0)
TRACE_HOOK(sub_82A5E868)
TRACE_HOOK(sub_82A533A0)
TRACE_HOOK(sub_82A5B9F0)

// Job-system trace hooks: name resolution happens on r3 (pointer to a
// name-bearing struct); log the pointed-to bytes for recognition.
#define TRACE_HOOK_NAME(name)                                               \
    void name(PPCContext& ctx, uint8_t* base) {                             \
        if (::pr::g_trace_hooks) {                                          \
            char nbuf[40] = {0};                                            \
            uint32_t p = ctx.r3.u32;                                        \
            if (p >= 0x82000000 && p < 0xC0000000) {                        \
                for (int i = 0; i < 39; i++) {                              \
                    uint8_t c = 0;                                          \
                    if (p + i < 0xC0000000)                                 \
                        c = *(uint8_t*)(::pr::g_guest_base + (uint64_t)(p + i)); \
                    nbuf[i] = (char)c;                                      \
                    if (!c) break;                                          \
                }                                                           \
            }                                                               \
            ::pr::LogLine(::pr::LogCategory::kTrace,                        \
                          ">> %s(r3=%08X '%s' r4=%08X r5=%08X r6=%08X)",    \
                          #name, ctx.r3.u32, nbuf, ctx.r4.u32, ctx.r5.u32,  \
                          ctx.r6.u32);                                      \
        }                                                                   \
        __imp__##name(ctx, base);                                           \
        if (::pr::g_trace_hooks)                                            \
            ::pr::LogLine(::pr::LogCategory::kTrace, "<< %s r3=%08X", #name,\
                          ctx.r3.u32);                                      \
    }

// Custom: GetModule internals — r4 points at the 16-byte name buffer whose
// first char is the module tag; dump it so the requested module name is
// visible. Also hook the public entry sub_82ADDE88(r3=name char*).
void sub_82ADDD88(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char nbuf[24] = {0};
        uint32_t p = ctx.r4.u32;
        if (p >= 0x82000000 && p < 0xC0000000) {
            for (int i = 0; i < 15; i++) {
                uint8_t c = *(uint8_t*)(::pr::g_guest_base + (uint64_t)(p + i));
                nbuf[i] = (char)c;
                if (!c) break;
            }
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82ADDD88[GET-MODULE](req=%08X name='%s' r5=%08X r6=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, nbuf, ctx.r5.u32, ctx.r6.u32,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82ADDD88(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82ADDD88 r3=%08X", ctx.r3.u32);
}
void sub_82ADDE88(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char nbuf[24] = {0};
        uint32_t p = ctx.r3.u32;
        if (p >= 0x82000000 && p < 0xC0000000) {
            for (int i = 0; i < 15; i++) {
                uint8_t c = *(uint8_t*)(::pr::g_guest_base + (uint64_t)(p + i));
                nbuf[i] = (char)c;
                if (!c) break;
            }
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82ADDE88[MODULE-ENTRY](name='%s' r4=%08X) [lr=%08X tid=%u]",
                      nbuf, ctx.r4.u32,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82ADDE88(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82ADDE88 r3=%08X", ctx.r3.u32);
}
void sub_82ADDD18(PPCContext& ctx, uint8_t* base) {
    static std::atomic<uint64_t> n{0};
    uint64_t idx = n.fetch_add(1);
    if (::pr::g_trace_hooks && idx < 12) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        auto LB = [](uint32_t p) { return *(uint8_t*)(::pr::g_guest_base + (uint64_t)p); };
        uint32_t req = ctx.r3.u32;
        uint32_t count = L(0x82DF5150);
        char mods[256] = {0};
        int nn = 0;
        for (uint32_t i = 0; i < count && i < 16 && nn < 240; i++) {
            uint32_t m = 0x82DF4838 + i * 284;
            nn += snprintf(mods + nn, sizeof(mods) - nn, "[%u]=%02X ", i, LB(m));
        }
        uint8_t tagv = (req >= 0x70000000 && req < 0xC0000000) ? LB(req) : 0xFF;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82ADDD18[MOD-LOOKUP](req=%08X tag=%02X) module_count=%u mods: %s [lr=%08X tid=%u]",
                      req, tagv, count, mods,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82ADDD18(ctx, base);
    if (::pr::g_trace_hooks && idx < 12)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82ADDD18 #%llu r3=%d", (unsigned long long)idx, (int)ctx.r3.u32);
}
TRACE_HOOK_NAME(sub_82AE5760)
TRACE_HOOK_NAME(sub_82AE5630)
TRACE_HOOK_NAME(sub_82AEA980)
TRACE_HOOK_NAME(sub_82AEAA00)
TRACE_HOOK_NAME(sub_82AEB180)
TRACE_HOOK_NAME(sub_82AEA010)

// Thread spawners: r7 = entry point, r8 = arg on these.
TRACE_HOOK_ARGS(sub_82A5F100, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK_ARGS(sub_82A60B20, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK_ARGS(sub_82B4AAF8, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")

TRACE_HOOK(sub_82ADE0C8)
TRACE_HOOK(sub_82ADE2C8)
TRACE_HOOK(sub_82AD27E0)
TRACE_HOOK(sub_82ADE278)
TRACE_HOOK_ARGS(sub_82AEA108, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK_ARGS(sub_82AE3330, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK_ARGS(sub_82AE4020, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK(sub_82AE5FA8)
TRACE_HOOK_ARGS(sub_82AEB318, "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X")
TRACE_HOOK(sub_82AE2948)

// ---- main-thread async-wait chain (content-pipeline investigation) ----
// Cold path (called once per boot): full args + first string at r3/r4.
#define TRACE_HOOK_COLD(name)                                                \
    void name(PPCContext& ctx, uint8_t* base) {                              \
        if (::pr::g_trace_hooks) {                                           \
            char s1[48] = {0}, s2[48] = {0};                                 \
            auto rdstr = [](uint32_t p, char* out) {                         \
                if (p >= 0x82000000 && p < 0xC0000000) {                     \
                    for (int i = 0; i < 47; i++) {                            \
                        uint8_t c = *(uint8_t*)(::pr::g_guest_base +          \
                            (uint64_t)(p + i));                              \
                        out[i] = (char)c;                                     \
                        if (!c) break;                                        \
                    }                                                         \
                }                                                             \
            };                                                                \
            rdstr(ctx.r3.u32, s1); rdstr(ctx.r4.u32, s2);                    \
            ::pr::LogLine(::pr::LogCategory::kTrace,                          \
                          ">> %s(r3=%08X '%s' r4=%08X '%s' r5=%08X r6=%08X " \
                          "r7=%08X) [lr=%08X tid=%u]", #name,               \
                          ctx.r3.u32, s1, ctx.r4.u32, s2, ctx.r5.u32,        \
                          ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr,          \
                          ::pr::GuestThread::GetCurrent()                     \
                              ? ::pr::GuestThread::GetCurrent()->thread_id : 0);\
        }                                                                     \
        __imp__##name(ctx, base);                                             \
        if (::pr::g_trace_hooks)                                             \
            ::pr::LogLine(::pr::LogCategory::kTrace,                         \
                          "<< %s r3=%08X", #name, ctx.r3.u32);               \
    }

// Hot spin-loop functions: log the first 4 calls only (disk safety).
#define TRACE_HOOK_HOT(name)                                                 \
    void name(PPCContext& ctx, uint8_t* base) {                              \
        static std::atomic<uint64_t> n{0};                                   \
        if (::pr::g_trace_hooks && n.fetch_add(1) < 4)                       \
            ::pr::LogLine(::pr::LogCategory::kTrace,                         \
                          ">> %s(r3=%08X r4=%08X r5=%08X) [lr=%08X] #%llu",\
                          #name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,          \
                          (uint32_t)ctx.lr,                                  \
                          (unsigned long long)n.load());                     \
        __imp__##name(ctx, base);                                            \
        if (::pr::g_trace_hooks && n.load() <= 4)                            \
            ::pr::LogLine(::pr::LogCategory::kTrace,                         \
                          "<< %s r3=%08X", #name, ctx.r3.u32);               \
    }

TRACE_HOOK_COLD(sub_82B84420)
TRACE_HOOK_COLD(sub_82238E58)
TRACE_HOOK_COLD(sub_82AF0A58)
TRACE_HOOK_COLD(sub_823F5768)
TRACE_HOOK_COLD(sub_8239D438)
TRACE_HOOK_COLD(sub_82365288)
TRACE_HOOK_COLD(sub_823F3BB8)
TRACE_HOOK_COLD(sub_823F3C40)
TRACE_HOOK_COLD(sub_823F3D88)
TRACE_HOOK_COLD(sub_823F3ED8)
TRACE_HOOK_COLD(sub_82239F58)
TRACE_HOOK_COLD(sub_822509C8)
TRACE_HOOK_COLD(sub_82267718)
TRACE_HOOK_COLD(sub_82267C10)
TRACE_HOOK_COLD(sub_8226E260)
TRACE_HOOK_COLD(sub_82230B58)
TRACE_HOOK_HOT(sub_82230DE0)
TRACE_HOOK_HOT(sub_827E1E58)
TRACE_HOOK_HOT(sub_827E1F80)
TRACE_HOOK_COLD(sub_8226DD70)
TRACE_HOOK_HOT(sub_8226DA40)
TRACE_HOOK_HOT(sub_82807120)
TRACE_HOOK_HOT(sub_8280C460)
TRACE_HOOK_HOT(sub_827F9428)
TRACE_HOOK_HOT(sub_82807710)
TRACE_HOOK_HOT(sub_827F98F0)
TRACE_HOOK_HOT(sub_82AD2D18)
TRACE_HOOK_HOT(sub_822309C8)
TRACE_HOOK_HOT(sub_82235C60)
TRACE_HOOK_HOT(sub_82806900)
TRACE_HOOK_HOT(sub_82807C30)
TRACE_HOOK_HOT(sub_8280AD80)
TRACE_HOOK_HOT(sub_82807668)
TRACE_HOOK_HOT(sub_82806A08)
TRACE_HOOK_HOT(sub_82267FB0)

// RCF mount chain (gated by 8227C0B0 + 82267FB0 results).
TRACE_HOOK_ARGS(sub_827E7420, "r3=%08X r4=%08X r5=%08X")
TRACE_HOOK_COLD(sub_827E70A0)
TRACE_HOOK_COLD(sub_827E55E0)
TRACE_HOOK_COLD(sub_827E5420)
TRACE_HOOK_COLD(sub_82861B78)
TRACE_HOOK_COLD(sub_82861D78)
TRACE_HOOK_COLD(sub_82844850)
TRACE_HOOK_COLD(sub_82A8E7A0)
TRACE_HOOK_COLD(sub_8239EF58)
TRACE_HOOK_COLD(sub_8239D908)
TRACE_HOOK_COLD(sub_8227C0B0)
TRACE_HOOK_COLD(sub_82A58CA8)
static void RdGuestStr(char* out, size_t n, uint32_t p);

// custom: dump the cement-library global state at registration time
void sub_82A58B80(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char s[96] = {0};
        RdGuestStr(s, sizeof s, ctx.r4.u32);
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        // cement global struct @ 0x82DCF578 (lis 0x82DD + addi -0xA88)
        uint32_t g = 0x82DCF578;
        char dump[600] = {0};
        int n = 0;
        for (int off = 0; off < 0x150 && n < 560; off += 4) {
            if (L(g + off) != 0)
                n += snprintf(dump + n, sizeof(dump) - n, "+%X=%08X ", off, L(g + off));
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A58B80[REG-ARCHIVE](type=%u path='%s') cement nonzero: %s [lr=%08X tid=%u]",
                      (unsigned)ctx.r3.u32, s, dump,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A58B80(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82A58B80 r3=%08X", ctx.r3.u32);
}
TRACE_HOOK_COLD(sub_82A56CC8)
TRACE_HOOK_COLD(sub_82A588C8)
TRACE_HOOK_COLD(sub_823CE4A8)
TRACE_HOOK_COLD(sub_82A54028)
TRACE_HOOK_COLD(sub_8232C6B8)
TRACE_HOOK_COLD(sub_82A87608)
TRACE_HOOK_COLD(sub_82A87810)
TRACE_HOOK_COLD(sub_82A880D0)
TRACE_HOOK_COLD(sub_82A69E70)
TRACE_HOOK_COLD(sub_82A69E78)
TRACE_HOOK_COLD(sub_82A876B0)
TRACE_HOOK_COLD(sub_82A874C0)
TRACE_HOOK_COLD(sub_82A87460)
TRACE_HOOK_COLD(sub_8239D138)

// ---- cement-library init chain (Phase 2E archive-manager blocker) ----
// sub_82A5AA28 = 'ATEM' chunk handler (the manifest chunk inside
// cementfiles.p3d) -> ... -> 82A5AE38: bl sub_82A5A920 (cement global init,
// writes 0x82DCF574 struct, creates the cement thread at 0x82A6A558).
// If these never fire, the cement library never initializes, [g+0xB4] stays
// NULL and sub_82A58B80 archive registration silently no-ops.
extern "C" {
void __imp__sub_82A5AA28(PPCContext&, uint8_t*);
void __imp__sub_82A5A920(PPCContext&, uint8_t*);
void __imp__sub_82A52FF8(PPCContext&, uint8_t*);
void __imp__sub_82A53738(PPCContext&, uint8_t*);
void __imp__sub_82A55448(PPCContext&, uint8_t*);
void __imp__sub_82A66718(PPCContext&, uint8_t*);
void __imp__sub_82A66BF8(PPCContext&, uint8_t*);
void __imp__sub_82A65918(PPCContext&, uint8_t*);
void __imp__sub_82331270(PPCContext&, uint8_t*);
void __imp__sub_828D2558(PPCContext&, uint8_t*);
void __imp__sub_828D2720(PPCContext&, uint8_t*);
void __imp__sub_828D2FD0(PPCContext&, uint8_t*);
void __imp__sub_828D2C88(PPCContext&, uint8_t*);
void __imp__sub_828D2E24(PPCContext&, uint8_t*);
void __imp__sub_828D31A8(PPCContext&, uint8_t*);
void __imp__sub_828D1730(PPCContext&, uint8_t*);
void __imp__sub_828E6BA0(PPCContext&, uint8_t*);
void __imp__sub_828E6D88(PPCContext&, uint8_t*);
void __imp__sub_8296F4C0(PPCContext&, uint8_t*);
}
TRACE_HOOK_COLD(sub_82A5AA28)
TRACE_HOOK_COLD(sub_82A5A920)
TRACE_HOOK_COLD(sub_82A52FF8)
TRACE_HOOK_COLD(sub_82A53738)
TRACE_HOOK_COLD(sub_82A55448)
TRACE_HOOK_COLD(sub_82A66718)
TRACE_HOOK_COLD(sub_82A66BF8)
TRACE_HOOK_COLD(sub_82A65918)

TRACE_HOOK_COLD(sub_82331270)
void sub_828D2720(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t obj = ctx.r3.u32, key = ctx.r4.u32;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2720[FIND](obj=%08X key=%08X) obj18=%08X obj0=%08X vt=%08X [lr=%08X tid=%u]",
                      obj, key,
                      (obj >= 0x82000000 && obj < 0xC0000000) ? L(obj + 0x18) : 0,
                      (obj >= 0x82000000 && obj < 0xC0000000) ? L(obj) : 0,
                      (obj >= 0x82000000 && obj < 0xC0000000 && L(obj) >= 0x82000000 && L(obj) < 0xC0000000) ? L(L(obj) + 8) : 0,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828D2720(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2720 r3=%08X", ctx.r3.u32);
}
void sub_828D2FD0(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2FD0[FALLBACK](key=%08X path=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    __imp__sub_828D2FD0(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2FD0 r3=%08X", ctx.r3.u32);
}
void sub_828D2C88(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2C88[ARCH-CTOR](r3=%08X r4=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    __imp__sub_828D2C88(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2C88 r3=%08X", ctx.r3.u32);
}
void sub_828E6BA0(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char s1[64] = {0};
        uint32_t p = ctx.r4.u32;
        if (p >= 0x82000000 && p < 0xC0000000) {
            for (int i = 0; i < 63; i++) {
                uint8_t c = *(uint8_t*)(::pr::g_guest_base + (uint64_t)(p + i));
                s1[i] = (char)c;
                if (!c) break;
            }
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828E6BA0[REQ-FACTORY](r3=%08X path='%s' r5=%08X r6=%08X r7=%08X r8=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, s1, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828E6BA0(ctx, base);
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t q) { return ::pr::LoadU32(q); };
        uint32_t r = ctx.r3.u32;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      "<< sub_828E6BA0 r3=%08X [r8=%08X r14=%08X]",
                      r, (r >= 0x82000000 && r < 0xC0000000) ? L(r + 8) : 0,
                      (r >= 0x82000000 && r < 0xC0000000) ? L(r + 0x14) : 0);
    }
}
void sub_828E6D88(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828E6D88[LOAD-MISS](r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828E6D88(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828E6D88 r3=%08X", ctx.r3.u32);
}
void sub_8296F4C0(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t q) { return ::pr::LoadU32(q); };
        uint32_t r = ctx.r4.u32;
        uint32_t r8 = (r >= 0x82000000 && r < 0xC0000000) ? L(r + 8) : 0;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_8296F4C0[QUERY](r4=%08X [r4+8]=%08X vt=%08X) [lr=%08X tid=%u]",
                      r, r8,
                      (r8 >= 0x82000000 && r8 < 0xC0000000) ? L(r8) : 0,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_8296F4C0(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_8296F4C0 r3=%08X", ctx.r3.u32);
}
void sub_828D1730(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t tbl = ctx.r3.u32, key = ctx.r4.u32;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D1730[BUCKET](tbl=%08X key=%08X) tbl0=%08X key0=%08X key4=%08X",
                      tbl, key,
                      (tbl >= 0x82000000 && tbl < 0xC0000000) ? L(tbl) : 0,
                      (key >= 0x82000000 && key < 0xC0000000) ? L(key) : 0,
                      (key >= 0x82000000 && key < 0xC0000000) ? L(key + 4) : 0);
    }
    __imp__sub_828D1730(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D1730 r3=%08X", ctx.r3.u32);
}

// ---- .rz decompression chain (Phase 2E frontend-asset loading) ----
// The content loader pumps decompress tasks via sub_827F2798 -> sub_827F3680
// (ring-buffer LZ decompressor) and read pumps via sub_827F5900 -> sub_82A37000
// (strided copy). Crash under investigation: dst buffer NULL.
extern "C" {
void __imp__sub_827F28D0(PPCContext&, uint8_t*);
void __imp__sub_827F3680(PPCContext&, uint8_t*);
void __imp__sub_827F5900(PPCContext&, uint8_t*);
void __imp__sub_82A37000(PPCContext&, uint8_t*);
void __imp__sub_827F5218(PPCContext&, uint8_t*);
void __imp__sub_827F35F8(PPCContext&, uint8_t*);
void __imp__sub_82A372A0(PPCContext&, uint8_t*);
void __imp__sub_82A371C8(PPCContext&, uint8_t*);
void __imp__sub_828D2D60(PPCContext&, uint8_t*);
void __imp__sub_828DF140(PPCContext&, uint8_t*);
void __imp__sub_828DE318(PPCContext&, uint8_t*);
void __imp__sub_828CAE98(PPCContext&, uint8_t*);
void __imp__sub_828EF900(PPCContext&, uint8_t*);
void __imp__sub_828C40F8(PPCContext&, uint8_t*);
void __imp__sub_828D45A8(PPCContext&, uint8_t*);
void __imp__sub_828D4588(PPCContext&, uint8_t*);
void __imp__sub_828D2400(PPCContext&, uint8_t*);
void __imp__sub_828EDD10(PPCContext&, uint8_t*);
void __imp__sub_828EDB90(PPCContext&, uint8_t*);
void __imp__sub_828D2558(PPCContext&, uint8_t*);
void __imp__sub_828D25D8(PPCContext&, uint8_t*);
void __imp__sub_827F4980(PPCContext&, uint8_t*);
void __imp__sub_8286A780(PPCContext&, uint8_t*);
void __imp__sub_82A5BE88(PPCContext&, uint8_t*);
void __imp__sub_82A9B4A8(PPCContext&, uint8_t*);
void __imp__sub_82A53D80(PPCContext&, uint8_t*);
}

void sub_828D2400(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t q = ctx.r3.u32;
        char extra[400] = {0};
        if (q >= 0x82000000 && q < 0xC0000000) {
            int n = 0;
            for (int off = 0; off <= 0x30 && n < 340; off += 4)
                n += snprintf(extra + n, sizeof(extra) - n, "+%02X=%08X ", off, L(q + off));
            // bucket array at [q+0x18]: [+0]=base, [+4]=count, entries 16B
            uint32_t ba = L(q + 0x18);
            if (ba >= 0x82000000 && ba < 0xC0000000) {
                uint32_t bbase = ba + 8, bcount = L(ba + 4);
                int nonempty = 0;
                if (bbase >= 0x82000000 && bbase < 0xC0000000 && bcount < 0x10000) {
                    for (uint32_t i = 0; i < bcount && i < 4096; i++) {
                        uint32_t e0 = L(bbase + 0);   // entries at bbase + i*16? per disasm: r10 = ba+8 start
                        if (e0 != 0xFFFFFFFE) nonempty++;
                        bbase += 16;
                    }
                }
                ::pr::LogLine(::pr::LogCategory::kTrace,
                              "[buckets] ba=%08X count=%08X nonempty=%d e0=%08X e1=%08X e2=%08X",
                              ba, bcount, nonempty, L(ba + 8), L(ba + 12), L(ba + 16));
            }
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2400(q=%08X r4=%08X r5=%08X) %s [lr=%08X tid=%u]",
                      q, ctx.r4.u32, ctx.r5.u32, extra, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828D2400(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2400 r3=%08X", ctx.r3.u32);
}

void sub_828EDD10(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828EDD10(r3=%08X r4=%08X r5=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828EDD10(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828EDD10 r3=%08X", ctx.r3.u32);
}

void sub_828EDB90(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828EDB90(r3=%08X r4=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828EDB90(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828EDB90 r3=%08X", ctx.r3.u32);
}

void sub_828EF900(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t req = ctx.r3.u32, reg = ctx.r4.u32;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828EF900(req=%08X reg=%08X r5=%08X r6=%08X) [lr=%08X tid=%u]",
                      req, reg, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
        if (reg >= 0x82000000 && reg < 0xC0000000) {
            uint32_t p8 = L(reg + 8), p20 = L(reg + 0x20);
            ::pr::LogLine(::pr::LogCategory::kTrace,
                          "[reg] +08=%08X (deref=%08X) +20=%08X (vt=%08X m0=%08X m1=%08X)",
                          p8, (p8 >= 0x82000000 && p8 < 0x82C00000) ? L(p8) : 0,
                          p20,
                          (p20 >= 0x82000000 && p20 < 0xC0000000) ? L(p20) : 0,
                          (p20 >= 0x82000000 && p20 < 0xC0000000) ? L(p20 + 8) : 0,
                          (p20 >= 0x82000000 && p20 < 0xC0000000 && L(p20) >= 0x82000000 && L(p20) < 0x82C00000) ? L(L(p20) + 4) : 0);
        }
    }
    __imp__sub_828EF900(ctx, base);
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        if (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0xC0000000)
            ::pr::LogLine(::pr::LogCategory::kTrace,
                          "<< sub_828EF900 req=%08X f38=%08X f3C=%08X f40=%08X f44=%08X",
                          ctx.r3.u32, L(ctx.r3.u32 + 0x38), L(ctx.r3.u32 + 0x3C),
                          L(ctx.r3.u32 + 0x40), L(ctx.r3.u32 + 0x44));
    }
}

// ---- RCF archive lookup chain (Phase 2E: the frontend-asset blocker) ----
// sub_828D2D60(archive, path, ?, ?) -> NULL when [archive+4] (registry) is
// NULL or the finder sub_828DF140 fails. Callers deref the result -> crash.
static void RdGuestStr(char* out, size_t n, uint32_t p) {
    if (p < 0x82000000 || p >= 0xC0000000) { out[0] = 0; return; }
    for (size_t i = 0; i + 1 < n; i++) {
        uint8_t c = *(uint8_t*)(::pr::g_guest_base + (uint64_t)(p + i));
        out[i] = (char)c;
        if (!c) return;
    }
    out[n - 1] = 0;
}

void sub_828CAE98(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char s[96] = {0};
        RdGuestStr(s, sizeof s, ctx.r5.u32);
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828CAE98(r3=%08X r4=%08X r5=%08X '%s' r6=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, s, ctx.r6.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828CAE98(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828CAE98 r3=%08X", ctx.r3.u32);
}

void sub_828D2D60(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char s[96] = {0};
        RdGuestStr(s, sizeof s, ctx.r4.u32);
        uint32_t arch = ctx.r3.u32;
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t reg = (arch >= 0x82000000 && arch < 0xC0000000) ? L(arch + 4) : 0xDEAD;
        // The global cement-library archive list at 0x82D35D68 (queried via
        // vtable+4 by the load-on-miss path sub_828E6D88).
        uint32_t glist = L(0x82D35D68);
        uint32_t glist_vt = (glist >= 0x82000000 && glist < 0xC0000000) ? L(glist) : 0;
        // Global key-intern singleton at 0x82D34DDC: +0x10 = the shared
        // interned-keys table (the insert target during searches).
        uint32_t intern_tbl = L(0x82D34DDC + 0x10);
        // The registry's hash-table object [reg+0x20] and its table root
        // [+0x18] (0 = empty; find fails instantly).
        uint32_t reg20 = (reg >= 0x82000000 && reg < 0xC0000000) ? L(reg + 0x20) : 0;
        uint32_t reg20_18 = (reg20 >= 0x82000000 && reg20 < 0xC0000000) ? L(reg20 + 0x18) : 0;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2D60(archive=%08X path=%08X '%s' r5=%08X r6=%08X) reg=[arch+4]=%08X "
                      "archf0=%08X archf8=%08X glist=%08X(gvt=%08X) reg20=%08X tbl18=%08X intern10=%08X [lr=%08X tid=%u]",
                      arch, ctx.r4.u32, s, ctx.r5.u32, ctx.r6.u32, reg,
                      (arch >= 0x82000000 && arch < 0xC0000000) ? L(arch) : 0,
                      (arch >= 0x82000000 && arch < 0xC0000000) ? L(arch + 8) : 0,
                      glist, glist_vt, reg20, reg20_18, intern_tbl,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828D2D60(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2D60 r3=%08X", ctx.r3.u32);
}

void sub_828DF140(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        char s[96] = {0};
        RdGuestStr(s, sizeof s, ctx.r4.u32);
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t reg = ctx.r3.u32;
        char extra[512] = {0};
        if (reg >= 0x82000000 && reg < 0xC0000000) {
            // dump registry fields 0x00..0x40
            int n = 0;
            for (int off = 0; off <= 0x40 && n < 400; off += 4) {
                n += snprintf(extra + n, sizeof(extra) - n, "+%02X=%08X ", off, L(reg + off));
            }
        }
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828DF140(reg=%08X path='%s' r5=%08X r6=%08X r7=%08X) %s [lr=%08X tid=%u]",
                      reg, s, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, extra, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828DF140(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828DF140 r3=%08X", ctx.r3.u32);
}

void sub_828DE318(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t c = ctx.r3.u32;
        uint32_t c10 = (c >= 0x82000000 && c < 0xC0000000) ? L(c + 0x10) : 0;
        uint32_t tbl = (c10 >= 0x82000000 && c10 < 0xC0000000) ? L(c10 + 0x10) : 0;
        uint32_t c14 = (c >= 0x82000000 && c < 0xC0000000) ? L(c + 0x14) : 0;
        uint32_t c38 = (c >= 0x82000000 && c < 0xC0000000) ? L(c + 0x38) : 0;
        uint32_t c3818 = (c38 >= 0x82000000 && c38 < 0xC0000000) ? L(c38 + 0x18) : 0;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828DE318(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X) ctx10=%08X ctx1010=%08X ctx14=%08X ctx38=%08X(18=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                      c10, tbl, c14, c38, c3818, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828DE318(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828DE318 r3=%08X", ctx.r3.u32);
}

static void DumpRing(const char* tag, uint32_t st) {
    if (st < 0x82000000 || st >= 0xC0000000) return;
    auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
    uint32_t ring = L(st + 0x24);
    if (ring < 0x82000000 || ring >= 0xC0000000) {
        ::pr::LogLine(::pr::LogCategory::kTrace, "[ring] %s st=%08X ring=INVALID(%08X)", tag, st, ring);
        return;
    }
    ::pr::LogLine(::pr::LogCategory::kTrace,
                  "[ring] %s st=%08X ring=%08X f38=%08X f1C=%08X f3C=%08X bytes: "
                  "%02X %02X %02X %02X | %02X %02X %02X %02X | %02X %02X %02X %02X | %02X %02X %02X %02X",
                  tag, st, ring, L(st + 0x38), L(st + 0x1C), L(st + 0x3C),
                  *(uint8_t*)(::pr::g_guest_base + ring),
                  *(uint8_t*)(::pr::g_guest_base + ring + 1),
                  *(uint8_t*)(::pr::g_guest_base + ring + 2),
                  *(uint8_t*)(::pr::g_guest_base + ring + 3),
                  *(uint8_t*)(::pr::g_guest_base + ring + 4),
                  *(uint8_t*)(::pr::g_guest_base + ring + 5),
                  *(uint8_t*)(::pr::g_guest_base + ring + 6),
                  *(uint8_t*)(::pr::g_guest_base + ring + 7),
                  *(uint8_t*)(::pr::g_guest_base + ring + 8),
                  *(uint8_t*)(::pr::g_guest_base + ring + 9),
                  *(uint8_t*)(::pr::g_guest_base + ring + 10),
                  *(uint8_t*)(::pr::g_guest_base + ring + 11),
                  *(uint8_t*)(::pr::g_guest_base + ring + 12),
                  *(uint8_t*)(::pr::g_guest_base + ring + 13),
                  *(uint8_t*)(::pr::g_guest_base + ring + 14),
                  *(uint8_t*)(::pr::g_guest_base + ring + 15));
}

void sub_827F35F8(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_827F35F8(state=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
        DumpRing("reset-pre", ctx.r3.u32);
    }
    __imp__sub_827F35F8(ctx, base);
    if (::pr::g_trace_hooks)
        DumpRing("reset-post", ctx.r3.u32);
}

void sub_82A372A0(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A372A0(value=%08X mode=%08X alloc=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A372A0(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82A372A0 r3=%08X", ctx.r3.u32);
}

void sub_82A371C8(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A371C8(obj=%08X value=%08X mode=%08X alloc=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A371C8(ctx, base);
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        if (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0xC0000000)
            ::pr::LogLine(::pr::LogCategory::kTrace,
                          "<< sub_82A371C8 obj=%08X vt=%08X f14=%08X f18=%08X f1C=%08X",
                          ctx.r3.u32, L(ctx.r3.u32), L(ctx.r3.u32 + 0x14),
                          L(ctx.r3.u32 + 0x18), L(ctx.r3.u32 + 0x1C));
    }
}

static void DumpDecompState(const char* tag, uint32_t st) {
    // Decompressor state layout (from static analysis of sub_827F3680):
    // +0x0C allocator/stream object, +0x10 bufsize, +0x14 limit, +0x18 outbuf,
    // +0x24 ?, +0x2C uncompressed size, +0x30/0x34 cursor fields.
    if (st < 0x82000000 || st >= 0xC0000000) return;
    auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
    ::pr::LogLine(::pr::LogCategory::kTrace,
                  "[decomp] %s st=%08X: alloc=%08X bufsz=%08X limit=%08X "
                  "outbuf=%08X f24=%08X uncsz=%08X f30=%08X f34=%08X "
                  "| this60: f1C=%08X f0=%08X f8=%08X",
                  tag, st, L(st + 0xC), L(st + 0x10), L(st + 0x14),
                  L(st + 0x18), L(st + 0x24), L(st + 0x2C), L(st + 0x30),
                  L(st + 0x34), L(st + 0x7C), L(st + 0x60), L(st + 0x68));
}

void sub_827F3680(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_827F3680(state=%08X src=%08X len=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
        DumpDecompState("pre", ctx.r3.u32);
    }
    __imp__sub_827F3680(ctx, base);
    if (::pr::g_trace_hooks)
        DumpDecompState("post", ctx.r3.u32);
}

void sub_827F5900(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        uint32_t stream = ctx.r4.u32;
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_827F5900(this=%08X stream=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, stream, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
        if (stream >= 0x82000000 && stream < 0xC0000000) {
            uint32_t bufd = L(stream + 0x3C);
            ::pr::LogLine(::pr::LogCategory::kTrace,
                          "[readpump] stream=%08X bufd=%08X base=%08X f4=%08X f8=%08X fC=%08X f10=%08X",
                          stream, bufd,
                          (bufd >= 0x82000000 && bufd < 0xC0000000) ? L(bufd) : 0,
                          (bufd >= 0x82000000 && bufd < 0xC0000000) ? L(bufd + 4) : 0,
                          (bufd >= 0x82000000 && bufd < 0xC0000000) ? L(bufd + 8) : 0,
                          (bufd >= 0x82000000 && bufd < 0xC0000000) ? L(bufd + 0xC) : 0,
                          (bufd >= 0x82000000 && bufd < 0xC0000000) ? L(bufd + 0x10) : 0);
        }
    }
    __imp__sub_827F5900(ctx, base);
}

void sub_82A37000(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A37000(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A37000(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82A37000 r3=%08X", ctx.r3.u32);
}

void sub_827F28D0(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_827F28D0(r3=%08X r4=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
        if (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0xC0000000) {
            auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
            ::pr::LogLine(::pr::LogCategory::kTrace,
                          "[task] r3 obj: fC0=%08X fC4=%08X fE0=%08X fE4=%08X fE8=%08X fEC=%08X fF0=%08X",
                          L(ctx.r3.u32 + 0xC0), L(ctx.r3.u32 + 0xC4), L(ctx.r3.u32 + 0xE0),
                          L(ctx.r3.u32 + 0xE4), L(ctx.r3.u32 + 0xE8), L(ctx.r3.u32 + 0xEC),
                          L(ctx.r3.u32 + 0xF0));
        }
    }
    __imp__sub_827F28D0(ctx, base);
}


static std::atomic<uint64_t> g_insert_count{0};
static std::atomic<uint64_t> g_insert_made{0};

void sub_828D2558(PPCContext& ctx, uint8_t* base) {
    g_insert_count.fetch_add(1);
    if (::pr::g_trace_hooks && g_insert_count.load() <= 8) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D2558[insert](this=%08X r4=%08X r5=%08X r6=%08X r7=%08X) #%llu [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                      (unsigned long long)g_insert_count.load(), (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828D2558(ctx, base);
    if (::pr::g_trace_hooks && g_insert_count.load() <= 8)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_828D2558 r3=%08X", ctx.r3.u32);
    if (ctx.r3.u32 != 0) g_insert_made.fetch_add(1);
}

void sub_828D25D8(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks && g_insert_count.load() < 3) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_828D25D8(this=%08X r4=%08X r5=%08X r6=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_828D25D8(ctx, base);
}


void sub_827F4980(PPCContext& ctx, uint8_t* base) {
    if (::pr::g_trace_hooks) {
        auto L = [](uint32_t p) { return ::pr::LoadU32(p); };
        char s[48] = {0};
        RdGuestStr(s, sizeof s, ctx.r3.u32);
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_827F4980[RCF-PARSE](r3=%08X '%s' r4=%08X) f1C=%08X f14=%08X [lr=%08X tid=%u]",
                      ctx.r3.u32, s, ctx.r4.u32,
                      (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0xC0000000) ? L(ctx.r3.u32 + 0x1C) : 0,
                      (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0xC0000000) ? L(ctx.r3.u32 + 0x14) : 0,
                      (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_827F4980(ctx, base);
    if (::pr::g_trace_hooks)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_827F4980 r3=%08X", ctx.r3.u32);
}

void sub_8286A780(PPCContext& ctx, uint8_t* base) {
    static std::atomic<uint64_t> n{0};
    if (::pr::g_trace_hooks && n.fetch_add(1) < 4) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_8286A780[RCF-MOUNT-TASK](r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X) [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_8286A780(ctx, base);
}


void sub_82A5BE88(PPCContext& ctx, uint8_t* base) {
    static std::atomic<uint64_t> n{0};
    uint64_t idx = n.fetch_add(1);
    if (::pr::g_trace_hooks && idx < 40) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A5BE88[DISC-READ](r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X) #%llu [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                      (unsigned long long)idx, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A5BE88(ctx, base);
    if (::pr::g_trace_hooks && idx < 40)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82A5BE88 #%llu r3=%08X", (unsigned long long)idx, ctx.r3.u32);
}


void sub_82A9B4A8(PPCContext& ctx, uint8_t* base) {
    static std::atomic<uint64_t> n{0};
    uint64_t idx = n.fetch_add(1);
    if (::pr::g_trace_hooks && idx < 60) {
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A9B4A8[MSG-DISPATCH](r3=%08X r4=%08X r5=%08X r6=%08X r10=%u) #%llu [lr=%08X tid=%u]",
                      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (unsigned)ctx.r10.u32,
                      (unsigned long long)idx, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A9B4A8(ctx, base);
}


void sub_82A53D80(PPCContext& ctx, uint8_t* base) {
    static std::atomic<uint64_t> n{0};
    uint64_t idx = n.fetch_add(1);
    if (::pr::g_trace_hooks && idx < 30) {
        char s[64] = {0};
        RdGuestStr(s, sizeof s, ctx.r3.u32);
        ::pr::LogLine(::pr::LogCategory::kTrace,
                      ">> sub_82A53D80[CFG-HASH](r3=%08X '%s' r4=%08X r5=%08X) #%llu [lr=%08X tid=%u]",
                      ctx.r3.u32, s, ctx.r4.u32, ctx.r5.u32,
                      (unsigned long long)idx, (uint32_t)ctx.lr,
                      ::pr::GuestThread::GetCurrent() ? ::pr::GuestThread::GetCurrent()->thread_id : 0);
    }
    __imp__sub_82A53D80(ctx, base);
    if (::pr::g_trace_hooks && idx < 30)
        ::pr::LogLine(::pr::LogCategory::kTrace, "<< sub_82A53D80 #%llu r3=%08X (want 65B)", (unsigned long long)idx, ctx.r3.u32);
}




namespace pr {
void TraceHooksInit() {
    g_trace_hooks = getenv("PR_TRACE_HOOKS") != nullptr;
    if (g_trace_hooks) g_log_enabled[(size_t)LogCategory::kTrace] = true;
    EhTraceInit();
}
}  // namespace pr
