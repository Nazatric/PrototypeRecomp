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
    default:
        // Stack arguments: PPC passes args 9+ (index 8+) on the caller's
        // stack at r1 + 0x54 + (arg_index - 8) * 8 (empirically established
        // for this title — see the Phase 2B stack-args ABI analysis; the
        // slots are 8-byte spaced). r11/r12 are VOLATILE and never carry
        // arguments.
        if (i >= 8 && i < 24) {
            uint32_t sp = ctx.r1.u32;
            uint32_t slot = sp + 0x54 + (uint32_t)(i - 8) * 8;
            // Any mapped guest address is valid: stacks live at 0x7000xxxx,
            // the image at 0x82xxxxxx, heap/physical at 0xA0xxxxxx+.
            if (slot >= 0x10000 && slot < 0xC0000000)
                return LoadU32(slot);
        }
        return 0;
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
