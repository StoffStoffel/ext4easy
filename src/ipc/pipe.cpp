// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/pipe.h"

#include <windows.h>

#include <cstdlib>

namespace ext4easy {

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    for (;;) {
        size_t j = line.find('\t', i);
        if (j == std::string::npos) {
            out.push_back(line.substr(i));
            break;
        }
        out.push_back(line.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::string joinFields(const std::vector<std::string>& fields) {
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out.push_back('\t');
        for (char c : fields[i]) out.push_back(c == '\t' || c == '\n' || c == '\r' ? ' ' : c);
    }
    return out;
}

std::vector<std::string> VolumeRecord::toFields() const {
    return {"VOL",
            std::to_string(id),
            state,
            mountPoint,
            label,
            std::to_string(size),
            kind,
            disk,
            std::to_string(partition),
            uuid,
            policy,
            isImage ? "1" : "0",
            removable ? "1" : "0",
            message,
            writable ? "1" : "0",
            allowWrite ? "1" : "0",
            writeBlocker};
}

bool VolumeRecord::fromFields(const std::vector<std::string>& f, VolumeRecord& v) {
    if (f.size() < 14 || f[0] != "VOL") return false;
    v.id = std::atoi(f[1].c_str());
    v.state = f[2];
    v.mountPoint = f[3];
    v.label = f[4];
    v.size = std::strtoull(f[5].c_str(), nullptr, 10);
    v.kind = f[6];
    v.disk = f[7];
    v.partition = std::atoi(f[8].c_str());
    v.uuid = f[9];
    v.policy = f[10];
    v.isImage = f[11] == "1";
    v.removable = f[12] == "1";
    v.message = f[13];
    // added with write support; older services do not send them
    v.writable = f.size() > 14 && f[14] == "1";
    v.allowWrite = f.size() > 15 && f[15] == "1";
    v.writeBlocker = f.size() > 16 ? f[16] : "";
    return true;
}

PipeStream::~PipeStream() { close(); }

void PipeStream::close() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    buffer_.clear();
}

bool PipeStream::connect(unsigned timeoutMs) {
    close();
    for (;;) {
        // FILE_WRITE_DATA instead of GENERIC_WRITE: normal users are not
        // granted FILE_CREATE_PIPE_INSTANCE, which GENERIC_WRITE includes.
        HANDLE h = CreateFileW(kPipeName, GENERIC_READ | FILE_WRITE_DATA, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            handle_ = h;
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipeName, timeoutMs))
            return false;
    }
}

bool PipeStream::readLine(std::string& line) {
    for (;;) {
        size_t nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line = buffer_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer_.erase(0, nl + 1);
            return true;
        }
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), chunk, sizeof chunk, &got, nullptr) || got == 0)
            return false;
        buffer_.append(chunk, got);
        if (buffer_.size() > 1024 * 1024) return false;  // runaway peer
    }
}

bool PipeStream::writeLine(const std::string& line) {
    std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(static_cast<HANDLE>(handle_), data.data(), static_cast<DWORD>(data.size()),
                     &written, nullptr) &&
           written == data.size();
}

bool pipeRequest(const std::vector<std::string>& request, std::vector<std::string>& replyLines,
                 std::string& error) {
    replyLines.clear();
    PipeStream pipe;
    if (!pipe.connect()) {
        error = "service not reachable";
        return false;
    }
    if (!pipe.writeLine(joinFields(request))) {
        error = "cannot send request";
        return false;
    }
    std::string line;
    while (pipe.readLine(line)) {
        replyLines.push_back(line);
        if (line == "END" || line == "OK" || line.rfind("OK\t", 0) == 0 || line.rfind("ERR", 0) == 0)
            return true;
    }
    error = "connection closed";
    return false;
}

}  // namespace ext4easy
