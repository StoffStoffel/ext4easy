// SPDX-License-Identifier: GPL-3.0-or-later
// Small fixed write scenarios, each run on a fresh image and then checked
// with e2fsck (tests/run_scenarios.ps1). Used to pin down lwext4 bugs.
//
//   ext4scenario <image> <name>

#include "core/disk_source.h"
#include "core/ext4_volume.h"
#include "core/text.h"

#include <windows.h>

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace ext4easy;

namespace {

std::unique_ptr<Ext4Volume> g_vol;

bool ok(int e, const char* what) {
    if (e == 0) return true;
    std::fprintf(stderr, "%s: %s\n", what, ext4ErrorText(e).c_str());
    return false;
}

bool writeAt(FileHandle* f, uint64_t off, size_t len, char c = 'x') {
    std::string d(len, c);
    size_t w = 0;
    return ok(g_vol->writeFile(f, off, d.data(), d.size(), w), "write");
}

// Reads the whole file and compares it with the expected content.
bool expect(FileHandle* f, const std::string& want, const char* when) {
    uint64_t size = g_vol->fileSize(f);
    std::string got(static_cast<size_t>(size), '\0');
    size_t n = 0;
    if (size) g_vol->readFile(f, 0, got.data(), got.size(), n);
    if (size == want.size() && got == want) return true;
    size_t i = 0;
    while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
    std::fprintf(stderr, "%s: content differs at byte %zu (size %llu, expected %zu)\n", when, i,
                 static_cast<unsigned long long>(size), want.size());
    return false;
}

FileHandle* create(const char* p) {
    int e = 0;
    FileHandle* f = g_vol->createFile(p, e);
    ok(e, "create");
    return f;
}

const std::map<std::string, std::function<bool()>> kScenarios = {
    {"truncate-down", [] {  // 60 KiB file, cut to 16 KiB
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 61440) && ok(g_vol->setFileSize(f, 16384), "truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"truncate-unaligned", [] {
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 61440) && ok(g_vol->setFileSize(f, 22737), "truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"extend-then-truncate", [] {  // sparse grow, write inside, cut
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 1000) && ok(g_vol->setFileSize(f, 61440), "extend") &&
                  writeAt(f, 30000, 100) && ok(g_vol->setFileSize(f, 16384), "truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"gap-write", [] {  // write far behind the end
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 100) && writeAt(f, 50000, 11440);
         g_vol->closeFile(f);
         return r;
     }},
    {"gap-write-truncate", [] {
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 100) && writeAt(f, 50000, 11440) &&
                  ok(g_vol->setFileSize(f, 16384), "truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"extend-only", [] {
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 5000) && ok(g_vol->setFileSize(f, 61440), "extend");
         g_vol->closeFile(f);
         return r;
     }},
    {"overwrite-shorter", [] {  // write, truncate to 0, write less
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 61440) && ok(g_vol->setFileSize(f, 0), "truncate0") &&
                  writeAt(f, 0, 16384);
         g_vol->closeFile(f);
         return r;
     }},
    {"leading-hole-truncate0", [] {  // file starts with a hole, cut to 0
         FileHandle* f = create("/a");
         bool r = f && ok(g_vol->setFileSize(f, 61440), "extend") && writeAt(f, 50000, 100) &&
                  ok(g_vol->setFileSize(f, 0), "truncate0");
         g_vol->closeFile(f);
         return r;
     }},
    {"sparse-truncate-inside", [] {  // cut inside a later extent
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 4096) && writeAt(f, 40960, 20480) &&
                  ok(g_vol->setFileSize(f, 50000), "truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"fill-holes-front", [] {  // all-hole file; write block 1, then block 0, then cut
         FileHandle* f = create("/a");
         std::string m(11615, '\0');
         bool r = f && ok(g_vol->setFileSize(f, 11615), "extend") && expect(f, m, "after extend");
         r = r && writeAt(f, 4096, 2965, 'B');
         m.replace(4096, 2965, 2965, 'B');
         r = r && expect(f, m, "after write block 1");
         r = r && writeAt(f, 0, 1383, 'A');
         m.replace(0, 1383, 1383, 'A');
         r = r && expect(f, m, "after write block 0");
         r = r && ok(g_vol->setFileSize(f, 4694), "truncate");
         m.resize(4694);
         r = r && expect(f, m, "after truncate");
         g_vol->closeFile(f);
         return r;
     }},
    {"seed2-history", [] {  // exact sequence from ext4stress seed 2
         FileHandle* f = create("/a");
         std::string m;
         auto w = [&](uint64_t off, size_t len, char c) {
             if (!writeAt(f, off, len, c)) return false;
             if (off + len > m.size()) m.resize(static_cast<size_t>(off + len), '\0');
             m.replace(static_cast<size_t>(off), len, len, c);
             char what[64];
             std::snprintf(what, sizeof what, "after write %llu+%zu",
                           static_cast<unsigned long long>(off), len);
             return expect(f, m, what);
         };
         auto t = [&](uint64_t size) {
             if (!ok(g_vol->setFileSize(f, size), "truncate")) return false;
             m.resize(static_cast<size_t>(size), '\0');
             char what[64];
             std::snprintf(what, sizeof what, "after truncate %llu",
                           static_cast<unsigned long long>(size));
             return expect(f, m, what);
         };
         bool r = f && w(61604, 284831, 'a') && w(370078, 2325, 'b') && w(159744, 3240, 'c') &&
                  w(372403, 8192, 'd') && t(11615) && w(4096, 2965, 'e') && w(0, 1383, 'f') &&
                  t(4694);
         g_vol->closeFile(f);
         return r;
     }},
    {"append-after-truncate", [] {
         FileHandle* f = create("/a");
         bool r = f && writeAt(f, 0, 61440) && ok(g_vol->setFileSize(f, 10000), "truncate") &&
                  writeAt(f, 10000, 6384);
         g_vol->closeFile(f);
         return r;
     }},
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ext4scenario <image> <name>|--list\n");
        return 2;
    }
    std::string name = wideToUtf8(argv[2]);
    if (name == "--list") {
        for (const auto& [n, f] : kScenarios) std::printf("%s\n", n.c_str());
        return 0;
    }
    auto it = kScenarios.find(name);
    if (it == kScenarios.end()) return std::fprintf(stderr, "unknown scenario\n"), 2;
    std::wstring werr;
    auto disk = DiskSource::open(argv[1], werr, true);
    if (!disk) return std::fprintf(stderr, "%s\n", wideToUtf8(werr).c_str()), 1;
    std::string err;
    g_vol = Ext4Volume::mount(disk, 0, disk->size(), err, true);
    if (!g_vol) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    bool r = it->second();
    g_vol.reset();
    return r ? 0 : 1;
}
