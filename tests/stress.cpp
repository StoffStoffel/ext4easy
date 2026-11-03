// SPDX-License-Identifier: GPL-3.0-or-later
//
// Randomized write test for Ext4Volume. Every operation is mirrored in an
// in-memory model; the whole tree is compared against the model regularly
// and after remounts. At the end a manifest (paths + SHA-256) is written so
// the image can be checked independently on Linux (tests/verify_image.sh).
//
//   ext4stress <image> [--seed N] [--ops N] [--verify N] [--remount N]
//                      [--manifest FILE] [--quiet] [--fill]

#include "core/disk_source.h"
#include "core/ext4_volume.h"
#include "core/text.h"

#include <windows.h>
#include <bcrypt.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ext4easy;

namespace {

struct Options {
    std::wstring image;
    uint64_t seed = 1;
    long ops = 5000;
    long verifyEvery = 500;
    long remountEvery = 1500;
    std::wstring manifest;
    bool quiet = false;
    bool fill = false;  // fill the volume completely first (ENOSPC handling)
};

class Stress {
public:
    explicit Stress(const Options& o) : opt_(o), rng_(o.seed) {}

    int run() {
        std::wstring werr;
        disk_ = DiskSource::open(opt_.image, werr, true);
        if (!disk_) return fail("open image: " + wideToUtf8(werr));
        if (!mount()) return 1;

        // Start from what is on the volume (normally only lost+found).
        if (!loadExisting("/")) return 1;
        capacity_ = vol_->info().totalBytes;

        auto start = std::chrono::steady_clock::now();
        if (opt_.fill && !fillDisk()) return failed();
        for (op_ = 1; op_ <= opt_.ops; ++op_) {
            if (!step()) return failed();
            if (opt_.verifyEvery && op_ % opt_.verifyEvery == 0 && !verifyAll()) return failed();
            if (opt_.remountEvery && op_ % opt_.remountEvery == 0) {
                vol_.reset();
                if (!mount() || !verifyAll()) return failed();
                ++remounts_;
            }
            if (!opt_.quiet && op_ % 1000 == 0)
                std::printf("  %ld ops, %zu files, %zu dirs, %s written\n", op_, files_.size(),
                            dirs_.size(), formatSize(bytesWritten_).c_str());
        }
        if (!verifyAll()) return failed();
        vol_.reset();  // clean unmount
        if (!mount() || !verifyAll()) return failed();
        vol_.reset();

        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("OK seed=%llu ops=%ld files=%zu dirs=%zu written=%s remounts=%d "
                    "time=%.1fs\n",
                    static_cast<unsigned long long>(opt_.seed), opt_.ops, files_.size(),
                    dirs_.size(), formatSize(bytesWritten_).c_str(), remounts_, secs);
        if (!opt_.manifest.empty() && !writeManifest()) return 1;
        return 0;
    }

private:
    // ---- helpers ----------------------------------------------------------------
    int fail(const std::string& msg) {
        std::fprintf(stderr, "FAIL seed=%llu op=%ld: %s\n",
                     static_cast<unsigned long long>(opt_.seed), op_, msg.c_str());
        std::fprintf(stderr, "recent operations:\n");
        for (const auto& l : trace_) std::fprintf(stderr, "  %s\n", l.c_str());
        return 1;
    }
    void note(const std::string& s) {
        trace_.push_back("#" + std::to_string(op_) + " " + s);
        if (trace_.size() > 2000) trace_.erase(trace_.begin());
    }
    int failed() { return 1; }  // message already printed

    bool check(int got, int want, const std::string& what) {
        if (got == want) return true;
        fail(what + ": got " + ext4ErrorText(got) + ", expected " + ext4ErrorText(want));
        return false;
    }

    bool mount() {
        std::string err;
        vol_ = Ext4Volume::mount(disk_, 0, disk_->size(), err, true);
        if (!vol_) {
            fail("mount: " + err);
            return false;
        }
        return true;
    }

    uint64_t rnd(uint64_t n) { return n ? rng_() % n : 0; }
    bool chance(int percent) { return rnd(100) < static_cast<uint64_t>(percent); }

