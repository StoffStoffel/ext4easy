// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/pipe_server.h"

#include "core/disk_source.h"
#include "core/text.h"
#include "ipc/pipe.h"
#include "service/log.h"
#include "service/volume_manager.h"

#include <windows.h>
#include <sddl.h>

#include <chrono>
#include <cstdlib>
#include <deque>

namespace ext4easy {

namespace {

// SYSTEM, Administrators and the pipe owner (the server itself, also when
// run in console mode by a normal user): full access. Authenticated users: read and
// write data only - not FILE_CREATE_PIPE_INSTANCE, so nobody can pose as
// the server.
const wchar_t* kPipeSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;0x12008B;;;AU)";

HANDLE createInstance(bool first) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1,
                                                              &sa.lpSecurityDescriptor, nullptr))
        return INVALID_HANDLE_VALUE;
    DWORD openMode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE h = CreateNamedPipeW(kPipeName, openMode,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    return h;
}

int toInt(const std::vector<std::string>& f, size_t i) {
    return i < f.size() ? std::atoi(f[i].c_str()) : 0;
}

}  // namespace

PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(std::string& error) {
    HANDLE h = createInstance(true);
    if (h == INVALID_HANDLE_VALUE) {
        error = "cannot create pipe (is the service already running?): " +
                wideToUtf8(win32ErrorText(GetLastError()));
        return false;
    }
    firstInstance_ = h;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void PipeServer::stop() {
    if (!acceptThread_.joinable()) return;
    stopping_ = true;

    // Unblock ConnectNamedPipe with a dummy connection.
    HANDLE dummy = CreateFileW(kPipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    acceptThread_.join();
    if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);

    std::unique_lock<std::mutex> lock(mutex_);
    for (void* c : clients_) CancelIoEx(static_cast<HANDLE>(c), nullptr);
    cv_.notify_all();
    cv_.wait_for(lock, std::chrono::seconds(5), [this] { return activeThreads_ == 0; });
}

void PipeServer::acceptLoop() {
    HANDLE next = static_cast<HANDLE>(firstInstance_);
    while (!stopping_) {
        if (next == INVALID_HANDLE_VALUE) next = createInstance(false);
        if (next == INVALID_HANDLE_VALUE) {
            logLine("cannot create pipe instance: " + wideToUtf8(win32ErrorText(GetLastError())));
            Sleep(1000);
            continue;
        }
        BOOL connected = ConnectNamedPipe(next, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (stopping_) {
            CloseHandle(next);
            break;
        }
        if (!connected) {
            CloseHandle(next);
            next = INVALID_HANDLE_VALUE;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clients_.insert(next);
            ++activeThreads_;
        }
        std::thread([this, next] { serve(next); }).detach();
        next = INVALID_HANDLE_VALUE;
    }
}

void PipeServer::serve(void* pipe) {
    {
        PipeStream stream(pipe);
        std::string line;
        if (stream.readLine(line)) {
            auto f = splitFields(line);
            const std::string& cmd = f[0];
            std::string error, mp;
            auto reply = [&](bool ok, const std::string& okText = "") {
                stream.writeLine(ok ? (okText.empty() ? "OK" : "OK\t" + okText) : "ERR\t" + error);
            };

            if (cmd == "LIST") {
                for (const auto& v : manager_.list()) stream.writeLine(joinFields(v.toFields()));
                stream.writeLine("END");
            } else if (cmd == "MOUNT") {
                wchar_t letter = f.size() > 2 && !f[2].empty() ? static_cast<wchar_t>(toupper(f[2][0])) : 0;
                bool ok = manager_.mount(toInt(f, 1), letter, mp, error);
                reply(ok, mp);
            } else if (cmd == "UNMOUNT") {
                reply(manager_.unmount(toInt(f, 1), error));
            } else if (cmd == "POLICY") {
                Policy p;
                if (f.size() < 3 || !parsePolicy(f[2], p)) {
                    error = "usage: POLICY id ask|always|never";
                    reply(false);
                } else {
                    reply(manager_.setPolicy(toInt(f, 1), p, error));
                }
            } else if (cmd == "ATTACH" && f.size() > 1) {
                // Open the file with the client's rights, not the service's.
                std::wstring werr;
                std::shared_ptr<DiskSource> image;
                if (ImpersonateNamedPipeClient(static_cast<HANDLE>(pipe))) {
                    image = DiskSource::open(utf8ToWide(f[1]), werr);
                    RevertToSelf();
                } else {
                    werr = L"cannot impersonate client";
                }
                if (image && image->isPhysicalDisk()) {
                    image.reset();
                    werr = L"only image files can be attached";
                }
                if (!image) {
                    error = wideToUtf8(werr);
                    reply(false);
                } else {
                    int n = manager_.attachImage(image, error);
                    reply(n > 0, std::to_string(n));
                }
            } else if (cmd == "DETACH") {
                reply(manager_.detach(toInt(f, 1), error));
            } else if (cmd == "SUBSCRIBE") {
                subscribe(stream);
            } else {
                error = "unknown request";
                reply(false);
            }
        }
        FlushFileBuffers(static_cast<HANDLE>(pipe));
        DisconnectNamedPipe(static_cast<HANDLE>(pipe));
        // PipeStream closes the handle here
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(pipe);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    --activeThreads_;
    cv_.notify_all();
}

void PipeServer::subscribe(PipeStream& stream) {
    std::deque<std::string> queue;
    int token = manager_.subscribe([this, &queue](const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue.push_back(line);
        cv_.notify_all();
    });
    bool alive = stream.writeLine("OK");
    while (alive && !stopping_) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(30),
                         [&] { return stopping_ || !queue.empty(); });
            batch.swap(queue);
        }
        if (batch.empty()) batch.push_back("EVENT\tPING");  // detects closed clients
        for (const auto& line : batch) {
            if (!stream.writeLine(line)) {
                alive = false;
                break;
            }
        }
    }
    manager_.unsubscribe(token);
}

}  // namespace ext4easy
