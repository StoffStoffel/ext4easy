// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>

namespace ext4easy {

enum class Policy { Ask, Always, Never };

const char* policyName(Policy p);
bool parsePolicy(const std::string& s, Policy& out);

// Per-filesystem settings, keyed by ext4 UUID. Stored as text in
// %ProgramData%\ext4easy\volumes.conf ("uuid policy letter ro|rw" per line;
// lines without the last field are read-only).
// Not thread-safe; the VolumeManager serializes access.
class Settings {
public:
    void load();
    Policy policy(const std::string& uuid) const;
    wchar_t preferredLetter(const std::string& uuid) const;  // 0 = none
    bool allowWrite(const std::string& uuid) const;
    void setPolicy(const std::string& uuid, Policy p);
    void setPreferredLetter(const std::string& uuid, wchar_t letter);
    void setAllowWrite(const std::string& uuid, bool allow);

private:
    struct Entry {
        Policy policy = Policy::Ask;
        wchar_t letter = 0;
        bool allowWrite = false;
    };
    void save() const;
    std::map<std::string, Entry> entries_;
};

}  // namespace ext4easy
