// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ext4_volume.h"

#include "core/disk_source.h"
#include "core/partitions.h"

#include <ext4.h>
#include <ext4_errno.h>
#include <ext4_inode.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_types.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>

namespace ext4easy {

namespace {

// lwext4 keeps global tables (devices, mount points) and is not thread-safe,
// so every call into it is serialized through this lock.
std::recursive_mutex g_lock;
bool g_slotUsed[CONFIG_EXT4_MOUNTPOINTS_COUNT] = {};

constexpr uint32_t kRootIno = 2;
constexpr int kMaxSymlinkHops = 40;
constexpr size_t kMaxSymlinkLen = 4096;

int bdOpen(struct ext4_blockdev*) { return EOK; }
int bdClose(struct ext4_blockdev*) { return EOK; }
int bdWrite(struct ext4_blockdev*, const void*, uint64_t, uint32_t) { return EROFS; }

int bdRead(struct ext4_blockdev* bd, void* buf, uint64_t blk, uint32_t cnt) {
    auto* disk = static_cast<DiskSource*>(bd->bdif->p_user);
    uint64_t bs = bd->bdif->ph_bsize;
    return disk->read(blk * bs, buf, static_cast<size_t>(cnt * bs)) ? EOK : EIO;
}

// ext4 timestamp (seconds + optional "extra" field) -> Windows FILETIME.
uint64_t toFileTime(uint32_t seconds, uint32_t extra, bool hasExtra) {
    int64_t s = static_cast<int32_t>(seconds);
    uint32_t ns = 0;
    if (hasExtra) {
        s += static_cast<int64_t>(extra & 3) << 32;  // epoch bits (dates after 2038)
        ns = extra >> 2;
    }
    int64_t ft = (s + 11644473600LL) * 10000000LL + ns / 100;
    return ft < 0 ? 0 : static_cast<uint64_t>(ft);
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) parts.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    return parts;
}

std::string joinPath(const std::vector<std::string>& parts) {
    if (parts.empty()) return "/";
    std::string out;
    for (const auto& p : parts) {
        out.push_back('/');
        out += p;
    }
    return out;
}

std::string childPath(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}

}  // namespace

struct FileHandle {
    ext4_file file{};
    uint64_t size = 0;
};

struct Ext4Volume::Impl {
    std::shared_ptr<DiskSource> disk;
    ext4_blockdev_iface iface{};
    ext4_blockdev bdev{};
    std::vector<uint8_t> physBuffer;
    int slot = -1;
    std::string devName;
    std::string mountPoint;  // e.g. "/mp3/"
    bool registered = false;
    bool mounted = false;
    bool needsRecovery = false;

    ~Impl() {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (mounted) ext4_umount(mountPoint.c_str());
        if (registered) ext4_device_unregister(devName.c_str());
        if (slot >= 0) g_slotUsed[slot] = false;
    }

    // "/dir/file" -> "/mpN/dir/file" (lwext4 path)
    std::string full(const std::string& path) const {
        size_t start = 0;
        while (start < path.size() && path[start] == '/') ++start;
        return mountPoint + path.substr(start);
    }

    ext4_sblock* sb() {
        ext4_sblock* s = nullptr;
        ext4_get_sblock(mountPoint.c_str(), &s);
        return s;
    }

    int lookup(const std::string& path, uint32_t& ino, ext4_inode& inode) {
        if (path == "/" || path.empty()) {
            ino = kRootIno;
            return ext4_raw_inode_get(mountPoint.c_str(), kRootIno, &inode);
        }
        return ext4_raw_inode_fill(full(path).c_str(), &ino, &inode);
    }

    int readLinkRaw(const std::string& path, std::string& target) {
        std::vector<char> buf(kMaxSymlinkLen + 1);
        size_t n = 0;
        int r = ext4_readlink(full(path).c_str(), buf.data(), kMaxSymlinkLen, &n);
        if (r != EOK) return r;
        target.assign(buf.data(), n);
        return EOK;
    }

    void fillStat(uint32_t ino, ext4_inode& in, FileStat& st) {
        ext4_sblock* s = sb();
        st = FileStat{};
        st.ino = ino;
        st.mode = ext4_inode_get_mode(s, &in);
        st.size = ext4_inode_get_size(s, &in);
        st.allocated = ext4_inode_get_blocks_count(s, &in) * 512;
        st.nlink = ext4_inode_get_links_cnt(&in);
        st.uid = ext4_inode_get_uid(&in);
        st.gid = ext4_inode_get_gid(&in);

        // Fields after the first 128 bytes exist only if extra_isize covers them.
        uint32_t extra = 0;
        if (ext4_get16(s, inode_size) > EXT4_GOOD_OLD_INODE_SIZE) extra = to_le16(in.extra_isize);
        auto has = [&](uint32_t offsetEnd) { return EXT4_GOOD_OLD_INODE_SIZE + extra >= offsetEnd; };

        st.ctime = toFileTime(to_le32(in.change_inode_time), to_le32(in.ctime_extra), has(0x88));
        st.mtime = toFileTime(to_le32(in.modification_time), to_le32(in.mtime_extra), has(0x8C));
        st.atime = toFileTime(to_le32(in.access_time), to_le32(in.atime_extra), has(0x90));
        if (has(0x94) && in.crtime != 0)
            st.crtime = toFileTime(to_le32(in.crtime), to_le32(in.crtime_extra), has(0x98));
        else
            st.crtime = st.ctime < st.mtime ? st.ctime : st.mtime;  // best guess
    }

