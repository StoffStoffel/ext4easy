// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>

namespace ext4win {

enum class Policy { Ask, Always, Never };

const char* policyName(Policy p);
bool parsePolicy(const std::string& s, Policy& out);

// Per-filesystem settings, keyed by ext4 UUID. Stored as text in
// %ProgramData%\ext4win\volumes.conf ("uuid policy letter" per line).
// Not thread-safe; the VolumeManager serializes access.
class Settings {
public:
    void load();
    Policy policy(const std::string& uuid) const;
    wchar_t preferredLetter(const std::string& uuid) const;  // 0 = none
    void setPolicy(const std::string& uuid, Policy p);
    void setPreferredLetter(const std::string& uuid, wchar_t letter);

private:
    struct Entry {
        Policy policy = Policy::Ask;
        wchar_t letter = 0;
    };
    void save() const;
    std::map<std::string, Entry> entries_;
};

}  // namespace ext4win
