// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/disk_source.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace ext4win {

namespace {
constexpr size_t kBounceSize = 1024 * 1024;  // max bytes per underlying read

bool startsWith(const std::wstring& s, const wchar_t* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// \\.\... or \\?\... but not \\?\C:\... (a long file path)
bool looksLikeDevice(const std::wstring& path) {
    if (startsWith(path, L"\\\\.\\")) return true;
    return startsWith(path, L"\\\\?\\") && !(path.size() > 5 && path[5] == L':');
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

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        error = L"Cannot open " + path + L": " + win32ErrorText(e);
        if (e == ERROR_ACCESS_DENIED && looksLikeDevice(path))
            error += L" - raw disk access requires administrator rights";
        return nullptr;
    }
    d->handle_ = h;

    DWORD ret = 0;
    GET_LENGTH_INFORMATION len{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &ret,
                        nullptr)) {
        d->physical_ = true;
        d->size_ = static_cast<uint64_t>(len.Length.QuadPart);

        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo,
                            &ret, nullptr) &&
            geo.Geometry.BytesPerSector >= 512) {
            d->sectorSize_ = geo.Geometry.BytesPerSector;
            d->removable_ = geo.Geometry.MediaType == RemovableMedia;
        }
        STORAGE_DEVICE_NUMBER num{};
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &num, sizeof num,
                            &ret, nullptr))
            d->deviceNumber_ = static_cast<int>(num.DeviceNumber);
        d->queryDescription();
    } else {
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz)) {
            error = L"Cannot query size of " + path + L": " + win32ErrorText(GetLastError());
            return nullptr;
        }
        d->size_ = static_cast<uint64_t>(sz.QuadPart);
        size_t slash = path.find_last_of(L"\\/");
        d->description_ = slash == std::wstring::npos ? path : path.substr(slash + 1);
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

void DiskSource::queryDescription() {
    // Vendor and product name, e.g. "SanDisk Ultra".
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(1024);
    DWORD ret = 0;
    if (!DeviceIoControl(static_cast<HANDLE>(handle_), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr) ||
        ret < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return;
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    auto field = [&](DWORD off) -> std::string {
        if (off == 0 || off >= ret) return {};
        const char* p = reinterpret_cast<const char*>(buf.data() + off);
        std::string v(p, strnlen(p, ret - off));
        while (!v.empty() && v.back() == ' ') v.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
    };
    std::string name = field(desc->VendorIdOffset);
    std::string product = field(desc->ProductIdOffset);
    if (!product.empty()) name += (name.empty() ? "" : " ") + product;
    description_.assign(name.begin(), name.end());  // plain ASCII
    if (desc->RemovableMedia || desc->BusType == BusTypeUsb || desc->BusType == BusTypeSd ||
        desc->BusType == BusTypeMmc)
        removable_ = true;
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
