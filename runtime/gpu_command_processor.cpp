// PrototypeRecomp Phase 2C — Xenos Command Processor + MMIO register model.
//
// This is the real GPU front-end for the recompiled Prototype title. It is
// driven exclusively by the commands the game's own recompiled D3D runtime
// submits:
//
//   * The game builds its primary ring buffer in the physical pool
//     (MmAllocatePhysicalMemory window: guest VA 0xA0000000+ with
//     PA = VA & 0x1FFFFFFF; Xenia-verified layout).
//   * The pusher thread copies PM4 packets into the ring, updates
//     dev+10956 (write cursor) and rings the doorbell: an MMIO write of
//     the new write index to CP_RB_WPTR at guest 0x7FC80714
//     (sub_82A79E28/sub_82A79FAC, observed live).
//   * This CP parses every packet (type 0/1/2/3, Xenia opcode semantics),
//     recurses into PM4_INDIRECT_BUFFER chains, evaluates
//     PM4_WAIT_REG_MEM against real register/memory state (blocking until
//     the CPU-side condition becomes true — never faked), performs the
//     memory side effects of PM4_EVENT_WRITE_* / PM4_MEM_WRITE / etc.,
//     and advances the read pointer ONLY after processing.
//   * The read pointer is written back through the generic physical->VA
//     translation (writeback PA 0x019AFDBC lands at guest VA 0xA19AFDBC =
//     the game's primary-buffer control block + 0x3C, which the game's
//     own DPC and kick path poll — sub_82A797A0 reads mirror+60 directly).
//   * Graphics interrupts are dispatched through the AUTHENTIC guest path:
//     the callback registered with VdSetGraphicsInterruptCallback is
//     invoked as callback(source, user_data) (r3=source, r4=user_data —
//     Xenia GraphicsSystem::DispatchInterruptCallback ABI). source=1 for
//     command-stream interrupts (Xenia: PM4_INTERRUPT / ring completion),
//     source=0 for vblank (Xenia: MarkVblank, 60Hz).
//
// Register model (guest-mem-backed so unmarked plain reads also observe
// values; MM-marked accesses from the generated TUs route through the
// PR_Mmio* hooks below):
//   0x7FC80000 page (Xenia graphics_system.cc):
//     +0x070C CP_RB_BASE          +0x0714 CP_RB_WPTR (doorbell)
//     +0x0718 CP_RB_RPTR_ADDR     +0x6544 interrupt status (bit0 vblank)
//     +0x6110 D1GRPH_PRIMARY_SURFACE_ADDRESS (vblank flip target)
//     +0x0F00/+0x0F01/+0x194C/+0x1961 Xenia fixed read values
//   Register-file indices (Xenia register_table.inc):
//     0x0A2F COHER_SIZE_HOST  0x0A30 COHER_BASE_HOST
//     0x0A31 COHER_STATUS_HOST (flush trigger/status; MakeCoherent model)
#include "state.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdarg>

namespace pr {

// Defined in imports_core.cpp at global scope.
pr::GuestEvent* LookupInPlaceEvent(uint32_t addr);

namespace {

// ============================================================== address model
// Generic Xbox 360 GPU/CPU address translation used by this runtime:
//   * Guest physical memory pool lives at VA 0xA0000000..0xBFFFFFFF
//     (512 MiB); physical address = VA & 0x1FFFFFFF.
//   * GPU (Xenos) addresses ARE physical addresses (0..0x1FFFFFFF).
//   * Therefore GPU PA -> guest VA = PA | 0xA0000000, and
//     guest VA -> GPU PA = VA & 0x1FFFFFFF (matches the game's own
//     computation in sub_82A7B148: clrlwi r11,r11,3 == & 0x1FFFFFFF).
constexpr uint32_t kPhysicalWindowBase = 0xA0000000u;   // guest VA window
constexpr uint32_t kPhysicalWindowMask = 0x1FFFFFFFu;   // PA bits

inline uint32_t GpuPaToGuestVa(uint32_t pa) {
    return (pa & kPhysicalWindowMask) | kPhysicalWindowBase;
}
inline uint32_t GuestVaToGpuPa(uint32_t va) {
    return va & kPhysicalWindowMask;
}

// Xenos register pages (guest absolute).
constexpr uint32_t kRegPage0x7FC8 = 0x7FC80000u;
constexpr uint32_t kRegPage0x7FEA = 0x7FEA0000u;

inline bool IsRegAddr0x7FC8(uint32_t a) {
    return a >= kRegPage0x7FC8 && a < kRegPage0x7FC8 + 0x10000;
}
inline bool IsRegAddr0x7FEA(uint32_t a) {
    return a >= kRegPage0x7FEA && a < kRegPage0x7FEA + 0x2000;
}
inline uint32_t RegIndex0x7FC8(uint32_t a) {  // dword index, Xenia style
    return (a & 0xFFFF) >> 2;
}

// Register indices (Xenia register_table.inc / graphics_system.cc).
constexpr uint32_t kRegCP_RB_BASE         = 0x01C0;  // +0x700
constexpr uint32_t kRegCP_RB_CNTL         = 0x01C1;  // +0x704
constexpr uint32_t kRegCP_RB_RPTR_ADDR    = 0x01C2;  // +0x708 (0x70C per Xenia comment)
constexpr uint32_t kRegCP_RB_WPTR         = 0x01C5;  // +0x714 DOORBELL
constexpr uint32_t kRegRB_EDRAM_TIMING    = 0x0F00;
constexpr uint32_t kRegRB_BC_CONTROL      = 0x0F01;
constexpr uint32_t kRegCOHER_SIZE_HOST    = 0x0A2F;
constexpr uint32_t kRegCOHER_BASE_HOST    = 0x0A30;
constexpr uint32_t kRegCOHER_STATUS_HOST    = 0x0A31;
constexpr uint32_t kRegVGT_EVENT_INITIATOR= 0x0906;  // event writeback mirror
constexpr uint32_t kRegSCRATCH_UMSK    = 0x01DC;  // scratch writeback mask
constexpr uint32_t kRegSCRATCH_ADDR    = 0x01DD;  // scratch writeback address
constexpr uint32_t kRegSCRATCH_REG0    = 0x0578;  // interrupt sync
constexpr uint32_t kRegSCRATCH_REG7    = 0x057F;
constexpr uint32_t kRegD1MODE_V_COUNTER   = 0x194C;
constexpr uint32_t kRegINT_STATUS         = 0x1951;  // +0x6544
constexpr uint32_t kRegD1MODE_VIEWPORT    = 0x1961;
constexpr uint32_t kRegD1GRPH_PRIMARY     = 0x1844;  // +0x6110 flip register
constexpr uint32_t kRegVBL_COUNTER        = 0x1950;  // vblank counter (frame)

// ==================================================================== state
struct XenosGpu {
    std::atomic<uint32_t> doorbell_wptr{0};       // last CP_RB_WPTR value
    std::atomic<bool>     doorbell_written{false};
    std::atomic<uint64_t> frame_counter{0};       // Xenia counter_ (vblank)
    std::atomic<uint64_t> packets_parsed{0};
    std::atomic<uint64_t> interrupts_fired{0};
    std::atomic<uint64_t> interrupts_dropped{0};   // pre-ISR-registration
    std::atomic<uint64_t> ibs_executed{0};
    std::atomic<uint64_t> waits_blocked{0};
    std::atomic<uint64_t> draws_seen{0};
    std::atomic<uint64_t> im_loads_seen{0};

