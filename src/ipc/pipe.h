// SPDX-License-Identifier: GPL-3.0-or-later
//
// Communication between the service and its clients (tray app, "ext4easy ctl")
// over a local named pipe. Plain UTF-8 lines, fields separated by TAB.
//
// Requests and replies:
//   LIST                      -> VOL<TAB>fields...  (one per volume), then END
//   MOUNT id [letter]         -> OK mountpoint | ERR message
//   UNMOUNT id                -> OK | ERR message
//   POLICY id ask|always|never-> OK | ERR message   (remembered per filesystem UUID)
//   WRITE id on|off           -> OK | ERR message   (allow write access; remembered per
//                                UUID, a mounted volume is mounted again)
//   ATTACH imagepath          -> OK count | ERR message   (opened as the calling user)
//   DETACH id                 -> OK | ERR message   (image files only)
//   SUBSCRIBE                 -> OK, then events until the connection closes:
//       EVENT CHANGED         volume list changed (re-run LIST)
//       EVENT ASK id          ask the user whether to mount volume id
//       EVENT MOUNTED id mp   volume was mounted automatically
//
// VOL fields, in order: see VolumeRecord.
#pragma once

#include <string>
#include <vector>

namespace ext4easy {

inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\ext4easy";

std::vector<std::string> splitFields(const std::string& line);
std::string joinFields(const std::vector<std::string>& fields);  // TABs/newlines in fields become spaces

// One volume as reported by LIST.
struct VolumeRecord {
    int id = 0;
    std::string state;       // mounted | available | unsupported | error
    std::string mountPoint;  // "X:" when mounted
    std::string label;
    unsigned long long size = 0;
    std::string kind;        // ext2 | ext3 | ext4
    std::string disk;        // "SanDisk Ultra" or image file name
    int partition = 0;
    std::string uuid;
    std::string policy;      // ask | always | never
    bool isImage = false;
    bool removable = false;
    std::string message;     // error / warning text
    bool writable = false;   // mounted with write access
    bool allowWrite = false; // write access is wanted (setting)
    std::string writeBlocker;  // why the filesystem can only be read ("" = writable)

    std::vector<std::string> toFields() const;  // including the leading "VOL"
    static bool fromFields(const std::vector<std::string>& f, VolumeRecord& out);
};

// Blocking line-based I/O on a pipe handle.
class PipeStream {
public:
    explicit PipeStream(void* handle = nullptr) : handle_(handle) {}
    ~PipeStream();
    PipeStream(const PipeStream&) = delete;
    PipeStream& operator=(const PipeStream&) = delete;

    // Connects to the service pipe (waits up to timeoutMs if all instances are busy).
    bool connect(unsigned timeoutMs = 2000);
    bool readLine(std::string& line);
    bool writeLine(const std::string& line);
    void* handle() const { return handle_; }
    void close();

private:
    void* handle_;
    std::string buffer_;
};

// Sends one request and collects the reply lines (until OK/ERR, or END for LIST).
bool pipeRequest(const std::vector<std::string>& request, std::vector<std::string>& replyLines,
                 std::string& error);

}  // namespace ext4easy
