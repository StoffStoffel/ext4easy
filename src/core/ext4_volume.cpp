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

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>

// lwext4 is built with its own open flags; make sure no system header
// replaced them with the MSVC values in this file.
static_assert(O_RDWR == 02 && O_CREAT == 0100, "lwext4 open flags expected");

namespace ext4win {

namespace {

// lwext4 keeps global tables (devices, mount points) and is not thread-safe,
// so every call into it is serialized through this lock.
std::recursive_mutex g_lock;
bool g_slotUsed[CONFIG_EXT4_MOUNTPOINTS_COUNT] = {};

constexpr uint32_t kRootIno = 2;
constexpr int kMaxSymlinkHops = 40;
constexpr size_t kMaxSymlinkLen = 4096;
constexpr size_t kMaxNameLen = 255;

int bdOpen(struct ext4_blockdev*) { return EOK; }
int bdClose(struct ext4_blockdev*) { return EOK; }

int bdRead(struct ext4_blockdev* bd, void* buf, uint64_t blk, uint32_t cnt) {
    auto* disk = static_cast<DiskSource*>(bd->bdif->p_user);
    uint64_t bs = bd->bdif->ph_bsize;
    return disk->read(blk * bs, buf, static_cast<size_t>(cnt * bs)) ? EOK : EIO;
}

int bdWrite(struct ext4_blockdev* bd, const void* buf, uint64_t blk, uint32_t cnt) {
    auto* disk = static_cast<DiskSource*>(bd->bdif->p_user);
    if (!disk->isWritable()) return EROFS;
    uint64_t bs = bd->bdif->ph_bsize;
    return disk->write(blk * bs, buf, static_cast<size_t>(cnt * bs)) ? EOK : EIO;
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

void fromFileTime(uint64_t ft, int64_t& sec, uint32_t& nsec) {
    sec = static_cast<int64_t>(ft / 10000000ULL) - 11644473600LL;
    nsec = static_cast<uint32_t>(ft % 10000000ULL) * 100;
}

uint64_t nowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
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

std::string parentOf(const std::string& path) {
    auto parts = splitPath(path);
    if (!parts.empty()) parts.pop_back();
    return joinPath(parts);
}

}  // namespace

struct FileHandle {
    ext4_file file{};
    bool writable = false;
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
    bool writable = false;
    bool journalStarted = false;
    bool needsRecovery = false;
    CreateOptions createOptions;