    std::mutex interrupt_mtx;                     // serialize guest ISR runs

    // -------- draw-state provenance: last register writes before a draw.
    // Recorded by GpuSetReg so a DRAW dump shows exactly the state the
    // game's command stream referenced (no speculative decoding).
    struct RegWrite { uint16_t idx; uint32_t value; };
    RegWrite reg_writes[96] = {};
    uint32_t reg_write_pos = 0;
    uint64_t reg_write_seq = 0;
    void RecordRegWrite(uint32_t idx, uint32_t value) {
        if (idx >= 0x4000) return;
        RegWrite& w = reg_writes[reg_write_pos % 96];
        w.idx = (uint16_t)idx;
        w.value = value;
        reg_write_pos++;
        if (reg_write_pos >= 96) reg_write_pos = 0;   // ring
    }

    // Register storage mirrors guest memory at 0x7FC80000..+0x10000 so that
    // plain (non-MM) guest reads observe the same values.
    volatile uint32_t* Regs() {
        return reinterpret_cast<volatile uint32_t*>(g_guest_base + kRegPage0x7FC8);
    }

    uint32_t GetReg(uint32_t idx) {
        if (idx >= 0x4000) return 0;
        // BE swap-free: values are stored via StoreU32 (BE), so use LoadU32.
        return LoadU32(kRegPage0x7FC8 + idx * 4);
    }
    void SetReg(uint32_t idx, uint32_t v) {
        if (idx < 0x4000) StoreU32(kRegPage0x7FC8 + idx * 4, v);
    }
};
XenosGpu g_gpu;

bool g_cp_running = false;

// Free-standing register accessors (used by the CP and packet handlers).
uint32_t GpuGetReg(uint32_t idx) { return g_gpu.GetReg(idx); }
void GpuSetReg(uint32_t idx, uint32_t v) {
    g_gpu.SetReg(idx, v);
    g_gpu.RecordRegWrite(idx, v);
}

// Dump the draw-state provenance: the last register writes recorded before
// this draw (ring order, oldest first) plus key Xenos draw registers.
static void DumpDrawState(const char* trigger) {
    PRLOG(Gpu, "  draw state @ %s: recent register writes (oldest->newest):",
          trigger);
    uint32_t n = g_gpu.reg_write_pos < 96 ? g_gpu.reg_write_pos : 96;
    char line[512];
    size_t off = 0;
    for (uint32_t k = 0; k < n; k++) {
        auto& w = g_gpu.reg_writes[k];
        if (off == 0) off += snprintf(line + off, sizeof(line) - off, "   ");
        off += snprintf(line + off, sizeof(line) - off, "r%04X=%08X ", w.idx,
                         w.value);
        if (off > 108) {   // ~8 regs per line
            line[off] = 0;
            PRLOG(Gpu, "%s", line);
            off = 0;
        }
    }
    if (off) { line[off] = 0; PRLOG(Gpu, "%s", line); }
    // Key draw registers (Xenia register_table indices; reg*4 = MMIO offset).
    struct { uint32_t idx; const char* name; } keys[] = {
        {0x0409, "PA_SU_SC_MODE_CNTL"}, {0x0408, "PA_SU_VTX_CNTL"},
        {0x0480, "PA_CL_CLIP_CNTL"},     {0x0400, "PA_SC_SCREEN_SCISSOR"},
        {0x0484, "PA_CL_VTE_CNTL"},      {0x2182, "VGT_DRAW_INITIATOR"},
        {0x2184, "VGT_PRIMITIVE_TYPE"},  {0x21C4, "VGT_INDX_OFFSET"},
        {0x2380, "SQ_PROGRAM_CNTL"},     {0x2381, "SQ_CONTEXT_MISC"},
        {0x0A2F, "COHER_SIZE_HOST"},     {0x0A30, "COHER_BASE_HOST"},
        {0x1844, "D1GRPH_PRIMARY"},      {0x1961, "D1MODE_VIEWPORT"},
    };
    off = 0;
    for (auto& k : keys) {
        if (off == 0) off += snprintf(line + off, sizeof(line) - off, "   ");
        off += snprintf(line + off, sizeof(line) - off, "%s=%08X ", k.name,
                         GpuGetReg(k.idx));
        if (off > 108) { line[off] = 0; PRLOG(Gpu, "%s", line); off = 0; }
    }
    if (off) { line[off] = 0; PRLOG(Gpu, "%s", line); }
}

// ============================================================== interrupt ABI
// Invoke the game's graphics-interrupt callback with the authentic
// (source, user_data) argument pair, on the given GuestThread's context.
// This mirrors Xenia GraphicsSystem::DispatchInterruptCallback ->
// Processor::ExecuteInterrupt: r3 = source, r4 = interrupt_callback_data_.
void DispatchGraphicsInterruptLocked(uint32_t source) {
    uint32_t cb   = K().vd_interrupt_callback;
    uint32_t arg  = K().vd_interrupt_callback_arg;
    if (!cb) return;
    GuestThread* t = GuestThread::GetCurrent();
    if (!t) {
        PRLOG(Gpu, "INTerrupt dispatch skipped: no guest thread context");
        return;
    }
    // r1 must be inside our own stack with padding for the callee.
    uint64_t saved_r1 = t->ctx->r1.u64;
    if (saved_r1 < t->stack_limit + 0x1000 ||
        saved_r1 > t->stack_base) {
        t->ctx->r1.u64 = t->stack_base - 176;
    }
    g_gpu.interrupts_fired++;
    if (g_gpu.interrupts_fired <= 16 ||
        (g_gpu.interrupts_fired % 600) == 0) {
        PRLOG(Gpu, "GPU interrupt #%llu source=%u cb=%08X arg=%08X",
              (unsigned long long)g_gpu.interrupts_fired, source, cb, arg);
    }
    uint64_t args[2] = {source, arg};
    RunGuestFunction(t, cb, args, 2);
    // Restore stack cursor (the callback returns through blr normally,
    // so r1 should already be back; be defensive anyway).
    t->ctx->r1.u64 = t->stack_base - 176;
}

void DispatchGraphicsInterrupt(uint32_t source) {
    std::lock_guard<std::mutex> lk(g_gpu.interrupt_mtx);
    DispatchGraphicsInterruptLocked(source);
}

// ============================================================== MMIO handlers
uint32_t MmioRead0x7FC8(uint32_t addr) {
    uint32_t r = RegIndex0x7FC8(addr);
    switch (r) {
        case kRegRB_EDRAM_TIMING:   return 0x08100748u;   // Xenia fixed
        case kRegRB_BC_CONTROL:     return 0x0000200Eu;   // Xenia fixed
        case kRegD1MODE_V_COUNTER: return 0x000002D0u;   // Xenia fixed
        case kRegD1MODE_VIEWPORT:  return 0x050002D0u;   // 1280x720
        case kRegINT_STATUS: {
            // Bit0 = vblank pending. Kept set between vblank dispatches
            // (Xenia returns a constant 1 here).
            uint32_t v = GpuGetReg(kRegINT_STATUS);
            return v | 1u;
        }
        default:
            return GpuGetReg(r);
    }
}

void MmioWrite0x7FC8(uint32_t addr, uint32_t value) {
    uint32_t r = RegIndex0x7FC8(addr);
    switch (r) {
        case kRegCP_RB_WPTR: {
            // DOORBELL: the game's pusher thread announcing ring work.
            g_gpu.doorbell_wptr.store(value);
            g_gpu.doorbell_written.store(true);
            GpuSetReg(r, value);
            return;
        }
        case kRegCOHER_STATUS_HOST: {
            // Game-triggered cache-coherency flush over
            // [COHER_BASE_HOST, +COHER_SIZE_HOST). Xenia performs the flush
            // (MakeCoherent) and clears the register; we have a single
            // unified memory domain, so completion is immediate.
            GpuSetReg(kRegCOHER_STATUS_HOST, value);
            if (value) {
                uint32_t base = GpuGetReg(kRegCOHER_BASE_HOST);
                uint32_t size = GpuGetReg(kRegCOHER_SIZE_HOST);
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    PRLOG(Gpu, "COHER flush: base=%08X size=%08X status=%08X "
                               "(unified memory: completes immediately)",
                               base, size, value);
                }
                GpuSetReg(kRegCOHER_STATUS_HOST, 0);
            }
            return;
        }
        case kRegD1GRPH_PRIMARY: {
            // Frontbuffer flip committed by the vblank DPC.
            GpuSetReg(r, value);
            PRLOG(Gpu, "PRIMARY_SURFACE_ADDRESS (flip) = %08X (PA, guest VA "
                       "%08X)", value, GpuPaToGuestVa(value));
            return;
        }
        default:
            GpuSetReg(r, value);
            return;
    }
}

}  // namespace
}  // namespace pr

