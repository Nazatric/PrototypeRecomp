# Phase 2D — Boot Handshake Repair: From Stalled Task Queues to the Content Gate

Session date: 2026-10-07 (UTC+8)
Starting point: Phase 2C stable checkpoint (22 threads, 94 PM4 packets, 24 draws,
7496 vblanks, display-init stalled on the 'unlit' material lookup).

## Result

The boot pipeline now advances through the complete handshake chain and stops at
the first *content* dependency — the first time in the project's history that
the limiting factor is missing game data rather than a runtime defect:

| Metric | Phase 2C checkpoint | Phase 2D result |
|---|---|---|
| PM4 packets processed | 94 | 1160 |
| Indirect-buffer chains | 7 | 30 |
| DRAW_INDX draws | 24 | 103 |
| Shader loads (IM_LOAD) | 2 | 19 |
| Display-init flag [0x82D844F8] | 0 (never set) | **1 (set)** |
| Disc file requests | 2 (args.txt, root) | 4 (+skuinfo.p3d, skuinfo.p3d.rz) |
| Steady state | display-init spin | module-init / content gate |

The game now executes past display-init, runs the module registration chain
(`sub_823CB238` → ten subsystem registrations), creates additional ATG workers,
and requests `skuinfo.p3d` / `skuinfo.p3d.rz` from the disc — the SKU/region
validation files that precede RCF mounting in the real boot sequence.

## Root causes found and fixed (three kernel-semantics bugs + one recompiler defect)

### 1. LARGE_INTEGER timeout semantics (all wait paths)

Xbox 360 `PLARGE_INTEGER` timeouts: **negative = relative duration in 100 ns
units; positive = absolute FILETIME**. The runtime divided the *unsigned*
value by 100 and treated the result as nanoseconds. A −30 ms relative timeout
(`0xFFFFFFFFFFFB6C20`) became ~5.8 × 10⁸ seconds. Every timed wait in the game
either parked effectively forever or expired instantly.

Fix: `TimeoutToRelativeNanos()` (imports_core.cpp) implements both forms and is
applied to thread/event/semaphore/mutant/timer and multi-object waits.

### 2. KeDelayExecutionThread ABI (argument count)

The import takes **three** arguments:
`(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)`.
The implementation read the interval pointer from r4 (the Alertable boolean),
so `interval_ptr` was 0/1 and **every guest sleep was a no-op**. The
display-init thread's task-queue pump spun at ~250k cycles/second
(1.7 M critical-section acquisitions in 12 s), starving everything else.

Fix: read `ARG(2)` for the interval.

### 3. KeDelayExecutionThread interval unit conversion

