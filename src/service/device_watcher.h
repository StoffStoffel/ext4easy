// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ext4win {

// Reports disks that are present at start and disks that are plugged in or
// removed later, plus volume (partition) changes - those also happen when
// a memory card is swapped in a card reader. Callbacks run on a worker
// thread, one at a time.
class DeviceWatcher {
public:
    enum class Event { DiskArrived, DiskRemoved, VolumeArrived, VolumeRemoved };
    using Callback = std::function<void(const std::wstring& devicePath, Event event)>;

    ~DeviceWatcher();
    bool start(Callback cb);
    void stop();

    // Called from the system notification callback.
    void post(const std::wstring& devicePath, Event event);

private:
    void run();

    Callback callback_;
    void* diskNotification_ = nullptr;    // HCMNOTIFICATION
    void* volumeNotification_ = nullptr;  // HCMNOTIFICATION
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::wstring, Event>> queue_;
    bool stopping_ = false;
};

}  // namespace ext4win
