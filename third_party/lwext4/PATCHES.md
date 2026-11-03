# Local changes to lwext4

Vendored from https://github.com/gkostka/lwext4, commit
58bcf89a121b72d4fb66334f1693d3b30e4cb9c5 (2022-09-22).

1. **metadata_csum_seed support** (incompat 0x2000, default in e2fsprogs >= 1.47):
   - `ext4_sblock`: added `prj_quota_inum` and `checksum_seed` fields.
   - `EXT4_FINCOM_CSUM_SEED` added to the supported incompat features.
   - New `ext4_sb_csum_seed()`; all metadata checksums now start from it
     instead of `crc32c(uuid)`.
2. **`ext4_raw_inode_get()`**: load an inode by number (fast directory listing).
3. **Sparse file fix in `ext4_fread()`**: holes and unwritten extents in the
   middle or at the end of a read returned the contents of disk block 0
   instead of zeros. The whole-block loop was rewritten (contiguous runs are
   still read in one request).
4. **Safe partial writes in `ext4_fwrite()`**: blocks newly allocated for a
   partial write (holes, unwritten extents, appended blocks) are zero-filled
   first. A mapping result of block 0 (a hole in a non-extent file, which
   lwext4 cannot allocate) now fails with EIO instead of overwriting the
   start of the filesystem.
5. **`ext4_fextend()`**: grow a file sparsely, zeroing the rest of the old
   last block.
6. **`ext4_times_set()`**: set atime/mtime/ctime/crtime with nanoseconds and
   the post-2038 epoch bits.

Patches 4-6 were applied with `tools/patches/lwext4_write_safety.py`.
7. **255-byte names**: `ext4_path_check()` stopped one character short, so
   names of exactly 255 bytes (the ext4 maximum) could be neither found nor
   created.
8. **New directory checksums** (`ext4_dir_dx_init()`): the first leaf block
   of a new indexed directory got its checksum before its inode field was
   cleared, over stale disk data, so empty new directories often failed
   e2fsck's checksum check. Both new blocks are now zeroed first.
9. **Truncating sparse files** (`ext4_extent_remove_space()`): when the new
   end fell into a hole, nothing was freed and all later blocks stayed
   allocated beyond i_size. Removal now starts at the next extent.
10. **Extent prepend** (`ext4_ext_insert_leaf()`): when a new extent was
    merged in front of an existing one (filling a hole right before
    allocated blocks), the logical start was moved but not the physical
    start. The file then pointed at wrong blocks, its real block leaked and
    another block was later handed out twice (data corruption). Found by
    the stress test.
11. **Directory block split** (`ext4_dir_dx_split_data()`): when an insert
    failed only because the free space in a leaf block was fragmented (less
    than half the block in use, e.g. after deletions and with a long name),
    no split point was found and the code read `sort[-1]` - a crash or a
    corrupted directory. It now splits by entry count. Found by the stress
    test.
12. **Hardware CRC32C**: `ext4_crc32c()` uses the SSE4.2 `crc32`
    instruction on x64 (with a self test against the table version at first
    use). Metadata checksums were the main cost of writing.
