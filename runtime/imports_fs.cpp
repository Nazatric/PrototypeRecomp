// PrototypeRecomp Phase 2B runtime — filesystem imports: NtCreateFile,
// NtReadFile, directory enumeration, overlapped I/O.
#include "state.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace pr;


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

// Resolve symbolic link prefixes (\??\D: etc).
static std::string ApplySymbolicLinks(const std::string& raw) {
    std::string p = raw;
    // Strip \??\ prefix.
    if (p.rfind("\\??\\", 0) == 0) p = p.substr(4);
    // Try full-prefix symbolic links, longest first.
    std::lock_guard<std::mutex> lk(K().symlink_mutex);
    std::map<size_t, std::string> matches;
    for (auto& [link, target] : K().symbolic_links) {
        std::string l = link;
        // Symbolic links may be like "D:" or "\\??\\D:".
        if (l.rfind("\\??\\", 0) == 0) l = l.substr(4);
        if (p.rfind(l, 0) == 0) {
            matches[l.size()] = target + p.substr(l.size());
        }
    }
    if (!matches.empty()) return matches.rbegin()->second;
    return p;
}

// Map an Xbox device path to a host path under fs_root.
static bool MapToHost(const std::string& device_path, std::string* out) {
    std::string p = device_path;
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

    // Device opens (\Device\Cdrom0 or \Device\*) succeed as device objects.
    bool is_device = device_path.rfind("\\Device\\", 0) == 0 &&
                     device_path.find('\\', 8) == std::string::npos;

    auto* f = new GuestFile();
    f->guest_path = device_path;
    f->type = kObjTypeFile;

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
    bool is_dir = false, exists = false;
    int fd = OpenHostFile(host, (desired_access & 0xC0000000u) != 0, &is_dir,
                          &exists);
    if (fd < 0 && !is_dir) {
        // create_disp: 1=CREATE_NEW, 2=CREATE_ALWAYS, 3=OPEN_EXISTING,
        // 4=OPEN_ALWAYS, 5=TRUNCATE_EXISTING
        if (create_disp == 4 /*OPEN_ALWAYS*/ || create_disp == 2 ||
            create_disp == 1) {
            if (!K().fs_root.empty()) {
                fd = open(host.c_str(), O_RDWR | O_CREAT, 0644);
                if (fd >= 0) exists = true;
            }
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

    bool is_device = device_path.rfind("\\Device\\", 0) == 0 &&
                     device_path.find('\\', 8) == std::string::npos;

    auto* f = new GuestFile();
    f->guest_path = device_path;
    f->type = kObjTypeFile;
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
    bool is_dir = false, exists = false;
    int fd = OpenHostFile(host, false, &is_dir, &exists);
    if (fd < 0 && !is_dir) {
        delete f;
        if (handle_ptr) StoreU32(handle_ptr, 0);
        if (io_status) StoreU32(io_status, X_STATUS_NO_SUCH_FILE);
        PRLOG(Filesystem, "NtOpenFile('%s') = NO_SUCH_FILE (host '%s')",
              device_path.c_str(), host.c_str());
        RET(X_STATUS_NO_SUCH_FILE);
    }
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
    PRLOG(Filesystem, "NtReadFile(%08X '%s') = %u bytes @ %llu", handle,
          f->guest_path.c_str(), (uint32_t)n, (unsigned long long)off);
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
    uint32_t io_status = ARG(1);
    uint32_t info = ARG(2);
    uint32_t length = ARG(3);
    uint32_t class_ = ARG(4);
    if (class_ == 1 /*FileFsVolumeInformation*/) {
        // { u64 creation, u32 serial, u32 label_len, u8 supports objects,
        //   wchar label[] }
        if (length >= 20) {
            StoreU64(info + 0, 0);
            StoreU32(info + 8, 0x50524F54);  // serial
            StoreU32(info + 12, 0);
            StoreU8(info + 16, 0);
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
    struct stat st {};
    if (K().fs_root.empty() || stat(host.c_str(), &st) != 0) {
        PRLOG(Filesystem, "NtQueryFullAttributesFile('%s') = NOT_FOUND",
              raw.c_str());
        RET(X_STATUS_NO_SUCH_FILE);
    }
    // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes):
    // u64 creation, u64 last_access, u64 last_write, u64 change,
    // u32 attrs, u64 alloc_sz, u64 eof
    StoreU64(attrs + 0x00, (uint64_t)st.st_ctime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x08, (uint64_t)st.st_atime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x10, (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull);
    StoreU64(attrs + 0x18, (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull);
    StoreU32(attrs + 0x20, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    StoreU64(attrs + 0x28, (uint64_t)st.st_size);
    StoreU64(attrs + 0x30, (uint64_t)st.st_size);
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
    uint32_t handle = ARG(0);
    uint32_t io_status = ARG(4);
    uint32_t ioctl = ARG(5);
    CompleteIO(io_status, X_STATUS_SUCCESS, 0);
    PRLOGW("NtDeviceIoControlFile(%08X, ioctl=%08X) — stub success", handle,
           ioctl);
    RET(X_STATUS_SUCCESS);
}


