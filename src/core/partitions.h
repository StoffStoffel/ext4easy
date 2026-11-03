// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ext4easy {

class DiskSource;

// Result of looking for an ext2/3/4 superblock at some offset.
struct ExtProbe {
    bool found = false;
    std::string kind;   // "ext2", "ext3" or "ext4"
    std::string label;  // UTF-8, may be empty
    std::string uuid;   // xxxxxxxx-xxxx-...
    uint32_t compat = 0, incompat = 0, roCompat = 0;
    uint64_t sizeBytes = 0;  // filesystem size according to the superblock

    bool needsRecovery() const { return (incompat & 0x0004) != 0; }  // journal not replayed
    // Feature names that make the filesystem unmountable for us (empty = OK).
    std::vector<std::string> unsupportedFeatures() const;
};

struct Partition {
    int number = 0;        // 1-based number in the partition table; 0 = whole device
    std::string scheme;    // "GPT", "MBR" or "none"
    uint64_t offset = 0;   // bytes
    uint64_t size = 0;     // bytes
    std::string typeName;  // e.g. "Linux filesystem", "0x83"
    ExtProbe ext;
};

ExtProbe probeExt(DiskSource& disk, uint64_t offset);

// Lists all partitions (plus a whole-device filesystem if there is no table)
// and probes each of them for ext2/3/4.
std::vector<Partition> scanPartitions(DiskSource& disk);

}  // namespace ext4easy