    static std::string child(const std::string& dir, const std::string& name) {
        return dir == "/" ? "/" + name : dir + "/" + name;
    }
    static std::string parentOf(const std::string& p) {
        size_t s = p.find_last_of('/');
        return s == 0 ? "/" : p.substr(0, s);
    }
    static bool ignored(const std::string& p) { return p == "/lost+found"; }

    std::string randomName() {
        static const char* pieces[] = {"a", "b", "c", "x", "y", "z", "0", "1", "7", "_", "-",
                                       ".", "data", "file", "Dir", "ä", "ö", "ü", "€", "日本",
                                       " ", "~", "#"};
        std::string s;
        if (chance(2)) {
            s.assign(200 + rnd(55), 'L');  // long name (<= 255 bytes)
        } else {
            size_t n = 1 + rnd(8);
            for (size_t i = 0; i < n; ++i) s += pieces[rnd(sizeof pieces / sizeof *pieces)];
        }
        if (s == "." || s == "..") s += "x";
        return s;
    }

    std::string pick(const std::set<std::string>& s) {
        auto it = s.begin();
        std::advance(it, static_cast<long>(rnd(s.size())));
        return *it;
    }
    std::string pickFile() {
        auto it = files_.begin();
        std::advance(it, static_cast<long>(rnd(files_.size())));
        return it->first;
    }
    std::string pickDir() { return pick(dirs_); }

    bool exists(const std::string& p) const { return files_.count(p) || dirs_.count(p); }

    std::string freshPath() {
        for (;;) {
            std::string dir = pickDir();
            if (ignored(dir)) continue;
            int depth = 0;
            for (char c : dir) depth += c == '/';
            if (depth > 6) continue;
            std::string p = child(dir, randomName());
            if (!exists(p)) return p;
        }
    }

    void randomBytes(std::string& buf, size_t n) {
        buf.resize(n);
        size_t i = 0;
        if (chance(30)) {  // simple pattern
            char c = static_cast<char>(rnd(256));
            for (; i < n; ++i) buf[i] = static_cast<char>(c + i % 7);
            return;
        }
        for (; i + 8 <= n; i += 8) {
            uint64_t v = rng_();
            std::memcpy(&buf[i], &v, 8);
        }
        for (; i < n; ++i) buf[i] = static_cast<char>(rng_());
    }

    uint64_t modelBytes() const {
        uint64_t total = 0;
        for (const auto& [p, c] : files_) total += c.size();
        return total;
    }

    // ---- operations -------------------------------------------------------------
    bool step() {
        // stay well below ENOSPC (the --fill phase leaves the volume half full)
        bool full = modelBytes() > capacity_ * (opt_.fill ? 3 : 2) / 5;
        uint64_t r = rnd(100);
        if (files_.empty() || r < 14) return full ? opRemove() : opCreate();
        if (r < 44) return full ? opTruncate() : opWrite();
        if (r < 52) return opTruncate();
        if (r < 58) return opRemove();
        if (r < 63) return opMkdir();
        if (r < 67) return opRmdir();
        if (r < 75) return opRenameFile();
        if (r < 78) return opRenameDir();
        if (r < 88) return opVerifyFile(pickFile());
        if (r < 91) return opSetTimes();
        if (r < 96) return opNegative();
        if (r < 97 && !full) return opBulkCreate();
        return opReopenWrite();
    }

    bool opCreate() {
        std::string p = freshPath();
        int e = 0;
        FileHandle* f = vol_->createFile(p, e);
        if (!check(e, 0, "create " + p)) return false;
        std::string content;
        note("create " + p);
        if (chance(70)) {
            randomBytes(content, static_cast<size_t>(chance(80) ? rnd(20000) : rnd(3 << 20)));
            note("  initial write len=" + std::to_string(content.size()));
            size_t w = 0;
            e = vol_->writeFile(f, 0, content.data(), content.size(), w);
            bytesWritten_ += w;
            if (e != 0 || w != content.size()) {
                vol_->closeFile(f);
                if (e != 0) return check(e, 0, "initial write " + p);
                return fail("short write " + p), false;
            }
        }
        vol_->closeFile(f);
        files_[p] = content;
        return true;
    }

