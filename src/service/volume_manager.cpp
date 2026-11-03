// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/volume_manager.h"

#include "core/disk_source.h"
#include "core/ext4_volume.h"
#include "core/text.h"
#include "mount/winfsp_fs.h"
#include "service/log.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace ext4win {

struct VolumeManager::Volume {
    int id = 0;
    std::wstring diskKey;
    std::shared_ptr<DiskSource> disk;
    Partition part;
    bool isImage = false;
    std::unique_ptr<WinFspMount> mount;
    std::string state;    // mounted | available | unsupported | error
    std::string message;  // warning or error text
    bool asked = false;   // user was already asked (or decided) since the disk appeared
};

struct VolumeManager::Disk {
    std::wstring key;
    std::shared_ptr<DiskSource> src;
    bool isImage = false;
    std::vector<int> volumeIds;
};

namespace {

std::wstring normalizeKey(const std::wstring& path) {
    std::wstring k = path;
    std::transform(k.begin(), k.end(), k.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return k;
}

bool driveLetterFree(wchar_t letter) {
    if (letter < L'A' || letter > L'Z') return false;
    return (GetLogicalDrives() & (1u << (letter - L'A'))) == 0;
}

std::string volumeName(const Partition& p) {
    return p.ext.label.empty() ? p.ext.kind + " @" + std::to_string(p.number) : p.ext.label;
}

}  // namespace

VolumeManager::VolumeManager() { settings_.load(); }

VolumeManager::~VolumeManager() { unmountAll(); }

void VolumeManager::diskArrived(const std::wstring& devicePath) {
    std::wstring key = normalizeKey(devicePath);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disks_.count(key)) return;
    }
    std::wstring error;
    auto src = DiskSource::open(devicePath, error);  // outside the lock: may take a moment
    if (!src) {
        logLine(L"cannot open disk: " + error);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (disks_.count(key)) return;
    for (const auto& [k, d] : disks_) {
        if (!d->isImage && src->deviceNumber() >= 0 && d->src->deviceNumber() == src->deviceNumber())
            return;  // same disk seen through another path
    }
    addDiskLocked(std::move(src), key, false);
}

void VolumeManager::diskRemoved(const std::wstring& devicePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    removeDiskLocked(normalizeKey(devicePath));
}

int VolumeManager::attachImage(std::shared_ptr<DiskSource> image, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring key = normalizeKey(image->path());
    if (disks_.count(key)) {
        error = "image is already attached";
        return -1;
    }
    addDiskLocked(image, key, true);
    auto& disk = disks_[key];
    if (disk->volumeIds.empty()) {
        disks_.erase(key);
        error = "no ext2/3/4 filesystem found in the image";
        return -1;
    }
    return static_cast<int>(disk->volumeIds.size());
}

bool VolumeManager::detach(int volumeId, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = volumes_.find(volumeId);
    if (it == volumes_.end()) {
        error = "unknown volume";
        return false;
    }
    if (!it->second->isImage) {
        error = "only image files can be detached";
        return false;
    }
    removeDiskLocked(it->second->diskKey);
    return true;
}

void VolumeManager::addDiskLocked(std::shared_ptr<DiskSource> src, const std::wstring& key,
                                  bool isImage) {
    auto disk = std::make_unique<Disk>();
    disk->key = key;
    disk->src = src;
    disk->isImage = isImage;

    for (auto& p : scanPartitions(*src)) {
        if (!p.ext.found) continue;
        auto v = std::make_unique<Volume>();
        v->id = nextVolumeId_++;
        v->diskKey = key;
        v->disk = src;
        v->part = p;
        v->isImage = isImage;
        auto unsupported = p.ext.unsupportedFeatures();
        if (!unsupported.empty()) {
            v->state = "unsupported";
            v->message = "unsupported features:";
            for (const auto& f : unsupported) v->message += " " + f;
        } else {
            v->state = "available";
            if (p.ext.needsRecovery())
                v->message = "not cleanly unmounted - recent changes may be missing";
        }
        logLine("found " + p.ext.kind + " \"" + volumeName(p) + "\" on " +
                wideToUtf8(src->description()) + " partition " + std::to_string(p.number) + " (" +
                formatSize(p.size) + "), " + v->state);
        disk->volumeIds.push_back(v->id);
        volumes_[v->id] = std::move(v);
    }
    disks_[key] = std::move(disk);

    for (int id : disks_[key]->volumeIds) applyPolicyLocked(*volumes_[id]);
    broadcastLocked("EVENT\tCHANGED");
}

void VolumeManager::removeDiskLocked(const std::wstring& key) {
    auto it = disks_.find(key);
    if (it == disks_.end()) return;
    for (int id : it->second->volumeIds) {
        auto v = volumes_.find(id);
        if (v == volumes_.end()) continue;
        if (v->second->mount)
            logLine("unmounting \"" + volumeName(v->second->part) + "\" (disk removed)");
        volumes_.erase(v);  // destroys the mount
    }
    disks_.erase(it);
    broadcastLocked("EVENT\tCHANGED");
}

