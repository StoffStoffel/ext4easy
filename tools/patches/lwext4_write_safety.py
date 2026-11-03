#!/usr/bin/env python3
"""One-off patch script for the vendored lwext4 (already applied; kept for
reference). See third_party/lwext4/PATCHES.md items 4-6."""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'third_party', 'lwext4')


def replace_once(s, old, new, what):
    if s.count(old) != 1:
        sys.exit(f'patch anchor not found exactly once: {what}')
    return s.replace(old, new, 1)


def main():
    p = os.path.join(ROOT, 'src', 'ext4.c')
    s = open(p, encoding='utf-8').read()
    if 'ext4_fwrite_zero_block' in s:
        print('already patched')
        return

    # ---------- A: zero new blocks before partial writes, refuse block 0 ----------
    s = replace_once(s, '\nint ext4_fwrite(ext4_file *file, const void *buf, size_t size, size_t *wcnt)\n{', '''
/* lwext4-win: zero-fill a freshly allocated data block, so that a partial
 * write does not leave stale disk contents visible in the rest of it. */
static int ext4_fwrite_zero_block(struct ext4_blockdev *bdev, ext4_fsblk_t fblk,
				  uint32_t block_size)
{
	int r;
	uint8_t *zeros = ext4_calloc(1, block_size);
	if (!zeros)
		return ENOMEM;
	r = ext4_block_writebytes(bdev, fblk * block_size, zeros, block_size);
	ext4_free(zeros);
	return r;
}

int ext4_fwrite(ext4_file *file, const void *buf, size_t size, size_t *wcnt)
{''', 'fwrite signature')

    s = replace_once(s, '''		r = ext4_fs_init_inode_dblk_idx(&ref, iblk_idx, &fblk);
		if (r != EOK)
			goto Finish;

		off = fblk * block_size + unalg;''', '''		ext4_fsblk_t old_fblk = 0;
		r = ext4_fs_get_inode_dblk_idx(&ref, iblk_idx, &old_fblk, true);
		if (r != EOK)
			goto Finish;

		r = ext4_fs_init_inode_dblk_idx(&ref, iblk_idx, &fblk);
		if (r != EOK)
			goto Finish;

		/* lwext4-win: holes in non-extent files cannot be allocated */
		if (fblk == 0) {
			r = EIO;
			goto Finish;
		}
		if (old_fblk == 0) {
			r = ext4_fwrite_zero_block(file->mp->fs.bdev, fblk, block_size);
			if (r != EOK)
				goto Finish;
		}

		off = fblk * block_size + unalg;''', 'fwrite unaligned head')

    s = replace_once(s, '''			iblk_idx++;

			if (!fblock_start) {
				fblock_start = fblk;
			}''', '''			iblk_idx++;

			/* lwext4-win: never write to block 0 (unallocated hole) */
			if (fblk == 0) {
				r = EIO;
				goto Finish;
			}

			if (!fblock_start) {
				fblock_start = fblk;
			}''', 'fwrite whole blocks')

    s = replace_once(s, '''	if (size) {
		uint64_t off;
		if (iblk_idx < ifile_blocks) {
			r = ext4_fs_init_inode_dblk_idx(&ref, iblk_idx, &fblk);
			if (r != EOK)
				goto Finish;
		} else {
			r = ext4_fs_append_inode_dblk(&ref, &fblk, &iblk_idx);
			if (r != EOK)
				/*Node size sholud be updated.*/
				goto out_fsize;
		}

		off = fblk * block_size;''', '''	if (size) {
		uint64_t off;
		ext4_fsblk_t old_fblk = 0;
		if (iblk_idx < ifile_blocks) {
			r = ext4_fs_get_inode_dblk_idx(&ref, iblk_idx, &old_fblk,
						       true);
			if (r != EOK)
				goto Finish;
			r = ext4_fs_init_inode_dblk_idx(&ref, iblk_idx, &fblk);
			if (r != EOK)
				goto Finish;
		} else {
			r = ext4_fs_append_inode_dblk(&ref, &fblk, &iblk_idx);
			if (r != EOK)
				/*Node size sholud be updated.*/
				goto out_fsize;
		}

		/* lwext4-win: refuse holes we cannot allocate, zero new blocks */
		if (fblk == 0) {
			r = EIO;
			goto Finish;
		}
		if (old_fblk == 0) {
			r = ext4_fwrite_zero_block(file->mp->fs.bdev, fblk, block_size);
			if (r != EOK)
				goto Finish;
		}

		off = fblk * block_size;''', 'fwrite tail')

    # ---------- B: ext4_fextend ----------
    s = replace_once(s, 'int ext4_ftruncate(ext4_file *f, uint64_t size)\n{', '''int ext4_fextend(ext4_file *f, uint64_t size)
{
	int r;
	struct ext4_inode_ref ref;
	ext4_assert(f && f->mp);

	if (f->mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(f->mp);
	ext4_trans_start(f->mp);

	struct ext4_fs *const fs = &f->mp->fs;
	r = ext4_fs_get_inode_ref(fs, f->inode, &ref);
	if (r != EOK) {
		ext4_trans_abort(f->mp);
		EXT4_MP_UNLOCK(f->mp);
		return r;
	}

	uint64_t fsize = ext4_inode_get_size(&fs->sb, ref.inode);
	uint32_t block_size = ext4_sb_get_block_size(&fs->sb);
	f->fsize = fsize;
	if (size <= fsize)
		goto Finish;

	/* Holes can only be represented with extents. */
	if (!ext4_sb_feature_incom(&fs->sb, EXT4_FINCOM_EXTENTS) ||
	    !ext4_inode_has_flag(ref.inode, EXT4_INODE_FLAG_EXTENTS)) {
		r = ENOTSUP;
		goto Finish;
	}

	/* Bytes after the old end in the last block must read as zero. */
	uint32_t tail = (uint32_t)(fsize % block_size);
	if (tail) {
		ext4_fsblk_t fblk = 0;
		r = ext4_fs_get_inode_dblk_idx(&ref, (ext4_lblk_t)(fsize / block_size),
					       &fblk, true);
		if (r != EOK)
			goto Finish;
		if (fblk) {
			uint64_t n = block_size - tail;
			if (n > size - fsize)
				n = size - fsize;
			uint8_t *zeros = ext4_calloc(1, (size_t)n);
			if (!zeros) {
				r = ENOMEM;
				goto Finish;
			}
			r = ext4_block_writebytes(fs->bdev, fblk * block_size + tail,
						  zeros, (uint32_t)n);
			ext4_free(zeros);
			if (r != EOK)
				goto Finish;
		}
	}

	ext4_inode_set_size(ref.inode, size);
	ref.dirty = true;
	f->fsize = size;

Finish:
	{
		int rr = ext4_fs_put_inode_ref(&ref);
		if (r == EOK)
			r = rr;
	}
	if (r != EOK)
		ext4_trans_abort(f->mp);
	else
		ext4_trans_stop(f->mp);
	EXT4_MP_UNLOCK(f->mp);
	return r;
}

int ext4_ftruncate(ext4_file *f, uint64_t size)
{''', 'ftruncate')

    # ---------- C: ext4_times_set ----------
    s = replace_once(s, 'int ext4_mode_set(const char *path, uint32_t mode)\n{', '''static void ext4_encode_time(int64_t sec, uint32_t nsec, uint32_t *lo,
			     uint32_t *extra)
{
	*lo = to_le32((uint32_t)sec);
	*extra = to_le32((uint32_t)(((sec - (int32_t)sec) >> 32) & 3) |
			 (nsec << 2));
}

int ext4_times_set(const char *path, const struct ext4_times *t)
{
	int r;
	struct ext4_inode_ref ref;
	struct ext4_mountpoint *mp = ext4_get_mount(path);

	if (!mp)
		return ENOENT;
	if (mp->fs.read_only)
		return EROFS;

	EXT4_MP_LOCK(mp);
	r = ext4_trans_get_inode_ref(path, mp, &ref);
	if (r != EOK)
		goto Finish;

	struct ext4_inode *in = ref.inode;
	uint32_t extra = 0;
	if (ext4_get16(&mp->fs.sb, inode_size) > EXT4_GOOD_OLD_INODE_SIZE)
		extra = to_le16(in->extra_isize);
#define HAS_FIELD(end) (EXT4_GOOD_OLD_INODE_SIZE + extra >= (end))
	uint32_t lo, ex;
	if (t->mask & EXT4_TIME_ATIME) {
		ext4_encode_time(t->sec[0], t->nsec[0], &lo, &ex);
		in->access_time = lo;
		if (HAS_FIELD(0x90))
			in->atime_extra = ex;
	}
	if (t->mask & EXT4_TIME_MTIME) {
		ext4_encode_time(t->sec[1], t->nsec[1], &lo, &ex);
		in->modification_time = lo;
		if (HAS_FIELD(0x8C))
			in->mtime_extra = ex;
	}
	if (t->mask & EXT4_TIME_CTIME) {
		ext4_encode_time(t->sec[2], t->nsec[2], &lo, &ex);
		in->change_inode_time = lo;
		if (HAS_FIELD(0x88))
			in->ctime_extra = ex;
	}
	if ((t->mask & EXT4_TIME_CRTIME) && HAS_FIELD(0x98)) {
		ext4_encode_time(t->sec[3], t->nsec[3], &lo, &ex);
		in->crtime = lo;
		in->crtime_extra = ex;
	}
#undef HAS_FIELD
	ref.dirty = true;
	r = ext4_trans_put_inode_ref(mp, &ref);
Finish:
	EXT4_MP_UNLOCK(mp);
	return r;
}

int ext4_mode_set(const char *path, uint32_t mode)
{''', 'mode_set')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

    # ---------- header ----------
    p = os.path.join(ROOT, 'include', 'ext4.h')
    h = open(p, encoding='utf-8').read()

    def insert_before_doc(h, anchor, add):
        i = h.index(anchor)
        j = h.rfind('/**', 0, i)
        return h[:j] + add + h[j:]

    h = insert_before_doc(h, 'int ext4_ftruncate(ext4_file *file, uint64_t size);', '''/**@brief   Grow a file to size (lwext4-win addition). The new range is a
 *          hole that reads as zeros; extent-mapped files only (ENOTSUP
 *          otherwise).
 *
 * @param   file File handle.
 * @param   size New size, larger than the current one.
 *
 * @return  Standard error code.*/
int ext4_fextend(ext4_file *file, uint64_t size);

''')
    h = insert_before_doc(h, 'int ext4_mode_set(const char *path, uint32_t mode);', '''#define EXT4_TIME_ATIME 1
#define EXT4_TIME_MTIME 2
#define EXT4_TIME_CTIME 4
#define EXT4_TIME_CRTIME 8

/**@brief   Timestamps for @ref ext4_times_set. Index 0..3 = atime, mtime,
 *          ctime, crtime; mask selects which ones are written.*/
struct ext4_times {
	int64_t sec[4];
	uint32_t nsec[4];
	uint32_t mask;
};

/**@brief   Set timestamps with nanoseconds and creation time
 *          (lwext4-win addition).
 *
 * @param   path Path to file/dir/link.
 * @param   t    Times to set.
 *
 * @return  Standard error code.*/
int ext4_times_set(const char *path, const struct ext4_times *t);

''')
    open(p, 'w', encoding='utf-8', newline='\n').write(h)
    print('patched')


if __name__ == '__main__':
    main()
