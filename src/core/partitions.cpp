// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/partitions.h"

#include "core/disk_source.h"

#include <cstdio>
#include <cstring>

namespace ext4win {

namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

std::string formatUuid(const uint8_t* u) {
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
                  u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13],
                  u[14], u[15]);
    return buf;
}

// GPT GUIDs are stored mixed-endian; this prints them in the usual form.
std::string formatGuid(const uint8_t* g) {
    char buf[40];
    std::snprintf(buf, sizeof buf,
                  "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", le32(g), le16(g + 4),
                  le16(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

std::string gptTypeName(const std::string& guid) {
    struct Known { const char* guid; const char* name; };
    static const Known known[] = {
        {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem"},
        {"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)"},
        {"933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux home"},
        {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap"},
        {"E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM"},
        {"A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID"},
        {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system"},
        {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data"},
        {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved"},
        {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery"},
        {"21686148-6449-6E6F-744E-656564454649", "BIOS boot"},
    };
    for (const auto& k : known)
        if (guid == k.guid) return k.name;
    return guid;
}

std::string mbrTypeName(uint8_t type) {
    switch (type) {
        case 0x83: return "Linux";
        case 0x82: return "Linux swap";
        case 0x8E: return "Linux LVM";
        case 0xFD: return "Linux RAID";
        case 0x07: return "NTFS/exFAT";
        case 0x0B:
        case 0x0C: return "FAT32";
        case 0xEF: return "EFI system";
        default: {
            char buf[8];
            std::snprintf(buf, sizeof buf, "0x%02X", type);
            return buf;
        }
    }
}

bool isExtended(uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }

void addPartition(DiskSource& disk, std::vector<Partition>& out, int number, const char* scheme,
                  uint64_t offset, uint64_t size, const std::string& typeName) {
    if (size == 0 || offset >= disk.size()) return;
    if (size > disk.size() - offset) size = disk.size() - offset;
    Partition p;
    p.number = number;
    p.scheme = scheme;
    p.offset = offset;
    p.size = size;
    p.typeName = typeName;
    p.ext = probeExt(disk, offset);
    out.push_back(std::move(p));
}

bool scanGpt(DiskSource& disk, std::vector<Partition>& out) {
    const uint32_t ss = disk.sectorSize();
    std::vector<uint8_t> hdr(ss);
    if (!disk.read(ss, hdr.data(), ss)) return false;
    if (std::memcmp(hdr.data(), "EFI PART", 8) != 0) return false;

    uint64_t entryLba = le64(&hdr[72]);
    uint32_t count = le32(&hdr[80]);
    uint32_t entrySize = le32(&hdr[84]);
    if (entrySize < 128 || entrySize > 4096 || count == 0 || count > 1024) return false;

    std::vector<uint8_t> entries(static_cast<size_t>(count) * entrySize);
    if (!disk.read(entryLba * ss, entries.data(), entries.size())) return false;

    static const uint8_t zero[16] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &entries[static_cast<size_t>(i) * entrySize];
        if (std::memcmp(e, zero, 16) == 0) continue;  // unused entry
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (last < first) continue;
        addPartition(disk, out, static_cast<int>(i + 1), "GPT", first * ss, (last - first + 1) * ss,
                     gptTypeName(formatGuid(e)));
    }
    return true;
}

bool scanMbr(DiskSource& disk, std::vector<Partition>& out) {
    const uint32_t ss = disk.sectorSize();
    uint8_t mbr[512];
    if (!disk.read(0, mbr, sizeof mbr)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;

    // Sanity check: status bytes must be 0x00 or 0x80, else this is likely
    // a boot sector of a filesystem, not a partition table.
    for (int i = 0; i < 4; ++i) {
        uint8_t status = mbr[446 + i * 16];
        if (status != 0x00 && status != 0x80) return false;
    }

    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = &mbr[446 + i * 16];
        uint8_t type = e[4];
        uint64_t start = le32(e + 8), count = le32(e + 12);
        if (type == 0 || count == 0) continue;
        if (type == 0xEE) return scanGpt(disk, out);  // protective MBR

        if (isExtended(type)) {
            // Walk the chain of extended boot records; logical partitions get 5, 6, ...
            uint64_t extBase = start, ebr = start;
            int number = 5;
            for (int guard = 0; guard < 128; ++guard) {
                uint8_t rec[512];
                if (!disk.read(ebr * ss, rec, sizeof rec) || rec[510] != 0x55 || rec[511] != 0xAA)
                    break;
                const uint8_t* l = &rec[446];
                const uint8_t* next = &rec[446 + 16];
                if (l[4] != 0 && le32(l + 12) != 0)
                    addPartition(disk, out, number++, "MBR", (ebr + le32(l + 8)) * ss,
                                 static_cast<uint64_t>(le32(l + 12)) * ss, mbrTypeName(l[4]));
                if (!isExtended(next[4]) || le32(next + 8) == 0) break;
                ebr = extBase + le32(next + 8);
            }
            continue;
        }
        addPartition(disk, out, i + 1, "MBR", start * ss, count * ss, mbrTypeName(type));
    }
    return true;
}

}  // namespace

std::vector<std::string> ExtProbe::unsupportedFeatures() const {
    struct Feature { uint32_t bit; const char* name; };
    // Incompatible features lwext4 cannot handle (see EXT4_SUPPORTED_FINCOM).
    static const Feature incompatNames[] = {
        {0x0001, "compression"}, {0x0008, "journal_dev"}, {0x0400, "ea_inode"},
        {0x1000, "dirdata"},     {0x4000, "large_dir"},   {0x8000, "inline_data"},
        {0x10000, "encrypt"},    {0x20000, "casefold"},
    };
    const uint32_t supported = 0x0002 | 0x0004 | 0x0010 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x2000;
    std::vector<std::string> out;
    uint32_t rest = incompat & ~supported;
    for (const auto& f : incompatNames) {
        if (rest & f.bit) {
            out.push_back(f.name);
            rest &= ~f.bit;
        }
    }
    if (rest) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "unknown(0x%X)", rest);
        out.push_back(buf);
    }
    return out;
}

ExtProbe probeExt(DiskSource& disk, uint64_t offset) {
    ExtProbe r;
    uint8_t sb[1024];
    if (offset + 2048 > disk.size() || !disk.read(offset + 1024, sb, sizeof sb)) return r;
    if (le16(&sb[0x38]) != 0xEF53) return r;

    r.found = true;
    r.compat = le32(&sb[0x5C]);
    r.incompat = le32(&sb[0x60]);
    r.roCompat = le32(&sb[0x64]);
    r.uuid = formatUuid(&sb[0x68]);
    const char* label = reinterpret_cast<const char*>(&sb[0x78]);
    r.label.assign(label, strnlen(label, 16));

    uint64_t blocks = le32(&sb[0x04]);
    if (r.incompat & 0x0080) blocks |= static_cast<uint64_t>(le32(&sb[0x150])) << 32;  // 64bit
    uint32_t logBlockSize = le32(&sb[0x18]);
    if (logBlockSize <= 6) r.sizeBytes = blocks * (1024ull << logBlockSize);

    if (r.incompat & (0x0040 | 0x0080 | 0x0200))  // extents, 64bit, flex_bg
        r.kind = "ext4";
    else if (r.compat & 0x0004)  // has_journal
        r.kind = "ext3";
    else
        r.kind = "ext2";
    return r;
}

std::vector<Partition> scanPartitions(DiskSource& disk) {
    std::vector<Partition> out;
    bool hasTable = scanMbr(disk, out);
    if (!hasTable) hasTable = scanGpt(disk, out);  // GPT without protective MBR
    if (!hasTable || out.empty()) {
        Partition whole;
        whole.number = 0;
        whole.scheme = "none";
        whole.offset = 0;
        whole.size = disk.size();
        whole.typeName = "whole device";
        whole.ext = probeExt(disk, 0);
        if (whole.ext.found) out.push_back(std::move(whole));
    }
    return out;
}

}  // namespace ext4win
