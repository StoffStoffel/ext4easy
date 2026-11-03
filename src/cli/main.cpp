// SPDX-License-Identifier: GPL-3.0-or-later
//
// ext4easy - command line tool.
//   ext4easy list [source]              show disks/partitions with ext2/3/4
//   ext4easy ls <source> [path]         list a directory
//   ext4easy cat <source> <path>        write a file to stdout
//   ext4easy stat <source> <path>       show file details
//   ext4easy mount <source> [X:]        mount as drive letter
//   ext4easy install | uninstall        background service + tray autostart
//   ext4easy service [--console]        the background service itself
//   ext4easy ctl <request...>           talk to the running service
//
// <source> is an image file or \\.\PhysicalDriveN, optionally followed by
// @N to choose partition N (default: first ext partition).

#include "core/disk_source.h"
#include "core/ext4_volume.h"
#include "core/partitions.h"
#include "core/text.h"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#include "mount/winfsp_fs.h"
#include "service/service_main.h"

using namespace ext4easy;

namespace {

void printUsage() {
    std::printf(
        "ext4easy - read ext2/3/4 filesystems on Windows\n\n"
        "Usage:\n"
        "  ext4easy list [source]          show disks/partitions with ext2/3/4\n"
        "  ext4easy ls   <source> [path]   list a directory\n"
        "  ext4easy cat  <source> <path>   write a file to stdout\n"
        "  ext4easy stat <source> <path>   show file details\n"
        "  ext4easy mount <source> [X:]    mount read-only as a drive letter\n"
        "\n"
        "  ext4easy install                install background service + tray app (admin)\n"
        "  ext4easy uninstall              remove them again (admin)\n"
        "  ext4easy service --console      run the service in this window (testing)\n"
        "  ext4easy ctl LIST|MOUNT id|...  send a request to the running service\n"
        "\n<source> is an image file or \\\\.\\PhysicalDriveN, optionally with @N\n"
        "to choose partition N (default: first ext partition).\n"
        "Raw disk access needs administrator rights.\n");
}

void err(const std::string& msg) { std::fprintf(stderr, "error: %s\n", msg.c_str()); }
void err(const std::wstring& msg) { err(wideToUtf8(msg)); }

std::string formatTime(uint64_t fileTime) {
    if (fileTime == 0) return "-";
    FILETIME ft{static_cast<DWORD>(fileTime), static_cast<DWORD>(fileTime >> 32)};
    FILETIME local;
    SYSTEMTIME s;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", s.wYear, s.wMonth, s.wDay,
                  s.wHour, s.wMinute, s.wSecond);
    return buf;
}

std::string modeString(uint32_t mode) {
    std::string s(10, '-');
    switch (mode & 0xF000) {
        case 0x4000: s[0] = 'd'; break;
        case 0xA000: s[0] = 'l'; break;
        case 0x2000: s[0] = 'c'; break;
        case 0x6000: s[0] = 'b'; break;
        case 0x1000: s[0] = 'p'; break;
        case 0xC000: s[0] = 's'; break;
    }
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (mode & (0400 >> i)) s[1 + i] = rwx[i];
    return s;
}

void printPartitions(const std::wstring& path, DiskSource& disk) {
    auto parts = scanPartitions(disk);
    std::printf("%s  (%s, sector %u)\n", wideToUtf8(path).c_str(), formatSize(disk.size()).c_str(),
                disk.sectorSize());
    if (parts.empty()) {
        std::printf("    no partitions\n");
        return;
    }
    for (const auto& p : parts) {
        std::printf("  @%-3d %-4s %10s  %-22s", p.number, p.scheme.c_str(),
                    formatSize(p.size).c_str(), p.typeName.c_str());
        if (p.ext.found) {
            std::printf(" %s", p.ext.kind.c_str());
            if (!p.ext.label.empty()) std::printf(" \"%s\"", p.ext.label.c_str());
            auto unsupported = p.ext.unsupportedFeatures();
            if (!unsupported.empty()) {
                std::printf("  [unsupported:");
                for (const auto& f : unsupported) std::printf(" %s", f.c_str());
                std::printf("]");
            }
            if (p.ext.needsRecovery()) std::printf("  [journal needs recovery]");
        }
        std::printf("\n");
    }
}

int cmdList(const std::vector<std::wstring>& args) {
    if (!args.empty()) {
        std::wstring e;
        auto disk = DiskSource::open(args[0], e);
        if (!disk) {
            err(e);
            return 1;
        }
        printPartitions(args[0], *disk);
        return 0;
    }
    int found = 0, denied = 0;
    for (int i = 0; i < 64; ++i) {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
        std::wstring e;
        auto disk = DiskSource::open(path, e);
        if (!disk) {
            if (e.find(L"administrator") != std::wstring::npos) ++denied;
            continue;
        }
        ++found;
        printPartitions(path, *disk);
    }
    if (denied > 0)
        std::printf("Access denied for %d disk(s) - run as administrator.\n", denied);
    else if (found == 0)
        std::printf("No disks found.\n");
    return 0;
}