    bool opBulkCreate() {  // many files in one new directory: htree split
        std::string d = freshPath();
        if (!check(vol_->mkdir(d), 0, "mkdir " + d)) return false;
        dirs_.insert(d);
        int n = 300 + static_cast<int>(rnd(1500));
        for (int i = 0; i < n; ++i) {
            std::string name = "bulk_" + std::to_string(i) + "_" + randomName();
            if (name.size() > 255) name.resize(255);  // long names are plain ASCII
            std::string p = child(d, name);
            if (exists(p)) continue;
            int e = 0;
            FileHandle* f = vol_->createFile(p, e);
            if (!check(e, 0, "bulk create " + p)) return false;
            std::string c = "#" + std::to_string(i);
            size_t w = 0;
            e = vol_->writeFile(f, 0, c.data(), c.size(), w);
            vol_->closeFile(f);
            if (!check(e, 0, "bulk write " + p)) return false;
            files_[p] = c;
        }
        return true;
    }

    bool opWrite() {
        std::string p = pickFile();
        std::string& model = files_[p];
        uint64_t size = model.size();
        uint64_t off;
        switch (rnd(5)) {
            case 0: off = 0; break;
            case 1: off = size; break;                       // append
            case 2: off = size + rnd(70000); break;          // gap -> zeros
            case 3: off = rnd(size + 1) / 4096 * 4096; break;  // block aligned
            default: off = rnd(size + 1); break;
        }
        size_t len = static_cast<size_t>(chance(70) ? 1 + rnd(9000) : 1 + rnd(600000));
        if (chance(20)) len = (len / 4096 + 1) * 4096;
        std::string data;
        randomBytes(data, len);

        int e = 0;
        FileHandle* f = vol_->openFile(p, e, true);
        if (!check(e, 0, "open for write " + p)) return false;
        size_t w = 0;
        note("write " + p + " off=" + std::to_string(off) + " len=" + std::to_string(len) +
             " size=" + std::to_string(size));
        e = vol_->writeFile(f, off, data.data(), data.size(), w);
        vol_->closeFile(f);
        bytesWritten_ += w;
        if (!check(e, 0, "write " + p + " @" + std::to_string(off))) return false;
        if (w != len) return fail("short write " + p), false;

        if (off + len > model.size()) model.resize(static_cast<size_t>(off + len), '\0');
        model.replace(static_cast<size_t>(off), len, data);
        return chance(30) ? opVerifyFile(p) : true;
    }

    bool opReopenWrite() {  // several writes through one handle, then verify
        std::string p = pickFile();
        std::string& model = files_[p];
        int e = 0;
        FileHandle* f = vol_->openFile(p, e, true);
        if (!check(e, 0, "open " + p)) return false;
        for (int i = 0; i < 5; ++i) {
            uint64_t off = rnd(model.size() + 5000);
            std::string data;
            randomBytes(data, static_cast<size_t>(1 + rnd(5000)));
            size_t w = 0;
            note("multi write " + p + " off=" + std::to_string(off) + " len=" +
                 std::to_string(data.size()) + " size=" + std::to_string(model.size()));
            e = vol_->writeFile(f, off, data.data(), data.size(), w);
            if (!check(e, 0, "multi write " + p)) {
                vol_->closeFile(f);
                return false;
            }
            if (off + data.size() > model.size()) model.resize(static_cast<size_t>(off + data.size()), '\0');
            model.replace(static_cast<size_t>(off), data.size(), data);
        }
        if (vol_->fileSize(f) != model.size()) {
            vol_->closeFile(f);
            return fail("size after multi write " + p), false;
        }
        vol_->closeFile(f);
        return opVerifyFile(p);
    }

    bool opTruncate() {
        std::string p = pickFile();
        std::string& model = files_[p];
        uint64_t ns;
        switch (rnd(4)) {
            case 0: ns = 0; break;
            case 1: ns = rnd(model.size() + 1); break;
            case 2: ns = model.size() + rnd(100000); break;
            default: ns = rnd(model.size() + 1) / 4096 * 4096; break;
        }
        int e = 0;
        FileHandle* f = vol_->openFile(p, e, true);
        if (!check(e, 0, "open " + p)) return false;
        note("truncate " + p + " " + std::to_string(model.size()) + " -> " + std::to_string(ns));
        e = vol_->setFileSize(f, ns);
        vol_->closeFile(f);
        if (!check(e, 0, "truncate " + p + " to " + std::to_string(ns))) return false;
        model.resize(static_cast<size_t>(ns), '\0');
        return chance(40) ? opVerifyFile(p) : true;
    }