    ~Impl() {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (journalStarted) ext4_journal_stop(mountPoint.c_str());
        if (mounted) ext4_umount(mountPoint.c_str());
        if (registered) ext4_device_unregister(devName.c_str());
        if (writable && disk) disk->flush();
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

    // Stat without following symlinks; path must already be resolved.
    int lstatRaw(const std::string& path, FileStat& st) {
        uint32_t ino = 0;
        ext4_inode inode{};
        int r = lookup(path, ino, inode);
        if (r == EOK) fillStat(ino, inode, st);
        return r;
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

    // For operations on a name: resolves the parent directory (following
    // symlinks) and returns the real path of the entry itself, which may
    // or may not exist.
    int resolveEntry(const std::string& path, std::string& real, FileStat& parent) {
        auto parts = splitPath(path);
        if (parts.empty()) return EINVAL;  // the root itself
        std::string name = parts.back();
        parts.pop_back();
        if (name == "." || name == "..") return EINVAL;
        if (name.size() > kMaxNameLen) return ENAMETOOLONG;
        std::string parentReal;
        int r = statImpl(joinPath(parts), parent, &parentReal);
        if (r != EOK) return r == ENOENT ? ENOTDIR : r;
        if (!parent.isDir()) return ENOTDIR;
        real = childPath(parentReal, name);
        return EOK;
    }

    int setTimesRaw(const std::string& real, uint64_t at, uint64_t mt, uint64_t ct,
                    uint64_t crt) {
        ext4_times t{};
        const uint64_t v[4] = {at, mt, ct, crt};
        for (int i = 0; i < 4; ++i) {
            if (!v[i]) continue;
            fromFileTime(v[i], t.sec[i], t.nsec[i]);
            t.mask |= 1u << i;
        }
        if (!t.mask) return EOK;
        return ext4_times_set(full(real).c_str(), &t);
    }

    // Directory contents changed: update its mtime and ctime.
    void touchDir(const std::string& real) {
        uint64_t now = nowFileTime();
        setTimesRaw(real, 0, now, now, 0);
    }

    // Mode, owner and all four timestamps of a freshly created inode.
    int initNewInode(const std::string& real, bool isDir, const FileStat& parent) {
        const std::string p = full(real);
        int r = ext4_mode_set(p.c_str(), isDir ? createOptions.dirMode : createOptions.fileMode);
        if (r != EOK) return r;
        uint32_t uid = createOptions.inheritOwner ? parent.uid : createOptions.uid;
        uint32_t gid = createOptions.inheritOwner ? parent.gid : createOptions.gid;
        r = ext4_owner_set(p.c_str(), uid, gid);
        if (r != EOK) return r;
        uint64_t now = nowFileTime();
        return setTimesRaw(real, now, now, now, now);
    }

    int isDirEmpty(const std::string& real, bool& empty) {
        std::string lw = full(real);
        if (lw.back() != '/') lw.push_back('/');
        ext4_dir dir{};
        int r = ext4_dir_open(&dir, lw.c_str());
        if (r != EOK) return r;
        empty = true;
        const ext4_direntry* de;
        while ((de = ext4_dir_entry_next(&dir)) != nullptr) {
            if (de->inode == 0) continue;
            std::string name(reinterpret_cast<const char*>(de->name), de->name_length);
            if (name != "." && name != "..") {
                empty = false;
                break;
            }
        }
        ext4_dir_close(&dir);
        return EOK;
    }

    // Keep the cached size of a handle in sync with the inode (other
    // handles may have changed the file).
    void syncSize(FileHandle* f) {
        ext4_inode in{};
        if (ext4_raw_inode_get(mountPoint.c_str(), f->file.inode, &in) == EOK)
            f->file.fsize = ext4_inode_get_size(sb(), &in);
    }

    int grow(FileHandle* f, uint64_t size) {
        int r = ext4_fextend(&f->file, size);
        if (r != ENOTSUP) return r;
        // Not extent-mapped: holes are impossible, write real zeros.
        std::vector<uint8_t> zeros(1 << 20);
        uint64_t pos = f->file.fsize;
        r = ext4_fseek(&f->file, static_cast<int64_t>(pos), SEEK_SET);
        while (r == EOK && pos < size) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(zeros.size(), size - pos));
            size_t done = 0;
            r = ext4_fwrite(&f->file, zeros.data(), n, &done);
            pos += done;
            if (r == EOK && done == 0) r = EIO;
        }
        return r;
    }
};

Ext4Volume::Ext4Volume(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Ext4Volume::~Ext4Volume() = default;

std::string Ext4Volume::writeBlocker(uint32_t compat, uint32_t incompat, uint32_t roCompat) {
    if (!(incompat & EXT4_FINCOM_EXTENTS))
        return "ext2/ext3 filesystems without extents can only be read";
    if (incompat & EXT4_FINCOM_RECOVER)
        return "the journal was not replayed (drive not cleanly removed) - connect it to Linux "
               "once or run e2fsck, then try again";
    if (incompat & EXT4_FINCOM_MMP) return "multi-mount protection (mmp) is enabled";
    if (compat & 0x0400) return "fast_commit journaling is enabled";

    struct Feature { uint32_t bit; const char* name; };
    static const Feature roNames[] = {
        {0x0004, "btree_dir"}, {0x0100, "quota"}, {0x0200, "bigalloc"}, {0x1000, "readonly"},
        {0x2000, "project"}, {0x4000, "shared_blocks"}, {0x8000, "verity"},
        {0x10000, "orphan_present"},
    };
    uint32_t rest = roCompat & ~static_cast<uint32_t>(EXT4_SUPPORTED_FRO_COM);
    if (!rest) return "";
    std::string msg = "features that cannot be maintained when writing:";
    for (const auto& f : roNames) {
        if (rest & f.bit) {
            msg += std::string(" ") + f.name;
            rest &= ~f.bit;
        }
    }
    if (rest) msg += " unknown(" + std::to_string(rest) + ")";
    return msg;
}

std::unique_ptr<Ext4Volume> Ext4Volume::mount(std::shared_ptr<DiskSource> disk, uint64_t offset,
                                              uint64_t size, std::string& error, bool writable) {
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
    if (writable) {
        std::string blocker = writeBlocker(probe.compat, probe.incompat, probe.roCompat);
        if (!blocker.empty()) {
            error = "Cannot mount writable: " + blocker;
            return nullptr;
        }
        if (!disk->isWritable()) {
            error = "Cannot mount writable: the disk was opened read-only";
            return nullptr;
        }
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
    m->writable = writable;

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

    r = ext4_mount(m->devName.c_str(), m->mountPoint.c_str(), !writable);
    if (r != EOK) {
        error = "Mount failed: " + ext4ErrorText(r);
        return nullptr;
    }
    m->mounted = true;

    if (writable) {
        r = ext4_journal_start(m->mountPoint.c_str());
        if (r != EOK) {
            error = "Cannot start the journal: " + ext4ErrorText(r);
            return nullptr;
        }
        m->journalStarted = true;
    }
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
    vi.readOnly = !impl_->writable;
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

FileHandle* Ext4Volume::openFile(const std::string& path, int& error, bool forWrite) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (forWrite && !impl_->writable) {
        error = EROFS;
        return nullptr;
    }
    std::string real;
    FileStat st;
    error = impl_->statImpl(path, st, &real);
    if (error != EOK) return nullptr;
    if (st.isDir()) {
        error = EISDIR;
        return nullptr;
    }
    auto* f = new FileHandle();
    f->writable = forWrite;
    error = ext4_fopen2(&f->file, impl_->full(real).c_str(), forWrite ? O_RDWR : O_RDONLY);
    if (error != EOK) {
        delete f;
        return nullptr;
    }
    return f;
}

int Ext4Volume::readFile(FileHandle* f, uint64_t offset, void* buffer, size_t length,
                         size_t& bytesRead) {
    bytesRead = 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    impl_->syncSize(f);
    uint64_t size = f->file.fsize;
    if (offset >= size || length == 0) return EOK;
    if (length > size - offset) length = static_cast<size_t>(size - offset);
    int r = ext4_fseek(&f->file, static_cast<int64_t>(offset), SEEK_SET);
    if (r != EOK) return r;
    return ext4_fread(&f->file, buffer, length, &bytesRead);
}

uint64_t Ext4Volume::fileSize(FileHandle* f) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    impl_->syncSize(f);
    return f->file.fsize;
}

void Ext4Volume::closeFile(FileHandle* f) {
    if (!f) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ext4_fclose(&f->file);
    delete f;
}

FileHandle* Ext4Volume::createFile(const std::string& path, int& error) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) {
        error = EROFS;
        return nullptr;
    }
    std::string real;
    FileStat parent, existing;
    error = impl_->resolveEntry(path, real, parent);
    if (error != EOK) return nullptr;
    if (impl_->lstatRaw(real, existing) == EOK) {
        error = EEXIST;
        return nullptr;
    }
    auto* f = new FileHandle();
    f->writable = true;
    error = ext4_fopen2(&f->file, impl_->full(real).c_str(), O_RDWR | O_CREAT);
    if (error != EOK) {  // not opened, nothing to close
        delete f;
        return nullptr;
    }
    error = impl_->initNewInode(real, false, parent);
    if (error != EOK) {
        ext4_fclose(&f->file);
        delete f;
        return nullptr;
    }
    impl_->touchDir(parentOf(real));
    return f;
}

int Ext4Volume::writeFile(FileHandle* f, uint64_t offset, const void* data, size_t length,
                          size_t& written) {
    written = 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable || !f->writable) return EROFS;
    if (length == 0) return EOK;
    impl_->syncSize(f);
    if (offset > f->file.fsize) {
        int r = impl_->grow(f, offset);
        if (r != EOK) return r;
    }
    int r = ext4_fseek(&f->file, static_cast<int64_t>(offset), SEEK_SET);
    if (r != EOK) return r;
    return ext4_fwrite(&f->file, data, length, &written);
}

