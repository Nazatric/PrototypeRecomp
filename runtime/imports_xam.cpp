// PrototypeRecomp Phase 2B runtime — XAM, XInput, audio, network, crypto,
// RTL/misc imports.
#include "state.h"

#include <ctime>
#include <unistd.h>

using namespace pr;


#include "args.h"
#define IMPORT(name) \
    void __imp__##name(PPCContext& ctx, uint8_t* base)
#define RET(v) do { ctx.r3.u64 = (uint64_t)(uint32_t)(v); return; } while (0)

// =============================================================== input

// Keyboard mapping (desktop stand-in until Android input lands).
static bool g_controller_connected[4] = {true, false, false, false};

IMPORT(XamInputGetState) {
    // (user_index, flags, X_INPUT_STATE*)
    uint32_t user_index = ARG(0);
    uint32_t flags = ARG(1);
    uint32_t state_ptr = ARG(2);

    PRLOG(Input, "XamInputGetState(user=%u flags=%08X state=%08X)", user_index,
          flags, state_ptr);

    if ((flags & 0xFF) && (flags & 0x01) == 0) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);  // not asking for gamepad
    }
    uint32_t actual = user_index;
    if ((actual & 0xFF) == 0xFF || (flags & 0xFF)) actual = 0;
    if (actual >= 4 || !g_controller_connected[actual]) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
    }
    // Games call with NULL state ptr as connectivity query.
    if (!state_ptr) RET(X_ERROR_SUCCESS);

    // Neutral-but-real controller state (idle physical controller).
    StoreU32(state_ptr + 0x00, 0);           // packet_number
    StoreU16(state_ptr + 0x04, 0);           // buttons
    StoreU8 (state_ptr + 0x06, 0);           // left trigger
    StoreU8 (state_ptr + 0x07, 0);           // right trigger
    StoreU16(state_ptr + 0x08, 0);           // thumb_lx
    StoreU16(state_ptr + 0x0A, 0);           // thumb_ly
    StoreU16(state_ptr + 0x0C, 0);           // thumb_rx
    StoreU16(state_ptr + 0x0E, 0);           // thumb_ry
    RET(X_ERROR_SUCCESS);
}

IMPORT(XamInputGetCapabilities) {
    uint32_t user_index = ARG(0);
    uint32_t flags = ARG(1);
    uint32_t caps_ptr = ARG(2);
    if (user_index >= 4 || !g_controller_connected[user_index & 3]) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
    }
    // X_INPUT_CAPABILITIES: type u8, sub_type u8, flags u16, buttons u16,
    // triggers, thumbs (0x10 total)
    if (caps_ptr) {
        GuestMemset(caps_ptr, 0, 0x10);
        StoreU8(caps_ptr + 0x00, 0);        // gamepad
        StoreU8(caps_ptr + 0x01, 1);        // sub_type gamepad
        StoreU16(caps_ptr + 0x02, 0);
    }
    RET(X_ERROR_SUCCESS);
}

IMPORT(XamInputSetState) {
    uint32_t user_index = ARG(0);
    uint32_t unk = ARG(1);
    RET(X_ERROR_SUCCESS);
}

// =============================================================== XAM core

IMPORT(XamGetCurrentTitleId) { RET(0x4156084Eu); }  // not imported; reserved

IMPORT(XamGetExecutionId) {
    uint32_t out_ptr = ARG(0);
    if (out_ptr) {
        GuestMemset(out_ptr, 0, 0x20);
        StoreU32(out_ptr + 0x00, 0x4156084Eu);  // title id
        StoreU32(out_ptr + 0x04, 1);            // disc number
        StoreU32(out_ptr + 0x08, 2);            // disc count? (approx)
        StoreU32(out_ptr + 0x0C, 0);            // format
        StoreU32(out_ptr + 0x10, 0x2D0008);     // flags/base? (approx)
        StoreU32(out_ptr + 0x14, 0x41560800u);  // publisher id? (approx)
    }
    RET(X_ERROR_SUCCESS);
}

IMPORT(XamGetSystemVersion) { RET(0x000B0000u); }  // dash version-ish

IMPORT(XamAlloc) {
    uint32_t size = ARG(0);
    uint32_t align = ARG(1);
    uint32_t addr = GuestMemory::Get().PoolAlloc((size + 7) & ~7u, 0);
    if (addr) GuestMemset(addr, 0, size);
    RET(addr);
}

IMPORT(XamFree) {
    uint32_t addr = ARG(0);
    if (addr) GuestMemory::Get().PoolFree(addr);
}

