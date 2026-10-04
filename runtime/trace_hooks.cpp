// PrototypeRecomp Phase 2B runtime — guest call tracer.
// Every generated guest function has a WEAK alias symbol (sub_XXXX) pointing
// at the __imp__ implementation. Defining a STRONG symbol with the same name
// overrides it at link time, letting us trace specific call sites without
// touching generated code. Used only for diagnostics; controlled by
// PR_TRACE_HOOKS env variable.
#include "state.h"

#include <unistd.h>

namespace pr {
extern bool g_trace_hooks;
bool g_trace_hooks = false;

static PPCFunc* Imp(const char* name);
}  // namespace pr

// Resolved via dlsym-free direct externs below.
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

// NOTE: these must be defined at global scope with the EXACT generated
// signature to override the weak aliases.
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

namespace pr {
void TraceHooksInit() {
    g_trace_hooks = getenv("PR_TRACE_HOOKS") != nullptr;
    if (g_trace_hooks) g_log_enabled[(size_t)LogCategory::kTrace] = true;
}
}  // namespace pr