// Opens "<path>[@N]" and mounts the chosen partition.
std::unique_ptr<Ext4Volume> openVolume(const std::wstring& source) {
    std::wstring path = source;
    int wanted = -1;
    size_t at = source.rfind(L'@');
    if (at != std::wstring::npos && at + 1 < source.size()) {
        wchar_t* end = nullptr;
        long n = std::wcstol(source.c_str() + at + 1, &end, 10);
        if (end && *end == 0) {
            wanted = static_cast<int>(n);
            path = source.substr(0, at);
        }
    }

    std::wstring e;
    auto disk = DiskSource::open(path, e);
    if (!disk) {
        err(e);
        return nullptr;
    }
    auto parts = scanPartitions(*disk);
    const Partition* chosen = nullptr;
    for (const auto& p : parts) {
        if (wanted >= 0 ? p.number == wanted : p.ext.found) {
            chosen = &p;
            break;
        }
    }
    if (!chosen) {
        err(wanted >= 0 ? "partition @" + std::to_string(wanted) + " not found"
                        : std::string("no ext2/3/4 partition found"));
        return nullptr;
    }
    if (chosen->ext.needsRecovery())
        std::fprintf(stderr,
                     "warning: filesystem was not cleanly unmounted (journal not replayed); "
                     "recent changes may be missing\n");

    std::string me;
    auto vol = Ext4Volume::mount(disk, chosen->offset, chosen->size, me);
    if (!vol) err(me);
    return vol;
}

std::string argPath(const std::wstring& a) {
    std::string p = wideToUtf8(a);
    for (auto& c : p)
        if (c == '\\') c = '/';
    if (p.empty() || p[0] != '/') p = "/" + p;
    return p;
}

int cmdLs(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        printUsage();
        return 2;
    }
    auto vol = openVolume(args[0]);
    if (!vol) return 1;
    std::string path = args.size() > 1 ? argPath(args[1]) : "/";
    std::vector<DirEntry> entries;
    int r = vol->listDir(path, entries);
    if (r != 0) {
        err(path + ": " + ext4ErrorText(r));
        return 1;
    }
    for (const auto& e : entries) {
        std::printf("%s %5u %5u %12llu  %s  %s%s\n", modeString(e.st.mode).c_str(), e.st.uid,
                    e.st.gid, static_cast<unsigned long long>(e.st.size),
                    formatTime(e.st.mtime).c_str(), e.name.c_str(),
                    e.st.isSymlinkTarget ? "  ->" : (e.st.isDir() ? "/" : ""));
    }
    return 0;
}

int cmdCat(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        printUsage();
        return 2;
    }
    auto vol = openVolume(args[0]);
    if (!vol) return 1;
    std::string path = argPath(args[1]);
    int r = 0;
    FileHandle* f = vol->openFile(path, r);
    if (!f) {
        err(path + ": " + ext4ErrorText(r));
        return 1;
    }
    _setmode(_fileno(stdout), _O_BINARY);
    std::vector<char> buf(1 << 20);
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        r = vol->readFile(f, off, buf.data(), buf.size(), got);
        if (r != 0) {
            err(path + ": " + ext4ErrorText(r));
            break;
        }
        if (got == 0) break;
        std::fwrite(buf.data(), 1, got, stdout);
        off += got;
    }
    vol->closeFile(f);
    return r == 0 ? 0 : 1;
}

int cmdStat(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        printUsage();
        return 2;
    }
    auto vol = openVolume(args[0]);
    if (!vol) return 1;
    std::string path = argPath(args[1]);
    FileStat l, st;
    int r = vol->lstat(path, l);
    if (r != 0) {
        err(path + ": " + ext4ErrorText(r));
        return 1;
    }
    if (l.isSymlink()) {
        std::string target;
        vol->readLink(path, target);
        std::printf("Symlink:  %s -> %s\n", path.c_str(), target.c_str());
    }
    std::string real;
    r = vol->stat(path, st, &real);
    if (r != 0) {
        err("target: " + ext4ErrorText(r));
        return 1;
    }
    std::printf("Path:     %s\nInode:    %u\nMode:     %s (%o)\nSize:     %llu\n"
                "On disk:  %llu\nLinks:    %u\nUID/GID:  %u/%u\nModified: %s\nChanged:  %s\n"
                "Accessed: %s\nCreated:  %s\n",
                real.c_str(), st.ino, modeString(st.mode).c_str(), st.mode & 07777,
                static_cast<unsigned long long>(st.size),
                static_cast<unsigned long long>(st.allocated), st.nlink, st.uid, st.gid,
                formatTime(st.mtime).c_str(), formatTime(st.ctime).c_str(),
                formatTime(st.atime).c_str(), formatTime(st.crtime).c_str());
    return 0;
}

int cmdMount(const std::vector<std::wstring>& args) {
    if (args.empty()) {
        printUsage();
        return 2;
    }
    auto vol = openVolume(args[0]);
    if (!vol) return 1;
    std::wstring mountPoint = args.size() > 1 ? args[1] : L"";
    return runMountUntilCtrlC(std::move(vol), mountPoint);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::wstring cmd = argv[1];
    std::vector<std::wstring> args(argv + 2, argv + argc);
    if (cmd == L"list") return cmdList(args);
    if (cmd == L"ls") return cmdLs(args);
    if (cmd == L"cat") return cmdCat(args);
    if (cmd == L"stat") return cmdStat(args);
    if (cmd == L"mount") return cmdMount(args);
    if (cmd == L"install") return installService();
    if (cmd == L"uninstall") return uninstallService();
    if (cmd == L"service")
        return !args.empty() && args[0] == L"--console" ? runServiceConsole() : runServiceDispatcher();
    if (cmd == L"ctl") return runCtl(args);
    printUsage();
    return 2;
}