    // Walks the path component by component, following symlinks.
    int resolve(const std::string& path, bool followLast, std::string& real, uint32_t& ino,
                ext4_inode& inode) {
        auto parts = splitPath(path);
        std::deque<std::string> pending(parts.begin(), parts.end());
        std::vector<std::string> done;
        bool haveInode = false;
        int hops = 0;

        while (!pending.empty()) {
            std::string c = std::move(pending.front());
            pending.pop_front();
            if (c == ".") continue;
            if (c == "..") {
                if (!done.empty()) done.pop_back();
                haveInode = false;
                continue;
            }
            done.push_back(c);
            std::string cur = joinPath(done);
            int r = lookup(cur, ino, inode);
            if (r != EOK) return (r == ENOENT && !pending.empty()) ? ENOTDIR : r;

            uint32_t type = ext4_inode_get_mode(sb(), &inode) & 0xF000;
            bool last = pending.empty();
            if (type == 0xA000 && (!last || followLast)) {
                if (++hops > kMaxSymlinkHops) return ELOOP;
                std::string target;
                r = readLinkRaw(cur, target);
                if (r != EOK) return r;
                done.pop_back();
                if (!target.empty() && target[0] == '/') done.clear();  // absolute: volume root
                auto t = splitPath(target);
                pending.insert(pending.begin(), t.begin(), t.end());
                haveInode = false;
            } else {
                if (!last && type != 0x4000) return ENOTDIR;
                haveInode = true;
            }
        }
        real = joinPath(done);
        if (!haveInode) return lookup(real, ino, inode);
        return EOK;
    }

    int statImpl(const std::string& path, FileStat& st, std::string* realPath) {
        std::string real;
        uint32_t ino = 0;
        ext4_inode inode{};
        int r = resolve(path, true, real, ino, inode);
        if (r != EOK) return r;
        fillStat(ino, inode, st);
        if (realPath) *realPath = real;
        return EOK;
    }
};

Ext4Volume::Ext4Volume(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Ext4Volume::~Ext4Volume() = default;

std::unique_ptr<Ext4Volume> Ext4Volume::mount(std::shared_ptr<DiskSource> disk, uint64_t offset,
                                              uint64_t size, std::string& error) {
    ExtProbe probe = probeExt(*disk, offset);
    if (!probe.found) {
        error = "No ext2/3/4 filesystem found at this location";
        return nullptr;
    }
    auto unsupported = probe.unsupportedFeatures();
    if (!unsupported.empty()) {
        error = "Filesystem uses unsupported features:";
        for (const auto& f : unsupported) error += " " + f;
        return nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto m = std::make_unique<Impl>();
    for (int i = 0; i < CONFIG_EXT4_MOUNTPOINTS_COUNT; ++i) {
        if (!g_slotUsed[i]) {
            g_slotUsed[i] = true;
            m->slot = i;
            break;
        }
    }
    if (m->slot < 0) {
        error = "Too many mounted volumes";
        return nullptr;
    }
    m->devName = "dev" + std::to_string(m->slot);
    m->mountPoint = "/mp" + std::to_string(m->slot) + "/";
    m->disk = std::move(disk);
    m->needsRecovery = probe.needsRecovery();

    m->physBuffer.resize(m->disk->sectorSize());
    m->iface.open = bdOpen;
    m->iface.bread = bdRead;
    m->iface.bwrite = bdWrite;
    m->iface.close = bdClose;
    m->iface.ph_bsize = m->disk->sectorSize();
    m->iface.ph_bcnt = m->disk->size() / m->disk->sectorSize();
    m->iface.ph_bbuf = m->physBuffer.data();
    m->iface.p_user = m->disk.get();
    m->bdev.bdif = &m->iface;
    m->bdev.part_offset = offset;
    m->bdev.part_size = size;

    int r = ext4_device_register(&m->bdev, m->devName.c_str());
    if (r != EOK) {
        error = "Cannot register block device: " + ext4ErrorText(r);
        return nullptr;
    }
    m->registered = true;

    r = ext4_mount(m->devName.c_str(), m->mountPoint.c_str(), true);
    if (r != EOK) {
        error = "Mount failed: " + ext4ErrorText(r);
        return nullptr;
    }
    m->mounted = true;
    return std::unique_ptr<Ext4Volume>(new Ext4Volume(std::move(m)));
}

VolumeInfo Ext4Volume::info() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    VolumeInfo vi;
    ext4_mount_stats stats{};
    if (ext4_mount_point_stats(impl_->mountPoint.c_str(), &stats) == EOK) {
        vi.blockSize = stats.block_size;
        vi.totalBytes = stats.blocks_count * stats.block_size;
        vi.freeBytes = stats.free_blocks_count * stats.block_size;
        vi.label.assign(stats.volume_name, strnlen(stats.volume_name, sizeof stats.volume_name));
    }
    if (ext4_sblock* s = impl_->sb()) {
        std::memcpy(vi.uuid, s->uuid, sizeof vi.uuid);
        uint32_t mkfs = ext4_get32(s, mkfs_time);
        vi.createTime = mkfs ? toFileTime(mkfs, 0, false) : 0;
    }
    vi.readOnly = true;
    vi.needsRecovery = impl_->needsRecovery;
    return vi;
}

int Ext4Volume::stat(const std::string& path, FileStat& st, std::string* realPath) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return impl_->statImpl(path, st, realPath);
}

