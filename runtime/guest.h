// PrototypeRecomp Phase 2B runtime — core guest definitions.
// The generated PPC code defines PPCContext in ppc_context.h; we include the
// exact same header so the ABI matches bit-for-bit.
#pragma once

#include "ppc_config.h"
#include "ppc_context.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <string>

namespace pr {

// ---------------------------------------------------------------- constants
// Guest memory map (mirrors Xenia regions; validated by runtime assertions).
constexpr uint32_t kGuestMemSize   = 0x100000000ull;  // 4 GiB
constexpr uint32_t kImageBase      = 0x82000000;
constexpr uint32_t kImageSize      = 0xE60000;
constexpr uint32_t kLookupTableBase = kImageBase + kImageSize;   // 0x82E60000
constexpr uint32_t kLookupTableSize = uint32_t(PPC_CODE_SIZE) * 2;

// Thread stacks (Xenia: kStackAddressRangeBegin/End 0x70000000..0x7F000000).
constexpr uint32_t kStackRegionBase  = 0x70000000;
constexpr uint32_t kStackRegionEnd   = 0x7F000000;
constexpr uint32_t kMainStackTop     = 0x7FFFF000;   // main thread grows down

// Kernel-structure arena: KTHREAD / KEVENT / PCR / TLS blocks / LDR data.
constexpr uint32_t kKernelStructBase = 0x80010000;
constexpr uint32_t kKernelStructEnd  = 0x80FF0000;

// System pool (ExAllocatePoolTypeWithTag / XamAlloc).
constexpr uint32_t kSystemPoolBase   = 0x84200000;
constexpr uint32_t kSystemPoolEnd    = 0x8FF00000;

// Physical memory arena (MmAllocatePhysicalMemoryEx / GPU buffers).
constexpr uint32_t kPhysicalArenaBase = 0xA0000000;
constexpr uint32_t kPhysicalArenaEnd  = 0xC0000000;

// General title heap (NtAllocateVirtualMemory with NULL base).
constexpr uint32_t kTitleHeapBase    = 0x00010000;
constexpr uint32_t kTitleHeapEnd     = 0x40000000;

// Xbox 360 NTSTATUS-ish codes used by the game.
constexpr uint32_t X_STATUS_SUCCESS            = 0x00000000;
constexpr uint32_t X_STATUS_NO_ERROR           = 0x00000000;
constexpr uint32_t X_STATUS_UNSUCCESSFUL       = 0xC0000001;
constexpr uint32_t X_STATUS_INVALID_HANDLE     = 0xC0000008;
constexpr uint32_t X_STATUS_NO_MEMORY          = 0xC0000017;
constexpr uint32_t X_STATUS_ACCESS_DENIED      = 0xC0000022;
constexpr uint32_t X_STATUS_NO_SUCH_FILE       = 0xC000000F;
constexpr uint32_t X_STATUS_INVALID_PARAMETER  = 0xC000000D;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t X_STATUS_END_OF_FILE        = 0xC0000011;
constexpr uint32_t X_STATUS_PENDING            = 0x00000103;
constexpr uint32_t X_STATUS_TIMEOUT            = 0x00000102;
constexpr uint32_t X_STATUS_WAIT_0             = 0x00000000;
constexpr uint32_t X_STATUS_WAIT_1             = 0x00000001;
constexpr uint32_t X_STATUS_ABANDONED          = 0x00000080;
constexpr uint32_t X_STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;

constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x000004CF; // XINPUT style
constexpr uint32_t X_ERROR_SUCCESS              = 0x00000000;
constexpr uint32_t X_ERROR_EMPTY                = 0x000004CA;
constexpr uint32_t X_ERROR_IO_PENDING           = 0x000003E5;

// X_INPUT_STATE layout (XINPUT diamond).
struct X_INPUT_GAMEPAD {
    uint16_t buttons;      // +0x00
    uint8_t  left_trigger; // +0x02
    uint8_t  right_trigger;// +0x03
    int16_t  thumb_lx;     // +0x04
    int16_t  thumb_ly;     // +0x06
    int16_t  thumb_rx;     // +0x08
    int16_t  thumb_ry;     // +0x0A
};                         // 0x0C
struct X_INPUT_STATE {
    uint32_t packet_number; // +0x00
    X_INPUT_GAMEPAD gamepad;// +0x04
};                          // 0x10

// ---------------------------------------------------------------- host->guest
extern uint8_t* g_guest_base;  // host pointer == guest address 0

inline uint8_t* HostFromGuest(uint32_t g) { return g_guest_base + (uint64_t)g; }
inline uint32_t GuestFromHost(const void* p) {
    return uint32_t((uintptr_t)p - (uintptr_t)g_guest_base);
}

// Big-endian load/store into guest memory (matches PPC_LOAD/STORE macros).
inline uint8_t  LoadU8 (uint32_t a)            { return *(uint8_t*)(g_guest_base + (uint64_t)a); }
inline uint16_t LoadU16(uint32_t a)            { return __builtin_bswap16(*(uint16_t*)(g_guest_base + (uint64_t)a)); }
inline uint32_t LoadU32(uint32_t a)            { return __builtin_bswap32(*(uint32_t*)(g_guest_base + (uint64_t)a)); }
inline uint64_t LoadU64(uint32_t a)            { return __builtin_bswap64(*(uint64_t*)(g_guest_base + (uint64_t)a)); }
inline void StoreU8 (uint32_t a, uint8_t  v)   { *(uint8_t*)(g_guest_base + (uint64_t)a) = v; }
inline void StoreU16(uint32_t a, uint16_t v)   { *(uint16_t*)(g_guest_base + (uint64_t)a) = __builtin_bswap16(v); }
inline void StoreU32(uint32_t a, uint32_t v)   { *(uint32_t*)(g_guest_base + (uint64_t)a) = __builtin_bswap32(v); }
inline void StoreU64(uint32_t a, uint64_t v)   { *(uint64_t*)(g_guest_base + (uint64_t)a) = __builtin_bswap64(v); }

inline void GuestMemset(uint32_t a, uint8_t v, uint32_t n) {
    memset(g_guest_base + (uint64_t)a, v, (size_t)n);
}
inline void GuestMemcpy(uint32_t d, uint32_t s, uint32_t n) {
    memcpy(g_guest_base + (uint64_t)d, g_guest_base + (uint64_t)s, (size_t)n);
}
inline void GuestWrite(uint32_t d, const void* src, uint32_t n) {
    memcpy(g_guest_base + (uint64_t)d, src, (size_t)n);
}
inline void GuestRead(void* dst, uint32_t s, uint32_t n) {
    memcpy(dst, g_guest_base + (uint64_t)s, (size_t)n);
}

// ANSI/Unicode guest string helpers.
std::string GuestAnsiString(uint32_t addr, uint32_t max_len = 1024);
void GuestWriteAnsiString(uint32_t addr, const std::string& s);

// ---------------------------------------------------------------- PPC helpers
// Call a generated guest function through the lookup table.
inline PPCFunc* LookupGuestFunc(uint32_t guest_addr) {
    return *(PPCFunc**)(g_guest_base + (uint64_t)(kLookupTableBase) +
                        (uint64_t(uint32_t(guest_addr) - PPC_CODE_BASE) * 2));
}

// Function-name lookup for diagnostics (ppc_func_mapping addresses only).
const char* GuestFuncName(uint32_t addr);

// Stack-argument read: PPC ABI observed in Prototype: extra args at
// [caller_r1 + 0x54 + 8*i] (8-byte slots). ctx.r1 at import entry equals the
// caller's r1 AFTER its stwu prologue, so stack args sit at r1+0x54.
inline uint32_t StackArg(const PPCContext& ctx, uint32_t index) {
    // index 0 -> r1+0x54, 1 -> r1+0x5C ...
    return LoadU32(ctx.r1.u32 + 0x54 + index * 8);
}

}  // namespace pr
