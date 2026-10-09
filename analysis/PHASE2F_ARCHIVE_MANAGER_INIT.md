# Phase 2F — Archive-Manager Initialization: Root Cause & Fix

Continuing from Phase 2E (remote main `424e39b`). This phase solved the
documented blocker chain:

```
[0x82DCF578+0xB4]==0 → sub_82A58B80 registration fails →
TOC parse never runs → archive hash tables empty →
fonts_latin.gfx lookups NULL → sub_828CAE98 crash
```

## Root cause — five kernel-semantics bugs (all Xenia-verified)

The cement library ("ATG CORE CEMENT LIBRARY" — the archive system) is
initialized by the game's own code, but only after a chain of kernel
interactions that our runtime implemented incorrectly. Each fix is
referenced against Xenia's implementation.

### 1. XexCheckExecutablePrivilege

The ATG cementer's 'ATEM' manifest handler (`sub_82A5AA28`, called from the
license-gated mount `sub_8239D908`) checks privileges 11 and 23 and requires
BOTH to return 0. Our stub returned unconditional 1, failing both checks
silently — the handler bailed before initializing the cement library.

Xenia (`xboxkrnl_modules.cc`): the privilege is a BIT POSITION in the
executable's `XEX_HEADER_SYSTEM_FLAGS` (key 0x00030000); the return is
`(flags >> privilege) & 1`. This title's system flags are **0x600**
(bits 9+10) — privileges 11/23 correctly return 0.

### 2. The raw HDD namespace

`sub_82A66718` (ATG thread creation) opens `\Device\Harddisk0\partition0`,
reads a 1024-byte owner block, checks a 'Josh' (0x4A6F7368) magic
signature, writes its claim, then mounts the cache volumes. Our FS mapped
the path under the read-only disc root → NO_SUCH_FILE →
RtlNtStatusToDosError = 2 → thread creation failed.

Xenia (`emulator.cc`): Partition0/Cache0/Cache1 are NullDevice territory —
"Cache/STFC code baked into games tries reading/writing to these." The
whole namespace now behaves as the null HDD: opens succeed, raw volume
**reads succeed without touching the buffer** (Xenia NullFile::ReadSync —
critical detail: XMountUtilityDrive's read-back checks only terminate
because the game's own written buffer contents remain visible), writes
succeed (discarded).

`partition0` additionally gets a REAL persistent backing file
(`<hdd_root>/partition0.img`, 2 MiB) — on hardware the SYSTEM partition
persists across boots (the ATG owner-block database lives there).

### 3. XMountUtilityDrive's ioctls + stack arguments

The cache mount queries `NtDeviceIoControlFile` with:
- `0x70000` (drive geometry): out `{cache_size/512, 512}`
- `0x74004` (partition info): out `{0, cache_size}` (Xenia: 0xFF000)

Our stub never wrote output buffers, AND `ARG(8)/ARG(9)` read r11/r12 —
volatile scratch registers — instead of the stack-argument area
`[caller r1 + 0x54 + (i-8)*8]` (the project's documented stack-args ABI;
the range guard also wrongly excluded guest stack addresses 0x7000xxxx).

### 4. FileFsSizeInformation

`sub_82A65F50` (cache verification) requires
`sectors_per_alloc_unit × bytes_per_sector == 0x8000` (the requested
cluster size). Implemented class 3 of NtQueryVolumeInformationFile:
`{31 units, 31 avail, 64 sectors/unit, 512 bps}`.

### 5. XamTaskSchedule

Xenia (`xam_task.cc`): schedules an asynchronous XThread whose entry is the
callback and whose argument is the XTASK_MESSAGE. The ATG framework drives
ALL worker threads through this export — including the cement-library
thread (entry `0x82A5A558`). Our stub returned success without creating
anything. Now spawns a real GuestThread (0x40000 stack). `IoCreateDevice`
also allocates Xenia's `[dev+0x18]` scratch block that XMountUtilityDrive
writes through.

## Validated results (45 s runs, real disc content)

```
XexCheckExecutablePrivilege(10)=1 (11)=0 (23)=0            [flags 0x600]
XMountUtilityDrive formats Cache0+Cache1                    [1M 4KB writes]
sub_82A66718 (ATG thread creation) = SUCCESS
XamTaskSchedule → task threads tid=35/37 (sub_82A664D8)
sub_82A5A920 (cement library initializer) RUNS              [lr=82A5AE3C]
cement global 0x82DCF578 fully populates                    [+B4 = 1]
sub_82A588C8 registers the 5 static-string RCFs
cement thread tid=39 (entry 0x82A5A558) runs
ALL 8 RCF archives re-open + parse header/TOC               [via the game]
```

The registration gate `[0x82DCF578+0xB4]` is now 1; `sub_82A58B80`
proceeds through `sub_82A588C8` for every archive; the game's own cementer
reads all TOCs (00art.rcf: 2048+18432 bytes = header + 1361 entries).

## Remaining blocker — the frontend image-loader registry

`fonts_latin.gfx` still resolves NULL through the frontend registry
(`sub_828D2D60` → `sub_828DF140` → `sub_828DE318`):

- The registry's hash-table root (`[reg+0x20]+0x18`) is 0 at the first
  lookup; the find (`sub_828D2720` → `sub_828D1730`) returns -1 instantly.
- The load-on-miss path (`sub_828E6D88` → request factory `sub_828E6BA0`
  with the FULL path 'art\hud\fonts_latin.gfx' → the static archive
  manager at 0x82D35D68 → `sub_8296F4C0` → the handler record at
  0x82D343D8) creates the request but the entry never materializes; the
  table root only allocates during the miss processing.
- The loose-file fallback `D:\art\hud\fonts_latin.gfx` correctly reports
  CONTENT MISSING (the file lives inside 00art.rcf).
- The fatal NULL dereference is `sub_828CAE98+386` from the display-init
  thread (tid 26), whose caller passes an empty path — upstream string
  construction to trace next.

Next steps: trace the image-request pump (which worker drains the
0x82D35D68 manager queue), and how the loaded entry inserts into the
registry hash table (the insert `sub_828D2558` is vtable slot +8 of the
`[reg+0x20]` object; it is reached via vtable dispatch and therefore
bypasses strong-symbol trace hooks).

## Reproduction

```bash
export PATH=/home/z/opt/bin:$PATH   # clang-19 + git-lfs
cd scripts/PrototypeRecomp
source ../../scripts/env.sh
bash build_runtime.sh
PR_FS_ROOT=/home/z/my-project/protodisc PR_LOG_TRACE=1 PR_TRACE_HOOKS=1 \
  timeout 45 ./runtime_build/prototype_runtime default.xex
# Milestones: XamTaskSchedule threads, sub_82A5A920 runs, +B4=1,
#             8 RCFs read header+TOC; crash at ~3 s in sub_828CAE98.
```
