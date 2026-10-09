// PrototypeRecomp Phase 2B runtime — filesystem imports: NtCreateFile,
// NtReadFile, directory enumeration, overlapped I/O.
#include "state.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace pr;

// ============================================================ request audit
// Content-pipeline diagnostics: dedup by (op, guest path) with counters only
// (disk-safe). The watchdog dumps a compact summary every 20 s; individual
// missing paths additionally report once via LogLineOnce.
struct FsReqStat {
    uint64_t count = 0;
    uint64_t ok = 0;
    uint64_t fail = 0;
};
static std::mutex g_fs_req_mtx;
static std::map<std::string, FsReqStat> g_fs_reqs;

static void FsAudit(const char* op, const std::string& path, bool ok) {
    std::lock_guard<std::mutex> lk(g_fs_req_mtx);
    std::string key = std::string(op) + "|" + path;
    auto& s = g_fs_reqs[key];
    s.count++;
    if (ok) s.ok++; else s.fail++;
}

namespace pr { void FsAuditDump() {
    std::lock_guard<std::mutex> lk(g_fs_req_mtx);
    if (g_fs_reqs.empty()) return;
    uint64_t total = 0, fails = 0;
    for (auto& [k, s] : g_fs_reqs) { total += s.count; fails += s.fail; }
    LogLine(LogCategory::kFilesystem,
            "FS audit: %zu unique requests, %llu ops, %llu failed",
            g_fs_reqs.size(), (unsigned long long)total,
            (unsigned long long)fails);
    // Top 12 by count.
    std::vector<std::pair<uint64_t, const std::string*>> by_count;
    for (auto& [k, s] : g_fs_reqs) by_count.push_back({s.count, &k});
    std::sort(by_count.begin(), by_count.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < by_count.size() && i < 12; i++) {
        const std::string& k = *by_count[i].second;
        auto& s = g_fs_reqs[k];
        LogLine(LogCategory::kFilesystem,
                "  FS top[%zu] %s  ok=%llu fail=%llu", i, k.c_str(),
                (unsigned long long)s.ok, (unsigned long long)s.fail);
    }
    // First failed (missing) resources — the content-gap report.
    for (auto& [k, s] : g_fs_reqs) {
        if (s.fail && s.ok == 0) {
            LogLine(LogCategory::kFilesystem,
                    "  FS MISSING: %s (x%llu)", k.c_str(),
                    (unsigned long long)s.fail);
        }
    }
}
}  // namespace pr

#include "args.h"
#define IMPORT(name) \
    void __imp__##name(PPCContext& ctx, uint8_t* base)
#define RET(v) do { ctx.r3.u64 = (uint64_t)(uint32_t)(v); return; } while (0)

// =============================================================== file objects

struct GuestFile : KernelObject {
    int host_fd = -1;
    std::string guest_path;      // e.g. \Device\Cdrom0\args.txt
    std::string fs_path;         // host path
    bool is_directory = false;
    bool is_device = false;      // \Device\Cdrom0 itself
    // Raw HDD volume open (\Device\Harddisk0\Partition0, \Cache0/1):
    // Xenia registers these as NullDevice — "cache/STFC code baked into
    // games tries reading/writing to these"; all IO succeeds, reads return
    // the caller's own buffer contents. The ATG thread framework reads a
    // 1024-byte owner block and checks a 'Josh' signature; on clean
    // hardware it reads zeros.
    bool is_null_volume = false;
    // Partition0 (the SYSTEM partition) persists across boots on real
    // hardware: the ATG framework's owner-block database lives there.
    // Backed by a real host file so written data reads back persistently.
    int raw_fd = -1;
    uint64_t position = 0;
    // Directory enumeration state.
    std::vector<std::string> dir_entries;
    size_t dir_index = 0;
    uint32_t dir_info_class = 0;
    uint32_t dir_file_class = 0;
    // Overlapped state.
    uint32_t apc_routine = 0;
    uint32_t apc_context = 0;
    uint32_t event_handle = 0;
};

// ---------------------------------------------------------------- path logic
// Xbox NT paths: \Device\Cdrom0\foo or \??\D:\foo or D:\foo.
// Symbolic links map "D:" -> \Device\Cdrom0 (game registers this itself via
// ObCreateSymbolicLink in its STFS device setup; we also accept a default).

static std::string NormalizeGuestPath(uint32_t obj_attrs_ptr, bool unicode) {
    // OBJECT_ATTRIBUTES: { u32 root_dir, u32 object_name, u32 attrs }
    // STRING/UNICODE_STRING: { u16 len, u16 maxlen, u32 buffer }
    if (!obj_attrs_ptr) return "";
    uint32_t name_ptr = LoadU32(obj_attrs_ptr + 4);
    if (!name_ptr) return "";
    uint16_t len = LoadU16(name_ptr);
    uint32_t buf = LoadU32(name_ptr + 4);
    std::string raw;
    if (unicode) {
        // UTF-16BE guest -> UTF-8.
        for (uint16_t i = 0; i + 1 < len; i += 2) {
            uint16_t ch = LoadU16(buf + i);
            if (ch < 0x80) raw.push_back((char)ch);
            else if (ch < 0x800) {
                raw.push_back((char)(0xC0 | (ch >> 6)));
                raw.push_back((char)(0x80 | (ch & 0x3F)));
            } else {
                raw.push_back((char)(0xE0 | (ch >> 12)));
                raw.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
                raw.push_back((char)(0x80 | (ch & 0x3F)));
            }
        }
    } else {
        for (uint16_t i = 0; i < len; i++) raw.push_back((char)LoadU8(buf + i));
    }
    return raw;
}

// Resolve symbolic link prefixes (\??\D:, GAME:\, ...). Xbox drive letters
// are CASE-INSENSITIVE, so prefixes match lowercased.
static std::string ApplySymbolicLinks(const std::string& raw) {
    std::string p = raw;
    // Strip \??\ prefix.
    if (p.rfind("\\??\\", 0) == 0) p = p.substr(4);
    std::string p_low = p;
    for (auto& c : p_low) if (c >= 'A' && c <= 'Z') c += 32;
    // Try full-prefix symbolic links, longest first.
    std::lock_guard<std::mutex> lk(K().symlink_mutex);
    std::map<size_t, std::string> matches;
    for (auto& [link, target] : K().symbolic_links) {
        std::string l = link;
        // Symbolic links may be like "D:" or "\??\D:".
        if (l.rfind("\\??\\", 0) == 0) l = l.substr(4);
        std::string l_low = l;
        for (auto& c : l_low) if (c >= 'A' && c <= 'Z') c += 32;
        if (p_low.rfind(l_low, 0) == 0) {
            matches[l.size()] = target + p.substr(l.size());
        }
    }
    if (!matches.empty()) return matches.rbegin()->second;
    return p;
}