int Ext4Volume::lstat(const std::string& path, FileStat& st) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::string real;
    uint32_t ino = 0;
    ext4_inode inode{};
    int r = impl_->resolve(path, false, real, ino, inode);
    if (r == EOK) impl_->fillStat(ino, inode, st);
    return r;
}

int Ext4Volume::readLink(const std::string& path, std::string& target) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return impl_->readLinkRaw(path, target);
}

int Ext4Volume::listDir(const std::string& path, std::vector<DirEntry>& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    out.clear();
    std::string real;
    FileStat dirSt;
    int r = impl_->statImpl(path, dirSt, &real);
    if (r != EOK) return r;
    if (!dirSt.isDir()) return ENOTDIR;

    std::string lwPath = impl_->full(real);
    if (lwPath.back() != '/') lwPath.push_back('/');
    ext4_dir dir{};
    r = ext4_dir_open(&dir, lwPath.c_str());
    if (r != EOK) return r;

    const ext4_direntry* de;
    while ((de = ext4_dir_entry_next(&dir)) != nullptr) {
        if (de->inode == 0) continue;
        std::string name(reinterpret_cast<const char*>(de->name), de->name_length);
        if (name == "." || name == "..") continue;

        DirEntry e;
        e.name = std::move(name);
        ext4_inode inode{};
        if (ext4_raw_inode_get(impl_->mountPoint.c_str(), de->inode, &inode) != EOK) continue;
        impl_->fillStat(de->inode, inode, e.st);
        if (e.st.isSymlink()) {
            FileStat target;
            if (impl_->statImpl(childPath(real, e.name), target, nullptr) == EOK) {
                target.isSymlinkTarget = true;
                e.st = target;
            }
        }
        out.push_back(std::move(e));
    }
    ext4_dir_close(&dir);
    return EOK;
}

FileHandle* Ext4Volume::openFile(const std::string& path, int& error) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::string real;
    FileStat st;
    error = impl_->statImpl(path, st, &real);
    if (error != EOK) return nullptr;
    if (st.isDir()) {
        error = EISDIR;
        return nullptr;
    }
    auto* f = new FileHandle();
    error = ext4_fopen2(&f->file, impl_->full(real).c_str(), O_RDONLY);
    if (error != EOK) {
        delete f;
        return nullptr;
    }
    f->size = ext4_fsize(&f->file);
    return f;
}

int Ext4Volume::readFile(FileHandle* f, uint64_t offset, void* buffer, size_t length,
                         size_t& bytesRead) {
    bytesRead = 0;
    if (offset >= f->size || length == 0) return EOK;
    if (length > f->size - offset) length = static_cast<size_t>(f->size - offset);

    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int r = ext4_fseek(&f->file, static_cast<int64_t>(offset), SEEK_SET);
    if (r != EOK) return r;
    return ext4_fread(&f->file, buffer, length, &bytesRead);
}

void Ext4Volume::closeFile(FileHandle* f) {
    if (!f) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ext4_fclose(&f->file);
    delete f;
}

std::string ext4ErrorText(int err) {
    switch (err) {
        case EOK: return "OK";
        case ENOENT: return "not found";
        case ENOTDIR: return "path not found / not a directory";
        case EISDIR: return "is a directory";
        case EIO: return "I/O error";
        case ENOMEM: return "out of memory";
        case ENOTSUP: return "not supported";
        case EROFS: return "read-only filesystem";
        case ELOOP: return "too many symbolic links";
        case EINVAL: return "invalid argument";
        default: return "error " + std::to_string(err);
    }
}

}  // namespace ext4easy
