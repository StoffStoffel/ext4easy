// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/log.h"

#include "core/text.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <mutex>

namespace ext4win {

namespace {
std::mutex g_logMutex;
bool g_console = false;
constexpr long kMaxLogSize = 1024 * 1024;  // rotate at 1 MiB
}  // namespace

std::wstring dataDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    if (dir.empty()) dir = L"C:\\ProgramData";
    dir += L"\\ext4win";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void logInit(bool console) {
    g_console = console;
    std::wstring path = dataDirectory() + L"\\service.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > kMaxLogSize)
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void logLine(const std::string& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond);
    std::string line = stamp + text + "\n";

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_console) std::fputs(line.c_str(), stderr);
    static std::wstring path = dataDirectory() + L"\\service.log";
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (f) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
}

void logLine(const std::wstring& text) { logLine(wideToUtf8(text)); }

}  // namespace ext4win