`|interval|` is in 100 ns units → nanoseconds is `× 100`, not `÷ 100`.
A 100 ms sleep slept 10 µs (compounding bug #2 once it was fixed).

### 4. Truncated 28-entry switch table in sub_82A9B4A8 (the decisive fix)

`switch_tables_phase2.toml` contained a partial entry for the ATG/audio
subsystem's message dispatch loop:

```toml
base = 0x82A9B614   # the lis instruction, not the bctr (0x82A9B628)
r = 10
labels = [ 0x82A9B6A0, 0x82A9B828 ]   # 2 of 28 labels
```

XenonRecomp matched the entry (the lookup persists from the first address hit)
and emitted a two-case switch with `default: __builtin_unreachable();` —
**26 of the 28 message types compiled to undefined behavior**. The task runner
thread (tid25, entry `sub_827E25E0` → `sub_827E22D8`) dispatched into this
function and never returned, so the ATG worker state machine stuck at state 2
(the 2→3 promotion is downstream of the dispatch), so display-init spun
forever waiting for state 3.

Fix: full 28-label table (verified against `pristine.bin` at `0x82A9B62C`,
inline after the `bctr`), `base = 0x82A9B628` (the `bctr` itself).

Evidence chain (all observable in one run after the fix):
state 1 → state 2 → state 3 → display-init completes → `[0x82D844F8] = 1` →
main thread exits `sub_8226E260` → module-init chain → skuinfo.p3d request.

## The verified boot handshake chain (full map)

```
main thread (tid7)                          display-init thread (tid26)
  sub_82239F58                                 sub_8226DD70 (entry)
    ├─ gate: sub_82267FB0 → 0                    ├─ sub_8226DA40 (display setup)
    ├─ sub_8226E260 (wait loop)                  │    └─ atomic set [0x82D844F8] (tail)
    │    ├─ pump: sub_827E1F70 → drain           ├─ sub_8226D820 (worker handshake)
    │    │    └─ sub_827E1E58 (drain)            │    ├─ sub_827E1F80: create worker
    │    ├─ sleep(100)  ← was broken             │    │    ├─ register task in queue
    │    └─ check [0x82D844F8] ──────────────┐   │    │    ├─ release queue semaphore
    └─ (flag set) → sub_8228ABE0             |   │    │    └─ pump until state == 3
         └─ module/service registration      |   │    └─ 'unlit' material lookup
              (sub_8228AAF8, 82A88150, ...)  |   └─ sub_8280AA88...
                                              |
        task-runner thread (tid25)            |
          sub_827E25E0 → sub_827E22D8         |
            ├─ wait queue semaphore ──────────┤ (tid26 releases it)
            ├─ pop task, requeue              |
            ├─ set worker state 2             |
            ├─ sub_82A0ACD8(46,...) ── audio/ATG bootstrap
            │    └─ sub_82A9B4A8: 28-way message dispatch ← WAS UB
            ├─ (dispatch completes)           |
            └─ set worker state 3 ────────────┘ (tid26's wait exits)
```

## Remaining state at the content gate (60 s steady state)

- tid7 (main): pumping the task queue inside the module-init worker-creation
  flow (`sub_827E1F80+324` on the stack) — waiting for the content loader.
- tid25: running the ATG worker loop (`sub_827E4880`).
- tid26: parked on pool semaphore F8000124 (normal idle).
- tid27: actively cycling its 30 ms service loop (the timeout fix in action).
- tid28: parked on its work event A1A05A50 — by design; work arrives via the
  hash-dispatch submit path (`sub_82A87608`: `KeSetEvent(param+32)`), which
  runs once the content loader submits render work.
- GPU: 1160 packets, 103 draws, 19 shader loads, 60 Hz vblank steady; swap
  queue still empty (no presentation until the loading screen exists).
- FS: `args.txt` ok; `skuinfo.p3d` / `skuinfo.p3d.rz` CONTENT MISSING — the
  current blocker.

## The content situation (re-verified this session)

The repository's five RCF archives (`00cells/01cells/00art/01art/02art.rcf`,
~1.1 GB total) are stored as **Git LFS pointer files only**. The GitHub LFS
batch API returns 404 "Object does not exist on the server" for every oid —
the binaries were never uploaded (verified twice, several hours apart). No
release assets, no other branches. Until the objects are uploaded
(`git lfs push origin main --all` from the machine that has the files) or the
content is supplied by another channel, the boot stops at the skuinfo/RCF gate.

`skuinfo.p3d` and `skuinfo.p3d.rz` are additionally required at the disc root
(precede the RCF mounts in the boot sequence). The disc tree in the container
(`protodisc/`) contains only `args.txt`.

## Diagnostics added this session

- Watchdog: critical-section owner/waiter dump (`CsStateDump`), ATG device
  service flags (`[dev+0x2ABE]`), boot handshake state (display flag + queue
  ring head/tail/count), dormant main-thread registers, thread-id mapping at
  creation.
- `NtQueryDirectoryFile` + per-entry `DIR-ENTRY` logging (the game has not
  enumerated the disc root yet — it probes paths directly).
- `KeDelayExecutionThread` call tracing; `KeWaitForSingleObject` argument dump
  for adopted in-place events.
- Trace hooks: RCF mount chain (sub_8239EF58/8239D908/82A58CA8/82A58B80/
  82A56CC8/82A588C8/823CE4A8), ATG worker state machine (sub_827E7420/
  827E70A0), audio dispatch internals.

## Reproduction

```bash
source scripts/env.sh
bash build_guest_o0.sh        # regenerates only changed TUs
bash build_runtime.sh
PR_FS_ROOT=/home/z/my-project/protodisc \
  timeout 60 ./runtime_build/prototype_runtime default.xex
# Watch for: dispflag[82D844F8]=00000001, gpu stats draws=103,
#            CONTENT MISSING: '\Device\Cdrom0\skuinfo.p3d'
```

## Next objectives (in order)

1. **Content upload** (user action): push the LFS objects for the five RCFs
   and add `skuinfo.p3d` (+ `.rz`) to the disc root. The boot chain through
   RCF mounting (`sub_8239EF58` → `sub_82A58CA8(8, paths, 5)`) is fully mapped
   and trace-hooked, ready to verify the moment content exists.
2. Watch the skuinfo parse → RCF open sequence through the FS audit; confirm
   the RCF header/index parse (`sub_82A56CC8`/`sub_82A588C8` inner mounts).
3. Confirm the startup package load, `unlit` registration (the material
   registry at `[0x82DD9BD4]`), swap-queue advance (`cur != target`), and the
   first MMIO flip to `0x7FC86110`.
4. Audit other recovered switch tables for the same truncation defect — the
   Phase 2B scan heuristic (`switch_scan.cpp`) guarded counts and can miss
   entries (this one had 2/28). A full re-scan with the corrected pattern
   (inline tables after `bctr`, sign-extended `addi` bases) is warranted.