// ------------------------------------------------------- extern "C" MMIO API
// Hooked from the generated TUs via PPC_MM_* macros (see ppc_context.h).
extern "C" {

uint8_t PR_MmioReadU8(uint32_t addr) {
    return pr::LoadU8(addr);
}
uint16_t PR_MmioReadU16(uint32_t addr) {
    return pr::LoadU16(addr);
}
uint32_t PR_MmioReadU32(uint32_t addr) {
    if (pr::IsRegAddr0x7FC8(addr)) return pr::MmioRead0x7FC8(addr);
    return pr::LoadU32(addr);
}
uint64_t PR_MmioReadU64(uint32_t addr) {
    return pr::LoadU64(addr);
}
void PR_MmioWriteU8(uint32_t addr, uint8_t v) {
    pr::StoreU8(addr, v);
}
void PR_MmioWriteU16(uint32_t addr, uint16_t v) {
    pr::StoreU16(addr, v);
}
void PR_MmioWriteU32(uint32_t addr, uint32_t v) {
    static uint64_t s_writes = 0;
    if (pr::IsRegAddr0x7FC8(addr)) {
        s_writes++;
        if (s_writes <= 40) {
            pr::LogLine(pr::LogCategory::kGpu,
                        "MMIO W %08X = %08X (#%llu)", addr, v,
                        (unsigned long long)s_writes);
        }
        pr::MmioWrite0x7FC8(addr, v);
        return;
    }
    if (pr::IsRegAddr0x7FEA(addr)) {
        pr::LogLine(pr::LogCategory::kGpu, "MMIO W(page2) %08X = %08X", addr, v);
        pr::StoreU32(addr, v);   // secondary page: plain backing for now
        return;
    }
    pr::StoreU32(addr, v);
}
void PR_MmioWriteU64(uint32_t addr, uint64_t v) {
    pr::StoreU64(addr, v);
}

}  // extern "C"

