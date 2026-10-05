// PrototypeRecomp Phase 2B runtime — guest call tracer.
// Every generated guest function has a WEAK alias symbol (sub_XXXX) pointing
// at the __imp__ implementation. Defining a STRONG symbol with the same name
// overrides it at link time, letting us trace specific call sites without
// touching generated code. Used only for diagnostics; controlled by
// PR_TRACE_HOOKS env variable.
#include "state.h"

#include <unistd.h>

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

TRACE_HOOK_NAME(sub_82ADDD88)
TRACE_HOOK_NAME(sub_82ADDD18)
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

namespace pr {
void TraceHooksInit() {
    g_trace_hooks = getenv("PR_TRACE_HOOKS") != nullptr;
    if (g_trace_hooks) g_log_enabled[(size_t)LogCategory::kTrace] = true;
    EhTraceInit();
}
}  // namespace pr