// Map an Xbox device path to a host path under fs_root.
static bool MapToHost(const std::string& device_path, std::string* out) {
    std::string p = device_path;
    // \Device\Harddisk0\Partition0..3 (with backslash, any case) are the
    // writable HDD cache partitions — they map under hdd_root, NOT the
    // read-only disc. Every retail console ships them.
    {
        std::string p_low = p;
        for (auto& c : p_low)
            if (c >= 'A' && c <= 'Z') c += 32;
        for (int part = 0; part < 4; part++) {
            std::string pref = "\\device\\harddisk0\\partition" +
                               std::to_string(part);
            std::string pref2 = "\\device\\harddisk0partition" +
                                std::to_string(part);
            if (p_low.rfind(pref, 0) == 0) {
                p = p.substr(pref.size());
                while (!p.empty() && (p[0] == '\\' || p[0] == '/'))
                    p = p.substr(1);
                std::replace(p.begin(), p.end(), '\\', '/');
                if (K().hdd_root.empty()) return false;
                *out = K().hdd_root + "/partition" + std::to_string(part) +
                       (p.empty() ? "" : "/" + p);
                return true;
            }
            if (p_low.rfind(pref2, 0) == 0) {
                p = p.substr(pref2.size());
                while (!p.empty() && (p[0] == '\\' || p[0] == '/'))
                    p = p.substr(1);
                std::replace(p.begin(), p.end(), '\\', '/');
                if (K().hdd_root.empty()) return false;
                *out = K().hdd_root + "/partition" + std::to_string(part) +
                       (p.empty() ? "" : "/" + p);
                return true;
            }
        }
    }
    // \Device\Cdrom0\xxx -> <fs_root>/xxx
    if (p.rfind("\\Device\\Cdrom0", 0) == 0) {
        p = p.substr(strlen("\\Device\\Cdrom0"));
    } else if (p.rfind("\\Device\\Harddisk0Partition1", 0) == 0) {
        p = p.substr(strlen("\\Device\\Harddisk0Partition1"));
    } else if (p.rfind("\\Device\\Harddisk0Partition0", 0) == 0) {
        // Data partition where applicable.
        p = p.substr(strlen("\\Device\\Harddisk0Partition0"));
    }
    while (!p.empty() && (p[0] == '\\' || p[0] == '/')) p = p.substr(1);
    std::replace(p.begin(), p.end(), '\\', '/');
    *out = K().fs_root.empty() ? "" : (K().fs_root + "/" + p);
    return !K().fs_root.empty() || p.empty();
}

// Xbox file systems (XGD/XGDF and the NT object namespace) are
// CASE-INSENSITIVE. A host disc dump may not match the game's requested
// casing exactly, so resolve component-by-component with a case-insensitive
// directory scan when the direct stat() misses.
static std::string ToLowerAscii(const std::string& s) {
    std::string r = s;
    for (auto& c : r) if (c >= 'A' && c <= 'Z') c += 32;
    return r;
}

static bool ResolveHostPathCaseInsensitive(const std::string& host,
                                            std::string* out) {
    struct stat st {};
    if (stat(host.c_str(), &st) == 0) { *out = host; return true; }
    // Walk components from the root, matching case-insensitively.
    size_t slash = host.find('/');
    std::string cur = host.substr(0, slash == std::string::npos
                                         ? host.size() : slash);
    if (stat(cur.c_str(), &st) != 0) return false;
    size_t pos = (slash == std::string::npos) ? host.size() : slash;
    while (pos < host.size()) {
        size_t next = host.find('/', pos + 1);
        if (next == std::string::npos) next = host.size();
        std::string comp = host.substr(pos + 1, next - pos - 1);
        if (comp.empty()) { pos = next; continue; }
        std::string want = ToLowerAscii(comp);
        std::string found;
        DIR* d = opendir(cur.c_str());
        if (d) {
            struct dirent* de;
            while ((de = readdir(d))) {
                if (ToLowerAscii(de->d_name) == want) {
                    found = de->d_name;
                    break;
                }
            }
            closedir(d);
        }
        if (found.empty()) return false;   // component missing entirely
        cur += "/" + found;
        pos = next;
    }
    if (stat(cur.c_str(), &st) == 0) { *out = cur; return true; }
    return false;
}

static uint32_t OpenHostFile(const std::string& host_path, bool write,
                             bool* is_dir, bool* exists) {
    *is_dir = false;
    *exists = false;
    struct stat st {};
    if (stat(host_path.c_str(), &st) != 0) return -1;
    *exists = true;
    if (S_ISDIR(st.st_mode)) { *is_dir = true; return -1; }
    int flags = write ? (O_RDWR | O_CREAT) : O_RDONLY;
    int fd = open(host_path.c_str(), flags, 0644);
    return fd;
}

// =============================================================== NtCreateFile