    bool opRemove() {
        if (files_.empty()) return true;
        std::string p = pickFile();
        note("remove " + p);
        if (!check(vol_->removeFile(p), 0, "remove " + p)) return false;
        files_.erase(p);
        return true;
    }

    bool opMkdir() {
        std::string p = freshPath();
        if (!check(vol_->mkdir(p), 0, "mkdir " + p)) return false;
        dirs_.insert(p);
        return true;
    }

    bool isEmptyDir(const std::string& d) const {
        std::string prefix = d + "/";
        for (const auto& [p, c] : files_)
            if (p.rfind(prefix, 0) == 0) return false;
        for (const auto& p : dirs_)
            if (p.rfind(prefix, 0) == 0) return false;
        return true;
    }

    bool opRmdir() {
        std::vector<std::string> empty;
        for (const auto& d : dirs_)
            if (d != "/" && !ignored(d) && isEmptyDir(d)) empty.push_back(d);
        if (empty.empty()) return true;
        std::string d = empty[rnd(empty.size())];
        if (!check(vol_->removeDir(d), 0, "rmdir " + d)) return false;
        dirs_.erase(d);
        return true;
    }

    bool opRenameFile() {
        std::string from = pickFile();
        std::string to;
        bool replace = false;
        if (chance(25) && files_.size() > 1) {
            to = pickFile();  // overwrite an existing file
            replace = true;
            if (to == from) return true;
        } else {
            to = freshPath();
        }
        note("rename " + from + " -> " + to + (replace ? " (replace)" : ""));
        if (!check(vol_->rename(from, to, replace), 0, "rename " + from + " -> " + to))
            return false;
        std::string content = std::move(files_[from]);
        files_.erase(from);
        files_[to] = std::move(content);
        return true;
    }

    bool opRenameDir() {
        std::vector<std::string> candidates;
        for (const auto& d : dirs_)
            if (d != "/" && !ignored(d)) candidates.push_back(d);
        if (candidates.empty()) return true;
        std::string from = candidates[rnd(candidates.size())];
        std::string to = freshPath();
        if (to.rfind(from + "/", 0) == 0) {
            // moving a directory into itself must fail
            return check(vol_->rename(from, to, false), EINVAL, "rename dir into itself");
        }
        note("rename dir " + from + " -> " + to);
        if (!check(vol_->rename(from, to, false), 0, "rename dir " + from + " -> " + to))
            return false;
        // move everything below 'from' in the model
        std::map<std::string, std::string> nf;
        for (auto& [p, c] : files_) {
            if (p.rfind(from + "/", 0) == 0) nf[to + p.substr(from.size())] = std::move(c);
            else nf[p] = std::move(c);
        }
        files_.swap(nf);
        std::set<std::string> nd;
        for (const auto& p : dirs_) {
            if (p == from) nd.insert(to);
            else if (p.rfind(from + "/", 0) == 0) nd.insert(to + p.substr(from.size()));
            else nd.insert(p);
        }
        dirs_.swap(nd);
        return true;
    }

    bool opSetTimes() {
        std::string p = chance(70) || dirs_.size() < 2 ? pickFile() : pickDir();
        if (ignored(p)) return true;
        // random times between 1970 and ~2285 (e2fsck flags dates after 2310
        // as suspicious), 100 ns resolution
        auto rt = [&] { return 116444736000000000ULL + rnd(100000000000000000ULL); };
        uint64_t at = rt(), mt = rt(), ct = rt(), cr = rt();
        if (!check(vol_->setTimes(p, at, mt, ct, cr), 0, "setTimes " + p)) return false;
        FileStat st;
        if (!check(vol_->stat(p, st), 0, "stat " + p)) return false;
        if (st.atime != at || st.mtime != mt || st.ctime != ct || st.crtime != cr)
            return fail("times differ after setTimes " + p), false;
        return true;
    }

