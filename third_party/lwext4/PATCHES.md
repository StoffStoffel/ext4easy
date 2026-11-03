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
