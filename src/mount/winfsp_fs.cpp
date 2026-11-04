// SPDX-License-Identifier: GPL-3.0-or-later
//
// WinFsp file system on top of Ext4Volume. Read-only or writable, depending
// on how the volume was mounted.

// winfsp.h needs the full windows.h (PNTSTATUS comes from bcrypt.h).
#undef WIN32_LEAN_AND_MEAN
#include <winfsp/winfsp.h>  // must come first: sets up windows.h + ntstatus.h

#include "mount/winfsp_fs.h"

#include "core/ext4_volume.h"
#include "core/names.h"
#include "core/text.h"

#include <sddl.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ext4easy {

namespace {

// Read-only: everyone may read. Writable: everyone may change files, like on
// a USB stick with FAT (Linux owners and permissions are not mapped).
const wchar_t* kSddlReadOnly = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)";
const wchar_t* kSddlWritable = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";

constexpr DWORD kFlushIntervalMs = 2000;  // background flush after changes
constexpr UINT32 kFileDirectoryFile = 0x00000001;  // FILE_DIRECTORY_FILE (winternl.h)

struct FileContext {
    std::string path;  // resolved Linux path
    FileStat st;
    FileHandle* file = nullptr;  // regular files only
    PVOID dirBuffer = nullptr;  // WinFsp directory buffer (directories only)
    bool deleted = false;
};

NTSTATUS errnoToStatus(int e) {
    switch (e) {
        case 0: return STATUS_SUCCESS;
        case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case ELOOP: return STATUS_OBJECT_NAME_NOT_FOUND;
        case EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
        case ENOMEM: return STATUS_INSUFFICIENT_RESOURCES;
        case EACCES: return STATUS_ACCESS_DENIED;
        case EROFS: return STATUS_MEDIA_WRITE_PROTECTED;
        case EIO: return STATUS_DEVICE_DATA_ERROR;
        case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
        case ENOTEMPTY: return STATUS_DIRECTORY_NOT_EMPTY;
        case ENOSPC: return STATUS_DISK_FULL;
        case EFBIG: return STATUS_DISK_FULL;
        case ENAMETOOLONG: return STATUS_OBJECT_NAME_INVALID;
        case EINVAL: return STATUS_INVALID_PARAMETER;
        case ENOTSUP: return STATUS_NOT_SUPPORTED;
        case EMLINK: return STATUS_TOO_MANY_LINKS;
        default: return STATUS_UNSUCCESSFUL;
    }
}

std::string lastComponent(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

void toFileInfo(const FileStat& st, const std::string& name, FSP_FSCTL_FILE_INFO* fi) {
    std::memset(fi, 0, sizeof *fi);
    UINT32 attrs = 0;
    UINT64 size = st.size;
    if (st.isDir()) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
        size = 0;
    } else if (!st.isFile()) {
        // Devices, FIFOs, sockets, broken symlinks: visible but empty.
        attrs |= FILE_ATTRIBUTE_SYSTEM;
        size = 0;
    }
    if (!name.empty() && name[0] == '.' && name != "." && name != "..")
        attrs |= FILE_ATTRIBUTE_HIDDEN;
    if (attrs == 0) attrs = FILE_ATTRIBUTE_NORMAL;

    fi->FileAttributes = attrs;
    fi->FileSize = size;
    fi->AllocationSize = st.isFile() ? (st.allocated > size ? st.allocated : (size + 4095) / 4096 * 4096) : 0;
    fi->CreationTime = st.crtime;
    fi->LastAccessTime = st.atime;
    fi->LastWriteTime = st.mtime;
    fi->ChangeTime = st.ctime;
    fi->IndexNumber = st.ino;
}

uint64_t nowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

struct WinFspMount::State {
    std::unique_ptr<Ext4Volume> volume;
    FSP_FILE_SYSTEM* fs = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD sdSize = 0;
    VolumeInfo info;
    bool writable = false;
    bool dispatching = false;
    // Serializes access to FileContext fields. Calls into the volume are
    // serialized anyway (lwext4 is single-threaded).
    std::mutex lock;

    // Background flush: written data reaches the disk within a few seconds,
    // even if the drive is later removed without unmounting.
    std::atomic<bool> dirty{false};
    HANDLE stopFlusher = nullptr;
    std::thread flusher;