IMPORT(NtCreateFile) {
    // (handle_out, desired_access, obj_attrs, io_status, alloc_size,
    //  file_attrs, share_access, create_disp, options, ea_buffer, ea_size)
    // r3=handle_out r4=desired_access r5=obj_attrs r6=io_status_block
    // r7=alloc_size r8=file_attrs r9=share_access r10=create_disp
    // stack: options etc.
    uint32_t handle_ptr = ARG(0);
    uint32_t desired_access = ARG(1);
    uint32_t obj_attrs = ARG(2);
    uint32_t io_status = ARG(3);
    uint32_t create_disp = ARG(7);

    std::string raw = NormalizeGuestPath(obj_attrs, false);
    std::string device_path = ApplySymbolicLinks(raw);

    PRLOG(Filesystem, "NtCreateFile('%s') disp=%u", raw.c_str(), create_disp);
    FsAudit("create", device_path, false);   // updated below on success

    // Device opens (\Device\Cdrom0 or \Device\*) succeed as device objects.
    // A trailing backslash (\Device\Cdrom0\ — the VOLUME ROOT) is NOT a
    // device open; it falls through and opens fs_root as a directory.
    // The entire raw HDD namespace (\Device\Harddisk0\Partition0..3,
    // \Cache0, \Cache1 and everything under them) is Xenia NullDevice
    // territory: "Cache/STFC code baked into games tries reading/writing to
    // these. By using a NullDevice that just returns success to all IO
    // requests it should allow games to believe cache/raw disk was accessed
    // successfully." The volume root itself (exact path) is the RAW volume
    // (ioctls answer geometry/partition info); the root with a trailing
    // backslash opens as a directory; subpaths are null files.
    // Returns: 0 = not null-namespace, 1 = raw volume, 2 = volume root dir,
    // 3 = null file/dir under the volume.
    auto hdd_null_kind = [](const std::string& device_path) -> int {
        std::string p_low = device_path;
        for (auto& c : p_low)
            if (c >= 'A' && c <= 'Z') c += 32;
        static const char* kVols[] = {
            "\\device\\harddisk0\\partition0",
            "\\device\\harddisk0\\partition1",
            "\\device\\harddisk0\\partition2",
            "\\device\\harddisk0\\partition3",
            "\\device\\harddisk0\\cache0",
            "\\device\\harddisk0\\cache1",
            "\\device\\harddisk0partition0",
            "\\device\\harddisk0partition1",
            "\\device\\harddisk0partition2",
            "\\device\\harddisk0partition3",
        };
        for (const char* vol : kVols) {
            std::string v = vol;
            if (p_low == v) return 1;
            std::string vslash = v + "\\";
            if (p_low == vslash) return 2;
            if (p_low.rfind(vslash, 0) == 0) return 3;
        }
        return 0;
    };
    bool is_device = device_path.rfind("\\Device\\", 0) == 0 &&
                     device_path.find('\\', 8) == std::string::npos;

    auto* f = new GuestFile();
    f->guest_path = device_path;
    f->type = kObjTypeFile;

    int null_kind = hdd_null_kind(device_path);
    if (null_kind) {
        f->is_null_volume = (null_kind == 1 || null_kind == 3);
        f->is_directory = (null_kind == 2);
        f->fs_path = "(null-hdd)";
        f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
        f->size = 0x40;
        // Partition0 = the persistent SYSTEM partition on real hardware
        // (the ATG owner-block database). Give it a real backing file so
        // writes persist and later reads observe them — exactly like the
        // console. Cache0/Cache1 are scratch (reformatted every boot by
        // XMountUtilityDrive) and stay pure null volumes.
        if (null_kind == 1 && !K().hdd_root.empty()) {
            std::string p_low = device_path;
            for (auto& c : p_low)
                if (c >= 'A' && c <= 'Z') c += 32;
            if (p_low == "\\device\\harddisk0\\partition0" ||
                p_low == "\\device\\harddisk0partition0") {
                std::string img = K().hdd_root + "/partition0.img";
                f->raw_fd = open(img.c_str(), O_RDWR | O_CREAT, 0644);
                if (f->raw_fd >= 0) {
                    // 2 MiB persistent system-partition image.
                    ftruncate(f->raw_fd, 0x200000);
                    f->fs_path = img;
                }
            }
        }
        uint32_t h = K().objects.NewHandle(f);
        if (handle_ptr) StoreU32(handle_ptr, h);
        if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
        FsAudit("create", device_path, true);
        PRLOG(Filesystem, "NtCreateFile: null-hdd kind=%d '%s' -> handle %08X (rawfd=%d)",
              null_kind, device_path.c_str(), h, f->raw_fd);
        RET(X_STATUS_SUCCESS);
    }

    if (is_device) {
        f->is_device = true;
        f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
        f->size = 0x40;
        uint32_t h = K().objects.NewHandle(f);
        if (handle_ptr) StoreU32(handle_ptr, h);
        if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
        PRLOG(Filesystem, "NtCreateFile: device '%s' -> handle %08X",
              device_path.c_str(), h);
        RET(X_STATUS_SUCCESS);
    }

    std::string host;
    MapToHost(device_path, &host);
    std::string resolved = host;
    bool case_fixed = ResolveHostPathCaseInsensitive(host, &resolved);
    if (case_fixed) host = resolved;
    bool is_dir = false, exists = false;
    int fd = OpenHostFile(host, (desired_access & 0xC0000000u) != 0, &is_dir,
                          &exists);
    if (!exists) {
        // Report the FIRST missing content resource once (the content-gap
        // signal): rate-limited, never spammed.
        LogLineOnce(LogCategory::kFilesystem,
                    "CONTENT MISSING: '%s' (host '%s') — required by guest",
                    device_path.c_str(), host.c_str());
    }
    if (fd < 0 && !is_dir) {
        // Xbox NT create disposition (Xenia kernel_types.h) — NOT the Win32
        // CreateFile mapping:
        //   0 = FILE_SUPERSEDED   (create/truncate)
        //   1 = FILE_OPEN         (fail if missing)
        //   2 = FILE_CREATE       (fail if exists)
        //   3 = FILE_OPEN_IF      (open or create)
        //   4 = FILE_OVERWRITE    (fail if missing, truncate)
        //   5 = FILE_OVERWRITE_IF (open or create, truncate)
        // The disc volume (\Device\Cdrom0) is READ-ONLY media: creation on it
        // must fail — never fabricate content that the real dump does not
        // contain (the game probes e.g. skuinfo.p3d.rz and must observe its
        // absence so it falls back to the plain file).
        bool on_disc = device_path.rfind("\\Device\\Cdrom0", 0) == 0;
        bool create_if_missing =
            (create_disp == 0 || create_disp == 2 || create_disp == 3 ||
             create_disp == 5);
        if (create_if_missing && !on_disc && !K().fs_root.empty()) {
            fd = open(host.c_str(), O_RDWR | O_CREAT, 0644);
            if (fd >= 0) exists = true;
        }
        if (fd < 0 && !exists) {
            delete f;
            if (handle_ptr) StoreU32(handle_ptr, 0);
            if (io_status) StoreU32(io_status, X_STATUS_NO_SUCH_FILE);
            PRLOG(Filesystem, "NtCreateFile('%s') = NO_SUCH_FILE (host '%s')",
                  device_path.c_str(), host.c_str());
            RET(X_STATUS_NO_SUCH_FILE);
        }
    }
    FsAudit("create", device_path, true);

    f->host_fd = fd;
    f->is_directory = is_dir;
    f->fs_path = host;
    f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
    f->size = 0x40;
    uint32_t h = K().objects.NewHandle(f);
    if (handle_ptr) StoreU32(handle_ptr, h);
    if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
    PRLOG(Filesystem, "NtCreateFile('%s') = handle %08X fd=%d dir=%u",
          device_path.c_str(), h, fd, is_dir);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtOpenFile) {
    // (handle_out, desired_access, obj_attrs, io_status, share, options)
    // Same core as NtCreateFile with OPEN_EXISTING.
    uint32_t handle_ptr = ARG(0);
    uint32_t desired_access = ARG(1);
    uint32_t obj_attrs = ARG(2);
    uint32_t io_status = ARG(3);

    std::string raw = NormalizeGuestPath(obj_attrs, false);
    std::string device_path = ApplySymbolicLinks(raw);
    PRLOG(Filesystem, "NtOpenFile('%s')", raw.c_str());

    // The entire raw HDD namespace (\Device\Harddisk0\Partition0..3,
    // \Cache0, \Cache1 and everything under them) is Xenia NullDevice
    // territory: "Cache/STFC code baked into games tries reading/writing to
    // these. By using a NullDevice that just returns success to all IO
    // requests it should allow games to believe cache/raw disk was accessed
    // successfully." The volume root itself (exact path) is the RAW volume
    // (ioctls answer geometry/partition info); the root with a trailing
    // backslash opens as a directory; subpaths are null files.
    // Returns: 0 = not null-namespace, 1 = raw volume, 2 = volume root dir,
    // 3 = null file/dir under the volume.
    auto hdd_null_kind = [](const std::string& device_path) -> int {
        std::string p_low = device_path;
        for (auto& c : p_low)
            if (c >= 'A' && c <= 'Z') c += 32;
        static const char* kVols[] = {
            "\\device\\harddisk0\\partition0",
            "\\device\\harddisk0\\partition1",
            "\\device\\harddisk0\\partition2",
            "\\device\\harddisk0\\partition3",
            "\\device\\harddisk0\\cache0",
            "\\device\\harddisk0\\cache1",
            "\\device\\harddisk0partition0",
            "\\device\\harddisk0partition1",
            "\\device\\harddisk0partition2",
            "\\device\\harddisk0partition3",
        };
        for (const char* vol : kVols) {
            std::string v = vol;
            if (p_low == v) return 1;
            std::string vslash = v + "\\";
            if (p_low == vslash) return 2;
            if (p_low.rfind(vslash, 0) == 0) return 3;
        }
        return 0;
    };
    bool is_device = device_path.rfind("\\Device\\", 0) == 0 &&
                     device_path.find('\\', 8) == std::string::npos;

    auto* f = new GuestFile();
    f->guest_path = device_path;
    f->type = kObjTypeFile;

    int null_kind = hdd_null_kind(device_path);
    if (null_kind) {
        f->is_null_volume = (null_kind == 1 || null_kind == 3);
        f->is_directory = (null_kind == 2);
        f->fs_path = "(null-hdd)";
        f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
        f->size = 0x40;
        // Partition0 = the persistent SYSTEM partition on real hardware
        // (the ATG owner-block database). Give it a real backing file so
        // writes persist and later reads observe them — exactly like the
        // console. Cache0/Cache1 are scratch (reformatted every boot by
        // XMountUtilityDrive) and stay pure null volumes.
        if (null_kind == 1 && !K().hdd_root.empty()) {
            std::string p_low = device_path;
            for (auto& c : p_low)
                if (c >= 'A' && c <= 'Z') c += 32;
            if (p_low == "\\device\\harddisk0\\partition0" ||
                p_low == "\\device\\harddisk0partition0") {
                std::string img = K().hdd_root + "/partition0.img";
                f->raw_fd = open(img.c_str(), O_RDWR | O_CREAT, 0644);
                if (f->raw_fd >= 0) {
                    // 2 MiB persistent system-partition image.
                    ftruncate(f->raw_fd, 0x200000);
                    f->fs_path = img;
                }
            }
        }
        uint32_t h = K().objects.NewHandle(f);
        if (handle_ptr) StoreU32(handle_ptr, h);
        if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
        FsAudit("create", device_path, true);
        PRLOG(Filesystem, "NtCreateFile: null-hdd kind=%d '%s' -> handle %08X (rawfd=%d)",
              null_kind, device_path.c_str(), h, f->raw_fd);
        RET(X_STATUS_SUCCESS);
    }
    if (is_device) {
        f->is_device = true;
        f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
        f->size = 0x40;
        uint32_t h = K().objects.NewHandle(f);
        if (handle_ptr) StoreU32(handle_ptr, h);
        if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
        RET(X_STATUS_SUCCESS);
    }

    std::string host;
    MapToHost(device_path, &host);
    std::string resolved = host;
    if (ResolveHostPathCaseInsensitive(host, &resolved)) host = resolved;
    bool is_dir = false, exists = false;
    int fd = OpenHostFile(host, false, &is_dir, &exists);
    if (!exists) {
        LogLineOnce(LogCategory::kFilesystem,
                    "CONTENT MISSING: '%s' (host '%s') — required by guest",
                    device_path.c_str(), host.c_str());
    }
    if (fd < 0 && !is_dir) {
        delete f;
        if (handle_ptr) StoreU32(handle_ptr, 0);
        if (io_status) StoreU32(io_status, X_STATUS_NO_SUCH_FILE);
        PRLOG(Filesystem, "NtOpenFile('%s') = NO_SUCH_FILE (host '%s')",
              device_path.c_str(), host.c_str());
        FsAudit("open", device_path, false);
        RET(X_STATUS_NO_SUCH_FILE);
    }
    FsAudit("open", device_path, true);
    f->host_fd = fd;
    f->is_directory = is_dir;
    f->fs_path = host;
    f->guest_addr = GuestMemory::Get().SystemHeapAlloc(0x40, 8);
    f->size = 0x40;
    uint32_t h = K().objects.NewHandle(f);
    if (handle_ptr) StoreU32(handle_ptr, h);
    if (io_status) { StoreU32(io_status, X_STATUS_SUCCESS); StoreU32(io_status + 4, 0); }
    RET(X_STATUS_SUCCESS);
}

