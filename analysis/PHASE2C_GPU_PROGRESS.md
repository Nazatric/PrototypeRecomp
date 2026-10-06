# Phase 2C — GPU Progress (draw-state capture)

## Current state

The Xenos command processor is driven exclusively by the game's recompiled
D3D/Xenos submission path. Latest 40 s validation run (content absent, disc
root = args.txt only):

| Metric | Value |
|--------|-------|
| PM4 packets parsed | 94 |
| Indirect buffers executed | 7 (incl. nested depth-2) |
| DRAW_INDX_2 executed | 24 (periodic resubmission of the boot draw) |
| IM_LOAD_IMMEDIATE (shader loads) | 2 (VS 24 dwords, PS 9 dwords) |
| Vblank interrupts | 1497 (60 Hz) |
| Crashes | 0 |

## Draw-state capture (this session)

Every draw now dumps the provenance of its state: a 96-entry ring of the
last register writes (recorded in `GpuSetReg`, which the TYPE0/TYPE1 packet
paths route through) plus the key Xenos draw registers. Observed register
context for the title's boot draws:

```
r01C0=A64A0000  CP_RB_BASE            r01C2=01A08BBC  CP_RB_RPTR_ADDR (wb)
r1951=00000001  INT_STATUS            r01C5=00000019  CP_RB_WPTR (doorbell)
r0A2F=00B00000  COHER_SIZE_HOST       r0A30=059A0000  COHER_BASE_HOST
r0A02=C0100000 r0A03=07F00000 r0A04=C0000000 r0A05=00100000   PA_SC_* block
r0D02=00010800 r0D01=04000000 r0D00=00040401(x10)            SQ_VTX/SQ block
r2180=1000000E r2181=00000000 r2182=00010081                VGT block
r2100=0000FFFF r2102=00000000 r2204/2205=00010000 r2206=00000300
r2280=00080008 r2302=00000004 r2312=0000FFFF                 VGT/SQ fetch
r0E42=00001F60 r0C85=00000003 r0F01=0000200E                RB_* controls
r01DC/01DD scratch writeback pair, r05C8=00020000 event initiator
```

VGT_DRAW_INITIATOR (r2182 = 0x00010081) is written immediately before each
draw packet. `DRAW_INDX_2` as sent by this title: **count=1**, single payload
dword = the initiator value (src_sel=1 IMMEDIATE, major_mode=0,
instances=1). num_indices is not carried in the packet; it will be pinned
from VGT state once volume draws flow (the Xenia reference log shows the
disassembly form `PM4_DRAW_INDX_2(3, 8, 2)` for this title with a real disc
mounted).

## Shader loads (real inline microcode)

`PM4_IM_LOAD_IMMEDIATE` packets carry the actual shader program inline:

```
#1: type=0 (vertex) count=0x18 (24 dwords) — first words 00001003 ...
#2: type=1 (pixel)  count=0x09 (9 dwords)
```

Layout: `{shader_type (0=VS, 1=PS), dword_count, microcode...}`. The raw
payload logging retains the first words as decode evidence for the future
Xenos-instruction → host-IR translation stage.

## Known boot-boundary (see PHASE2C_CONTENT_PIPELINE.md)

The engine parks waiting for the `unlit` material from the startup package
(`art/startup_shaders.p3d` inside `game:\00art.rcf`); the boot draws are the
periodic resubmitted fallback. Real content is required to advance to
volume rendering, at which point the draw-state capture will exercise
fully (topologies, constants, textures, render targets).
