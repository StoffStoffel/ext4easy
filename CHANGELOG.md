# Changelog

## 0.4.0 – 2026-11-04

- **Write support for ext4** (with extents), off by default. Write support is new –
  back up important data before using it.
  - Tray menu *Allow write access* per drive (remembered per filesystem); a mounted
    drive is mounted again with write access. The menu shows read-only/read-write and
    why a drive can only be read (ext2/ext3, unreplayed journal, unsupported features);
    the reason is also written to the service log.
  - Command line: `ext4easy mount <source> [X:] --write`, `ext4easy ctl WRITE id on|off`;
    `list` marks read-only filesystems.
  - In Explorer: create, change, rename/move and delete files and folders, set
    timestamps. Data is written through the journal, flushed every 2 seconds and
    completely on unmount; after unmounting, the tray app says the drive can be removed.
  - Image files are opened writable when the user may change them.
- Write core and test suite (scenarios, random stress against a model, disk full, crash
  tests, writing through a drive letter), all checked with Linux `e2fsck`; fixes in
  lwext4 listed in `third_party/lwext4/PATCHES.md`.
- Settings from 0.3.0 are kept; all drives start read-only.
- Known limitations:
  - No cache flush barriers inside journal commits yet: a power loss while writing
    may require `e2fsck` afterwards.
  - ext4easy does not replay a journal itself yet. After a drive was not cleanly
    unmounted on Linux, mount it there once (or run `e2fsck`) to enable write access.

## 0.3.0 – 2026-11-03

- The project was renamed from **ext4win** to **ext4easy**, because the name ext4win was
  already taken. Program files, service name, settings folder
  (`%ProgramData%\ext4easy`), registry entries and the `EXT4EASY_DEBUG` variable changed
  accordingly. Settings of an earlier ext4win installation are not taken over;
  uninstall ext4win before installing ext4easy.
- Setup program `ext4easy-setup-<version>.exe`: installs WinFsp (if needed) and ext4easy to
  `C:\Program Files\ext4easy`, registers the service, tray autostart, start menu entry
  and `PATH`. Uninstall via *Settings → Apps*; WinFsp is kept.
- Separate `ext4easy-<version>.msi` for PCs that already have WinFsp.
- Own app icon and version information.
- Tray menu *About…* (and the command line help) shows version, license and the
  third-party notices, including the WinFsp copyright notice required by its license.
- Memory card swaps in card readers are detected.
- The service restarts itself after a crash; the tray app closes and restarts cleanly
  during updates.

## 0.2.0 – 2026-11-03

- Background service: notices plugged-in disks, finds ext2/3/4 partitions and mounts
  them read-only; drive letters are visible to all users.
- Tray app (German/English): asks whether to mount a new drive ("remember for this
  drive"), menu with open/unmount, per-drive rule (ask / always / never), mounting of
  image files.

## 0.1.0 – 2026-11-03

- Read-only access to ext2/ext3/ext4 (image files and disks, MBR/GPT), mounted as a
  drive letter via WinFsp.
- Command line tool: `list`, `ls`, `cat`, `stat`, `mount`.