static GuestFile* LookupFile(uint32_t handle) {
    auto* obj = K().objects.Lookup(handle);
    return dynamic_cast<GuestFile*>(obj);
}

// IO_STATUS_BLOCK completion helper.
static void CompleteIO(uint32_t io_status, uint32_t status, uint32_t info) {
    if (io_status) {
        StoreU32(io_status + 0, status);
        StoreU32(io_status + 4, info);
    }
}

IMPORT(NtReadFile) {
    // (handle, event, apc_routine, apc_context, io_status, buffer, length,
    //  byte_offset_ptr, key)
    uint32_t handle = ARG(0);
    uint32_t event = ARG(1);
    uint32_t apc_routine = ARG(2);
    uint32_t apc_context = ARG(3);
    uint32_t io_status = ARG(4);
    uint32_t buffer = ARG(5);
    uint32_t length = ARG(6);
    uint32_t byte_offset_ptr = ARG(7);

    GuestFile* f = LookupFile(handle);
    if (f && f->is_null_volume) {
        // Partition0 with a real backing file: persistent reads (hardware
        // behavior — the ATG owner-block database survives reboots).
        uint8_t* host_buf0 = HostFromGuest(buffer);
        if (f->raw_fd >= 0 && host_buf0) {
            off_t ro = (off_t)f->position;
            if (byte_offset_ptr && LoadU32(byte_offset_ptr) != 0xFFFFFFFF) {
                uint64_t lo = LoadU32(byte_offset_ptr);
                uint32_t hi = LoadU32(byte_offset_ptr + 4);
                ro = (off_t)((hi << 32) | lo);
            }
            ssize_t rn = pread(f->raw_fd, host_buf0, length, ro);
            if (rn < 0) rn = 0;
            f->position += rn;
            CompleteIO(io_status, X_STATUS_SUCCESS, (uint32_t)rn);
            PRLOG(Filesystem, "NtReadFile(%08X '%s') [partition0.img] = %u bytes @ %llu",
                  handle, f->guest_path.c_str(), (uint32_t)rn,
                  (unsigned long long)ro);
            RET(X_STATUS_SUCCESS);
        }
        // Other raw HDD volumes (Xenia NullFile::ReadSync): reads SUCCEED
        // without touching the buffer — the caller's prior buffer contents
        // (e.g. the ATG owner block it just wrote) remain visible, which is
        // exactly how Xenia's null device lets XMountUtilityDrive's
        // read-back-check loops terminate. NEVER zero the buffer here.
        f->position += length;
        CompleteIO(io_status, X_STATUS_SUCCESS, length);
        if (event) {
            auto* ev = dynamic_cast<GuestEvent*>(K().objects.Lookup(event));
            if (ev) {
                std::lock_guard<std::mutex> lk(ev->mtx);
                ev->signaled = true;
                ev->cv.notify_all();
            }
        }
        PRLOG(Filesystem, "NtReadFile(%08X '%s') [null-volume] = %u zeros @ %llu",
              handle, f->guest_path.c_str(), length,
              (unsigned long long)f->position);
        RET(X_STATUS_SUCCESS);
    }
    if (!f || f->host_fd < 0) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    off_t off = (off_t)f->position;
    bool use_offset = byte_offset_ptr != 0 && LoadU32(byte_offset_ptr) != 0xFFFFFFFF;
    if (use_offset) {
        uint64_t lo = LoadU32(byte_offset_ptr);
        uint32_t hi = LoadU32(byte_offset_ptr + 4);
        off = (off_t)((hi << 32) | lo);
    }
    uint8_t* host_buf = HostFromGuest(buffer);
    ssize_t n = pread(f->host_fd, host_buf, length, off);
    if (n < 0) n = 0;
    if (!use_offset) f->position += n;
    CompleteIO(io_status, X_STATUS_SUCCESS, (uint32_t)n);
    if (event) {
        auto* ev = dynamic_cast<GuestEvent*>(K().objects.Lookup(event));
        if (ev) {
            std::lock_guard<std::mutex> lk(ev->mtx);
            ev->signaled = true;
            ev->cv.notify_all();
        }
    }
    PRLOG(Filesystem, "NtReadFile(%08X '%s') = %u bytes @ %llu -> buf %08X ev=%08X apc=%08X apcctx=%08X ios=%08X lr=%08X tid=%u", handle,
          f->guest_path.c_str(), (uint32_t)n, (unsigned long long)off, buffer,
          event, apc_routine, apc_context, io_status,
          (uint32_t)ctx.lr, GuestThread::GetCurrent() ? GuestThread::GetCurrent()->thread_id : 0);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtWriteFile) {
    uint32_t handle = ARG(0);
    uint32_t event = ARG(1);
    uint32_t io_status = ARG(4);
    uint32_t buffer = ARG(5);
    uint32_t length = ARG(6);
    uint32_t byte_offset_ptr = ARG(7);
    GuestFile* f = LookupFile(handle);
    if (f && f->is_null_volume) {
        // Partition0 with a real backing file: persistent writes.
        if (f->raw_fd >= 0) {
            uint8_t* host_buf1 = HostFromGuest(buffer);
            if (host_buf1) {
                off_t wo = (off_t)f->position;
                bool wuse_off = byte_offset_ptr != 0;
                if (wuse_off) {
                    uint64_t lo = LoadU32(byte_offset_ptr);
                    uint32_t hi = LoadU32(byte_offset_ptr + 4);
                    wo = (off_t)((hi << 32) | lo);
                }
                ssize_t wn = pwrite(f->raw_fd, host_buf1, length, wo);
                if (wn < 0) wn = 0;
                f->position += wn;
                CompleteIO(io_status, X_STATUS_SUCCESS, (uint32_t)wn);
                PRLOG(Filesystem, "NtWriteFile(%08X '%s') [partition0.img] = %u bytes @ %llu",
                      handle, f->guest_path.c_str(), (uint32_t)wn,
                      (unsigned long long)wo);
                RET(X_STATUS_SUCCESS);
            }
        }
        // Other raw volumes (Xenia NullDevice): writes succeed (discarded).
        if (!byte_offset_ptr) f->position += length;
        CompleteIO(io_status, X_STATUS_SUCCESS, length);
        if (event) {
            auto* ev = dynamic_cast<GuestEvent*>(K().objects.Lookup(event));
            if (ev) {
                std::lock_guard<std::mutex> lk(ev->mtx);
                ev->signaled = true;
                ev->cv.notify_all();
            }
        }
        static std::atomic<uint64_t> nv_writes{0};
        uint64_t wn = nv_writes.fetch_add(1);
        if (wn < 8 || (wn & 0xFFFF) == 0)
            PRLOG(Filesystem, "NtWriteFile(%08X '%s') [null-volume] = %u "
                              "bytes discarded (total %llu)",
                  handle, f->guest_path.c_str(), length,
                  (unsigned long long)(wn + 1));
        RET(X_STATUS_SUCCESS);
    }
    if (!f || f->host_fd < 0) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    off_t off = (off_t)f->position;
    bool use_offset = byte_offset_ptr != 0;
    if (use_offset) {
        uint64_t lo = LoadU32(byte_offset_ptr);
        uint32_t hi = LoadU32(byte_offset_ptr + 4);
        off = (off_t)((hi << 32) | lo);
    }
    ssize_t n = pwrite(f->host_fd, HostFromGuest(buffer), length, off);
    if (n < 0) n = 0;
    if (!use_offset) f->position += n;
    CompleteIO(io_status, X_STATUS_SUCCESS, (uint32_t)n);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtReadFileScatter) {
    // (handle, event, apc, ctx, io_status, segment_array, length,
    //  byte_offset)
    // Segment array: u32 count, u32 pad, then 64-bit guest addresses.
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(4);
    uint32_t seg_array = ARG(5);
    uint32_t length = ARG(6);
    uint32_t byte_offset_ptr = ARG(7);
    GuestFile* f = LookupFile(handle);
    if (!f || f->host_fd < 0) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    off_t off = (off_t)f->position;
    if (byte_offset_ptr) {
        uint64_t lo = LoadU32(byte_offset_ptr);
        uint32_t hi = LoadU32(byte_offset_ptr + 4);
        off = (off_t)((hi << 32) | lo);
    }
    // Read into segments (each 64KB page on Xbox; we read sequentially).
    uint32_t remaining = length;
    uint32_t seg_count = LoadU32(seg_array);
    uint64_t total_read = 0;
    for (uint32_t i = 0; i < seg_count && remaining > 0; i++) {
        uint32_t seg_addr = LoadU32(seg_array + 8 + i * 8 + 4);
        uint32_t chunk = std::min<uint32_t>(remaining, 0x10000);
        ssize_t n = pread(f->host_fd, HostFromGuest(seg_addr), chunk, off + total_read);
        if (n <= 0) break;
        total_read += n;
        remaining -= n;
    }
    if (byte_offset_ptr == 0) f->position += total_read;
    CompleteIO(io_status, X_STATUS_SUCCESS, (uint32_t)total_read);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtQueryInformationFile) {
    // (handle, io_status, info, length, class)
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(1);
    uint32_t info = ARG(2);
    uint32_t length = ARG(3);
    uint32_t info_class = ARG(4);
    GuestFile* f = LookupFile(handle);
    if (!f) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    struct stat st {};
    uint64_t size = 0;
    if (!f->is_device && !f->is_directory && f->fs_path.size() &&
        stat(f->fs_path.c_str(), &st) == 0) {
        size = st.st_size;
    }
    switch (info_class) {
    case 4: {  // FileStandardInformation { alloc sz, eof sz } (u64 each)
        if (length >= 16) {
            StoreU64(info + 0, (size + 0xFFF) & ~0xFFFull);
            StoreU64(info + 8, size);
        }
        CompleteIO(io_status, X_STATUS_SUCCESS, 16);
        break;
    }
    case 5: {  // FilePositionInformation { u64 current }
        StoreU64(info, f->position);
        CompleteIO(io_status, X_STATUS_SUCCESS, 8);
        break;
    }
    case 0x0D: {  // FileAllInformation? partial support
        CompleteIO(io_status, X_STATUS_SUCCESS, length);
        break;
    }
    case 34: {  // FileNetworkOpenInformation (0x34 bytes):
        // {creation,access,write,change (FILETIME u64), allocation u64,
        //  end_of_file u64 (SIZE), attributes u32}
        uint64_t ctime = 0;
        if (!f->is_device && !f->is_directory && f->fs_path.size() &&
            stat(f->fs_path.c_str(), &st) == 0) {
            // 100ns since 1601 (FILETIME).
            ctime = (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull;
        }
        if (length >= 0x34) {
            StoreU64(info + 0x00, ctime);
            StoreU64(info + 0x08, ctime);
            StoreU64(info + 0x10, ctime);
            StoreU64(info + 0x18, ctime);
            StoreU64(info + 0x20, (size + 0xFFF) & ~0xFFFull);
            StoreU64(info + 0x28, size);          // EndOfFile = file size
            StoreU32(info + 0x30, f->is_directory ? 0x10 : 0x80);
        }
        CompleteIO(io_status, X_STATUS_SUCCESS, 0x34);
        break;
    }
    default:
        PRLOGW("NtQueryInformationFile: class %u unsupported", info_class);
        CompleteIO(io_status, X_STATUS_SUCCESS, length);
    }
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtSetInformationFile) {
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(1);
    uint32_t info = ARG(2);
    uint32_t length = ARG(3);
    uint32_t info_class = ARG(4);
    GuestFile* f = LookupFile(handle);
    if (!f) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }
    if (info_class == 0x0D /*FileEndInformation*/ || info_class == 10) {
        // Seek to end / truncate.
        if (length >= 8) {
            f->position = LoadU64(info);
            if (f->host_fd >= 0 && ftruncate(f->host_fd, (off_t)f->position) == 0) {
            }
        }
    } else if (info_class == 0) {  // rename
    }
    CompleteIO(io_status, X_STATUS_SUCCESS, length);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtQueryVolumeInformationFile) {
    // (handle, io_status, info, length, class)
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(1);
    uint32_t info = ARG(2);
    uint32_t length = ARG(3);
    uint32_t class_ = ARG(4);
    GuestFile* vf = LookupFile(handle);
    if (class_ == 1 /*FileFsVolumeInformation*/) {
        // { u64 creation, u32 serial, u32 label_len, u8 supports objects,
        //   wchar label[] }
        if (length >= 20) {
            StoreU64(info + 0, 0);
            StoreU32(info + 8, 0x50524F54);  // serial
            StoreU32(info + 12, 0);
            StoreU8(info + 16, 0);
        }
    } else if (class_ == 3 /*FileFsSizeInformation*/) {
        // { u64 total_units, u64 avail_units, u32 sectors_per_alloc_unit,
        //   u32 bytes_per_sector }
        // XMountUtilityDrive's cache-verification (sub_82A65F50) requires
        // sectors_per_alloc_unit * bytes_per_sector == the requested
        // cluster size (0x8000 for this title's ATG thread stacks). The
        // FATX utility volumes use 32 KiB clusters of 512-byte sectors;
        // capacity follows the X_IOCTL cache_size (0xFF000 bytes).
        if (length >= 24) {
            StoreU64(info + 0, 0x1F);   // total allocation units (31 * 32K)
            StoreU64(info + 8, 0x1F);   // available units
            StoreU32(info + 16, 64);    // sectors per allocation unit
            StoreU32(info + 20, 512);   // bytes per sector
        }
    }
    CompleteIO(io_status, X_STATUS_SUCCESS, length);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtQueryFullAttributesFile) {
    // (obj_attributes, attributes) — used to stat before opening.
    uint32_t obj_attrs = ARG(0);
    uint32_t attrs = ARG(1);
    std::string raw = NormalizeGuestPath(obj_attrs, false);
    std::string device_path = ApplySymbolicLinks(raw);
    std::string host;
    MapToHost(device_path, &host);
    std::string resolved = host;
    bool ok = ResolveHostPathCaseInsensitive(host, &resolved);
    if (ok) host = resolved;
    FsAudit("stat", device_path, ok);
    struct stat st {};
    if (!ok || K().fs_root.empty() || stat(host.c_str(), &st) != 0) {
        LogLineOnce(LogCategory::kFilesystem,
                    "CONTENT MISSING (stat): '%s' (host '%s')",
                    device_path.c_str(), host.c_str());
        PRLOG(Filesystem, "NtQueryFullAttributesFile('%s') = NOT_FOUND",
              raw.c_str());
        RET(X_STATUS_NO_SUCH_FILE);
    }
    // FILE_NETWORK_OPEN_INFORMATION — Xbox 360 packed layout (0x34 bytes,
    // Xenia xboxkrnl.h): creation/access/write/change (u64 each),
    // allocation_size u64 @0x20, file_size u64 @0x28, attributes u32 @0x30.
    StoreU64(attrs + 0x00, (uint64_t)st.st_ctime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x08, (uint64_t)st.st_atime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x10, (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x18, (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x20, ((uint64_t)st.st_size + 0xFFF) & ~0xFFFull);
    StoreU64(attrs + 0x28, (uint64_t)st.st_size);
    StoreU32(attrs + 0x30, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    PRLOG(Filesystem, "NtQueryFullAttributesFile('%s') ok size=%lld",
          raw.c_str(), (long long)st.st_size);
    RET(X_STATUS_SUCCESS);
}

IMPORT(NtFlushBuffersFile) {
    uint32_t handle = ARG(0);
    GuestFile* f = LookupFile(handle);
    if (f && f->host_fd >= 0) fsync(f->host_fd);
    RET(X_STATUS_SUCCESS);
}

// Directory enumeration: FILE_ID_BOTH_DIR_INFORMATION (0x94+) style.
static void FillDirEntry(uint32_t info, uint32_t info_len,
                         const std::string& name, const std::string& host_path,
                         uint32_t index, uint32_t next_offset) {
    struct stat st {};
    bool is_dir = false;
    uint64_t size = 0;
    if (stat(host_path.c_str(), &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
        size = st.st_size;
    }
    uint32_t fixed = 0x5C;  // sizeof(FILE_ID_BOTH_DIR_INFORMATION) w/o name
    uint32_t name_bytes = (uint32_t)name.size() * 2;
    uint32_t this_len = fixed + name_bytes;
    StoreU32(info + 0x00, next_offset);       // NextEntryOffset
    StoreU32(info + 0x04, index);             // FileIndex
    StoreU64(info + 0x08, 0);                 // CreationTime
    StoreU64(info + 0x10, 0);                 // LastAccessTime
    StoreU64(info + 0x18, 0);                 // LastWriteTime
    StoreU64(info + 0x20, 0);                 // ChangeTime
    StoreU64(info + 0x28, size);              // EndOfFile
    StoreU64(info + 0x30, size);              // AllocationSize
    StoreU32(info + 0x38, is_dir ? 0x10 : 0x20);  // FileAttributes
    StoreU32(info + 0x3C, name.size());       // FileNameLength (bytes)
    StoreU32(info + 0x40, 0);                 // EaSize
    StoreU16(info + 0x44, 0);                 // ShortNameLength
    // ShortName (24 bytes) at 0x46; FileId u64 at 0x5E? approximate:
    // We use the classic layout the game reads: name starts at 0x5C.
    for (size_t i = 0; i < name.size(); i++) {
        StoreU16(info + fixed + i * 2, (uint16_t)name[i]);
    }
}

IMPORT(NtQueryDirectoryFile) {
    // (handle, event, apc, ctx, io_status, file_info, length,
    //  file_info_class, return_single_entry, filename, restart_scan)
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(4);
    uint32_t file_info = ARG(5);
    uint32_t length = ARG(6);
    uint32_t file_info_class = ARG(7);
    uint32_t single = ARG(8);
    uint32_t filename_ptr = ARG(9);   // UNICODE_STRING*
    uint32_t restart = ARG(10);

    GuestFile* f = LookupFile(handle);
    PRLOGONCE(Filesystem, "NtQueryDirectoryFile: handle=%08X class=%u single=%u restart=%u fs_path=%s%s",
              handle, file_info_class, single, restart,
              f ? f->fs_path.c_str() : "(none)",
              f && f->is_directory ? "" : " NOT-DIR");
    if (!f || !f->is_directory) {
        CompleteIO(io_status, X_STATUS_INVALID_HANDLE, 0);
        RET(X_STATUS_INVALID_HANDLE);
    }

    if (restart || f->dir_entries.empty()) {
        f->dir_entries.clear();
        f->dir_index = 0;
        DIR* d = opendir(f->fs_path.c_str());
        if (d) {
            struct dirent* de;
            while ((de = readdir(d))) {
                std::string n = de->d_name;
                if (n == "." || n == "..") continue;
                f->dir_entries.push_back(n);
            }
            closedir(d);
        }
        std::sort(f->dir_entries.begin(), f->dir_entries.end());
    }

    // Optional filename filter (pattern match like *.bin).
    std::string pattern;
    if (filename_ptr) {
        uint16_t len = LoadU16(filename_ptr);
        uint32_t buf = LoadU32(filename_ptr + 4);
        for (uint16_t i = 0; i + 1 < len; i += 2) {
            uint16_t ch = LoadU16(buf + i);
            if (ch < 0x80) pattern.push_back((char)ch);
        }
    }

    // Write one entry per call (game loops until STATUS_NO_MORE_FILES).
    while (f->dir_index < f->dir_entries.size()) {
        std::string name = f->dir_entries[f->dir_index];
        f->dir_index++;
        if (!pattern.empty() && pattern != "*" &&
            name.find(pattern) == std::string::npos &&
            pattern != name) {
            // crude wildcard: *.ext
            if (pattern.size() > 1 && pattern[0] == '*') {
                std::string suffix = pattern.substr(1);
                if (name.size() < suffix.size() ||
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
            } else {
                continue;
            }
        }
        std::string host = f->fs_path + "/" + name;
        {
            static std::set<std::string> seen;
            if (seen.insert(f->fs_path + "|" + name + (pattern.empty() ? "" : "|pat:" + pattern)).second) {
                PRLOG(Filesystem, "DIR-ENTRY: %s -> %s%s", f->fs_path.c_str(), name.c_str(),
                      pattern.empty() ? "" : (" (pattern " + pattern + ")").c_str());
            }
        }
        FillDirEntry(file_info, length, name, host, (uint32_t)f->dir_index, 0);
        CompleteIO(io_status, X_STATUS_SUCCESS, length);
        RET(X_STATUS_SUCCESS);
    }
    // No more entries.
    CompleteIO(io_status, 0x80000006u /*STATUS_NO_MORE_FILES*/, 0);
    RET(0x80000006u);
}

// =============================================================== Io* device

IMPORT(IoCreateDevice) {
    // (driver_object, ext_len, device_name, type, characteristics,
    //  exclusive, device_out)
    uint32_t driver_object = ARG(0);
    uint32_t ext_len = ARG(1);
    uint32_t device_name_ptr = ARG(2);
    uint32_t type = ARG(3);
    uint32_t exclusive = ARG(5);
    uint32_t device_out = ARG(6);

    // Device name is UNICODE_STRING*.
    std::string name;
    if (device_name_ptr) {
        uint16_t len = LoadU16(device_name_ptr);
        uint32_t buf = LoadU32(device_name_ptr + 4);
        for (uint16_t i = 0; i + 1 < len; i += 2) {
            uint16_t ch = LoadU16(buf + i);
            if (ch < 0x80) name.push_back((char)ch);
        }
    }
    auto* dev = new KernelState::DeviceObject();
    dev->name = name;
    dev->driver_object = driver_object;
    dev->type = 4;  // device object marker
    dev->guest_addr = GuestMemory::Get().SystemHeapAlloc(
        std::max<uint32_t>(0x100, ext_len + 0x40), 8);
    dev->size = dev->guest_addr ? 0x100 : 0;
    GuestMemset(dev->guest_addr, 0, 0x100);
    // DEVICE_OBJECT-ish fields the game may inspect.
    StoreU32(dev->guest_addr + 0x00, 4);            // type marker
    StoreU32(dev->guest_addr + 0x04, 0x100);        // size
    StoreU32(dev->guest_addr + 0x08, driver_object);
    // Xenia (xboxkrnl_io.cc IoCreateDevice): "Called from XMountUtilityDrive
    // XAM-task code. That code tries writing things to a pointer at
    // out_struct+0x18 — we'll alloc some scratch space for it."
    {
        uint32_t scratch = GuestMemory::Get().SystemHeapAlloc(0x1000, 8);
        if (scratch) {
            GuestMemset(scratch, 0, 0x1000);
            StoreU32(dev->guest_addr + 0x18, scratch);
        }
    }
    {
        std::lock_guard<std::mutex> lk(K().device_mutex);
        K().devices[name] = dev;
    }
    if (device_out) StoreU32(device_out, dev->guest_addr);
    PRLOG(Filesystem, "IoCreateDevice('%s') = %08X (driver %08X)", name.c_str(),
          dev->guest_addr, driver_object);
    RET(X_STATUS_SUCCESS);
}

IMPORT(IoDeleteDevice) {
    uint32_t device_ptr = ARG(0);
    {
        std::lock_guard<std::mutex> lk(K().device_mutex);
        for (auto it = K().devices.begin(); it != K().devices.end(); ++it) {
            if (it->second->guest_addr == device_ptr) {
                delete it->second;
                K().devices.erase(it);
                break;
            }
        }
    }
    RET(X_STATUS_SUCCESS);
}

IMPORT(IoInvalidDeviceRequest) {
    PRLOGW("IoInvalidDeviceRequest called (IRP passthrough)");
    RET(X_STATUS_INVALID_DEVICE_REQUEST);
}

IMPORT(IoCompleteRequest) {
    uint32_t irp = ARG(0);
    uint32_t priority = ARG(1);
    // Phase 2B: async IRP completion dispatch is synchronous; log only.
    RET(0);
}

IMPORT(IoDismountVolume) { RET(X_STATUS_SUCCESS); }
IMPORT(IoDismountVolumeByFileHandle) { RET(X_STATUS_SUCCESS); }
IMPORT(IoCheckShareAccess) { RET(X_STATUS_SUCCESS); }
IMPORT(IoSetShareAccess) { RET(0); }
IMPORT(IoRemoveShareAccess) { }

IMPORT(NtDeviceIoControlFile) {
    // (handle, event, apc, ctx, io_status, ioctl, in_buf, in_len, out_buf,
    //  out_len)
    // Xenia (xboxkrnl_io.cc): "Called by XMountUtilityDrive cache-mounting
    // code (checks if the returned values look valid, values below seem to
    // pass the checks)". The cache volume geometry is console hardware
    // state, not game content:
    //   X_IOCTL_DISK_GET_DRIVE_GEOMETRY (0x70000):
    //       out[0..4) = total sectors (cache_size / 512)
    //       out[4..8) = bytes per sector (512)
    //   X_IOCTL_DISK_GET_PARTITION_INFO (0x74004):
    //       out[0..8) = 0
    //       out[8..16) = cache_size (0xFF000)
    static const uint32_t kCacheSize = 0xFF000;
    static const uint32_t kIoctlDiskGetDriveGeometry = 0x70000;
    static const uint32_t kIoctlDiskGetPartitionInfo = 0x74004;
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(4);
    uint32_t ioctl = ARG(5);
    uint32_t out_buf = ARG(8);
    uint32_t out_len = ARG(9);
    uint8_t* host_out =
        (out_buf && out_len) ? (uint8_t*)HostFromGuest(out_buf) : nullptr;
    if (ioctl == kIoctlDiskGetDriveGeometry) {
        if (host_out && out_len >= 8) {
            StoreU32(out_buf, kCacheSize / 512);
            StoreU32(out_buf + 4, 512);
        }
        CompleteIO(io_status, X_STATUS_SUCCESS, 8);
    } else if (ioctl == kIoctlDiskGetPartitionInfo) {
        if (host_out && out_len >= 0x10) {
            StoreU32(out_buf, 0);
            StoreU32(out_buf + 4, 0);
            StoreU32(out_buf + 8, kCacheSize);
            StoreU32(out_buf + 12, 0);
        }
        CompleteIO(io_status, X_STATUS_SUCCESS, 0x10);
    } else {
        // NullDevice semantics for everything else: success, zeroed output.
        if (host_out && out_len <= 0x10000) memset(host_out, 0, out_len);
        CompleteIO(io_status, X_STATUS_SUCCESS, out_len);
    }
    PRLOG(Filesystem, "NtDeviceIoControlFile(%08X, ioctl=%08X) [outbuf=%08X outlen=%u]",
          handle, ioctl, out_buf, out_len);
    RET(X_STATUS_SUCCESS);
}