IMPORT(XamNotifyCreateListener) {
    uint32_t mask = ARG(0);
    uint32_t handle_ptr = ARG(1);
    uint32_t handle = 0xF9000000u + (K().notify_listener_next_handle++ & 0xFF) * 4;
    if (handle_ptr) StoreU32(handle_ptr, handle);
    RET(X_ERROR_SUCCESS);
}

IMPORT(XNotifyGetNext) {
    // (listener_handle, reserved, payload_ptr, id_ptr)
    uint32_t handle = ARG(0);
    uint32_t payload_ptr = ARG(2);
    uint32_t id_ptr = ARG(3);
    if (id_ptr) StoreU32(id_ptr, 0);
    if (payload_ptr) StoreU32(payload_ptr, 0);
    RET(0);  // no notifications pending
}

// =============================================================== users

IMPORT(XamUserGetSigninState) {
    uint32_t user_index = ARG(0);
    RET(user_index == 0 ? 2 : 0);  // 2 = signed in locally
}

IMPORT(XamUserGetSigninInfo) {
    uint32_t user_index = ARG(0);
    uint32_t flags = ARG(1);
    uint32_t info_ptr = ARG(2);
    if (user_index != 0) RET(X_ERROR_DEVICE_NOT_CONNECTED);
    if (info_ptr) {
        // XUSER_SIGNIN_INFO (0x34): state u32, addr u32, xuid u64, ...
        GuestMemset(info_ptr, 0, 0x34);
        StoreU32(info_ptr + 0x00, 2);            // signed in
        StoreU64(info_ptr + 0x0C, 0x0100000000000001ull);  // xuid
        const char* name = "ProtoPlayer";
        for (int i = 0; name[i]; i++)
            StoreU16(info_ptr + 0x1C + i * 2, name[i]);
    }
    RET(X_ERROR_SUCCESS);
}

IMPORT(XamUserGetXUID) {
    uint32_t user_index = ARG(0);
    uint32_t xuid_ptr = ARG(1);
    if (xuid_ptr) StoreU64(xuid_ptr, 0x0100000000000001ull);
    RET(0);
}

IMPORT(XamUserGetName) {
    uint32_t user_index = ARG(0);
    uint32_t name_ptr = ARG(1);
    if (name_ptr) {
        const char* name = "ProtoPlayer";
        for (int i = 0; name[i]; i++) StoreU16(name_ptr + i * 2, name[i]);
        StoreU16(name_ptr + 16 * 2, 0);
    }
    RET(0);
}

IMPORT(XamUserAreUsersFriends) { RET(0); }
IMPORT(XamUserCheckPrivilege) { RET(0); }

IMPORT(XamUserReadProfileSettings) {
    // (title_id, user_count, user_ids, setting_count, setting_ids, sizes,
    //  buffer, overlapped)
    uint32_t overlapped = ARG(7);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);  // status success
        StoreU32(overlapped + 0x04, 0);  // length 0
    }
    RET(X_ERROR_SUCCESS);  // zero settings read
}

IMPORT(XamUserWriteProfileSettings) { RET(X_ERROR_SUCCESS); }

// =============================================================== content

IMPORT(XamContentCreateEx) {
    // (handle_out, display_name, type_mask, content_type, device_id,
    //  display_name2, cb1, cb2, flags, creation_flags)
    uint32_t handle_ptr = ARG(0);
    if (handle_ptr) StoreU32(handle_ptr, 0);
    RET(0x87D0000Du);  // E_CANNOT_ACCESS_STORAGE-ish (no storage mounted)
}

IMPORT(XamContentCreateEnumerator) {
    // (title_id, type_mask, flags, items_per_enum, size_out, handle_out,
    //  overlapped)
    uint32_t overlapped = ARG(6);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(0x87D0000Eu);  // no content
}

IMPORT(XamEnumerate) {
    // (handle, flags, buffer, size, items_written_ptr, items_total_ptr,
    //  overlapped)
    uint32_t overlapped = ARG(6);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(0);
}

IMPORT(XamContentClose) { RET(X_ERROR_SUCCESS); }
IMPORT(XamContentFlush) { RET(X_ERROR_SUCCESS); }
IMPORT(XamContentGetCreator) { RET(0); }
IMPORT(XamContentGetDeviceData) { RET(0x87D0000Fu); }
IMPORT(XamContentGetDeviceState) { RET(0x87D0000Fu); }
IMPORT(XamContentGetLicenseMask) { RET(0); }
IMPORT(XamContentSetThumbnail) { RET(0); }
IMPORT(XamContentDelete) { RET(0); }
IMPORT(XamCreateEnumeratorHandle) { RET(0); }
IMPORT(XamUserCreateStatsEnumerator) {
    // Like XamContentCreateEnumerator: no Live content offline.
    uint32_t overlapped = ARG(6);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(0x87D0000Eu);
}
IMPORT(XamUserCreateAchievementEnumerator) {
    uint32_t overlapped = ARG(6);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(0x87D0000Eu);
}
IMPORT(XamGetPrivateEnumStructureFromHandle) { RET(0); }