    bool opNegative() {
        switch (rnd(5)) {
            case 0: {  // create existing
                std::string p = pickFile();
                int e = 0;
                FileHandle* f = vol_->createFile(p, e);
                if (f) vol_->closeFile(f);
                return check(e, EEXIST, "create existing " + p);
            }
            case 1: {  // mkdir existing
                std::string p = pickDir();
                if (p == "/") return true;
                return check(vol_->mkdir(p), EEXIST, "mkdir existing " + p);
            }
            case 2: {  // rmdir non-empty
                for (const auto& d : dirs_)
                    if (d != "/" && !ignored(d) && !isEmptyDir(d))
                        return check(vol_->removeDir(d), ENOTEMPTY, "rmdir non-empty " + d);
                return true;
            }
            case 3: {  // removeFile on a directory
                std::string d = pickDir();
                if (d == "/") return true;
                return check(vol_->removeFile(d), EISDIR, "removeFile on dir " + d);
            }
            default: {  // rename onto existing without replace
                if (files_.size() < 2) return true;
                std::string a = pickFile(), b = pickFile();
                if (a == b) return true;
                return check(vol_->rename(a, b, false), EEXIST, "rename without replace");
            }
        }
    }

    // Writes 1 MiB chunks until the volume is full. The failing write must
    // report ENOSPC, the data before it must be intact, and after deleting
    // some files the volume must be fully usable again.
    bool fillDisk() {
        std::string dir = "/fill";
        if (!check(vol_->mkdir(dir), 0, "mkdir " + dir)) return false;
        dirs_.insert(dir);
        std::string chunk;
        for (int n = 0;; ++n) {
            std::string p = child(dir, "f" + std::to_string(n));
            int e = 0;
            FileHandle* f = vol_->createFile(p, e);
            if (e == ENOSPC) break;  // no free inode or directory block
            if (!check(e, 0, "fill create " + p)) return false;
            std::string content;
            bool full = false;
            for (int i = 0; i < 8 && !full; ++i) {
                randomBytes(chunk, 1 << 20);
                size_t w = 0;
                e = vol_->writeFile(f, content.size(), chunk.data(), chunk.size(), w);
                content.append(chunk, 0, w);
                if (e == ENOSPC) {
                    full = true;
                } else if (!check(e, 0, "fill write " + p)) {
                    vol_->closeFile(f);
                    return false;
                }
            }
            // the file must contain exactly what was reported as written
            uint64_t size = vol_->fileSize(f);
            vol_->closeFile(f);
            if (size != content.size()) {
                // a failed write may leave the size anywhere up to the chunk end;
                // accept what is there if the known part matches
                int e2 = 0;
                FileHandle* r = vol_->openFile(p, e2);
                if (!check(e2, 0, "reopen " + p)) return false;
                std::string actual(static_cast<size_t>(size), '\0');
                size_t got = 0;
                vol_->readFile(r, 0, actual.data(), actual.size(), got);
                vol_->closeFile(r);
                if (actual.compare(0, std::min(actual.size(), content.size()), content, 0,
                                   std::min(actual.size(), content.size())) != 0)
                    return fail("data before ENOSPC damaged in " + p), false;
                content = actual;
            }
            files_[p] = content;
            if (!opVerifyFile(p)) return false;
            if (full) break;
        }
        uint64_t freeBytes = vol_->info().freeBytes;
        std::printf("  volume full: %zu files in /fill, %s free\n", files_.size(),
                    formatSize(freeBytes).c_str());
        if (!verifyAll()) return false;
        // free about half of it again
        std::vector<std::string> fill;
        for (const auto& [p, c] : files_)
            if (p.rfind(dir + "/", 0) == 0) fill.push_back(p);
        for (size_t i = 0; i < fill.size(); i += 2) {
            if (!check(vol_->removeFile(fill[i]), 0, "remove " + fill[i])) return false;
            files_.erase(fill[i]);
        }
        return verifyAll();
    }

    // ---- verification -------------------------------------------------------------
    bool opVerifyFile(const std::string& p) {
        const std::string& model = files_[p];
        int e = 0;
        FileHandle* f = vol_->openFile(p, e);
        if (!check(e, 0, "open for verify " + p)) return false;
        std::string data(model.size(), '\0');
        size_t got = 0;
        e = model.empty() ? 0 : vol_->readFile(f, 0, data.data(), data.size(), got);
        uint64_t size = vol_->fileSize(f);
        vol_->closeFile(f);
        if (!check(e, 0, "read " + p)) return false;
        if (size != model.size())
            return fail("size of " + p + ": " + std::to_string(size) + " != " +
                        std::to_string(model.size())), false;
        if (got != model.size() || data != model) {
            size_t i = 0;
            while (i < got && data[i] == model[i]) ++i;
            return fail("content of " + p + " differs at byte " + std::to_string(i)), false;
        }
        return true;
    }