void VolumeManager::applyPolicyLocked(Volume& v) {
    if (v.state != "available") return;
    Policy p = v.isImage ? Policy::Always : settings_.policy(v.part.ext.uuid);
    if (p == Policy::Always) {
        std::string error;
        if (mountLocked(v, 0, error))
            broadcastLocked("EVENT\tMOUNTED\t" + std::to_string(v.id) + "\t" +
                            wideToUtf8(v.mount->mountPoint()));
    } else if (p == Policy::Ask && !subscribers_.empty() && !v.asked) {
        v.asked = true;
        broadcastLocked("EVENT\tASK\t" + std::to_string(v.id));
    }
}

bool VolumeManager::mountLocked(Volume& v, wchar_t letter, std::string& error) {
    if (v.mount) return true;
    auto volume = Ext4Volume::mount(v.disk, v.part.offset, v.part.size, error);
    if (!volume) {
        v.state = "error";
        v.message = error;
        logLine("mount of \"" + volumeName(v.part) + "\" failed: " + error);
        return false;
    }

    bool explicitLetter = letter != 0;
    if (!explicitLetter && !v.isImage) letter = settings_.preferredLetter(v.part.ext.uuid);
    if (letter && !driveLetterFree(letter)) {
        if (explicitLetter) {
            error = std::string("drive letter ") + static_cast<char>(letter) + ": is in use";
            return false;
        }
        letter = 0;
    }
    std::wstring mp = letter ? std::wstring(1, letter) + L":" : L"";
    std::wstring werr;
    auto m = WinFspMount::start(std::move(volume), mp, werr);
    if (!m) {
        error = wideToUtf8(werr);
        v.state = "error";
        v.message = error;
        logLine("mount of \"" + volumeName(v.part) + "\" failed: " + error);
        return false;
    }
    std::wstring actual = m->mountPoint();
    v.mount = std::move(m);
    v.state = "mounted";
    v.asked = true;
    if (!v.isImage && !actual.empty()) settings_.setPreferredLetter(v.part.ext.uuid, actual[0]);
    logLine("mounted \"" + volumeName(v.part) + "\" at " + wideToUtf8(actual));
    return true;
}

std::vector<VolumeRecord> VolumeManager::list() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VolumeRecord> out;
    for (const auto& [id, v] : volumes_) out.push_back(recordLocked(*v));
    return out;
}

VolumeRecord VolumeManager::recordLocked(const Volume& v) const {
    VolumeRecord r;
    r.id = v.id;
    r.state = v.state;
    r.mountPoint = v.mount ? wideToUtf8(v.mount->mountPoint()) : "";
    r.label = v.part.ext.label;
    r.size = v.part.size;
    r.kind = v.part.ext.kind;
    r.disk = wideToUtf8(v.disk->description());
    r.partition = v.part.number;
    r.uuid = v.part.ext.uuid;
    r.policy = policyName(v.isImage ? Policy::Always : settings_.policy(v.part.ext.uuid));
    r.isImage = v.isImage;
    r.removable = v.disk->isRemovable();
    r.message = v.message;
    return r;
}

bool VolumeManager::mount(int volumeId, wchar_t letter, std::string& mountPoint,
                          std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = volumes_.find(volumeId);
    if (it == volumes_.end()) {
        error = "unknown volume";
        return false;
    }
    Volume& v = *it->second;
    if (v.state == "unsupported") {
        error = v.message;
        return false;
    }
    v.asked = true;
    bool ok = mountLocked(v, letter, error);
    if (ok) mountPoint = wideToUtf8(v.mount->mountPoint());
    broadcastLocked("EVENT\tCHANGED");
    return ok;
}

bool VolumeManager::unmount(int volumeId, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = volumes_.find(volumeId);
    if (it == volumes_.end()) {
        error = "unknown volume";
        return false;
    }
    Volume& v = *it->second;
    if (!v.mount) {
        error = "not mounted";
        return false;
    }
    v.mount.reset();
    v.state = "available";
    v.asked = true;
    logLine("unmounted \"" + volumeName(v.part) + "\"");
    broadcastLocked("EVENT\tCHANGED");
    return true;
}

bool VolumeManager::setPolicy(int volumeId, Policy p, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = volumes_.find(volumeId);
    if (it == volumes_.end()) {
        error = "unknown volume";
        return false;
    }
    Volume& v = *it->second;
    if (v.isImage) {
        error = "image files have no policy";
        return false;
    }
    settings_.setPolicy(v.part.ext.uuid, p);
    logLine("policy for \"" + volumeName(v.part) + "\" set to " + policyName(p));
    if (p == Policy::Always) applyPolicyLocked(v);
    broadcastLocked("EVENT\tCHANGED");
    return true;
}

int VolumeManager::subscribe(EventSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    int token = nextToken_++;
    // Ask the new client about volumes that are still waiting for a decision.
    for (auto& [id, v] : volumes_) {
        if (v->state == "available" && !v->asked && !v->isImage &&
            settings_.policy(v->part.ext.uuid) == Policy::Ask) {
            v->asked = true;
            sink("EVENT\tASK\t" + std::to_string(id));
        }
    }
    subscribers_[token] = std::move(sink);
    return token;
}

void VolumeManager::unsubscribe(int token) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(token);
}

void VolumeManager::broadcastLocked(const std::string& line) {
    for (auto& [token, sink] : subscribers_) sink(line);
}

void VolumeManager::unmountAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, v] : volumes_) v->mount.reset();
    volumes_.clear();
    disks_.clear();
}

}  // namespace ext4win
