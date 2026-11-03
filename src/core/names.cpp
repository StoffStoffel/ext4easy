// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/names.h"

#include <cstdint>

namespace ext4easy {

namespace {

constexpr char32_t kSpecialBase = 0xF000;  // forbidden ASCII chars
constexpr char32_t kRawByteBase = 0xF100;  // invalid UTF-8 bytes (0x80..0xFF)

bool forbiddenOnWindows(unsigned char c) {
    if (c < 0x20) return true;
    switch (c) {
        case '"': case '*': case ':': case '<': case '>': case '?': case '\\': case '|':
            return true;
        default:
            return false;
    }
}

// Decodes one UTF-8 sequence. Returns its length, or 0 if invalid.
size_t decodeUtf8(std::string_view s, size_t i, char32_t& cp) {
    auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = b(i);
    size_t len;
    char32_t min;
    if (c < 0x80) { cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

void appendUtf16(std::wstring& out, char32_t cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
        out.push_back(static_cast<wchar_t>(cp));
    }
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::wstring linuxNameToWindows(std::string_view name) {
    std::wstring out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size();) {
        char32_t cp;
        size_t len = decodeUtf8(name, i, cp);
        if (len == 0) {
            out.push_back(static_cast<wchar_t>(kRawByteBase + static_cast<unsigned char>(name[i])));
            ++i;
            continue;
        }
        bool last = (i + len == name.size());
        if (cp < 0x80 && (forbiddenOnWindows(static_cast<unsigned char>(cp)) ||
                          (last && (cp == '.' || cp == ' ') && name != "." && name != "..")))
            out.push_back(static_cast<wchar_t>(kSpecialBase + cp));
        else
            appendUtf16(out, cp);
        i += len;
    }
    return out;
}

std::string windowsNameToLinux(std::wstring_view name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        char32_t cp = static_cast<uint16_t>(name[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < name.size()) {
            char32_t lo = static_cast<uint16_t>(name[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp > kSpecialBase && cp < kSpecialBase + 0x80)
            out.push_back(static_cast<char>(cp - kSpecialBase));
        else if (cp >= kRawByteBase + 0x80 && cp < kRawByteBase + 0x100)
            out.push_back(static_cast<char>(cp - kRawByteBase));
        else
            appendUtf8(out, cp);
    }
    return out;
}

std::string windowsPathToLinux(std::wstring_view path) {
    std::string out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == L'\\' || path[i] == L'/')) ++i;
        size_t j = i;
        while (j < path.size() && path[j] != L'\\' && path[j] != L'/') ++j;
        if (j > i) {
            out.push_back('/');
            out += windowsNameToLinux(path.substr(i, j - i));
        }
        i = j;
    }
    return out.empty() ? "/" : out;
}

}  // namespace ext4easy