    bool verifyDir(const std::string& d, size_t& seenFiles, size_t& seenDirs) {
        std::vector<DirEntry> entries;
        if (!check(vol_->listDir(d, entries), 0, "list " + d)) return false;
        std::set<std::string> names;
        for (const auto& e : entries) {
            std::string p = child(d, e.name);
            if (ignored(p)) continue;
            if (!names.insert(e.name).second) return fail("duplicate entry " + p), false;
            if (e.st.isDir()) {
                if (!dirs_.count(p)) return fail("unexpected dir " + p), false;
                ++seenDirs;
                if (!verifyDir(p, seenFiles, seenDirs)) return false;
            } else {
                auto it = files_.find(p);
                if (it == files_.end()) return fail("unexpected file " + p), false;
                if (e.st.size != it->second.size()) return fail("listed size of " + p), false;
                ++seenFiles;
                if (!opVerifyFile(p)) return false;
            }
        }
        return true;
    }

    bool verifyAll() {
        size_t seenFiles = 0, seenDirs = 1;  // root
        if (!verifyDir("/", seenFiles, seenDirs)) return false;
        size_t modelDirs = 0;
        for (const auto& d : dirs_) modelDirs += ignored(d) ? 0 : 1;
        if (seenFiles != files_.size() || seenDirs != modelDirs)
            return fail("tree has " + std::to_string(seenFiles) + " files/" +
                        std::to_string(seenDirs) + " dirs, model " +
                        std::to_string(files_.size()) + "/" + std::to_string(modelDirs)), false;
        return true;
    }

    bool loadExisting(const std::string& d) {
        dirs_.insert(d);
        std::vector<DirEntry> entries;
        if (!check(vol_->listDir(d, entries), 0, "list " + d)) return false;
        for (const auto& e : entries) {
            std::string p = child(d, e.name);
            if (ignored(p)) {
                dirs_.insert(p);
                continue;
            }
            if (e.st.isDir()) {
                if (!loadExisting(p)) return false;
            } else {
                return fail("test image must be empty (found " + p + ")"), false;
            }
        }
        return true;
    }

    static std::string sha256(const std::string& data) {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        unsigned char hash[32];
        BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), hash, sizeof hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        std::string hex;
        char b[3];
        for (unsigned char c : hash) {
            std::snprintf(b, sizeof b, "%02x", c);
            hex += b;
        }
        return hex;
    }

    bool writeManifest() {
        FILE* f = _wfopen(opt_.manifest.c_str(), L"wb");
        if (!f) return fail("cannot write manifest"), false;
        for (const auto& d : dirs_)
            if (d != "/" && !ignored(d)) std::fprintf(f, "D\t%s\n", d.c_str());
        for (const auto& [p, c] : files_)
            std::fprintf(f, "F\t%s\t%s\t%zu\n", p.c_str(), sha256(c).c_str(), c.size());
        std::fclose(f);
        return true;
    }

    Options opt_;
    std::mt19937_64 rng_;
    std::shared_ptr<DiskSource> disk_;
    std::unique_ptr<Ext4Volume> vol_;
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
    uint64_t capacity_ = 0;
    uint64_t bytesWritten_ = 0;
    long op_ = 0;
    int remounts_ = 0;
    std::vector<std::string> trace_;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--seed") o.seed = std::wcstoull(next().c_str(), nullptr, 10);
        else if (a == L"--ops") o.ops = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--verify") o.verifyEvery = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--remount") o.remountEvery = std::wcstol(next().c_str(), nullptr, 10);
        else if (a == L"--manifest") o.manifest = next();
        else if (a == L"--quiet") o.quiet = true;
        else if (a == L"--fill") o.fill = true;
        else o.image = a;
    }
    if (o.image.empty()) {
        std::fprintf(stderr, "usage: ext4stress <image> [--seed N] [--ops N] [--verify N] "
                             "[--remount N] [--manifest FILE] [--quiet]\n");
        return 2;
    }
    return Stress(o).run();
}