namespace pr {
namespace {

// ==================================================================== PM4
enum Type3Opcode {
    PM4_ME_INIT               = 0x48,
    PM4_NOP                   = 0x10,
    PM4_INDIRECT_BUFFER       = 0x3f,
    PM4_INDIRECT_BUFFER_PFD   = 0x37,
    PM4_WAIT_FOR_IDLE         = 0x26,
    PM4_WAIT_REG_MEM          = 0x3c,
    PM4_WAIT_REG_EQ           = 0x52,
    PM4_WAIT_REG_GTE          = 0x53,
    PM4_WAIT_UNTIL_READ       = 0x5c,
    PM4_WAIT_IB_PFD_COMPLETE  = 0x5d,
    PM4_REG_RMW               = 0x21,
    PM4_REG_TO_MEM            = 0x3e,
    PM4_MEM_WRITE             = 0x3d,
    PM4_MEM_WRITE_CNTR        = 0x4f,
    PM4_COND_EXEC             = 0x44,
    PM4_COND_WRITE            = 0x45,
    PM4_EVENT_WRITE           = 0x46,
    PM4_EVENT_WRITE_SHD       = 0x58,
    PM4_EVENT_WRITE_CFL       = 0x59,
    PM4_EVENT_WRITE_EXT       = 0x5a,
    PM4_EVENT_WRITE_ZPD       = 0x5b,
    PM4_DRAW_INDX             = 0x22,
    PM4_DRAW_INDX_2           = 0x36,
    PM4_DRAW_INDX_BIN         = 0x34,
    PM4_DRAW_INDX_2_BIN       = 0x35,
    PM4_VIZ_QUERY             = 0x23,
    PM4_SET_STATE             = 0x25,
    PM4_SET_CONSTANT          = 0x2d,
    PM4_SET_CONSTANT2         = 0x55,
    PM4_SET_SHADER_CONSTANTS  = 0x56,
    PM4_LOAD_ALU_CONSTANT     = 0x2f,
    PM4_LOAD_CONSTANT_CONTEXT = 0x2e,
    PM4_IM_LOAD               = 0x27,
    PM4_IM_LOAD_IMMEDIATE     = 0x2b,
    PM4_INVALIDATE_STATE      = 0x3b,
    PM4_SET_SHADER_BASES      = 0x4A,
    PM4_SET_BIN_BASE_OFFSET   = 0x4B,
    PM4_SET_BIN_MASK          = 0x50,
    PM4_SET_BIN_SELECT        = 0x51,
    PM4_CONTEXT_UPDATE        = 0x5e,
    PM4_INTERRUPT             = 0x54,
    PM4_IM_STORE              = 0x2c,
    PM4_SET_BIN_MASK_LO       = 0x60,
    PM4_SET_BIN_MASK_HI       = 0x61,
    PM4_SET_BIN_SELECT_LO     = 0x62,
    PM4_SET_BIN_SELECT_HI     = 0x63,
    PM4_XE_SWAP               = 0x64,   // Xenia-specific
    PM4_SWAP                  = 0x28,   // Xbox 360 frontbuffer swap
};

const char* Type3OpcodeName(uint32_t opcode) {
    switch (opcode) {
        case PM4_ME_INIT: return "ME_INIT";
        case PM4_NOP: return "NOP";
        case PM4_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
        case PM4_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
        case PM4_WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
        case PM4_WAIT_REG_MEM: return "WAIT_REG_MEM";
        case PM4_WAIT_REG_EQ: return "WAIT_REG_EQ";
        case PM4_WAIT_REG_GTE: return "WAIT_REG_GTE";
        case PM4_WAIT_UNTIL_READ: return "WAIT_UNTIL_READ";
        case PM4_WAIT_IB_PFD_COMPLETE: return "WAIT_IB_PFD_COMPLETE";
        case PM4_REG_RMW: return "REG_RMW";
        case PM4_REG_TO_MEM: return "REG_TO_MEM";
        case PM4_MEM_WRITE: return "MEM_WRITE";
        case PM4_MEM_WRITE_CNTR: return "MEM_WRITE_CNTR";
        case PM4_COND_EXEC: return "COND_EXEC";
        case PM4_COND_WRITE: return "COND_WRITE";
        case PM4_EVENT_WRITE: return "EVENT_WRITE";
        case PM4_EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
        case PM4_EVENT_WRITE_CFL: return "EVENT_WRITE_CFL";
        case PM4_EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
        case PM4_EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
        case PM4_DRAW_INDX: return "DRAW_INDX";
        case PM4_DRAW_INDX_2: return "DRAW_INDX_2";
        case PM4_DRAW_INDX_BIN: return "DRAW_INDX_BIN";
        case PM4_DRAW_INDX_2_BIN: return "DRAW_INDX_2_BIN";
        case PM4_VIZ_QUERY: return "VIZ_QUERY";
        case PM4_SET_STATE: return "SET_STATE";
        case PM4_SET_CONSTANT: return "SET_CONSTANT";
        case PM4_SET_CONSTANT2: return "SET_CONSTANT2";
        case PM4_SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
        case PM4_LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
        case PM4_LOAD_CONSTANT_CONTEXT: return "LOAD_CONSTANT_CONTEXT";
        case PM4_IM_LOAD: return "IM_LOAD";
        case PM4_IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
        case PM4_INVALIDATE_STATE: return "INVALIDATE_STATE";
        case PM4_SET_SHADER_BASES: return "SET_SHADER_BASES";
        case PM4_SET_BIN_BASE_OFFSET: return "SET_BIN_BASE_OFFSET";
        case PM4_SET_BIN_MASK: return "SET_BIN_MASK";
        case PM4_SET_BIN_SELECT: return "SET_BIN_SELECT";
        case PM4_CONTEXT_UPDATE: return "CONTEXT_UPDATE";
        case PM4_INTERRUPT: return "INTERRUPT";
        case PM4_IM_STORE: return "IM_STORE";
        case PM4_SWAP: return "SWAP";
        default: return "UNKNOWN";
    }
}

// Xenia GpuSwap semantics: the GPU's native view of memory is the RAW
// little-endian load of the guest's big-endian bytes (= bswap32 of the
// logical guest dword). GpuSwap(raw, endian) selects the presentation:
//   kNone(0) -> raw, k8in16(1) -> byte-in-hword, k8in32(2) -> logical,
//   k16in32(3) -> hword swap.
inline uint32_t GpuSwap(uint32_t v, uint32_t endian) {
    switch (endian) {
        case 0: return v;
        case 1: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
        case 2: return __builtin_bswap32(v);
        default: return ((v >> 16) & 0xFFFFu) | (v << 16);
    }
}

// GPU-memory read with the endianness encoded in the low address bits.
uint32_t GpuReadMem(uint32_t pa) {
    uint32_t addr = GpuPaToGuestVa(pa & ~3u);
    uint32_t raw = __builtin_bswap32(LoadU32(addr));  // LE load of BE bytes
    return GpuSwap(raw, pa & 0x3);
}
// GPU-memory write (Xenia: GpuSwap then LE store).
void GpuWriteMem(uint32_t pa, uint32_t value) {
    uint32_t out = GpuSwap(value, pa & 0x3);
    // LE store of `out` == BE store of bswap32(out).
    StoreU32(GpuPaToGuestVa(pa & ~3u), __builtin_bswap32(out));
}
// GPU writeback of scratch registers (Xenia: store_and_swap == BE store).
void GpuWriteMemBE(uint32_t pa, uint32_t value) {
    StoreU32(GpuPaToGuestVa(pa & ~3u), value);
}

// Register write with side effects (Xenia CommandProcessor::WriteRegister):
//   * SCRATCH_REG0..7: when SCRATCH_UMSK enables the index, the value is
//     written back to guest memory at SCRATCH_ADDR + index*4 (BE store).
//     This is how the game's GPU command stream signals the CPU (observed:
//     SCRATCH_REG1 "present interval" write lands in the interrupt object
//     and releases the WAIT_REG_MEM vblank throttle).
void WriteRegister(uint32_t idx, uint32_t value) {
    GpuSetReg(idx, value);
    if (idx >= kRegSCRATCH_REG0 && idx <= kRegSCRATCH_REG7) {
        uint32_t scratch_reg = idx - kRegSCRATCH_REG0;
        if ((1u << scratch_reg) & GpuGetReg(kRegSCRATCH_UMSK)) {
            uint32_t scratch_addr = GpuGetReg(kRegSCRATCH_ADDR);
            uint32_t mem_addr = scratch_addr + scratch_reg * 4;
            GpuWriteMemBE(mem_addr, value);
            static uint32_t s_logged = 0;
            if (s_logged++ < 8) {
                PRLOG(Gpu, "SCRATCH_REG%u writeback: [%08X]=%08X (UMSK=%08X "
                           "ADDR=%08X)", scratch_reg, mem_addr, value,
                      GpuGetReg(kRegSCRATCH_UMSK), scratch_addr);
            }
        }
    }
}

struct XenosCP {
    uint32_t read_index = 0;    // dword index into primary ring
    uint32_t logged_opcodes = 0;// per-kind log budget
    uint32_t opcode_logged[128] = {0};

    uint32_t ReadRing(uint32_t ring_va, uint32_t ring_bytes, uint32_t idx) {
        return LoadU32(ring_va + ((idx * 4) % ring_bytes));
    }

