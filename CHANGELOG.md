# Changelog

## 0.3.0 – 2026-11-03

- Setup program `ext4win-setup-<version>.exe`: installs WinFsp (if needed) and ext4win to
  `C:\Program Files\ext4win`, registers the service, tray autostart, start menu entry
  and `PATH`. Uninstall via *Settings → Apps*; WinFsp is kept.
- Separate `ext4win-<version>.msi` for PCs that already have WinFsp.
- Own app icon and version information.
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
