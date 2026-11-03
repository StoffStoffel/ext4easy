// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only WinFsp file system on top of Ext4Volume.

// winfsp.h needs the full windows.h (PNTSTATUS comes from bcrypt.h).
#undef WIN32_LEAN_AND_MEAN
#include <winfsp/winfsp.h>  // must come first: sets up windows.h + ntstatus.h

#include "mount/winfsp_fs.h"

#include "core/ext4_volume.h"
#include "core/names.h"
#include "core/text.h"

#include <sddl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ext4easy {

namespace {

// Everyone may read; the volume itself is read-only anyway.
const wchar_t* kSddl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)";

struct FileContext {
    std::string path;  // resolved Linux path
    FileStat st;
    FileHandle* file = nullptr;  // regular files only
    PVOID dirBuffer = nullptr;  // WinFsp directory buffer (directories only)
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

}  // namespace

struct WinFspMount::State {
    std::unique_ptr<Ext4Volume> volume;
    FSP_FILE_SYSTEM* fs = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD sdSize = 0;
    VolumeInfo info;
    bool dispatching = false;

    ~State() {
        if (fs) {
            if (dispatching) FspFileSystemStopDispatcher(fs);
            FspFileSystemDelete(fs);
        }
        if (sd) LocalFree(sd);
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
        ctx->file = s->volume->openFile(ctx->path, r);
        if (!ctx->file) return errnoToStatus(r);
    }
    toFileInfo(ctx->st, lastComponent(path), fileInfo);
    *context = ctx.release();
    return STATUS_SUCCESS;
}

VOID Close(FSP_FILE_SYSTEM* fs, PVOID context) {
    auto* ctx = static_cast<FileContext*>(context);
    if (ctx->file) stateOf(fs)->volume->closeFile(ctx->file);
    if (ctx->dirBuffer) FspFileSystemDeleteDirectoryBuffer(&ctx->dirBuffer);
    delete ctx;
}

NTSTATUS Read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
              PULONG transferred) {
    auto* ctx = static_cast<FileContext*>(context);
    *transferred = 0;
    if (!ctx->file) return ctx->st.isDir() ? STATUS_INVALID_DEVICE_REQUEST : STATUS_END_OF_FILE;
    if (offset >= ctx->st.size) return STATUS_END_OF_FILE;
    size_t got = 0;
    int r = stateOf(fs)->volume->readFile(ctx->file, offset, buffer, length, got);
    if (r != 0) return errnoToStatus(r);
    *transferred = static_cast<ULONG>(got);
    return STATUS_SUCCESS;
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM*, PVOID context, FSP_FSCTL_FILE_INFO* fileInfo) {
    auto* ctx = static_cast<FileContext*>(context);
    toFileInfo(ctx->st, lastComponent(ctx->path), fileInfo);
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
    if (!ctx->st.isDir()) return STATUS_NOT_A_DIRECTORY;

    NTSTATUS result = STATUS_SUCCESS;
    if (FspFileSystemAcquireDirectoryBuffer(&ctx->dirBuffer, marker == nullptr, &result)) {
        std::vector<DirEntry> entries;
        int r = s->volume->listDir(ctx->path, entries);
        if (r != 0) {
            result = errnoToStatus(r);
        } else {
            if (ctx->path != "/") {
                addDirInfo(&ctx->dirBuffer, L".", ctx->st, ".", &result);
                addDirInfo(&ctx->dirBuffer, L"..", ctx->st, "..", &result);
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

// WinFsp refuses every open unless Create and Overwrite exist, even on a
// read-only volume. They only report "write protected".
NTSTATUS Create(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64,
                PVOID*, FSP_FSCTL_FILE_INFO*) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

FSP_FILE_SYSTEM_INTERFACE makeInterface() {
    FSP_FILE_SYSTEM_INTERFACE i{};
    i.GetVolumeInfo = GetVolumeInfo;
    i.GetSecurityByName = GetSecurityByName;
    i.Create = Create;
    i.Overwrite = Overwrite;
    i.Open = Open;
    i.Close = Close;
    i.Read = Read;
    i.GetFileInfo = GetFileInfo;
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
WinFspMount::~WinFspMount() = default;

std::wstring WinFspMount::mountPoint() const {
    PWSTR mp = FspFileSystemMountPoint(state_->fs);
    return mp ? mp : L"";
}

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

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &s->sd,
                                                              &s->sdSize)) {
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
    vp.FileInfoTimeout = 10000;  // read-only: metadata can be cached
    vp.CaseSensitiveSearch = 1;
    vp.CasePreservedNames = 1;
    vp.UnicodeOnDisk = 1;
    vp.PersistentAcls = 1;
    vp.ReadOnlyVolume = 1;
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
    std::printf("Mounted read-only at %s - press Ctrl+C to unmount.\n",
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
