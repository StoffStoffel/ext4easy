// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace ext4easy {

class VolumeManager;

// Serves the protocol described in ipc/pipe.h; one thread per client.
class PipeServer {
public:
    explicit PipeServer(VolumeManager& manager) : manager_(manager) {}
    ~PipeServer();

    bool start(std::string& error);
    void stop();

private:
    void acceptLoop();
    void serve(void* pipe);
    void subscribe(class PipeStream& stream);

    VolumeManager& manager_;
    void* firstInstance_ = nullptr;
    std::thread acceptThread_;
    std::atomic<bool> stopping_{false};

    std::mutex mutex_;
    std::condition_variable cv_;  // wakes subscriber threads and stop()
    std::set<void*> clients_;     // open client pipe handles
    int activeThreads_ = 0;
};

}  // namespace ext4easy
