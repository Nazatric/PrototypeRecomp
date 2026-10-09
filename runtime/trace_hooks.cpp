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
void __imp__sub_8239D908(PPCContext&, uint8_t*);
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
TRACE_HOOK_COLD(sub_82A58B80)
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

namespace pr {
void TraceHooksInit() {
    g_trace_hooks = getenv("PR_TRACE_HOOKS") != nullptr;
    if (g_trace_hooks) g_log_enabled[(size_t)LogCategory::kTrace] = true;
    EhTraceInit();
}
}  // namespace pr
