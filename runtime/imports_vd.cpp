// PrototypeRecomp Phase 2B runtime — graphics (Vd*) imports. The Xenos
// command processor itself is Phase 2C; here we provide the guest-visible
// state the game needs to build its ring and command buffers.
#include "state.h"

using namespace pr;


#include "args.h"
#define IMPORT(name) \
    void __imp__##name(PPCContext& ctx, uint8_t* base)
#define RET(v) do { ctx.r3.u64 = (uint64_t)(uint32_t)(v); return; } while (0)

// X_VIDEO_MODE (Xenia xbox.h, 48 bytes):
//   +0x00 display_width, +0x04 display_height, +0x08 is_interlaced,
//   +0x0C is_widescreen, +0x10 is_hi_def, +0x14 refresh_rate (float),
//   +0x18 video_standard, +0x1C unknown_0x8a, +0x20 unknown_0x01,
//   +0x24 reserved[3].
static void WriteVideoMode(uint32_t out) {
    GuestMemset(out, 0, 48);
    StoreU32(out + 0x00, 1280);
    StoreU32(out + 0x04, 720);
    StoreU32(out + 0x08, 0);      // progressive
    StoreU32(out + 0x0C, 1);      // widescreen
    StoreU32(out + 0x10, 1);      // hi-def
    uint32_t rr; float f = 60.0f;
    memcpy(&rr, &f, 4);
    StoreU32(out + 0x14, rr);     // refresh rate 60Hz (BE raw bits)
    StoreU32(out + 0x18, 1);      // NTSC
    StoreU32(out + 0x1C, 0x4A);
    StoreU32(out + 0x20, 1);
}

IMPORT(VdInitializeEngines) {
    // (0x4F810000, callback, arg, pfp_ptr, me_ptr)
    uint32_t param = ARG(0);
    uint32_t callback = ARG(1);
    uint32_t arg = ARG(2);
    uint32_t pfp_ptr = ARG(3);
    uint32_t me_ptr = ARG(4);
    if (pfp_ptr) StoreU32(pfp_ptr, 0);
    if (me_ptr) StoreU32(me_ptr, 0);
    PRLOG(Gpu, "VdInitializeEngines(%08X cb=%08X arg=%08X)", param, callback,
          arg);
    RET(1);  // Xenia stub returns 1
}

IMPORT(VdShutdownEngines) { }

IMPORT(VdGetGraphicsAsicID) { RET(0x11); }  // Xenia: games compare < 0x10 -> EDRAM init path

IMPORT(VdInitializeRingBuffer) {
    // (physical_ptr, size_log2) — Xenia: primary_buffer_size = 1<<(log2+3).
    uint32_t ptr = ARG(0);
    uint32_t size_log2 = ARG(1);
    K().vd_ring_buffer_ptr = ptr;
    K().vd_ring_buffer_size_log2 = size_log2;
    XenosSetRingRegs(ptr, K().vd_rptr_writeback_ptr);
    PRLOG(Gpu, "VdInitializeRingBuffer(ptr=%08X size=2^%u -> %u bytes)", ptr,
          size_log2, 1u << (size_log2 + 3));
}

IMPORT(VdEnableRingBufferRPtrWriteBack) {
    uint32_t ptr = ARG(0);
    uint32_t block_log2 = ARG(1);
    K().vd_rptr_writeback_ptr = ptr;
    K().vd_rptr_writeback_block_log2 = block_log2;
    // Initialize read pointer to 0 (guest-visible GPU progress) through
    // BOTH the raw physical address (diagnostics) and the virtual window
    // alias the game actually polls (PA | 0xA0000000).
    StoreU32(ptr, 0);
    if (ptr < 0x20000000u) StoreU32(ptr | 0xA0000000u, 0);
    XenosSetRingRegs(K().vd_ring_buffer_ptr, ptr);
    PRLOG(Gpu, "VdEnableRingBufferRPtrWriteBack(ptr=%08X block=2^%u "
               "guest-VA-alias=%08X)", ptr, block_log2,
          (ptr < 0x20000000u) ? (ptr | 0xA0000000u) : ptr);
}

IMPORT(VdGetSystemCommandBuffer) {
    uint32_t p0 = ARG(0);
    uint32_t p1 = ARG(1);
    if (p0) GuestMemset(p0, 0, 0x94);
    if (p0) StoreU32(p0, 0xBEEF0000);
    if (p1) StoreU32(p1, 0xBEEF0001);
}

IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { }

IMPORT(VdEnableDisableClockGating) { }

IMPORT(VdRetrainEDRAM) {
    // Xenia returns 0 (stub). Returning non-zero sends the game down a
    // device-reset path that spins on dev+11012 (observed deadlock).
    uint32_t a = ARG(0);
    PRLOG(Gpu, "VdRetrainEDRAM(%08X) -> 0", a);
    RET(0);
}

IMPORT(VdRetrainEDRAMWorker) { RET(0); }  // Xenia: 0

IMPORT(VdIsHSIOTrainingSucceeded) { RET(1); }

IMPORT(VdQueryVideoMode) {
    uint32_t out = ARG(0);
    WriteVideoMode(out);
}

IMPORT(VdQueryVideoFlags) { RET(1); }  // 720p flag

IMPORT(XGetVideoMode) {
    // r3 = out ptr
    uint32_t out = ARG(0);
    WriteVideoMode(out);
    RET(0);
}