// =============================================================== UI dialogs

IMPORT(XamShowMessageBoxUIEx) { RET(0); }
IMPORT(XamShowSigninUI) { RET(0); }
IMPORT(XamShowFriendsUI) { RET(0); }
IMPORT(XamShowDeviceSelectorUI) { RET(0); }
IMPORT(XamShowMarketplaceUI) { RET(0); }
IMPORT(XamShowPlayersUI) { RET(0); }
IMPORT(XamShowMessageComposeUI) { RET(0); }
IMPORT(XamShowFriendRequestUI) { RET(0); }
IMPORT(XamShowDirtyDiscErrorUI) {
    PRLOGE("XamShowDirtyDiscErrorUI — game reports dirty disc");
    TerminateTitle(0xE691DDDC, "dirty disc UI");
}

// =============================================================== tasks/sessions

IMPORT(XamTaskCloseHandle) { RET(X_ERROR_SUCCESS); }
IMPORT(XamTaskSchedule) { RET(X_ERROR_SUCCESS); }
IMPORT(XamTaskShouldExit) { RET(0); }
IMPORT(XamSessionCreateHandle) {
    uint32_t handle_ptr = ARG(0);
    uint32_t flags = ARG(1);
    if (handle_ptr) StoreU32(handle_ptr, 0);
    RET(0x87D00016u);  // no Live session offline
}
IMPORT(XamSessionRefObjByHandle) { RET(0x87D00016u); }

// =============================================================== loader

IMPORT(XamLoaderLaunchTitle) {
    uint32_t path_ptr = ARG(0);
    PRLOGE("XamLoaderLaunchTitle — title requests another title launch");
    _exit(0);
}

IMPORT(XamLoaderTerminateTitle) {
    PRLOGE("XamLoaderTerminateTitle");
    _exit(0);
}

IMPORT(CurlOpenTitleBackingFile) {
    // (mode, filename?, ...) — Curl-open on title backing file.
    uint32_t mode = ARG(0);
    PRLOG(Import, "CurlOpenTitleBackingFile(mode=%u)", mode);
    RET(0);
}

// =============================================================== XMsg

IMPORT(XMsgStartIORequest) {
    // (port?, msg, flags, overlapped?) — approximated as async IO request.
    uint32_t overlapped = ARG(3);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(X_ERROR_SUCCESS);
}

IMPORT(XMsgStartIORequestEx) {
    uint32_t overlapped = ARG(6);
    if (overlapped) {
        StoreU32(overlapped + 0x00, 0);
        StoreU32(overlapped + 0x04, 0);
    }
    RET(X_ERROR_SUCCESS);
}

IMPORT(XMsgCancelIORequest) { RET(X_ERROR_SUCCESS); }
IMPORT(XMsgInProcessCall) { RET(0); }

// =============================================================== audio

IMPORT(XAudioRegisterRenderDriverClient) {
    // (priority, callback_ptr) -> client id
    uint32_t priority = ARG(0);
    uint32_t callback = ARG(1);
    std::lock_guard<std::mutex> lk(K().audio_mutex);
    K().audio_client_callback = callback;
    K().audio_client_priority = priority;
    K().audio_client_registered = true;
    PRLOG(Import, "XAudioRegisterRenderDriverClient(pri=%u cb=%08X)", priority,
          callback);
    RET(1);  // client id
}

IMPORT(XAudioUnregisterRenderDriverClient) {
    uint32_t client_id = ARG(0);
    std::lock_guard<std::mutex> lk(K().audio_mutex);
    K().audio_client_registered = false;
    K().audio_client_callback = 0;
    RET(0);
}

IMPORT(XAudioSubmitRenderDriverFrame) {
    // (client_id, buffer) — Phase 2C: no audio output yet.
    RET(0);
}

IMPORT(XAudioGetVoiceCategoryVolume) {
    uint32_t category = ARG(0);
    uint32_t r3 = ARG(1);
    RET(0);
}

IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) { RET(0); }

// =============================================================== XMA

