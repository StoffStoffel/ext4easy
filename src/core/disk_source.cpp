// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/disk_source.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace ext4win {

namespace {
constexpr size_t kBounceSize = 1024 * 1024;  // max bytes per underlying read

bool startsWith(const std::wstring& s, const wchar_t* prefix) {
    return s.rfind(prefix, 0) == 0;
}
}  // namespace

std::wstring win32ErrorText(unsigned long code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
        text.pop_back();
    return text + L" (" + std::to_wstring(code) + L")";
}

std::shared_ptr<DiskSource> DiskSource::open(const std::wstring& path, std::wstring& error) {
    std::shared_ptr<DiskSource> d(new DiskSource());
    d->path_ = path;
    d->physical_ = startsWith(path, L"\\\\.\\") || startsWith(path, L"\\\\?\\GLOBALROOT");

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        error = L"Cannot open " + path + L": " + win32ErrorText(e);
        if (e == ERROR_ACCESS_DENIED && d->physical_)
            error += L" - raw disk access requires administrator rights";
        return nullptr;
    }
    d->handle_ = h;

    if (d->physical_) {
        DWORD ret = 0;
        GET_LENGTH_INFORMATION len{};
        if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &ret,
                             nullptr)) {
            error = L"Cannot query size of " + path + L": " + win32ErrorText(GetLastError());
            return nullptr;
        }
        d->size_ = static_cast<uint64_t>(len.Length.QuadPart);

        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo,
                            &ret, nullptr) &&
            geo.Geometry.BytesPerSector >= 512) {
            d->sectorSize_ = geo.Geometry.BytesPerSector;
        }
    } else {
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz)) {
            error = L"Cannot query size of " + path + L": " + win32ErrorText(GetLastError());
            return nullptr;
        }
        d->size_ = static_cast<uint64_t>(sz.QuadPart);
    }

    d->bounceSize_ = kBounceSize;
    d->bounce_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, d->bounceSize_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!d->bounce_) {
        error = L"Out of memory";
        return nullptr;
    }
    return d;
}

DiskSource::~DiskSource() {
    if (bounce_) VirtualFree(bounce_, 0, MEM_RELEASE);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

bool DiskSource::read(uint64_t offset, void* buffer, size_t length) {
    if (length == 0) return true;
    if (offset > size_ || length > size_ - offset) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto* out = static_cast<uint8_t*>(buffer);
    const uint64_t ss = sectorSize_;

    while (length > 0) {
        // Read an aligned window into the bounce buffer, copy the wanted part.
        uint64_t alignedStart = offset / ss * ss;
        size_t skip = static_cast<size_t>(offset - alignedStart);
        size_t want = std::min(length, bounceSize_ - static_cast<size_t>(ss));
        uint64_t alignedLen = (skip + want + ss - 1) / ss * ss;
        alignedLen = std::min<uint64_t>(alignedLen, size_ - alignedStart);  // last partial sector of an image

        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(alignedStart);
        ov.OffsetHigh = static_cast<DWORD>(alignedStart >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), bounce_, static_cast<DWORD>(alignedLen), &got,
                      &ov))
            return false;
        if (got < skip + want) return false;

        std::memcpy(out, bounce_ + skip, want);
        out += want;
        offset += want;
        length -= want;
    }
    return true;
}

}  // namespace ext4win