    void flushLoop() {
        while (WaitForSingleObject(stopFlusher, kFlushIntervalMs) == WAIT_TIMEOUT) {
            if (dirty.exchange(false) && volume->flush() != 0) dirty = true;
        }
    }

    ~State() {
        if (fs && dispatching) FspFileSystemStopDispatcher(fs);
        if (flusher.joinable()) {
            SetEvent(stopFlusher);
            flusher.join();
        }
        if (stopFlusher) CloseHandle(stopFlusher);
        if (fs) FspFileSystemDelete(fs);
        if (sd) LocalFree(sd);
        // volume is destroyed last: stops the journal, unmounts and flushes.
    }
};

namespace {

WinFspMount::State* stateOf(FSP_FILE_SYSTEM* fs) {
    return static_cast<WinFspMount::State*>(fs->UserContext);
}

NTSTATUS copySecurity(WinFspMount::State* s, PSECURITY_DESCRIPTOR out, SIZE_T* size) {
    if (!size) return STATUS_SUCCESS;
    if (*size < s->sdSize) {
        *size = s->sdSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    *size = s->sdSize;
    if (out) std::memcpy(out, s->sd, s->sdSize);
    return STATUS_SUCCESS;
}

// Re-reads the inode data of an open file or directory (it may have been
// changed through this or another handle) and fills fileInfo.
void refreshInfo(WinFspMount::State* s, FileContext* ctx, FSP_FSCTL_FILE_INFO* fileInfo) {
    if (!ctx->deleted) {
        FileStat st;
        int r = ctx->file ? s->volume->statHandle(ctx->file, st) : s->volume->stat(ctx->path, st);
        if (r == 0) ctx->st = st;
    }
    if (fileInfo) toFileInfo(ctx->st, lastComponent(ctx->path), fileInfo);
}

NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* vi) {
    auto* s = stateOf(fs);
    VolumeInfo info = s->volume->info();
    vi->TotalSize = info.totalBytes;
    vi->FreeSize = info.freeBytes;
    std::wstring label = utf8ToWide(info.label);
    if (label.empty()) label = L"ext4";
    size_t n = label.size() < 32 ? label.size() : 32;
    std::memcpy(vi->VolumeLabel, label.data(), n * sizeof(WCHAR));
    vi->VolumeLabelLength = static_cast<UINT16>(n * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR fileName, PUINT32 attributes,
                           PSECURITY_DESCRIPTOR sd, SIZE_T* sdSize) {
    auto* s = stateOf(fs);
    std::string path = windowsPathToLinux(fileName);
    FileStat st;
    int r = s->volume->stat(path, st);
    if (r != 0) return errnoToStatus(r);
    if (attributes) {
        FSP_FSCTL_FILE_INFO fi;
        toFileInfo(st, lastComponent(path), &fi);
        *attributes = fi.FileAttributes;
    }
    return copySecurity(s, sd, sdSize);
}

NTSTATUS Open(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32, UINT32, PVOID* context,
              FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    std::string path = windowsPathToLinux(fileName);
    auto ctx = std::make_unique<FileContext>();
    int r = s->volume->stat(path, ctx->st, &ctx->path);
    if (r != 0) return errnoToStatus(r);
    if (ctx->st.isFile()) {
        ctx->file = s->volume->openFile(ctx->path, r, s->writable);
        if (!ctx->file) return errnoToStatus(r);
    }
    toFileInfo(ctx->st, lastComponent(path), fileInfo);
    *context = ctx.release();
    return STATUS_SUCCESS;
}

NTSTATUS Create(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32 createOptions, UINT32, UINT32,
                PSECURITY_DESCRIPTOR, UINT64, PVOID* context, FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    std::string path = windowsPathToLinux(fileName);
    auto ctx = std::make_unique<FileContext>();
    int r = 0;
    if (createOptions & kFileDirectoryFile) {
        r = s->volume->mkdir(path);
    } else {
        ctx->file = s->volume->createFile(path, r);
    }
    if (r != 0) return errnoToStatus(r);
    s->dirty = true;
    r = s->volume->stat(path, ctx->st, &ctx->path);
    if (r != 0) {
        if (ctx->file) s->volume->closeFile(ctx->file);
        return errnoToStatus(r);
    }
    toFileInfo(ctx->st, lastComponent(path), fileInfo);
    *context = ctx.release();
    return STATUS_SUCCESS;
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM* fs, PVOID context, UINT32, BOOLEAN, UINT64,
                   FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    if (!ctx->file) return STATUS_ACCESS_DENIED;
    int r = s->volume->setFileSize(ctx->file, 0);
    if (r != 0) return errnoToStatus(r);
    s->dirty = true;
    uint64_t now = nowFileTime();
    s->volume->setTimes(ctx->path, 0, now, now, 0);
    refreshInfo(s, ctx, fileInfo);
    return STATUS_SUCCESS;
}

VOID Cleanup(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR fileName, ULONG flags) {
    auto* s = stateOf(fs);
    if (!s->writable) return;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    if (ctx->deleted) return;

    if ((flags & FspCleanupDelete) && fileName) {
        // Delete the entry by its name: for a symlink that is the link
        // itself, not the target the context was opened on.
        if (ctx->file) {
            s->volume->closeFile(ctx->file);
            ctx->file = nullptr;
        }
        std::string path = windowsPathToLinux(fileName);
        int r = s->volume->removeFile(path);
        if (r == EISDIR) r = s->volume->removeDir(path);
        if (r == 0) ctx->deleted = true;
        s->dirty = true;
        return;
    }

    uint64_t now = nowFileTime();
    uint64_t at = (flags & FspCleanupSetLastAccessTime) ? now : 0;
    uint64_t mt = (flags & FspCleanupSetLastWriteTime) ? now : 0;
    uint64_t ct = (flags & FspCleanupSetChangeTime) ? now : 0;
    if (at || mt || ct) {
        s->volume->setTimes(ctx->path, at, mt, ct, 0);
        s->dirty = true;
    }
}

VOID Close(FSP_FILE_SYSTEM* fs, PVOID context) {
    auto* s = stateOf(fs);
    auto* ctx = static_cast<FileContext*>(context);
    {
        std::lock_guard<std::mutex> lock(s->lock);
        if (ctx->file) s->volume->closeFile(ctx->file);
    }
    if (ctx->dirBuffer) FspFileSystemDeleteDirectoryBuffer(&ctx->dirBuffer);
    delete ctx;
}

NTSTATUS Read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
              PULONG transferred) {
    auto* s = stateOf(fs);
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    *transferred = 0;
    if (!ctx->file) {
        if (ctx->deleted) return STATUS_END_OF_FILE;
        return ctx->st.isDir() ? STATUS_INVALID_DEVICE_REQUEST : STATUS_END_OF_FILE;
    }
    size_t got = 0;
    int r = s->volume->readFile(ctx->file, offset, buffer, length, got);
    if (r != 0) return errnoToStatus(r);
    if (got == 0 && length > 0) return STATUS_END_OF_FILE;
    *transferred = static_cast<ULONG>(got);
    return STATUS_SUCCESS;
}

NTSTATUS Write(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
               BOOLEAN writeToEndOfFile, BOOLEAN constrainedIo, PULONG transferred,
               FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    *transferred = 0;
    if (!ctx->file) {
        if (!ctx->deleted) return STATUS_INVALID_DEVICE_REQUEST;
        *transferred = length;  // late cached write to a deleted file: nothing to keep
        toFileInfo(ctx->st, lastComponent(ctx->path), fileInfo);
        return STATUS_SUCCESS;
    }

    uint64_t size = s->volume->fileSize(ctx->file);
    if (constrainedIo) {
        // Paging I/O must not change the file size.
        if (offset >= size) {
            refreshInfo(s, ctx, fileInfo);
            return STATUS_SUCCESS;
        }
        if (offset + length > size) length = static_cast<ULONG>(size - offset);
    }
    if (writeToEndOfFile) offset = size;

    size_t written = 0;
    int r = s->volume->writeFile(ctx->file, offset, buffer, length, written);
    if (written > 0 || r == 0) s->dirty = true;
    if (r != 0) return errnoToStatus(r);
    *transferred = static_cast<ULONG>(written);
    refreshInfo(s, ctx, fileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS Flush(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    std::lock_guard<std::mutex> lock(s->lock);
    if (s->writable) {
        s->dirty = false;
        int r = s->volume->flush();
        if (r != 0) {
            s->dirty = true;
            return errnoToStatus(r);
        }
    }
    if (context) refreshInfo(s, static_cast<FileContext*>(context), fileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    std::lock_guard<std::mutex> lock(s->lock);
    refreshInfo(s, static_cast<FileContext*>(context), fileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* fs, PVOID context, UINT32, UINT64 creationTime,
                      UINT64 lastAccessTime, UINT64 lastWriteTime, UINT64 changeTime,
                      FSP_FSCTL_FILE_INFO* fileInfo) {
    // File attributes (read-only, hidden, archive) have no place in ext4
    // and are ignored; only the timestamps are stored.
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    if (!ctx->deleted && (creationTime || lastAccessTime || lastWriteTime || changeTime)) {
        int r = s->volume->setTimes(ctx->path, lastAccessTime, lastWriteTime, changeTime,
                                    creationTime);
        if (r != 0) return errnoToStatus(r);
        s->dirty = true;
    }
    refreshInfo(s, ctx, fileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS SetFileSize(FSP_FILE_SYSTEM* fs, PVOID context, UINT64 newSize, BOOLEAN setAllocationSize,
                     FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    if (!ctx->file) return ctx->deleted ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
    int r = 0;
    if (setAllocationSize) {
        // Space is allocated on write; a smaller allocation cuts the file.
        if (newSize < s->volume->fileSize(ctx->file)) r = s->volume->setFileSize(ctx->file, newSize);
    } else {
        r = s->volume->setFileSize(ctx->file, newSize);
    }
    if (r != 0) return errnoToStatus(r);
    s->dirty = true;
    refreshInfo(s, ctx, fileInfo);
    return STATUS_SUCCESS;
}

NTSTATUS CanDelete(FSP_FILE_SYSTEM* fs, PVOID, PWSTR fileName) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    std::string path = windowsPathToLinux(fileName);
    if (path == "/") return STATUS_ACCESS_DENIED;
    FileStat st;
    int r = s->volume->lstat(path, st);
    if (r != 0) return errnoToStatus(r);
    if (st.isDir()) {
        std::vector<DirEntry> entries;
        r = s->volume->listDir(path, entries);
        if (r != 0) return errnoToStatus(r);
        if (!entries.empty()) return STATUS_DIRECTORY_NOT_EMPTY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Rename(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR fileName, PWSTR newFileName,
                BOOLEAN replaceIfExists) {
    auto* s = stateOf(fs);
    if (!s->writable) return STATUS_MEDIA_WRITE_PROTECTED;
    std::lock_guard<std::mutex> lock(s->lock);
    auto* ctx = static_cast<FileContext*>(context);
    std::string from = windowsPathToLinux(fileName);
    std::string to = windowsPathToLinux(newFileName);
    int r = s->volume->rename(from, to, replaceIfExists != FALSE);
    if (r == EISDIR || r == ENOTDIR) return STATUS_ACCESS_DENIED;  // Windows semantics
    if (r != 0) return errnoToStatus(r);
    s->dirty = true;
    // The context keeps the resolved path; follow the entry if it was the
    // one renamed (not when a symlink pointing to it was renamed).
    FileStat st;
    std::string real;
    if (s->volume->lstat(to, st) == 0 && st.ino == ctx->st.ino &&
        s->volume->stat(to, st, &real) == 0)
        ctx->path = real;
    return STATUS_SUCCESS;
}

NTSTATUS GetSecurity(FSP_FILE_SYSTEM* fs, PVOID, PSECURITY_DESCRIPTOR sd, SIZE_T* sdSize) {
    return copySecurity(stateOf(fs), sd, sdSize);
}

bool addDirInfo(PVOID* dirBuffer, const std::wstring& name, const FileStat& st,
                const std::string& linuxName, NTSTATUS* result) {
    union {
        UINT8 bytes[sizeof(FSP_FSCTL_DIR_INFO) + 512 * sizeof(WCHAR)];
        FSP_FSCTL_DIR_INFO info;
    } buf;
    size_t len = name.size() < 512 ? name.size() : 512;
    std::memset(&buf.info, 0, sizeof buf.info);
    buf.info.Size = static_cast<UINT16>(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + len * sizeof(WCHAR));
    toFileInfo(st, linuxName, &buf.info.FileInfo);
    std::memcpy(buf.info.FileNameBuf, name.data(), len * sizeof(WCHAR));
    return FspFileSystemFillDirectoryBuffer(dirBuffer, &buf.info, result) != FALSE;
}

NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR, PWSTR marker, PVOID buffer,
                       ULONG length, PULONG transferred) {
    auto* s = stateOf(fs);
    auto* ctx = static_cast<FileContext*>(context);
    std::string path;
    FileStat dirSt;
    {
        std::lock_guard<std::mutex> lock(s->lock);
        if (!ctx->st.isDir()) return STATUS_NOT_A_DIRECTORY;
        path = ctx->path;
        dirSt = ctx->st;
    }

    NTSTATUS result = STATUS_SUCCESS;
    if (FspFileSystemAcquireDirectoryBuffer(&ctx->dirBuffer, marker == nullptr, &result)) {
        std::vector<DirEntry> entries;
        int r = s->volume->listDir(path, entries);
        if (r != 0) {
            result = errnoToStatus(r);
        } else {
            if (path != "/") {
                addDirInfo(&ctx->dirBuffer, L".", dirSt, ".", &result);
                addDirInfo(&ctx->dirBuffer, L"..", dirSt, "..", &result);
            }
            for (const auto& e : entries) {
                if (!addDirInfo(&ctx->dirBuffer, linuxNameToWindows(e.name), e.st, e.name, &result))
                    break;
            }
        }
        FspFileSystemReleaseDirectoryBuffer(&ctx->dirBuffer);
    }
    if (!NT_SUCCESS(result)) return result;
    FspFileSystemReadDirectoryBuffer(&ctx->dirBuffer, marker, buffer, length, transferred);
    return STATUS_SUCCESS;
}

FSP_FILE_SYSTEM_INTERFACE makeInterface() {
    // WinFsp refuses every open unless Create and Overwrite exist, even on a
    // read-only volume; there they only report "write protected".
    FSP_FILE_SYSTEM_INTERFACE i{};
    i.GetVolumeInfo = GetVolumeInfo;
    i.GetSecurityByName = GetSecurityByName;
    i.Create = Create;
    i.Open = Open;
    i.Overwrite = Overwrite;
    i.Cleanup = Cleanup;
    i.Close = Close;
    i.Read = Read;
    i.Write = Write;
    i.Flush = Flush;
    i.GetFileInfo = GetFileInfo;
    i.SetBasicInfo = SetBasicInfo;
    i.SetFileSize = SetFileSize;
    i.CanDelete = CanDelete;
    i.Rename = Rename;
    i.GetSecurity = GetSecurity;
    i.ReadDirectory = ReadDirectory;
    return i;
}

const FSP_FILE_SYSTEM_INTERFACE g_interface = makeInterface();

std::wstring ntStatusText(NTSTATUS st) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"NTSTATUS 0x%08lX", static_cast<unsigned long>(st));
    return buf;
}

}  // namespace

WinFspMount::WinFspMount(std::unique_ptr<State> state) : state_(std::move(state)) {}

WinFspMount::~WinFspMount() {
    if (!state_ || !state_->writable) return;
    // Let Windows write back data it still caches for this drive, then
    // write everything to the disk before the file system goes away.
    std::wstring mp = mountPoint();
    if (mp.size() == 2 && mp[1] == L':') {
        std::wstring dev = L"\\\\.\\" + mp;
        HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
            CloseHandle(h);
        }
    }
    std::lock_guard<std::mutex> lock(state_->lock);
    state_->volume->flush();
}

std::wstring WinFspMount::mountPoint() const {
    PWSTR mp = FspFileSystemMountPoint(state_->fs);
    return mp ? mp : L"";
}

bool WinFspMount::writable() const { return state_->writable; }

std::unique_ptr<WinFspMount> WinFspMount::start(std::unique_ptr<Ext4Volume> volume,
                                                const std::wstring& mountPoint,
                                                std::wstring& error) {
    NTSTATUS st = FspLoad(nullptr);
    if (!NT_SUCCESS(st)) {
        error = L"WinFsp is not installed (winfsp-x64.dll not found)";
        return nullptr;
    }

    auto s = std::make_unique<State>();
    s->volume = std::move(volume);
    s->info = s->volume->info();
    s->writable = !s->info.readOnly;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            s->writable ? kSddlWritable : kSddlReadOnly, SDDL_REVISION_1, &s->sd, &s->sdSize)) {
        error = L"Cannot create security descriptor";
        return nullptr;
    }

    FSP_FSCTL_VOLUME_PARAMS vp{};
    vp.Version = sizeof vp;
    vp.SectorSize = 512;
    uint32_t bs = s->info.blockSize ? s->info.blockSize : 4096;
    vp.SectorsPerAllocationUnit = static_cast<UINT16>(bs / 512);
    vp.MaxComponentLength = 255;
    vp.VolumeCreationTime = s->info.createTime;
    std::memcpy(&vp.VolumeSerialNumber, s->info.uuid, sizeof vp.VolumeSerialNumber);
    // Every request that changes something returns fresh file info, so
    // Windows may cache it; only this process writes to the volume.
    vp.FileInfoTimeout = s->writable ? 1000 : 10000;
    vp.CaseSensitiveSearch = 1;
    vp.CasePreservedNames = 1;
    vp.UnicodeOnDisk = 1;
    vp.PersistentAcls = 1;
    vp.ReadOnlyVolume = s->writable ? 0 : 1;
    // Writable: cached data reaches us when the last handle is closed, not
    // later through the lazy writer - removable drives stay consistent.
    vp.FlushAndPurgeOnCleanup = s->writable ? 1 : 0;
    vp.PostCleanupWhenModifiedOnly = 1;
    vp.UmFileContextIsUserContext2 = 1;
    wcscpy_s(vp.FileSystemName, L"EXT4");

    st = FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &vp, &g_interface,
                             &s->fs);
    if (!NT_SUCCESS(st)) {
        error = L"Cannot create file system: " + ntStatusText(st);
        return nullptr;
    }
    s->fs->UserContext = s.get();