IMPORT(XMACreateContext) {
    uint32_t ctx_out = ARG(0);
    uint32_t flags = ARG(1);
    if (ctx_out) {
        // XMA context is a large structure (~0x1000+). Allocate real guest
        // memory so the game can write/track it.
        uint32_t xc = GuestMemory::Get().PoolAlloc(0x1200, 0x100);
        if (xc) GuestMemset(xc, 0, 0x1200);
        StoreU32(ctx_out, xc);
    }
    RET(0);
}

IMPORT(XMAReleaseContext) {
    uint32_t xma_ctx = ARG(0);
    if (xma_ctx) GuestMemory::Get().PoolFree(xma_ctx);
    RET(0);
}

// =============================================================== XeCrypt

IMPORT(XeCryptShaInit) {
    uint32_t sha_ctx = ARG(0);
    // SHA1 context init: zero (game-side state machine handles the rest).
    GuestMemset(sha_ctx, 0, 0x60);
    RET(0);
}

IMPORT(XeCryptShaUpdate) {
    uint32_t sha_ctx = ARG(0);
    uint32_t data = ARG(1);
    uint32_t len = ARG(2);
    RET(0);
}

IMPORT(XeCryptShaFinal) {
    uint32_t sha_ctx = ARG(0);
    uint32_t digest = ARG(1);
    RET(0);
}

IMPORT(XeCryptSha) {
    // (src1, len1, src2, len2, src3, len3, digest) — one-shot SHA1.
    uint32_t src1 = ARG(0), len1 = ARG(1);
    uint32_t src2 = ARG(2), len2 = ARG(3);
    uint32_t src3 = ARG(4), len3 = ARG(5);
    uint32_t digest = ARG(6);
    RET(0);
}

IMPORT(XeCryptMd5Init) { uint32_t m = ARG(0); GuestMemset(m, 0, 0x58); RET(0); }
IMPORT(XeCryptMd5Update) { RET(0); }
IMPORT(XeCryptMd5Final) { RET(0); }

IMPORT(XeKeysConsolePrivateKeySign) { RET(0x87D00005u); }
IMPORT(XeKeysConsoleSignatureVerification) { RET(0x87D00005u); }

// =============================================================== network

IMPORT(NetDll_WSAStartup) {
    uint32_t version = ARG(0);
    uint32_t data = ARG(1);
    RET(0);
}
IMPORT(NetDll_WSACleanup) { RET(0); }
IMPORT(NetDll_WSAGetLastError) { RET(0); }
IMPORT(NetDll_socket) { RET(0xFFFFFFFF); }        // no socket (offline)
IMPORT(NetDll_closesocket) { RET(0); }
IMPORT(NetDll_connect) { RET(0xFFFFFFFF); }
IMPORT(NetDll_select) { RET(0); }
IMPORT(NetDll_send) { RET(0xFFFFFFFF); }
IMPORT(NetDll_recv) { RET(0xFFFFFFFF); }
IMPORT(NetDll_getsockopt) { RET(0); }
IMPORT(NetDll_setsockopt) { RET(0); }
IMPORT(NetDll_ioctlsocket) { RET(0); }
IMPORT(NetDll_XNetGetTitleXnAddr) {
    if (ARG(1)) StoreU32(ARG(1), 0);
    RET(0);
}
IMPORT(NetDll_XNetXnAddrToInAddr) { RET(0); }
IMPORT(NetDll_XNetXnAddrToMachineId) { RET(0); }
IMPORT(NetDll_XNetServerToInAddr) { RET(0); }
IMPORT(NetDll_XNetRandom) {
    uint32_t buf = ARG(0);
    uint32_t len = ARG(1);
    for (uint32_t i = 0; i < len; i++) StoreU8(buf + i, (uint8_t)(rand() & 0xFF));
    RET(0);
}

// =============================================================== XGet*

IMPORT(XGetAVPack) { RET(6); }        // standard A/V pack
IMPORT(XGetGameRegion) { RET(0xFFFFFFFFu); }  // all regions
IMPORT(XGetLanguage) { RET(1); }      // English

// =============================================================== Rtl/misc

IMPORT(RtlInitAnsiString) {
    // (ANSI_STRING*, char*)
    uint32_t str_ptr = ARG(0);
    uint32_t src = ARG(1);
    uint32_t len = 0;
    if (src) {
        std::string s = GuestAnsiString(src, 1024);
        len = (uint32_t)s.size();
    }
    StoreU16(str_ptr, (uint16_t)len);
    StoreU16(str_ptr + 2, (uint16_t)((len + 4) & ~3u));
    StoreU32(str_ptr + 4, src);
}

