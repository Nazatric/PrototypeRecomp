// PrototypeRecomp Phase 2B runtime — guest argument access.
// PPCContext does NOT store GPRs contiguously (r3, r0, r1, r2, r4, ...), so
// argument registers must be selected by name.
#pragma once

#include "guest.h"

namespace pr {

inline uint32_t ArgU32(PPCContext& ctx, int i) {
    switch (i) {
    case 0: return ctx.r3.u32;
    case 1: return ctx.r4.u32;
    case 2: return ctx.r5.u32;
    case 3: return ctx.r6.u32;
    case 4: return ctx.r7.u32;
    case 5: return ctx.r8.u32;
    case 6: return ctx.r9.u32;
    case 7: return ctx.r10.u32;
    case 8: return ctx.r11.u32;
    case 9: return ctx.r12.u32;
    default: return 0;
    }
}

inline uint64_t& ArgReg(PPCContext& ctx, int i) {
    // Only valid for 0..7 (r3..r10); returns reference for writing.
    switch (i) {
    case 0: return ctx.r3.u64;
    case 1: return ctx.r4.u64;
    case 2: return ctx.r5.u64;
    case 3: return ctx.r6.u64;
    case 4: return ctx.r7.u64;
    case 5: return ctx.r8.u64;
    case 6: return ctx.r9.u64;
    case 7: return ctx.r10.u64;
    default: return ctx.r3.u64;  // unreachable in practice
    }
}

}  // namespace pr

#define ARG(i) ::pr::ArgU32(ctx, (i))
