// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/settings.h"

#include "service/log.h"

#include <cstdio>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ext4easy {

namespace {
std::wstring settingsPath() { return dataDirectory() + L"\\volumes.conf"; }
}  // namespace

const char* policyName(Policy p) {
    switch (p) {
        case Policy::Always: return "always";
        case Policy::Never: return "never";
        default: return "ask";
    }
}

bool parsePolicy(const std::string& s, Policy& out) {
    if (s == "ask") out = Policy::Ask;
    else if (s == "always") out = Policy::Always;
    else if (s == "never") out = Policy::Never;
    else return false;
    return true;
}

void Settings::load() {
    entries_.clear();
    std::ifstream in(settingsPath());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string uuid, policy, letter, access;
        ls >> uuid >> policy >> letter >> access;
        Entry e;
        if (uuid.empty() || !parsePolicy(policy, e.policy)) continue;
        if (letter.size() == 1 && std::isalpha(static_cast<unsigned char>(letter[0])))
            e.letter = static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(letter[0])));
        e.allowWrite = access == "rw";
        entries_[uuid] = e;
    }
}

void Settings::save() const {
    std::ofstream out(settingsPath(), std::ios::trunc);
    out << "# ext4easy volume settings: uuid policy(ask|always|never) drive-letter(- = none) "
           "access(ro|rw)\n";
    for (const auto& [uuid, e] : entries_)
        out << uuid << ' ' << policyName(e.policy) << ' '
            << (e.letter ? static_cast<char>(e.letter) : '-') << ' '
            << (e.allowWrite ? "rw" : "ro") << '\n';
    if (!out) logLine("warning: cannot write volumes.conf");
}

Policy Settings::policy(const std::string& uuid) const {
    auto it = entries_.find(uuid);
    return it == entries_.end() ? Policy::Ask : it->second.policy;
}

wchar_t Settings::preferredLetter(const std::string& uuid) const {
    auto it = entries_.find(uuid);
    return it == entries_.end() ? 0 : it->second.letter;
}

bool Settings::allowWrite(const std::string& uuid) const {
    auto it = entries_.find(uuid);
    return it != entries_.end() && it->second.allowWrite;
}

void Settings::setAllowWrite(const std::string& uuid, bool allow) {
    entries_[uuid].allowWrite = allow;
    save();
}

void Settings::setPolicy(const std::string& uuid, Policy p) {
    entries_[uuid].policy = p;
    save();
}

void Settings::setPreferredLetter(const std::string& uuid, wchar_t letter) {
    auto& e = entries_[uuid];
    if (e.letter == letter) return;
    e.letter = letter;
    save();
}

}  // namespace ext4easy