IMPORT(RtlNtStatusToDosError) {
    uint32_t status = ARG(0);
    // Map common NTSTATUS -> DOS error (game uses for error reporting).
    uint32_t dos = 0;
    switch (status) {
    case X_STATUS_NO_SUCH_FILE:
    case X_STATUS_OBJECT_NAME_NOT_FOUND:
    case X_STATUS_OBJECT_PATH_NOT_FOUND: dos = 2; break;   // ERROR_FILE_NOT_FOUND
    case X_STATUS_ACCESS_DENIED: dos = 5; break;
    case X_STATUS_INVALID_HANDLE: dos = 6; break;
    case X_STATUS_NO_MEMORY: dos = 8; break;
    default: dos = 0; break;
    }
    RET(dos);
}

IMPORT(RtlComputeCrc32) {
    // (partial, data_ptr, len)
    uint32_t partial = ARG(0);
    uint32_t data = ARG(1);
    uint32_t len = ARG(2);
    uint32_t crc = ~partial;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= LoadU8(data + i);
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -((crc & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    RET(~crc);
}

IMPORT(RtlCompareMemoryUlong) {
    // (ptr, len, pattern)
    uint32_t ptr = ARG(0);
    uint32_t len = ARG(1);
    uint32_t pattern = ARG(2);
    uint32_t matched = 0;
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        if (LoadU32(ptr + i) != pattern) break;
        matched += 4;
    }
    RET(matched);
}

IMPORT(RtlFillMemoryUlong) {
    uint32_t ptr = ARG(0);
    uint32_t len = ARG(1);
    uint32_t value = ARG(2);
    for (uint32_t i = 0; i + 4 <= len; i += 4) StoreU32(ptr + i, value);
}

IMPORT(RtlCompareStringN) {
    uint32_t s1 = ARG(0);  // STRING*
    uint32_t s2 = ARG(1);
    uint32_t len = ARG(2);
    std::string a = GuestAnsiString(LoadU32(s1 + 4), LoadU16(s1));
    std::string b = GuestAnsiString(LoadU32(s2 + 4), LoadU16(s2));
    uint32_t n = std::min((uint32_t)a.size(), std::min((uint32_t)b.size(), len));
    int r = memcmp(a.data(), b.data(), n);
    RET(r < 0 ? 0xFFFFFFFFu : (r > 0 ? 1 : 0));
}

IMPORT(RtlUpcaseUnicodeChar) {
    uint32_t ch = ARG(0);
    RET((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
}

IMPORT(RtlMultiByteToUnicodeN) {
    // (unicode_dst, dst_max, bytes_written, ansi_src, src_len)
    uint32_t dst = ARG(0);
    uint32_t dst_max = ARG(1);
    uint32_t written_ptr = ARG(2);
    uint32_t src = ARG(3);
    uint32_t src_len = ARG(4);
    uint32_t out = 0;
    for (uint32_t i = 0; i < src_len && out + 2 <= dst_max; i++) {
        StoreU16(dst + out, (uint16_t)LoadU8(src + i));
        out += 2;
    }
    if (written_ptr) StoreU32(written_ptr, out);
    RET(0);
}

IMPORT(RtlUnicodeToMultiByteN) {
    uint32_t dst = ARG(0);
    uint32_t dst_max = ARG(1);
    uint32_t written_ptr = ARG(2);
    uint32_t src = ARG(3);
    uint32_t src_len = ARG(4);
    uint32_t out = 0;
    for (uint32_t i = 0; i + 1 < src_len && out < dst_max; i += 2) {
        uint16_t ch = LoadU16(src + i);
        StoreU8(dst + out, ch < 0x100 ? (uint8_t)ch : '?');
        out++;
    }
    if (written_ptr) StoreU32(written_ptr, out);
    RET(0);
}

static void GuestTimeToFields(uint64_t t_100ns, uint32_t fields_ptr) {
    // Guest time: 100ns since 1601. Convert to SYSTEMTIME fields (u16 each).
    uint64_t secs1601 = t_100ns / 10000000ull;
    uint64_t days = secs1601 / 86400;
    uint64_t rem = secs1601 % 86400;
    // Days since 1601-01-01 -> civil date (Howard Hinnant's algorithm).
    uint64_t z = days + 719468 + 719162;  // shift epoch 1601 -> 1970 calc
    z = days + 719468;
    uint64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = z - era * 146097;
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t y = yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;
    StoreU16(fields_ptr + 0x00, (uint16_t)(y + 1601));
    StoreU16(fields_ptr + 0x02, (uint16_t)m);
    StoreU16(fields_ptr + 0x04, (uint16_t)(days >= 0 ? ((days + 3) % 7) : 0));  // dow
    StoreU16(fields_ptr + 0x06, (uint16_t)d);
    StoreU16(fields_ptr + 0x08, (uint16_t)(rem / 3600));
    StoreU16(fields_ptr + 0x0A, (uint16_t)((rem / 60) % 60));
    StoreU16(fields_ptr + 0x0C, (uint16_t)(rem % 60));
    StoreU16(fields_ptr + 0x0E, (uint16_t)((t_100ns / 10000) % 1000));
    StoreU16(fields_ptr + 0x10, 0);
}

static uint64_t GuestSystemTime100ns() {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100ull +
           116444736000000000ull;
}

IMPORT(RtlTimeToTimeFields) {
    uint32_t time_ptr = ARG(0);
    uint32_t fields_ptr = ARG(1);
    GuestTimeToFields(LoadU64(time_ptr), fields_ptr);
}

IMPORT(RtlTimeFieldsToTime) {
    uint32_t fields_ptr = ARG(0);
    uint32_t time_ptr = ARG(1);
    // Inverse conversion (approximate days calc).
    uint16_t y = LoadU16(fields_ptr + 0), mo = LoadU16(fields_ptr + 2);
    uint16_t d = LoadU16(fields_ptr + 6), h = LoadU16(fields_ptr + 8);
    uint16_t mi = LoadU16(fields_ptr + 10), s = LoadU16(fields_ptr + 12);
    uint16_t ms = LoadU16(fields_ptr + 14);
    // days since 1601 (approx via unix mk on year/mo/d)
    uint64_t years = (y > 1601) ? y - 1601 : 0;
    uint64_t days = years * 365 + years / 4 - years / 100 + years / 400;
    static const uint16_t cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243,
                                     273, 304, 334};
    days += mo >= 1 && mo <= 12 ? cum[mo - 1] : 0;
    days += d - 1;
    uint64_t secs = days * 86400 + h * 3600 + mi * 60 + s;
    StoreU64(time_ptr, secs * 10000000ull + ms * 10000ull);
    RET(1);
}

IMPORT(RtlRaiseException) {
    uint32_t record_ptr = ARG(0);
    uint32_t frame_ptr = ARG(1);
    uint32_t code = LoadU32(record_ptr);
    PRLOG(Exception, "RtlRaiseException(code=%08X frame=%08X)", code, frame_ptr);
    // Exception dispatching to __C_specific_handler is guest-driven; for
    // Phase 2B propagate as guest unwind of the current thread and let the
    // game's handler chain decide (implemented as thread-exit diagnostic).
    GuestThread* t = GuestThread::GetCurrent();
    if (t) t->Exit(0xE6DEAD00 | (code & 0xFFFF));
    RET(0);
}

IMPORT(RtlUnwind) {
    // (frame_ptr, target_ip, record, return_value) — Xenia: non-returning on
    // real HW. The recompiled caller chain unwinds to the thread boundary.
    uint32_t frame = ARG(0);
    uint32_t target_ip = ARG(1);
    uint32_t record = ARG(2);
    uint32_t retval = ARG(3);
    PRLOG(Exception, "RtlUnwind(frame=%08X target=%08X record=%08X ret=%08X)",
          frame, target_ip, record, retval);
    GuestUnwind uw(target_ip ? target_ip : 0);
    uw.r[0] = retval;
    throw uw;
}

IMPORT(__C_specific_handler) {
    PRLOGW("__C_specific_handler reached via import (expect guest-internal "
           "handling)");
    RET(0);
}

IMPORT(DbgPrint) {
    uint32_t fmt_ptr = ARG(0);
    if (fmt_ptr) {
        // Format string is ANSI guest. Print raw (varargs unpacking of PPC
        // regs is deferred; print the template).
        std::string s = GuestAnsiString(fmt_ptr, 512);
        PRLOG(Import, "DbgPrint: %s", s.c_str());
    }
    RET(0);
}

IMPORT(sprintf) {
    // (dst, fmt, ...) — guest-ANSI sprintf with r5+ varargs.
    uint32_t dst = ARG(0);
    uint32_t fmt = ARG(1);
    std::string f = GuestAnsiString(fmt, 512);
    // Handle common subset: %s %d %u %x %c %%. r5..r10 = 6 regs + stack.
    uint32_t argv[16];
    int argc = 0;
    for (int i = 2; i <= 9; i++) argv[argc++] = ARG(i);
    for (int i = 0; i < 6; i++) argv[argc++] = StackArg(ctx, i);

    std::string out;
    size_t ai = 0;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) { out.push_back(f[i]); continue; }
        i++;
        char c = f[i];
        if (c == '%') { out.push_back('%'); continue; }
        if (c == 's') {
            if (ai < argc && argv[ai]) out += GuestAnsiString(argv[ai], 512);
            ai++;
        } else if (c == 'd' || c == 'i') {
            if (ai < argc) out += std::to_string((int32_t)argv[ai]);
            ai++;
        } else if (c == 'u') {
            if (ai < argc) out += std::to_string(argv[ai]);
            ai++;
        } else if (c == 'x' || c == 'X') {
            char buf[16];
            if (ai < argc) {
                snprintf(buf, sizeof(buf), c == 'x' ? "%x" : "%X", argv[ai]);
                out += buf;
            }
            ai++;
        } else if (c == 'c') {
            if (ai < argc) out.push_back((char)argv[ai]);
            ai++;
        } else {
            out.push_back('%');
            out.push_back(c);
        }
    }
    GuestWriteAnsiString(dst, out);
    RET((uint32_t)out.size());
}

