// PrototypeRecomp Phase 2B runtime — graphics (Vd*) imports. The Xenos
// command processor itself is Phase 2C; here we provide the guest-visible
// state the game needs to build its ring and command buffers.
#include "state.h"

using namespace pr;


#include "args.h"
#define IMPORT(name) \
    void __imp__##name(PPCContext& ctx, uint8_t* base)
#define RET(v) do { ctx.r3.u64 = (uint64_t)(uint32_t)(v); return; } while (0)

// X_VIDEO_MODE (XGetVideoMode result): Xenia returns a 0x30-ish struct.
static void WriteVideoMode(uint32_t out) {
    // { u32 width, u32 height, u32 is_widescreen, u32 is_hdtv?... }
    // Xenia XGetVideoMode: 1280x720 32bpp 60Hz.
    StoreU32(out + 0x00, 1280);
    StoreU32(out + 0x04, 720);
    StoreU32(out + 0x08, 1);     // interlaced = 0? fields approximated
    StoreU32(out + 0x0C, 0);     // safe region
    StoreU32(out + 0x10, 1);     // widescreen
    StoreU32(out + 0x14, 0x200); // format: 128bpp?
    GuestMemset(out, 0, 0x30);
    StoreU32(out + 0x00, 1280);
    StoreU32(out + 0x04, 720);
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

IMPORT(VdGetGraphicsAsicID) { RET(0x10); }  // > 0x10 -> EDRAM path

IMPORT(VdInitializeRingBuffer) {
    // (physical_ptr, size_log2)
    uint32_t ptr = ARG(0);
    uint32_t size_log2 = ARG(1);
    K().vd_ring_buffer_ptr = ptr;
    K().vd_ring_buffer_size_log2 = size_log2;
    PRLOG(Gpu, "VdInitializeRingBuffer(ptr=%08X size=2^%u)", ptr, size_log2);
}

IMPORT(VdEnableRingBufferRPtrWriteBack) {
    uint32_t ptr = ARG(0);
    uint32_t block_log2 = ARG(1);
    K().vd_rptr_writeback_ptr = ptr;
    K().vd_rptr_writeback_block_log2 = block_log2;
    // Initialize read pointer to 0 (guest-visible GPU progress).
    StoreU32(ptr, 0);
    PRLOG(Gpu, "VdEnableRingBufferRPtrWriteBack(ptr=%08X block=2^%u)", ptr,
          block_log2);
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
    uint32_t a = ARG(0);
    PRLOG(Gpu, "VdRetrainEDRAM(%08X)", a);
    RET(1);  // success
}

IMPORT(VdRetrainEDRAMWorker) { RET(1); }

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
    // (ptr?) returns current display info struct
    uint32_t out = ARG(0);
    if (out) {
        WriteVideoMode(out);
        StoreU32(out + 0x18, 0);  // gamma?
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
    // Xenia: calls registered interrupt callback if enabled.
    if (K().vd_interrupt_callback) {
        PRLOG(Gpu, "VdCallGraphicsNotificationRoutines -> guest cb %08X",
              K().vd_interrupt_callback);
        // Execute the guest callback on this thread (authentic semantics).
        GuestThread* t = GuestThread::GetCurrent();
        if (t) {
            uint64_t args[2] = {K().vd_interrupt_callback_arg, 0};
            // Reuse current context: callback(arg).
            PPCFunc* fn = LookupGuestFunc(K().vd_interrupt_callback);
            if (fn) {
                uint32_t saved_r3 = ctx.r3.u32;
                ctx.r3.u64 = K().vd_interrupt_callback_arg;
                // Callback stack: reuse current r1 with padding.
                ctx.r1.u32 -= 256;
                StoreU32(ctx.r1.u32, ctx.r1.u32 + 256);
                try {
                    fn(ctx, base);
                } catch (const GuestUnwind&) {
                }
                ctx.r1.u32 += 256;
                ctx.r3.u64 = saved_r3;
            }
        }
    }
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
    uint32_t src_xy = ARG(0);
    uint32_t src_wh = ARG(1);
    uint32_t buffer = ARG(2);
    RET(1);
}