int Ext4Volume::setFileSize(FileHandle* f, uint64_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable || !f->writable) return EROFS;
    impl_->syncSize(f);
    if (size < f->file.fsize) return ext4_ftruncate(&f->file, size);
    if (size > f->file.fsize) return impl_->grow(f, size);
    return EOK;
}

int Ext4Volume::mkdir(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EROFS;
    std::string real;
    FileStat parent, existing;
    int r = impl_->resolveEntry(path, real, parent);
    if (r != EOK) return r;
    if (impl_->lstatRaw(real, existing) == EOK) return EEXIST;
    r = ext4_dir_mk(impl_->full(real).c_str());
    if (r != EOK) return r;
    r = impl_->initNewInode(real, true, parent);
    impl_->touchDir(parentOf(real));
    return r;
}

int Ext4Volume::removeFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EROFS;
    std::string real;
    FileStat parent, st;
    int r = impl_->resolveEntry(path, real, parent);
    if (r != EOK) return r;
    r = impl_->lstatRaw(real, st);
    if (r != EOK) return r;
    if (st.isDir()) return EISDIR;
    r = ext4_fremove(impl_->full(real).c_str());
    if (r == EOK) impl_->touchDir(parentOf(real));
    return r;
}

int Ext4Volume::removeDir(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EROFS;
    std::string real;
    FileStat parent, st;
    int r = impl_->resolveEntry(path, real, parent);
    if (r != EOK) return r;
    r = impl_->lstatRaw(real, st);
    if (r != EOK) return r;
    if (!st.isDir()) return ENOTDIR;
    bool empty = false;
    r = impl_->isDirEmpty(real, empty);
    if (r != EOK) return r;
    if (!empty) return ENOTEMPTY;
    r = ext4_dir_rm(impl_->full(real).c_str());
    if (r == EOK) impl_->touchDir(parentOf(real));
    return r;
}