    void LogPacket(bool is_ring, int depth, const char* kind, uint32_t index,
                   uint32_t count, const char* fmt, ...) {
        g_gpu.packets_parsed++;
        uint32_t key = (uint32_t)(uintptr_t)kind & 0x7F;
        if (opcode_logged[key & 127] >= 6) return;
        opcode_logged[key & 127]++;
        char detail[128] = {0};
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
        PRLOG(Gpu, "%s%s[%u] %s x%u (%s) #%llu", is_ring ? "ring" : "ib  ",
               depth > 0 ? ">" : " ", index, kind, count, detail,
               (unsigned long long)g_gpu.packets_parsed);
    }

    // Evaluate a WAIT_REG_MEM condition. Returns true when satisfied.
    // COHER_STATUS_HOST polls trigger the flush model (Xenia MakeCoherent).
    bool EvaluateWaitRegMem(uint32_t wait_info, uint32_t poll_addr,
                            uint32_t ref, uint32_t mask, uint32_t* out_val) {
        bool is_memory = (wait_info & 0x10) != 0;
        uint32_t value;
        if (is_memory) {
            value = GpuReadMem(poll_addr);
        } else {
            if (poll_addr == kRegCOHER_STATUS_HOST) {
                // Xenia: MakeCoherent() on poll; unified memory -> clear.
                GpuSetReg(kRegCOHER_STATUS_HOST, 0);
            }
            value = GpuGetReg(poll_addr);
        }
        *out_val = value;
        switch (wait_info & 0x7) {
            case 0x0: return false;                       // never
            case 0x1: return (value & mask) <  ref;
            case 0x2: return (value & mask) <= ref;
            case 0x3: return (value & mask) == ref;
            case 0x4: return (value & mask) != ref;
            case 0x5: return (value & mask) >= ref;
            case 0x6: return (value & mask) >  ref;
            default:   return true;                       // always
        }
    }