IMPORT(VdGetCurrentDisplayInformation) {
    // (ptr) writes current display info struct (video mode + gamma)
    uint32_t out = ARG(0);
    if (out) {
        WriteVideoMode(out);
        StoreU32(out + 0x18, 1);  // NTSC
    }
    RET(0);
}

IMPORT(VdGetCurrentDisplayGamma) {
    uint32_t out_ptr = ARG(0);   // float* gamma? r3 = ptr
    if (out_ptr) {
        // Xenia writes 2.2f as f32 BE.
        uint32_t f; float g = 2.2f;
        memcpy(&f, &g, 4);
        StoreU32(out_ptr, f);
    }
    RET(0);
}

IMPORT(VdSetDisplayMode) {
    uint32_t width = ARG(0);
    uint32_t height = ARG(1);
    PRLOG(Gpu, "VdSetDisplayMode(%ux%u)", width, height);
    RET(0);
}

IMPORT(VdPersistDisplay) { RET(0); }

IMPORT(VdSetGraphicsInterruptCallback) {
    uint32_t callback = ARG(0);
    uint32_t arg = ARG(1);
    K().vd_interrupt_callback = callback;
    K().vd_interrupt_callback_arg = arg;
    PRLOG(Gpu, "VdSetGraphicsInterruptCallback(cb=%08X arg=%08X)", callback,
          arg);
}

IMPORT(VdCallGraphicsNotificationRoutines) {
    // (source(=1 observed), args_ptr) — the game's D3D asks the kernel to
    // run the registered graphics notification callbacks (observed call
    // site sub_82A80750: r3=1, r4=&BufferScaling after swap submission).
    // Xenia stubs this (its CP generates interrupts itself); the authentic
    // semantic is a notification dispatch to the registered interrupt
    // callback. Dispatch with source=0 (the game's "poll + process"
    // interrupt path, Xenia MarkVblank-compatible).
    uint32_t source_arg = ARG(0);
    if (K().vd_interrupt_callback) {
        PRLOG(Gpu, "VdCallGraphicsNotificationRoutines(%u) -> guest cb %08X",
              source_arg, K().vd_interrupt_callback);
        GuestThread* t = GuestThread::GetCurrent();
        if (t) {
            uint64_t args[2] = {0, K().vd_interrupt_callback_arg};
            PPCFunc* fn = LookupGuestFunc(K().vd_interrupt_callback);
            if (fn) {
                uint32_t saved_r3 = ctx.r3.u32;
                uint32_t saved_r4 = ctx.r4.u32;
                uint64_t saved_r1 = ctx.r1.u64;
                ctx.r3.u64 = 0;                              // source
                ctx.r4.u64 = K().vd_interrupt_callback_arg;  // user_data
                ctx.r1.u32 -= 256;
                StoreU32(ctx.r1.u32, ctx.r1.u32 + 256);
                try {
                    fn(ctx, base);
                } catch (const GuestUnwind&) {
                }
                ctx.r1.u32 += 256;
                ctx.r3.u64 = saved_r3;
                ctx.r4.u64 = saved_r4;
                ctx.r1.u64 = saved_r1;
            }
        }
    }
    RET(0);
}

IMPORT(VdSwap) {
    // (buffer_ptr, fetch_ptr, wb_ptr, cmd_buf, cmd_buf2, frontbuffer_ptr,
    //  format_ptr, colorspace_ptr, width_ptr, height_ptr)
    K().vd_swap_count++;
    if (K().vd_swap_count <= 3 || (K().vd_swap_count % 60) == 0) {
        PRLOG(Gpu, "VdSwap #%llu (front=%08X)", (unsigned long long)K().vd_swap_count,
              ARG(5) ? LoadU32(ARG(5)) : 0);
    }
    // Phase 2C boundary: no Xenos command processor yet. The swap completes
    // from the game's perspective (writeback advanced in VdSwap path by
    // game-driven ring processing is NOT faked: rptr stays where it is
    // unless the game processes its own ring).
    // Update the ring-buffer read pointer writeback to match the submitted
    // packet so the game's ring logic can proceed — this mirrors what the
    // real GPU does with the system command buffer submitted in VdSwap:
    // the frontbuffer swap packet is a system-buffer packet, not part of
    // the game's primary ring, so no rptr update is performed.
    RET(0);
}

IMPORT(VdInitializeScalerCommandBuffer) {
    // (scaler_source_xy, scaler_source_wh, scaled_output_xy, scaled_output_wh,
    //  front_buffer_wh, v_filter_type, v_filter_params, h_filter_type,
    //  h_filter_params, unk9, dest_ptr, dest_count)
    // Xenia: fill the destination with PM4 NOP words (0x80000000) and return
    // dest_count — the real scaler programming is display-only. Args 11/12
    // (0-based 10/11) sit in stack slots r1+0x54+8*2 / +8*3.
    uint32_t dest_ptr = StackArg(ctx, 2);
    uint32_t dest_count = StackArg(ctx, 3);
    if (dest_ptr >= 0x80000000 && dest_ptr < 0xC0000000 && dest_count &&
        dest_count < 0x10000) {
        for (uint32_t i = 0; i < dest_count; i++) {
            StoreU32(dest_ptr + i * 4, 0x80000000u);
        }
        PRLOG(Gpu, "VdInitializeScalerCommandBuffer -> %u NOPs at %08X",
              dest_count, dest_ptr);
        RET(dest_count);
    }
    RET(0);
}