int Ext4Volume::rename(const std::string& from, const std::string& to, bool replace) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EROFS;
    std::string realFrom, realTo;
    FileStat parentFrom, parentTo, src, dst;
    int r = impl_->resolveEntry(from, realFrom, parentFrom);
    if (r != EOK) return r;
    r = impl_->resolveEntry(to, realTo, parentTo);
    if (r != EOK) return r;
    r = impl_->lstatRaw(realFrom, src);
    if (r != EOK) return r;
    if (realFrom == realTo) return EOK;
    if (src.isDir() && realTo.rfind(realFrom + "/", 0) == 0) return EINVAL;  // into itself

    if (impl_->lstatRaw(realTo, dst) == EOK) {
        if (!replace) return EEXIST;
        if (dst.isDir()) {
            if (!src.isDir()) return EISDIR;
            bool empty = false;
            r = impl_->isDirEmpty(realTo, empty);
            if (r != EOK) return r;
            if (!empty) return ENOTEMPTY;
            r = ext4_dir_rm(impl_->full(realTo).c_str());
        } else {
            if (src.isDir()) return ENOTDIR;
            r = ext4_fremove(impl_->full(realTo).c_str());
        }
        if (r != EOK) return r;
    }

    r = src.isDir() ? ext4_dir_mv(impl_->full(realFrom).c_str(), impl_->full(realTo).c_str())
                    : ext4_frename(impl_->full(realFrom).c_str(), impl_->full(realTo).c_str());
    if (r != EOK) return r;
    uint64_t now = nowFileTime();
    impl_->setTimesRaw(realTo, 0, 0, now, 0);
    impl_->touchDir(parentOf(realFrom));
    if (parentOf(realTo) != parentOf(realFrom)) impl_->touchDir(parentOf(realTo));
    return EOK;
}

int Ext4Volume::setTimes(const std::string& path, uint64_t atime, uint64_t mtime, uint64_t ctime,
                         uint64_t crtime) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EROFS;
    std::string real;
    FileStat st;
    int r = impl_->statImpl(path, st, &real);
    if (r != EOK) return r;
    return impl_->setTimesRaw(real, atime, mtime, ctime, crtime);
}

int Ext4Volume::flush() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!impl_->writable) return EOK;
    int r = ext4_cache_flush(impl_->mountPoint.c_str());
    if (!impl_->disk->flush() && r == EOK) r = EIO;
    return r;
}

void Ext4Volume::setCreateOptions(const CreateOptions& o) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    impl_->createOptions = o;
}

std::string ext4ErrorText(int err) {
    switch (err) {
        case EOK: return "OK";
        case ENOENT: return "not found";
        case ENOTDIR: return "path not found / not a directory";
        case EISDIR: return "is a directory";
        case EEXIST: return "already exists";
        case ENOTEMPTY: return "directory not empty";
        case ENOSPC: return "no space left on the drive";
        case ENAMETOOLONG: return "name too long (max. 255 bytes)";
        case EIO: return "I/O error";
        case ENOMEM: return "out of memory";
        case ENOTSUP: return "not supported";
        case EROFS: return "read-only filesystem";
        case ELOOP: return "too many symbolic links";
        case EINVAL: return "invalid argument";
        default: return "error " + std::to_string(err);
    }
}

}  // namespace ext4win
