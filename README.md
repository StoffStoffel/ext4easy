# ext4win

Read Linux ext2/ext3/ext4 drives on Windows 10/11 — mounted as a normal drive letter in Explorer.

Status: **phase 2 — read-only, background service + tray app.** Planned next: installer, then write support.

## Installation (background service + tray app)

From an administrator command prompt, in the folder with `ext4win.exe` and `ext4win-tray.exe`:

```
ext4win install      registers and starts the service, adds the tray app to autostart
ext4win uninstall    removes both again
```

- The **service** runs as SYSTEM, notices plugged-in disks, finds ext partitions and mounts them read-only. Drive letters are visible to all users.
- The **tray app** asks "Mount 'xyz'?" for new drives (with "remember for this drive"), lists all Linux drives with open/unmount, the per-drive rule (ask / always / never) and can mount image files.
- Settings: `%ProgramData%\ext4win\volumes.conf` (per filesystem UUID: rule and last drive letter). Log: `%ProgramData%\ext4win\service.log`.
- Any logged-on user can mount detected Linux drives read-only through the tray app, also partitions on internal disks. Linux file permissions are not applied.

For testing without installing: `ext4win service --console` runs the service in a window; `ext4win ctl LIST` / `MOUNT id` / `ATTACH file.img` / `SUBSCRIBE` talk to it.

## Command line

```
ext4win list                         show all disks and their ext partitions (admin)
ext4win list disk.img                show partitions of an image file
ext4win mount \\.\PhysicalDrive2 X:  mount the first ext partition as X: (admin)
ext4win mount disk.img@2             mount partition 2 at the next free letter
ext4win ls   <source> /some/dir      list a directory
ext4win cat  <source> /some/file     print a file
ext4win stat <source> /some/path     file details
```

Raw disks (`\\.\PhysicalDriveN`) need administrator rights; image files don't.
Set `EXT4WIN_DEBUG=1` to log every file system request.

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

Result: `build\Release\ext4win.exe` (static runtime; the target PC only needs WinFsp).

## Tests

```
wsl bash tests/make_test_images.sh build/testdata
build\Release\ext4win.exe mount build\testdata\plain.img Q:
python tests/verify_mount.py Q: build/testdata/expected.txt
```

## Licence

GPL-3.0-or-later. Uses [lwext4](https://github.com/gkostka/lwext4) (BSD-3 / GPL-2.0-or-later, vendored with fixes listed in `third_party/lwext4/PATCHES.md`) and [WinFsp](https://winfsp.dev) (GPLv3 with FLOSS exception).
