// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ext4win {

class DiskSource;

// Error codes are errno values (0 = success), e.g. ENOENT, ENOTDIR, EIO.
// All paths are UTF-8, absolute within the volume, e.g. "/home/user".

struct FileStat {
    uint32_t ino = 0;
    uint32_t mode = 0;  // Linux st_mode (type + permissions)
    uint64_t size = 0;
    uint64_t allocated = 0;  // bytes on disk
    uint32_t nlink = 0, uid = 0, gid = 0;
    // Windows FILETIME values (100 ns since 1601-01-01).
    uint64_t atime = 0, mtime = 0, ctime = 0, crtime = 0;
    bool isSymlinkTarget = false;  // stat of a symlink's target, not the link itself

    uint32_t type() const { return mode & 0xF000; }
    bool isDir() const { return type() == 0x4000; }
    bool isFile() const { return type() == 0x8000; }
    bool isSymlink() const { return type() == 0xA000; }
};

struct DirEntry {
    std::string name;  // raw Linux name (bytes)
    FileStat st;       // symlinks are reported with their target's stat when resolvable
};

struct VolumeInfo {
    uint64_t totalBytes = 0, freeBytes = 0;
    uint32_t blockSize = 0;
    std::string label;
    uint8_t uuid[16] = {};
    uint64_t createTime = 0;  // FILETIME, 0 if unknown
    bool readOnly = true;
    bool needsRecovery = false;
};

struct FileHandle;  // opaque handle for reading file contents

class Ext4Volume {
public:
    // Mounts the filesystem at [offset, offset+size) of the disk, read-only.
    static std::unique_ptr<Ext4Volume> mount(std::shared_ptr<DiskSource> disk, uint64_t offset,
                                             uint64_t size, std::string& error);
    ~Ext4Volume();

    Ext4Volume(const Ext4Volume&) = delete;
    Ext4Volume& operator=(const Ext4Volume&) = delete;

    VolumeInfo info();

    // Follows symlinks (also in intermediate components). realPath receives
    // the resolved path if not null. ENOENT = last component missing,
    // ENOTDIR = an intermediate component is missing or not a directory.
    int stat(const std::string& path, FileStat& st, std::string* realPath = nullptr);
    int lstat(const std::string& path, FileStat& st);  // does not follow the last symlink
    int listDir(const std::string& path, std::vector<DirEntry>& out);
    int readLink(const std::string& path, std::string& target);

    FileHandle* openFile(const std::string& path, int& error);
    int readFile(FileHandle* f, uint64_t offset, void* buffer, size_t length, size_t& bytesRead);
    void closeFile(FileHandle* f);

    struct Impl;

private:
    explicit Ext4Volume(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Text for an errno value returned by the functions above.
std::string ext4ErrorText(int err);

}  // namespace ext4win
