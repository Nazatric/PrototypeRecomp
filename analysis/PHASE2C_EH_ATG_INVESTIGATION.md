# Phase 2C — CRT Format Engine / Lua / Gap-Function Investigation

## Status: SOLVED (this session)

The `ret@rsp = 0xffffffff7ea00000` crash chain and the ATG/MSVC-EH framing
were both misdiagnoses of the previous session. Everything below is
evidence-derived from real guest execution.

## The actual system: embedded Lua 5.1

| Address | Identity | Evidence |
|---------|----------|----------|
| 0xA080DAB8 | `lua_State* L` (registry) | resolver#1 entry kept it; format engine passes it |
| `[L+16]` | `global_State*` | strt at +0/+4/+8, frealloc at +12, ud at +16, nblocks at +68 |
| sub_82A02ED0 | `luaM_realloc_` (tracked grow/free) | ABI (L, old, oldsize, delta); nblocks += delta-oldsize |
| sub_82A01E80 | `luaS_newlstring` | hash init `step=(l>>5)+1` via `rlwinm r11,r5,27,5,31`; bucket walk |
| sub_82A01D90 | `newlstr` | node {next@0, hash@8, len@12, str@16}; table resize |
| sub_82A01C88 | `luaS_resize` | alloc/free via luaM_realloc_ |
| sub_82243AD0 | tracked realloc impl | heap 122 dispatch → 82230B58 → [[0x82BF327C+122*16]+0] policy [+12] |
| 829FCxxx family | `luaO_pushvfstring`/format engine | "%s:%d: %s", "%.7g", " near '", Lua error assembly |
| 829FBFC0 | persist-stream writer | dispatches serializers by type (27 = string) |
| 82A07E58 | chunk loader wrapper | (L, source, record, "=LuaNamePersistScript") |
| sub_82A04128 | `llex` | inline ZIO read + keyword check; 19 calls = 19 tokens observed |
| sub_82A04880 / 82A048F8 | `luaX_next` / `luaX_lookahead` | lookahead slot [ls+20]==287(TK_EOS) logic exact |
| sub_82A06080 | `primaryexp` | '(' / NAME / "unexpected symbol" error site |
| sub_82A061F8 / 82A06550 / 82A05E98 | `simpleexp` / `subexpr` / `explist1` | token enum matches Lua 5.1 (262=END, 265=FUNCTION, 273=RETURN, 285=NAME, 287=EOS) |
| sub_82A063A8 | `getbinopr` | table dispatch over tokens 37..94/262.. |

The interned names during boot are the Lua 5.1 stdlib opens ("Lua 5.1",
setmetatable/tonumber/…, coroutine/string/math/table/debug) followed by the
game's own bindings (Vector.normalize/mag/dot…, UID.CreateUID/UIDFromHash,
__persist, hashVal) — i.e. the full game-side Lua environment initializing.

## Root cause chain of the original crash

1. Previous session misdiagnosed sub_82A3F018 (MSVC `_output` printf format
   engine) as `__CxxFrameHandler3` and its format-specifier dispatch (u16
   offset table @ 0x820E8EB8, handler base 0x82A3F460) as an "EH interpreter
   jump table" (they also mis-added entries at 0x82A46xxx derived from a
   wrong table address decode).
2. The 16 forced "EH" entries partitioned the formatter; the resulting code
   clobbered nonvolatile r30 across sprintf, so the persist resolver
   (sub_829FEB28) restored L=NULL from r30 and luaS_newlstring dereferenced
   the NULL global_State: luaM_realloc_ loaded CTR=[[NULL+16]+12]=0 and
   PPC_CALL_INDIRECT_FUNC(0) faulted in the lookup table — the observed
   `__imp__sub_82A02ED0+0x160` CTR=0 crash. (The 0xffffffff7ea00000 host
   value was a stale host-stack artifact, not the guest target.)
3. Removing the bogus entries alone was not enough: the formatter's dispatch
   (and five sibling scanf-family dispatches) were never recovered as switch
   tables because an interleaved NOP (`ori r0,r0,0`) breaks XenonAnalyse's
   fixed patterns.

## Fixes

1. **Six format dispatch switch tables** added to switch_tables_phase2.toml
   (recovered by scripts/decode_dispatch.py; every label verified inside its
   .pdata function): bases 0x82A3F438 (printf _output), 0x82A44E2C,
   0x82A497DC, 0x82A4AAB8, 0x82A4C208 (scanf family), 0x82A7C34C (ctype).
2. **61 gap-region forced functions** (scripts/chunk_gaps_final.py): 29
   .pdata gap regions contain real functions whose in-function switch labels
   the gap-filling Function::Analyze leaves "outside" (cases emitted as
   `return`). Worst offender: getbinopr (0x82A063A8..0x82A06550) returned the
   raw token instead of OPR_NOBINOPR → a phantom binary operator →
   "LuaNamePersistScript:4: unexpected symbol near ''" → KeBugCheck. Entry
   points = bl targets AND .rdata/.data indirect pointers (an ATG worker
   otherwise crashed calling ctr=0x82818230 which had no lookup entry).
3. **Ring-advance interrupt gated on ISR registration**: the game registers
   its ISR ([[dev+10900]+16]) via the SCRATCH_REG4 writeback after ring init;
   firing the completion interrupt earlier made callback 82A79A00 bctrl into
   the heap poison (0BADF00D). Hardware semantics: no handler → no-op.
4. `-Wl,--allow-multiple-definition`: XenonRecomp's TU splitter emits the
   .text-tail block (0x82BA7564+, no .pdata there) into two TUs when the
   function count shifts the 256-function boundary; 220/220 duplicated
   bodies verified byte-identical (build plumbing only).

## Result

The Lua persist script
`return function( hashVal, string ) … return UIDFromHash( hashVal, string ) end end`
(literal @ 0x820126C0, 115 bytes) now parses; the game boots stably for
150+ seconds: 94 PM4 packets, 7 IBs, 6 DRAW_INDX_2, 7496 vblank interrupts,
22 threads. It now waits on its content pipeline (disc root has only
args.txt).

## Instrumentation (runtime/eh_trace.cpp)

Allocator rings, intern-table canary, mprotect write-watchpoint on the
string table, resolver/luaX_next/llex/primaryexp token tracing, Lua chunk
source dumps, CS-wait deadlock watchdog in RtlEnterCriticalSection.
All logging capped / rate-limited (disk safety).

## Key scripts

- scripts/decode_dispatch.py — decodes the six u16-offset dispatch tables
- scripts/scan_bctr_tables.py — finds bctr sites without TOML coverage
- scripts/fix_gap_switch_functions.py — maps recompiler switch errors to gaps
- scripts/align_gap_functions.py / chunk_gaps_final.py — chunked forced entries
- scripts/PrototypeRecomp/build_guest_o0.sh — parallel -O0 guest build

## Next blockers

1. The game's content pipeline: it awaits data beyond args.txt (real disc
   layout needed, or a synthetic minimal STFS/content set).
2. Main thread + tid26 spin in the queue dispatch (sub_827E1E58); render
   thread (827E25E0) parked at 82A9B7F8 — determine the exact awaited event
   once content exists.
3. Remaining 763 unrecognized bctr sites are currently compiled as indirect
   tail calls; any that are actually in-function switches will surface as
   new miscompiles (watch for "no switch table entry" / mid-function
   entry symptoms).
