// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/partitions.h"
#include "ipc/pipe.h"
#include "service/settings.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ext4win {

class DiskSource;
class WinFspMount;

// Keeps track of all disks and their ext partitions, mounts them according to
// the stored policy and informs subscribers (tray apps) about changes.
// Thread-safe.
class VolumeManager {
public:
    using EventSink = std::function<void(const std::string& line)>;  // must not block

    VolumeManager();
    ~VolumeManager();

    // Physical disk appeared/disappeared (device interface path).
    void diskArrived(const std::wstring& devicePath);
    void diskRemoved(const std::wstring& devicePath);

    // Image file opened by the caller (as the requesting user). Its volumes
    // are mounted right away. Returns the number of ext volumes found.
    int attachImage(std::shared_ptr<DiskSource> image, std::string& error);
    bool detach(int volumeId, std::string& error);

    std::vector<VolumeRecord> list();
    bool mount(int volumeId, wchar_t letter, std::string& mountPoint, std::string& error);
    bool unmount(int volumeId, std::string& error);
    bool setPolicy(int volumeId, Policy p, std::string& error);

    int subscribe(EventSink sink);
    void unsubscribe(int token);

    void unmountAll();

private:
    struct Volume;
    struct Disk;

    void addDiskLocked(std::shared_ptr<DiskSource> src, const std::wstring& key, bool isImage);
    void removeDiskLocked(const std::wstring& key);
    void applyPolicyLocked(Volume& v);
    bool mountLocked(Volume& v, wchar_t letter, std::string& error);
    void broadcastLocked(const std::string& line);
    VolumeRecord recordLocked(const Volume& v) const;

    std::mutex mutex_;
    Settings settings_;
    std::map<std::wstring, std::unique_ptr<Disk>> disks_;  // key: normalized path
    std::map<int, std::unique_ptr<Volume>> volumes_;
    std::map<int, EventSink> subscribers_;
    int nextVolumeId_ = 1;
    int nextToken_ = 1;
};

}  // namespace ext4win