static int GuestVsnprintfCore(PPCContext& ctx, uint32_t dst, uint32_t count,
                              uint32_t fmt, uint32_t varargs_base) {
    // varargs_base = index (2-based) of first vararg in ARG().
    std::string f = GuestAnsiString(fmt, 512);
    uint32_t argv[16];
    int argc = 0;
    for (int i = varargs_base; i <= 9; i++) argv[argc++] = ARG(i);
    for (int i = 0; i < 6; i++) argv[argc++] = StackArg(ctx, i);
    std::string out;
    size_t ai = 0;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) { out.push_back(f[i]); continue; }
        i++;
        char c = f[i];
        if (c == '%') { out.push_back('%'); continue; }
        if (c == 's') {
            if (ai < argc && argv[ai]) out += GuestAnsiString(argv[ai], 512);
            ai++;
        } else if (c == 'd' || c == 'i') {
            if (ai < argc) out += std::to_string((int32_t)argv[ai]);
            ai++;
        } else if (c == 'u') {
            if (ai < argc) out += std::to_string(argv[ai]);
            ai++;
        } else if (c == 'x' || c == 'X') {
            char buf[16];
            if (ai < argc) {
                snprintf(buf, sizeof(buf), c == 'x' ? "%x" : "%X", argv[ai]);
                out += buf;
            }
            ai++;
        } else if (c == 'c') {
            if (ai < argc) out.push_back((char)argv[ai]);
            ai++;
        } else if (c == 'f') {
            // Double vararg: PPC passes doubles in f1+; approximation via
            // GPR pair.
            if (ai + 1 < argc) {
                uint64_t v = ((uint64_t)argv[ai] << 32) | argv[ai + 1];
                double d;
                memcpy(&d, &v, 8);
                char buf[64];
                snprintf(buf, sizeof(buf), "%f", d);
                out += buf;
            }
            ai += 2;
        } else {
            out.push_back('%');
            out.push_back(c);
        }
    }
    if (count) {
        uint32_t n = std::min<uint32_t>(count - 1, (uint32_t)out.size());
        GuestWrite(dst, out.data(), n);
        StoreU8(dst + n, 0);
    }
    return (int)out.size();
}

