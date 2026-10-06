# Phase 2C — Content Pipeline Investigation

## Status: BLOCKER IDENTIFIED — genuine content dependency (real game data required)

The 150+ second stable checkpoint (22 threads, Lua persist parsing, D3D/Xenos
init, 94 PM4 packets, 7 IBs, 6 DRAW_INDX_2, 60 Hz vblank) does not stall on a
runtime bug. It stalls on a **missing-content dependency**: the engine waits
for the `unlit` material, which is registered only when the startup package
(`art/startup_shaders.p3d` + `art/startup.p3d` + `art/startup_tod.p3d`, stored
inside the disc's RCF archives) is loaded. The container has no extracted game
data — the disc root holds only `args.txt` — so the boot state machine parks in
a legitimate retry loop. Everything below is evidence from real guest
execution plus static verification against the pristine image.

## The complete wait graph (all addresses verified live)

| Thread | Where it parks | Wait condition |
|--------|----------------|----------------|
| tid=7 (main, entry 82A52C00) | `sub_8226E260` wait loop @8226E2D4 (pumps task queue via `sub_827E1F70`→`sub_827E1E58`, sleeps 100 units via `0x822365B0`→`0x82A53240`) | global flag `[0x82D844F8] != 0` ("display system active") |
| tid=26 (display-init thread, real entry `sub_8226DD70`, ATG obj 82D84110, handle F800009C) | inside `sub_8226DA40` → 8280xxxx display-subsystem init chain | the `unlit` material lookup returning != -1 |
| tid=25 (render thread) | busy loop in `sub_82A9B4A8` body: hash (0x82A9ED80) → `[r30] <= 28` → branch back to 0x82A9B60C | boot-stage counter `[r30] > 28` |
| tid=27/28 (entry 82A883E8) | `KeWaitForSingleObject` (thunk 0x82BA7FA4) on in-place events `A1A05A00`/`A1A05A50` (= dev+0x2D00/+0x2D50 near D3D device A1A02D00) | present/swap events, signaled by the swap-queue processing in the vblank handler |
| tid=10..24 (job workers, entry 82236A90) | pool wait at 82A5E968 | pool semaphores F8000014/24/3C/58/78/8C |
| tid=8/9 (FEED0001/2) | CP + vsync host threads | GPU work (94 packets, 7 IBs, 60 Hz interrupts — healthy) |

### Wait-tracking instrumentation (new)

`GuestThread::wait_obj / wait_start_ms / wait_lr` are recorded by
`KeWaitForSingleObject`, `NtWaitForSingleObjectEx` and
`NtWaitForMultipleObjectsEx` (RAII `WaitScope`); the 5 s watchdog prints
`WAIT obj=... since=Ns at=<lr>` for every blocked thread. Deep dumps now
include one stack scan per distinct non-parked worker entry.

## The display-init dependency chain (root cause)

1. Main thread boot stage 3 (`sub_82239F58`) submits an async request
   (`sub_82267C10` → `sub_82267718(iothread obj A06E28F0, ...)`) and then
   waits in `sub_8226E260` for the display-system flag.
2. `sub_8226DD70` (thread entry) → `sub_8226DA40` = display-system
   initializer: reads config (defaults 640x480, mode switch on
   `[0x82BD8360+0xC0]`), builds the display subsystem, and **sets
   `[0x82D844F8] = r28 (=1)** + `[0x82D84220]=0`, `[0x82D841D0]=0` at
   0x8226DD40..0x8226DD5C, then pumps (`0x82230DE0`).
3. Inside the display subsystem init, `sub_827F98F0(obj, "unlit", 0x50)`
   computes the material UID from the string (`0x82AD20E0` — same family as
   the Lua `UIDFromHash`) and looks it up via `sub_827F9428` in the registry
   at `[0x82DD9BD4]` — **returns -1 (not found) on every retry**.
4. The registry is populated by the content loader when the startup package
   loads. XEX string evidence (pristine image):

   | Address | String |
   |---------|--------|
   | 0x8202FB9C | `game:\00cells.rcf` |
   | 0x8202FBB0 | `game:\01cells.rcf` |
   | 0x8202FBC4 | `game:\00art.rcf` |
   | 0x8202FBD4 | `game:\01art.rcf` |
   | 0x8202FBE4 | `game:\02art.rcf` |
   | 0x82033C90 | `art/startup_shaders.p3d;art/startup.p3d;art/startup_tod.p3d` |
   | 0x82033C88 | `startup` (package name) |
   | 0x82001014 | `art/frontendshaders.p3d` |
   | 0x820024B0 | `unlit` (the required material) |

5. Archive mounting: `sub_82282838` (get-or-create) → descriptor
   `sub_8232B8C8` writes `{path='game:\00art.rcf', type=1}` at 0x82DA6A98 →
   archive factory `0x827E7F80` → RCF constructor `0x827E7A20` registers into
   the global archive list at `[0x82DD9B20]` (lazy open; vtable 0x820CE120).
6. The startup package registration lives in `sub_823CE4A8` = the
   **"InGame Simulation Task"** module initializer (module table at
   0x82033B3C, name string follows at 0x82033B44).
7. No `NtCreateFile`/`NtOpenFile` for any RCF ever appears in the run logs —
   the content load path never fires because the display init it depends on
   cannot complete without the materials the content would provide.

## Independent confirmation

The Xenia canary reference log (`research/xenia_proto.log`, real disc image
mounted) shows the title reaching a continuous draw loop
(`PM4_DRAW_INDX_2` + "Resolve region is empty" repeated — the loading screen)
— i.e., past this boundary, with content. It also shows
`XamContentCreateEnumerator` serving `profile.bin` / `slot-A.bin` saves from
its content root; our XAM correctly returns "no content" for a fresh system.

## Filesystem layer hardening implemented this session

- `game:` and `D:` symbolic links are case-insensitive (`GAME:\…` resolves).
- Host path resolution walks components case-insensitively (XGD semantics)
  when the direct `stat` misses.
- `\Device\Cdrom0\` (trailing backslash = volume root) now opens fs_root as
  a directory instead of failing.
- `NtQueryFullAttributesFile` writes the Xenia-packed
  `FILE_NETWORK_OPEN_INFORMATION` layout (alloc @0x20, eof @0x28,
  attributes @0x30; 0x34 bytes) — the previous desktop-style field order was
  wrong.
- Deduplicated FS request audit (`op|path → ok/fail` counters) dumped by the
  watchdog every 20 s; first-missing-content paths report once via
  `LogLineOnce` ("CONTENT MISSING: …").
- Verified live: `open|\Device\Cdrom0\ ok=1`, `create|\Device\Cdrom0\args.txt
  ok=1`, 22 threads stable for 45 s.

## What is needed to proceed (the exact content set)

Place the real extracted Xbox 360 disc files into the disc root
(`PR_FS_ROOT`, currently `protodisc/`), preserving the original layout:

- `00cells.rcf`, `01cells.rcf`, `00art.rcf`, `01art.rcf`, `02art.rcf`
  (game:\ root) — RCF archives (Radical Content Format: 60-byte header,
  (hash, offset, length) entry triples, metadata with filenames — see
  `research/rcftools/rcf.h` for the verified format reference).
- The startup package inside the art RCFs: `art/startup_shaders.p3d`,
  `art/startup.p3d`, `art/startup_tod.p3d` (plus `art/frontendshaders.p3d`
  for the frontend).

No transformation is required: the game's own RCF reader consumes the files
through `NtCreateFile`/`NtReadFile`, which the runtime serves directly from
the disc root. A deterministic manifest tool
(`scripts/content_manifest.py`) inventories a supplied tree (path, size,
extension, CRC32) so the content set can be validated before a run.

## Next objectives once content exists

1. Re-run with `PR_FS_ROOT` pointed at the real disc tree; confirm
   `game:\00art.rcf` opens and the RCF reader parses the header/index
   (watch the FS audit + CONTENT MISSING report).
2. Confirm the startup package loads, the `unlit` material lookup returns a
   real index, and `[0x82D844F8]` flips to 1 (display-init completes).
3. Watch the main-thread boot state machine advance (render thread's
   `[r30]` stage > 28) and the swap-queue processing start
   (`[dev+0x413C] != [dev+0x4140]`, processed counter increments, first
   MMIO flip to 0x7FC86110).
4. With real draws flowing, capture per-draw GPU state (topology, shader
   IDs, constants, bindings) for the Vulkan-side translation work.
