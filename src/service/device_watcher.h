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
// removed later. Callbacks run on a worker thread, one at a time.
class DeviceWatcher {
public:
    using Callback = std::function<void(const std::wstring& devicePath, bool arrived)>;

    ~DeviceWatcher();
    bool start(Callback cb);
    void stop();

    // Called from the system notification callback.
    void post(const std::wstring& devicePath, bool arrived);

private:
    void run();

    Callback callback_;
    void* notification_ = nullptr;  // HCMNOTIFICATION
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::wstring, bool>> queue_;
    bool stopping_ = false;
};

}  // namespace ext4win