IMPORT(_snprintf) {
    // (dst, count, fmt, ...)
    uint32_t dst = ARG(0);
    uint32_t count = ARG(1);
    uint32_t fmt = ARG(2);
    RET((uint32_t)GuestVsnprintfCore(ctx, dst, count, fmt, 3));
}

IMPORT(_vsnprintf) {
    // (dst, count, fmt, va_list) — va_list points to reg save area in the
    // guest frame. Approximate: treat va_list as guest ptr to u32 sequence.
    uint32_t dst = ARG(0);
    uint32_t count = ARG(1);
    uint32_t fmt = ARG(2);
    uint32_t va_ptr = ARG(3);
    std::string f = GuestAnsiString(fmt, 512);
    std::string out;
    uint32_t argv[16];
    int argc = 0;
    if (va_ptr) {
        // Read up to 16 u32s from the va_list area.
        for (int i = 0; i < 16; i++) argv[argc++] = LoadU32(va_ptr + i * 4);
    }
    size_t ai = 0;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) { out.push_back(f[i]); continue; }
        i++;
        char c = f[i];
        if (c == 's') {
            if (ai < argc && argv[ai]) out += GuestAnsiString(argv[ai], 512);
            ai++;
        } else if (c == 'd' || c == 'i') {
            if (ai < argc) out += std::to_string((int32_t)argv[ai]);
            ai++;
        } else if (c == 'u') {
            if (ai < argc) out += std::to_string(argv[ai]);
            ai++;
        } else if (c == 'x' || c == 'X') {
            char b[16];
            if (ai < argc) {
                snprintf(b, sizeof(b), c == 'x' ? "%x" : "%X", argv[ai]);
                out += b;
            }
            ai++;
        } else {
            out.push_back('%');
            out.push_back(c);
        }
    }
    if (count) {
        uint32_t n = std::min<uint32_t>(count - 1, (uint32_t)out.size());
        GuestWrite(dst, out.data(), n);
        StoreU8(dst + n, 0);
    }
    RET((uint32_t)out.size());
}