    // Execute a PM4 stream. Returns dwords consumed. If a WAIT_REG_MEM
    // condition is not yet satisfied, stops BEFORE it (returns the count
    // up to that packet) so the caller retries later — the packet is never
    // skipped or faked.
    uint32_t ExecuteStream(uint32_t base_va, uint32_t ring_bytes,
                           uint32_t start, uint32_t avail, bool is_ring,
                           int depth) {
        uint32_t i = start;
        uint32_t end = start + avail;
        while (i < end) {
            uint32_t packet = ReadRing(base_va, ring_bytes, i);
            if (packet == 0) { i++; continue; }
            uint32_t type = packet >> 30;
            if (type == 0) {
                uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                if (i + 1 + count > end) break;      // partial: wait
                uint32_t base_index = packet & 0x7FFF;
                uint32_t write_one = (packet >> 15) & 0x1;
                for (uint32_t m = 0; m < count; m++) {
                    uint32_t data = ReadRing(base_va, ring_bytes, i + 1 + m);
                    uint32_t target = write_one ? base_index : base_index + m;
                    // COHER_SIZE/BASE simply stored; COHER_STATUS triggers
                    // the flush (handled via SetReg + immediate completion).
                    if (target == kRegCOHER_STATUS_HOST && data) {
                        GpuSetReg(kRegCOHER_SIZE_HOST,
                               GpuGetReg(kRegCOHER_SIZE_HOST));
                        GpuSetReg(kRegCOHER_STATUS_HOST, data);
                        MmioWrite0x7FC8(kRegPage0x7FC8 + kRegCOHER_STATUS_HOST * 4,
                                        data);
                    } else {
                        WriteRegister(target, data);
                    }
                }
                LogPacket(is_ring, depth, "TYPE0", i, count,
                          "reg[%04X..] x%u", base_index, count);
                i += 1 + count;
            } else if (type == 1) {
                if (i + 3 > end) break;
                uint32_t r1 = packet & 0x7FF;
                uint32_t r2 = (packet >> 11) & 0x7FF;
                uint32_t d1 = ReadRing(base_va, ring_bytes, i + 1);
                uint32_t d2 = ReadRing(base_va, ring_bytes, i + 2);
                GpuSetReg(r1, d1);
                GpuSetReg(r2, d2);
                LogPacket(is_ring, depth, "TYPE1", i, 2, "r[%04X],r[%04X]",
                          r1, r2);
                i += 3;
            } else if (type == 2) {
                LogPacket(is_ring, depth, "TYPE2", i, 0, "nop");
                i += 1;
            } else {
                uint32_t opcode = (packet >> 8) & 0x7F;
                uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                if (i + 1 + count > end) break;      // partial: wait
                LogPacket(is_ring, depth, Type3OpcodeName(opcode), i, count,
                          "op=%02X", opcode);
                switch (opcode) {
                    case PM4_INDIRECT_BUFFER:
                    case PM4_INDIRECT_BUFFER_PFD: {
                        uint32_t list_ptr = ReadRing(base_va, ring_bytes, i + 1)
                                            & 0x1FFFFFFFu;
                        uint32_t list_len = ReadRing(base_va, ring_bytes, i + 2)
                                            & 0xFFFFFu;
                        if (depth < 8 && list_len) {
                            g_gpu.ibs_executed++;
                            ExecuteStream(GpuPaToGuestVa(list_ptr),
                                          list_len * 4 /* linear, no wrap */,
                                          0, list_len, false, depth + 1);
                        }
                        break;
                    }
                    case PM4_WAIT_REG_MEM: {
                        // { wait_info, poll_addr, ref, mask, poll }.
                        uint32_t wait_info = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t poll_addr = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t ref       = ReadRing(base_va, ring_bytes, i + 3);
                        uint32_t mask      = ReadRing(base_va, ring_bytes, i + 4);
                        uint32_t value = 0;
                        bool matched = EvaluateWaitRegMem(wait_info, poll_addr,
                                                          ref, mask, &value);
                        if (!matched) {
                            g_gpu.waits_blocked++;
                            if (g_gpu.waits_blocked <= 8 ||
                                (g_gpu.waits_blocked % 4096) == 0) {
                                PRLOG(Gpu, "WAIT_REG_MEM blocked #%llu: "
                                           "info=%u addr=%08X ref=%08X "
                                           "mask=%08X val=%08X",
                                           (unsigned long long)g_gpu.waits_blocked,
                                           wait_info, poll_addr, ref, mask, value);
                            }
                            return i - start;   // stop BEFORE the packet
                        }
                        break;
                    }
                    case PM4_WAIT_REG_EQ:
                    case PM4_WAIT_REG_GTE: {
                        // { reg, ref, mask }: register-compare waits.
                        uint32_t reg  = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t ref  = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t mask = ReadRing(base_va, ring_bytes, i + 3);
                        uint32_t value = GpuGetReg(reg & 0x7FF);
                        bool matched = opcode == PM4_WAIT_REG_EQ
                                           ? (value & mask) == ref
                                           : (value & mask) >= ref;
                        if (!matched) {
                            g_gpu.waits_blocked++;
                            if (g_gpu.waits_blocked <= 8) {
                                PRLOG(Gpu, "WAIT_REG_%s blocked: reg=%08X "
                                           "ref=%08X mask=%08X val=%08X",
                                           opcode == PM4_WAIT_REG_EQ ? "EQ" : "GTE",
                                           reg, ref, mask, value);
                            }
                            return i - start;
                        }
                        break;
                    }
                    case PM4_INTERRUPT: {
                        // { cpu_mask }: bit n -> dispatch(source=1, cpu=n).
                        uint32_t cpu_mask = ReadRing(base_va, ring_bytes, i + 1);
                        for (uint32_t n = 0; n < 6; n++) {
                            if (cpu_mask & (1u << n)) {
                                DispatchGraphicsInterrupt(1);
                            }
                        }
                        break;
                    }
                    case PM4_EVENT_WRITE_SHD:
                    case PM4_EVENT_WRITE_CFL:
                    case PM4_EVENT_WRITE_ZPD: {
                        // { initiator, address, value }: write value (or the
                        // frame counter when initiator bit31) to memory.
                        uint32_t initiator = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t address   = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t value     = ReadRing(base_va, ring_bytes, i + 3);
                        GpuSetReg(kRegVGT_EVENT_INITIATOR, initiator & 0x3F);
                        uint32_t data = (initiator >> 31) & 1
                                            ? (uint32_t)g_gpu.frame_counter.load()
                                            : value;
                        GpuWriteMem(address, data);
                        LogPacket(is_ring, depth, "EVENT_WR", i, 3,
                                  "-> [%08X]=%08X", address & ~3u, data);
                        break;
                    }
                    case PM4_EVENT_WRITE_EXT:
                    case PM4_EVENT_WRITE: {
                        uint32_t initiator = ReadRing(base_va, ring_bytes, i + 1);
                        GpuSetReg(kRegVGT_EVENT_INITIATOR, initiator & 0x3F);
                        if (opcode == PM4_EVENT_WRITE && count >= 2) {
                            // count==2: write event initiator to address.
                            uint32_t address = ReadRing(base_va, ring_bytes, i + 2);
                            GpuWriteMem(address, initiator & 0x3F);
                        }
                        break;
                    }
                    case PM4_REG_TO_MEM: {
                        // { reg, address }: register -> memory (endianness
                        // from address low bits; OR-mask flag bit31).
                        uint32_t reg_and_or = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t address    = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t value = GpuGetReg(reg_and_or & 0x7FF);
                        if (reg_and_or & 0x80000000u) {
                            value |= GpuReadMem(address);
                        }
                        GpuWriteMem(address, value);
                        break;
                    }
                    case PM4_REG_RMW: {
                        // { reg, and_mask, or_mask }: rmw a register.
                        uint32_t reg      = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t and_mask= ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t or_mask = ReadRing(base_va, ring_bytes, i + 3);
                        uint32_t v = GpuGetReg(reg & 0x7FF);
                        GpuSetReg(reg & 0x7FF, (v & and_mask) | or_mask);
                        break;
                    }
                    case PM4_MEM_WRITE: {
                        // { address, data... }: N words to memory.
                        uint32_t addr = ReadRing(base_va, ring_bytes, i + 1);
                        for (uint32_t k = 0; k + 2 <= count; k++) {
                            uint32_t v = ReadRing(base_va, ring_bytes, i + 2 + k);
                            GpuWriteMem(addr + k * 4, v);
                        }
                        break;
                    }
                    case PM4_MEM_WRITE_CNTR: {
                        // { address }: write frame counter to memory.
                        uint32_t addr = ReadRing(base_va, ring_bytes, i + 1);
                        GpuWriteMem(addr, (uint32_t)g_gpu.frame_counter.load());
                        break;
                    }
                    case PM4_COND_WRITE: {
                        // { wait_info, poll_addr, ref, mask, dst_addr, value }.
                        uint32_t wait_info = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t poll_addr = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t ref       = ReadRing(base_va, ring_bytes, i + 3);
                        uint32_t mask      = ReadRing(base_va, ring_bytes, i + 4);
                        uint32_t dst       = ReadRing(base_va, ring_bytes, i + 5);
                        uint32_t value     = ReadRing(base_va, ring_bytes, i + 6);
                        uint32_t v;
                        if (EvaluateWaitRegMem(wait_info, poll_addr, ref, mask, &v)) {
                            GpuWriteMem(dst, value);
                        }
                        break;
                    }
                    case PM4_DRAW_INDX_2:
                    case PM4_DRAW_INDX_2_BIN: {
                        // Observed from this title: count=1, single dword =
                        // the VGT_DRAW_INITIATOR value (0x00010081: src_sel=1
                        // IMMEDIATE, major=0, inst=1). The standard Xenia
                        // 2-dword form (prim_type, num_indices) is handled
                        // when count>=2; num_indices otherwise comes from
                        // VGT state (pinned once volume draws flow).
                        uint32_t initiator   = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t num_indices = count >= 2
                            ? ReadRing(base_va, ring_bytes, i + 2) : 0;
                        uint32_t src_sel = initiator & 3;          // 0 DMA,1 IMM,2 AUTO
                        uint32_t major_mode = (initiator >> 2) & 3;
                        uint32_t prim_type = (initiator >> 12) & 0xF;
                        uint32_t num_instances = (initiator >> 16) & 0xFFFFF;
                        GpuSetReg(0x2182 /*VGT_DRAW_INITIATOR*/, initiator);
                        g_gpu.draws_seen++;
                        bool verbose = g_gpu.draws_seen <= 32 ||
                                       (g_gpu.draws_seen % 256) == 0;
                        PRLOG(Gpu, "DRAW_INDX_2 #%llu: initiator=%08X "
                                   "(src_sel=%u major=%u prim=%u inst=%u) "
                                   "num_indices=%u%s",
                              (unsigned long long)g_gpu.draws_seen.load(),
                              initiator, src_sel, major_mode, prim_type,
                              num_instances, num_indices,
                              opcode == PM4_DRAW_INDX_2_BIN ? " [BIN]" : "");
                        if (verbose) {
                            // Raw payload evidence (first 6 dwords) to pin
                            // the exact packet layout before deeper decode.
                            if (g_gpu.draws_seen <= 8) {
                                char raw[160];
                                size_t o = 0;
                                for (uint32_t k = 1; k <= 6 && k < count + 1;
                                     k++) {
                                    o += snprintf(raw + o, sizeof(raw) - o,
                                                  "w%u=%08X ", k,
                                                  ReadRing(base_va, ring_bytes,
                                                           i + k));
                                }
                                PRLOG(Gpu, "  draw2 raw: %s(count=%u)", raw,
                                      count);
                            }
                            DumpDrawState(opcode == PM4_DRAW_INDX_2_BIN
                                              ? "draw2bin" : "draw2");
                        }
                        break;
                    }
                    case PM4_DRAW_INDX:
                    case PM4_DRAW_INDX_BIN: {
                        // { VGT_DRAW_INITIATOR, base_addr_lo, base_addr_hi,
                        //   num_indices, index_size (16/32) }: DMA-indexed.
                        uint32_t initiator = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t base_lo   = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t base_hi   = ReadRing(base_va, ring_bytes, i + 3);
                        uint32_t num_idx   = ReadRing(base_va, ring_bytes, i + 4);
                        uint32_t idx_sz    = ReadRing(base_va, ring_bytes, i + 5);
                        GpuSetReg(0x2182 /*VGT_DRAW_INITIATOR*/, initiator);
                        g_gpu.draws_seen++;
                        bool verbose = g_gpu.draws_seen <= 32 ||
                                       (g_gpu.draws_seen % 256) == 0;
                        PRLOG(Gpu, "DRAW_INDX #%llu: initiator=%08X "
                                   "base=%08X:%08X num_indices=%u index_size=%u",
                              (unsigned long long)g_gpu.draws_seen.load(),
                              initiator, base_hi, base_lo, num_idx, idx_sz);
                        if (verbose) DumpDrawState("draw");
                        break;
                    }
                    case PM4_IM_LOAD_IMMEDIATE: {
                        // Shader load (IMMEDIATE form): payload = shader
                        // type/address/size words. Log raw + best-guess
                        // decode; refine from evidence once dumps accumulate.
                        uint32_t w0 = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t w1 = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t w2 = ReadRing(base_va, ring_bytes, i + 3);
                        g_gpu.im_loads_seen++;
                        PRLOG(Gpu, "IM_LOAD_IMMEDIATE #%llu: type=%u "
                                   "(raw w0=%08X) addr=%08X size/len=%08X "
                                   "(%u dwords)",
                              (unsigned long long)g_gpu.im_loads_seen.load(),
                              w0 & 3, w0, w1, w2, w2);
                        break;
                    }
                    case PM4_IM_LOAD: {
                        uint32_t w0 = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t w1 = ReadRing(base_va, ring_bytes, i + 2);
                        uint32_t w2 = ReadRing(base_va, ring_bytes, i + 3);
                        g_gpu.im_loads_seen++;
                        PRLOG(Gpu, "IM_LOAD #%llu: raw w0=%08X w1=%08X w2=%08X",
                              (unsigned long long)g_gpu.im_loads_seen.load(),
                              w0, w1, w2);
                        break;
                    }
                    case PM4_SET_SHADER_CONSTANTS:
                    case PM4_SET_CONSTANT:
                    case PM4_SET_CONSTANT2: {
                        // { const_addr, data... }: const bank/offset in the
                        // first word. Log the addressing (first 2 dwords) at
                        // low volume; the data words follow in the stream.
                        static std::atomic<uint64_t> s_consts{0};
                        uint64_t n = s_consts.fetch_add(1) + 1;
                        if (n <= 64 || (n % 1024) == 0) {
                            uint32_t const_addr = ReadRing(base_va, ring_bytes,
                                                           i + 1);
                            uint32_t d0 = ReadRing(base_va, ring_bytes, i + 2);
                            uint32_t d1 = count >= 2
                                ? ReadRing(base_va, ring_bytes, i + 3) : 0;
                            PRLOG(Gpu, "%s #%llu: const_addr=%08X count=%u "
                                       "d0=%08X d1=%08X",
                                  Type3OpcodeName(opcode),
                                  (unsigned long long)n, const_addr, count,
                                  d0, d1);
                        }
                        break;
                    }
                    case PM4_SET_SHADER_BASES: {
                        uint32_t bases = ReadRing(base_va, ring_bytes, i + 1);
                        PRLOG(Gpu, "SET_SHADER_BASES: %08X (vs/ps base swap)",
                              bases);
                        break;
                    }
                    case PM4_SWAP: {
                        // Xbox 360 frontbuffer swap packet (VdSwap path).
                        // Payload observed from the game's D3D: mostly
                        // swap/flip metadata. Phase 2C rendering target:
                        // log it richly; no fake presentation.
                        uint32_t a0 = ReadRing(base_va, ring_bytes, i + 1);
                        uint32_t a1 = ReadRing(base_va, ring_bytes, i + 2);
                        PRLOG(Gpu, "PM4_SWAP packet: w1=%08X w2=%08X (%u "
                                   "dwords) — frontbuffer/flip metadata "
                                   "(Phase 2C render boundary)", a0, a1, count);
                        break;
                    }
                    default:
                        // Consumed + logged. Registers written by TYPE0/1
                        // packets carry all observed state so far; draw and
                        // shader opcodes are the Phase 2C+ frontier.
                        break;
                }
                i += 1 + count;
            }
        }
        return i - start;
    }
};

// ================================================================ CP worker
void XenosCPMain(GuestThread* t) {
    XenosCP cp;
    PRLOG(Gpu, "Xenos command processor online (guest thread %u, pcr=%08X)",
          t->thread_id, t->pcr);

    uint32_t ring_va = 0, ring_bytes = 0;
    uint64_t last_int_dispatch = 0;

    while (g_cp_running) {
        if (!K().vd_ring_buffer_ptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // (Re)capture ring geometry.
        if (!ring_va) {
            ring_va = GpuPaToGuestVa(K().vd_ring_buffer_ptr);
            ring_bytes = 1u << (K().vd_ring_buffer_size_log2 + 3);
            PRLOG(Gpu, "primary ring: PA=%08X VA=%08X bytes=%u",
                  K().vd_ring_buffer_ptr, ring_va, ring_bytes);
        }

        // Write pointer: the DOORBELL is authoritative (the game's pusher
        // rings CP_RB_WPTR after every copy). Until the first doorbell,
        // fall back to the game's dev+10908 insert cursor.
        uint32_t write = g_gpu.doorbell_wptr.load();
        if (!g_gpu.doorbell_written.load()) {
            uint32_t dev = K().vd_interrupt_callback_arg;
            if (dev) {
                uint32_t alt = LoadU32(dev + 10908);
                if (alt != 0xBAADF00D && alt) write = alt;
            }
        }
        if (write == 0xBAADF00D || write == cp.read_index) {
            // Idle: poll vblank tick cadence.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Process new packets (wraparound-aware).
        uint32_t consumed = 0;
        if (write > cp.read_index) {
            consumed = cp.ExecuteStream(ring_va, ring_bytes, cp.read_index,
                                        write - cp.read_index, true, 0);
        } else {
            // Wrapped: to end, then from 0.
            consumed = cp.ExecuteStream(ring_va, ring_bytes, cp.read_index,
                                        ring_bytes / 4 - cp.read_index, true, 0);
            if (consumed >= ring_bytes / 4 - cp.read_index) {
                consumed += cp.ExecuteStream(ring_va, ring_bytes, 0, write,
                                             true, 0);
            }
        }

        if (consumed == 0) {
            // A WAIT_REG_MEM (or partial packet) blocks progress: the CPU
            // side must change the observed state (e.g. the vblank DPC
            // clearing the flip-pending word). Back off and retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        cp.read_index += consumed;
        cp.read_index %= ring_bytes / 4;
        if (cp.read_index != write && write != 0xBAADF00D) {
            // Consumed a prefix (blocked mid-way); still publish progress.
        }

        // Publish the read pointer through the physical->VA translation.
        // The game passed the PHYSICAL address of mirror+60 to
        // VdEnableRingBufferRPtrWriteBack; its DPC and kick path poll the
        // VIRTUAL window alias (mirror+60 = PA + 0xA0000000).
        if (K().vd_rptr_writeback_ptr) {
            uint32_t wb_va = GpuPaToGuestVa(K().vd_rptr_writeback_ptr);
            StoreU32(wb_va, cp.read_index);
            // Also mirror to the raw PA for diagnostics/watchdogs.
            if (K().vd_rptr_writeback_ptr < 0x20000000u) {
                StoreU32(K().vd_rptr_writeback_ptr, cp.read_index);
            }
        }

        // Ring-advance interrupt (source=1): the game's D3D ring-init
        // handshake depends on this completion signal (observed: without
        // it the main thread parks in its queue dispatch loop forever).
        // Hardware semantics: an interrupt with no registered handler is a
        // no-op — the game registers its ISR ([[dev+10900]+16]) via the
        // SCRATCH_REG4 writeback only after this stage, so gate on the slot
        // being a valid code pointer to avoid bctrl'ing into the heap
        // poison (0BADF00D) the uninitialized slot contains.
        uint64_t now = g_gpu.packets_parsed.load();
        if (now != last_int_dispatch) {
            last_int_dispatch = now;
            bool isr_valid = false;
            uint32_t arg = K().vd_interrupt_callback_arg;
            if (arg) {
                uint32_t intobj = LoadU32(arg + 10900);
                if (intobj >= 0x82000000 && intobj < 0xC0000000) {
                    uint32_t isr = LoadU32(intobj + 16);
                    isr_valid = isr >= 0x82230000 && isr < 0x82BA880C &&
                                (isr & 3) == 0;
                }
            }
            if (isr_valid || g_gpu.interrupts_fired == 0) {
                // Fire the very first one unconditionally ONLY if the ISR
                // slot is sane; otherwise the game has no handler yet.
            }
            if (isr_valid) {
                DispatchGraphicsInterrupt(1);
            } else {
                g_gpu.interrupts_dropped++;
            }
        }
    }
    PRLOG(Gpu, "Xenos command processor stopped (packets=%llu ibs=%llu "
               "waits=%llu ints=%llu)",
          (unsigned long long)g_gpu.packets_parsed.load(),
          (unsigned long long)g_gpu.ibs_executed.load(),
          (unsigned long long)g_gpu.waits_blocked.load(),
          (unsigned long long)g_gpu.interrupts_fired.load());
}

// ================================================================ vsync tick
// Xenia GraphicsSystem::MarkVblank at ~60Hz: increment the frame counter
// then dispatch the vblank interrupt (source=0). The game's callback
// (82A79A00 source=0 path) reads the interrupt status register and runs
// its own vblank DPC (sub_82A696D8) — frame callbacks, flip commit
// ([obj+4] -> D1GRPH_PRIMARY_SURFACE_ADDRESS), etc.
void XenosVsyncMain(GuestThread* t) {
    PRLOG(Gpu, "Xenos vsync ticker online (guest thread %u)", t->thread_id);
    auto next = std::chrono::steady_clock::now();
    while (g_cp_running) {
        next += std::chrono::microseconds(16667);   // 60 Hz
        std::this_thread::sleep_until(next);
        if (!K().vd_interrupt_callback) continue;
        g_gpu.frame_counter.fetch_add(1);
        GpuSetReg(kRegVBL_COUNTER, (uint32_t)g_gpu.frame_counter.load());
        // Vblank pending bit (observed by the game's source=0 callback).
        GpuSetReg(kRegINT_STATUS, GpuGetReg(kRegINT_STATUS) | 1u);
        DispatchGraphicsInterrupt(0);
    }
}

}  // namespace

// ================================================================= bootstrap
// The CP and vsync tickers run as REAL guest threads (full PPCContext +
// PCR/TLS/KTHREAD) so guest interrupt callbacks execute with a valid
// r13/thread state, exactly like Xenia's XHostThread interrupt dispatch.
constexpr uint32_t kCpThreadEntryMarker   = 0xFEED0001u;
constexpr uint32_t kVsyncThreadEntryMarker= 0xFEED0002u;

void RunXenosCpThread(GuestThread* t)      { XenosCPMain(t); }
void RunXenosVsyncThread(GuestThread* t)   { XenosVsyncMain(t); }

void StartXenosCommandProcessor() {
    g_cp_running = true;

    auto start = [](uint32_t marker, const char* name,
                    void (*body)(GuestThread*)) {
        GuestThread* t = new GuestThread();
        t->launch.entry = marker;
        t->launch.arg = 0;
        t->launch.xapi_startup = 0;
        t->launch.creation_flags = 0;
        t->launch.stack_size = 64 * 1024;
        t->name = name;
        if (!t->Create(64 * 1024)) {
            PRLOGE("failed to create %s guest thread", name);
            delete t;
            return;
        }
        t->host = std::thread([t, body, name]() {
            // Mirror GuestThread::Run() prologue: bind this host thread to
            // the GuestThread so guest callbacks find a valid context.
            GuestThread::SetCurrentForHostThread(t);
            PRLOG(Thread, "%s thread %u executing (entry marker %08X)",
                  name, t->thread_id, t->launch.entry);
            body(t);
            t->finished = true;
            StoreU8(t->kthread + 0x01, 1);
        });
        t->host.detach();
    };

    start(kCpThreadEntryMarker, "XenosCP", &RunXenosCpThread);
    start(kVsyncThreadEntryMarker, "XenosVsync", &RunXenosVsyncThread);
}

void StopXenosCommandProcessor() { g_cp_running = false; }

// Runtime-side register poke used by the Vd imports to reflect kernel
// ring setup into the register file (like real HW would hold it).
void XenosSetRingRegs(uint32_t rb_base_pa, uint32_t rptr_wb_pa) {
    GpuSetReg(kRegCP_RB_BASE, rb_base_pa);
    GpuSetReg(kRegCP_RB_RPTR_ADDR, rptr_wb_pa);
}

uint64_t XenosGpuStats(uint32_t* packets, uint32_t* ibs, uint32_t* waits,
                       uint32_t* ints, uint64_t* draws, uint64_t* shaders) {
    if (packets) *packets = (uint32_t)g_gpu.packets_parsed.load();
    if (ibs)     *ibs     = (uint32_t)g_gpu.ibs_executed.load();
    if (waits)   *waits   = (uint32_t)g_gpu.waits_blocked.load();
    if (ints)    *ints    = (uint32_t)g_gpu.interrupts_fired.load();
    if (draws)   *draws   = g_gpu.draws_seen.load();
    if (shaders) *shaders = g_gpu.im_loads_seen.load();
    return g_gpu.frame_counter.load();
}

}  // namespace pr
