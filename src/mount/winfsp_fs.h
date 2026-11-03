// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

namespace ext4easy {

class Ext4Volume;

// A mounted ext4 volume, visible as a drive letter through WinFsp.
// Unmounts when destroyed.
class WinFspMount {
public:
    // mountPoint: "X:" or empty for the next free drive letter.
    static std::unique_ptr<WinFspMount> start(std::unique_ptr<Ext4Volume> volume,
                                              const std::wstring& mountPoint,
                                              std::wstring& error);
    ~WinFspMount();

    WinFspMount(const WinFspMount&) = delete;
    WinFspMount& operator=(const WinFspMount&) = delete;

    std::wstring mountPoint() const;

    struct State;

private:
    explicit WinFspMount(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Mounts and blocks until Ctrl+C. Returns a process exit code.
int runMountUntilCtrlC(std::unique_ptr<Ext4Volume> volume, const std::wstring& mountPoint);

}  // namespace ext4easy
