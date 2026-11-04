# ext4easy

Read and write Linux ext2/ext3/ext4 drives on Windows 10/11 — mounted as a normal drive letter in Explorer.

Status: **reading for ext2/3/4; writing for ext4 (opt-in per drive), with background service, tray app and installer.** Write support is new — keep a backup of important data.

## Installation

Run `ext4easy-setup-<version>.exe`. It installs [WinFsp](https://winfsp.dev) (if missing) and ext4easy to `C:\Program Files\ext4easy`, starts the service and the tray app, and adds `ext4easy` to `PATH`. Remove it via *Settings → Apps*. WinFsp stays installed, since other programs may use it.

The setup is not code-signed, so Windows SmartScreen may warn on first start ("More info" → "Run anyway").

Without the installer (development): from an administrator prompt in the build folder, `ext4easy install` registers the service and tray autostart for the exe in that folder; `ext4easy uninstall` removes them. Don't mix this with the installer - uninstall one before using the other.

### What runs

- The **service** runs as SYSTEM, notices plugged-in disks, finds ext partitions and mounts them (read-only unless write access is allowed for that drive). Drive letters are visible to all users.
- The **tray app** asks "Mount 'xyz'?" for new drives (with "remember for this drive"), lists all Linux drives with open/unmount, the per-drive rule (ask / always / never), **Allow write access** per drive, and can mount image files.
- Settings: `%ProgramData%\ext4easy\volumes.conf` (per filesystem UUID: rule, last drive letter, read-only/read-write). Log: `%ProgramData%\ext4easy\service.log`.
- Any logged-on user can mount detected Linux drives through the tray app and allow write access, also for partitions on internal disks. Linux file permissions are not applied.

For testing without installing: `ext4easy service --console` runs the service in a window; `ext4easy ctl LIST` / `MOUNT id` / `WRITE id on|off` / `ATTACH file.img` / `SUBSCRIBE` talk to it.

### Write access

Off by default. Switch it on per drive in the tray menu (*Allow write access*); the setting is remembered for that filesystem, and a mounted drive is mounted again with write access on the same letter.

- Only **ext4 with extents** can be written. ext2/ext3, filesystems with an unreplayed journal (not cleanly removed from Linux) and features that cannot be kept consistent (e.g. `bigalloc`, `quota`, `verity`, `mmp`, `fast_commit`) stay read-only; the tray menu shows why.
- Changes go through the ext4 journal. Data reaches the disk within about 2 seconds, and at the latest when a file is closed or flushed. **Before unplugging, choose *Unmount* in the tray menu** — then everything is written and the drive can be removed.
- New files and folders get mode 644/755 and the owner (uid/gid) of their parent folder. Windows attributes (read-only, hidden, archive) are not stored. Hard links, symlinks and permissions cannot be created or changed from Windows.
- Image files are opened writable when the user may change them; otherwise they stay read-only.

## Command line

```
ext4easy list                         show all disks and their ext partitions (admin)
ext4easy list disk.img                show partitions of an image file
ext4easy mount \\.\PhysicalDrive2 X:  mount the first ext partition as X: (admin)
ext4easy mount disk.img@2             mount partition 2 at the next free letter
ext4easy mount disk.img W: --write    mount with write access (Ctrl+C unmounts and writes everything)
ext4easy ls   <source> /some/dir      list a directory
ext4easy cat  <source> /some/file     print a file
ext4easy stat <source> /some/path     file details
```

Raw disks (`\\.\PhysicalDriveN`) need administrator rights; image files don't. `list` marks filesystems that can only be read with `[read-only]`.
Set `EXT4EASY_DEBUG=1` to log every file system request.

### How Linux specifics appear in Windows

| Linux | Windows |
|---|---|
| names with `: * ? " < > \| \` or a trailing dot/space | those characters appear as look-alike private-use characters (same scheme as WSL/Cygwin) |
| names that are not valid UTF-8 | each invalid byte becomes U+F1xx |
| `.dotfiles` | hidden |
| symlinks | followed transparently (absolute targets are resolved from the volume root) |
| broken/looping symlinks, devices, FIFOs, sockets | 0-byte system files |
| `Readme.md` and `README.md` in one folder | both visible (volume is case-sensitive) |

Not supported (the volume is refused): `inline_data`, `encrypt`, `casefold`, `large_dir`, `ea_inode`. A filesystem with an unreplayed journal is mounted with a warning — very recent changes may be missing.

## Building

Requirements: Visual Studio 2022+ Build Tools (C++), CMake, [WinFsp](https://winfsp.dev/rel/) with the *Developer* component.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Result: `build\Release\ext4easy.exe` and `ext4easy-tray.exe` (static runtime; the target PC only needs WinFsp).

### Installer

Needs [WiX Toolset 5](https://wixtoolset.org) and the WinFsp MSI:

```
dotnet tool install --global wix --version 5.0.2
wix extension add -g WixToolset.BootstrapperApplications.wixext/5.0.2
```

Download `winfsp-<version>.msi` from https://github.com/winfsp/winfsp/releases into `third_party\winfsp\`, then:

```
powershell -ExecutionPolicy Bypass -File installer\build.ps1
```

Output in `out\installer\`: `ext4easy-setup-<version>.exe` (WinFsp + ext4easy) and `ext4easy-<version>.msi` (ext4easy only). The version comes from `project(... VERSION ...)` in `CMakeLists.txt`. The app icon is generated by `python tools/make_icon.py`.

## Tests

Reading (mounted image, checksums of all files):

```
wsl bash tests/make_test_images.sh build/testdata
build\Release\ext4easy.exe mount build\testdata\plain.img Q:
python tests/verify_mount.py Q: build/testdata/expected.txt
```

Writing (needs WSL with e2fsprogs and Python; images are created and checked by Linux's own tools). These scripts use the build in `out\`:

```
cmake -S . -B out -A x64
cmake --build out --config Release
powershell -ExecutionPolicy Bypass -File tests\run_all.ps1
```

This runs:
- `run_scenarios.ps1` – small fixed write sequences (truncate, sparse files, holes), each checked with `e2fsck`
- `run_stress.ps1` – `ext4stress`: thousands of random operations (create, write anywhere, truncate, delete, mkdir/rmdir, rename/move, timestamps, error cases) mirrored in an in-memory model, compared after every few hundred operations and after remounts; afterwards `e2fsck` must be clean and every file must match on Linux. Five filesystem variants (current defaults, 1 KiB blocks, no checksums, no journal, no 64bit/flex_bg); ext3 must be refused for writing.
- disk-full runs (`-Fill`): writing until ENOSPC must fail cleanly and leave a consistent filesystem
- `crash_test.ps1` – kills the writer at a random moment; replaying the journal must be enough, no repairs
- `run_mount_tests.ps1` – `mount_write_test.py` writes through a drive letter with normal Windows file functions (create, overwrite, append, truncate, rename/move/replace, delete, `CopyFileW`, timestamps, Unicode and case-sensitive names, 600 files in one folder, random operations), compares with a model after a remount and lets Linux check the image; crash runs kill the mount process instead of unmounting – journal replay only, no data missing

Not covered by the tests: power loss on a real drive. The writer does not yet send cache flush barriers inside journal commits, so a drive that reorders its cached writes on power loss may need `e2fsck` afterwards.

## Licence

GPL-3.0-or-later. Uses [lwext4](https://github.com/gkostka/lwext4) (BSD-3 / GPL-2.0-or-later, vendored with fixes listed in `third_party/lwext4/PATCHES.md`) and [WinFsp](https://github.com/winfsp/winfsp) (GPLv3 with FLOSS exception) – **WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**. Details and license texts: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Version history: [CHANGELOG.md](CHANGELOG.md).