// =============================================================== STFS

IMPORT(StfsCreateDevice) {
    // (name, mount_path, magic?, device_out) — game builds its own STFS
    // device over \Device\Cdrom0 or cache. We register the device object.
    uint32_t name_ptr = ARG(0);
    uint32_t device_out = ARG(3);
    std::string name = GuestAnsiString(name_ptr);
    auto* dev = new KernelState::DeviceObject();
    dev->name = "\\Device\\" + name;
    dev->type = 4;
    dev->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x100, 8);
    dev->size = 0x100;
    GuestMemset(dev->guest_addr, 0, 0x100);
    StoreU32(dev->guest_addr + 0x00, 4);
    StoreU32(dev->guest_addr + 0x04, 0x100);
    {
        std::lock_guard<std::mutex> lk(K().device_mutex);
        K().devices[dev->name] = dev;
    }
    if (device_out) StoreU32(device_out, dev->guest_addr);
    PRLOG(Filesystem, "StfsCreateDevice('%s') = %08X", dev->name.c_str(),
          dev->guest_addr);
    RET(0);
}

IMPORT(StfsControlDevice) {
    uint32_t device = ARG(0);
    PRLOGW("StfsControlDevice(%08X) — control passthrough", device);
    RET(X_STATUS_SUCCESS);
}

// =============================================================== Xex*

IMPORT(XexGetModuleHandle) {
    // (name_ptr, handle_ptr) — NULL name = executable module.
    uint32_t name_ptr = ARG(0);
    uint32_t handle_ptr = ARG(1);
    if (handle_ptr) StoreU32(handle_ptr, K().var_module_handle_pp);
    RET(X_STATUS_SUCCESS);
}

IMPORT(XexGetProcedureAddress) {
    // (module_handle, ordinal_or_name, out)
    uint32_t module_handle = ARG(0);
    uint32_t proc = ARG(1);
    uint32_t out = ARG(2);
    if (out) StoreU32(out, 0);
    PRLOGW("XexGetProcedureAddress(module=%08X proc=%08X) — not found",
           module_handle, proc);
    RET(0xC0000139u);  // STATUS_ENTRYPOINT_NOT_FOUND
}

IMPORT(XexLoadImageHeaders) { RET(0xC0000135u); }  // module not found
IMPORT(XexCheckExecutablePrivilege) { RET(1); }

IMPORT(RtlImageXexHeaderField) {
    // (xex_header_ptr, field_key) -> value. The game derives the header via:
    // slot[XexExecutableModuleHandle] -> PP -> *PP = module_struct ->
    // *(module_struct + 0x58) = xex header base.
    uint32_t xex_header = ARG(0);
    uint32_t field = ARG(1);
    uint32_t value = 0;
    if (xex_header) {
        uint32_t header_count = LoadU32(xex_header + 0x14);
        for (uint32_t i = 0; i < header_count; i++) {
            uint32_t key = LoadU32(xex_header + 0x18 + i * 8);
            uint32_t val = LoadU32(xex_header + 0x18 + i * 8 + 4);
            if (key == field) { value = val; break; }
        }
    }
    PRLOG(Import, "RtlImageXexHeaderField(xexhdr=%08X field=%08X) = %08X",
          xex_header, field, value);
    RET(value);
}

// =============================================================== voice

IMPORT(XamVoiceCreate) { RET(0); }
IMPORT(XamVoiceClose) { RET(0); }
IMPORT(XamVoiceHeadsetPresent) { RET(0); }
IMPORT(XamVoiceSubmitPacket) { RET(0); }

// =============================================================== host hooks

void __cxa_throw_guard() {}  // (never referenced; placeholder)


