// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace ext4win {

// Read-only access to an image file or a raw disk (\\.\PhysicalDriveN).
// read() accepts any offset/length; sector alignment is handled internally.
// Thread-safe.
class DiskSource {
public:
    static std::shared_ptr<DiskSource> open(const std::wstring& path, std::wstring& error);
    ~DiskSource();

    DiskSource(const DiskSource&) = delete;
    DiskSource& operator=(const DiskSource&) = delete;

    bool read(uint64_t offset, void* buffer, size_t length);

    uint64_t size() const { return size_; }
    uint32_t sectorSize() const { return sectorSize_; }
    const std::wstring& path() const { return path_; }
    bool isPhysicalDisk() const { return physical_; }

private:
    DiskSource() = default;

    void* handle_ = nullptr;  // HANDLE
    uint64_t size_ = 0;
    uint32_t sectorSize_ = 512;
    bool physical_ = false;
    std::wstring path_;

    std::mutex mutex_;
    uint8_t* bounce_ = nullptr;  // sector-aligned buffer for unaligned requests
    size_t bounceSize_ = 0;
};

// Win32 error code as readable text.
std::wstring win32ErrorText(unsigned long code);

}  // namespace ext4win