    // EXT4EASY_DEBUG=1 logs every file system request to stderr.
    if (GetEnvironmentVariableW(L"EXT4EASY_DEBUG", nullptr, 0) > 0) {
        FspDebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
        FspFileSystemSetDebugLog(s->fs, static_cast<UINT32>(-1));
    }

    std::wstring mp = mountPoint;
    st = FspFileSystemSetMountPoint(s->fs, mp.empty() ? nullptr : mp.data());
    if (!NT_SUCCESS(st)) {
        error = L"Cannot mount at " + (mp.empty() ? std::wstring(L"free drive letter") : mp) +
                L": " + ntStatusText(st);
        return nullptr;
    }

    if (s->writable) {
        s->stopFlusher = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        State* raw = s.get();
        s->flusher = std::thread([raw] { raw->flushLoop(); });
    }

    st = FspFileSystemStartDispatcher(s->fs, 0);
    if (!NT_SUCCESS(st)) {
        error = L"Cannot start dispatcher: " + ntStatusText(st);
        return nullptr;
    }
    s->dispatching = true;
    return std::unique_ptr<WinFspMount>(new WinFspMount(std::move(s)));
}

namespace {
HANDLE g_stopEvent = nullptr;
BOOL WINAPI onCtrl(DWORD) {
    if (g_stopEvent) SetEvent(g_stopEvent);
    return TRUE;
}
}  // namespace

int runMountUntilCtrlC(std::unique_ptr<Ext4Volume> volume, const std::wstring& mountPoint) {
    std::wstring error;
    auto m = WinFspMount::start(std::move(volume), mountPoint, error);
    if (!m) {
        std::fprintf(stderr, "error: %s\n", wideToUtf8(error).c_str());
        return 1;
    }
    std::printf("Mounted %s at %s - press Ctrl+C to unmount.\n",
                m->writable() ? "with write access" : "read-only",
                wideToUtf8(m->mountPoint()).c_str());
    std::fflush(stdout);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    WaitForSingleObject(g_stopEvent, INFINITE);
    m.reset();
    std::printf("Unmounted.\n");
    return 0;
}

}  // namespace ext4easy
