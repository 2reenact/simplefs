/*
 * inode.c
 *
 * 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/iversion.h>

#include "vsfs_fs.h"
#include "vsfs.h"

static int vsfs_block_to_path(sector_t iblock, unsigned int offsets[4])
{
	int ptrs = VSFS_NODE_PER_BLK;
	int ptrs_bits = VSFS_NODE_PER_BLK_BIT;
	const long direct_blocks = VSFS_DIR_BLK_CNT,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	int n = 0;

	if (iblock < direct_blocks) {
		offsets[n++] = iblock;
	} else if ((iblock -= direct_blocks) < indirect_blocks) {
		offsets[n++] = VSFS_IND_BLK;
		offsets[n++] = iblock;
	} else if ((iblock -= indirect_blocks) < double_blocks) {
		offsets[n++] = VSFS_DIND_BLK;
		offsets[n++] = iblock >> ptrs_bits;
		offsets[n++] = iblock & (ptrs - 1);
	} else if (((iblock -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = VSFS_TIND_BLK;
		offsets[n++] = iblock >> (ptrs_bits * 2);
		offsets[n++] = (iblock >> ptrs_bits) & (ptrs - 1);
		offsets[n++] = iblock & (ptrs - 1);
	} else {
		vsfs_msg(KERN_ERR, "vsfs_block_to_path", "block > big");
	}
	return n;

}

typedef struct {
	__le32 *p;
	__le32 key;
	struct buffer_head *bh;
} Indirect;

static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

static Indirect *vsfs_find_branch(struct inode *inode, Indirect *chain, unsigned int *offsets, int depth, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct vsfs_inode_info *vsi = VSFS_I(inode);
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;

	add_chain(chain, NULL, vsi->i_data + *offsets);
	if (!p->key)
		goto no_block;
	while (--depth) {
		bh = sb_bread(sb, le32_to_cpu(p->key));
		if (!bh)
			goto failure;
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (__le32*)bh->b_data + *++offsets);
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;

failure:
	*err = -EIO;

no_block:
	return p;
}

static int vsfs_alloc_blocks(struct inode *inode, unsigned int *new_blocks, int indirect_blks, int blks, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct vsfs_sb_info *sbi = VSFS_SB(sb);
	struct buffer_head *bitmap_bh = NULL;
	unsigned i, target;
	int bno, ret = 0;

	*err = 0;
	target = blks + indirect_blks;

	for (i = 0; i < VSFS_GET_SB(blkcnt_dmap); i++) {
		brelse(bitmap_bh);
		bitmap_bh = sb_bread(sb, VSFS_GET_SB(dmap_blkaddr) + i);
		if (!bitmap_bh) {
			*err = -EIO;
			goto failed_alloc_blocks;
		}
find_next:
		bno = 0;

		bno = find_next_zero_bit_le(bitmap_bh->b_data, VSFS_BLKSIZE, 0);
		if (bno >= vsfs_max_bit(i + 1))
			continue;
		if (!test_and_set_bit_le(bno, bitmap_bh->b_data)) {
			*new_blocks++ = VSFS_GET_SB(data_blkaddr) + vsfs_max_bit(i) + bno;
			ret++;
			goto got_alloc_blocks;
		}
	}
	brelse(bitmap_bh);
	*err = -ENOSPC;
	goto failed_alloc_blocks;

got_alloc_blocks:
	mark_buffer_dirty(bitmap_bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	if (--target > 0)
		goto find_next;
	brelse(bitmap_bh);

	if (*(new_blocks - 1) > VSFS_GET_SB(total_blkcnt)) {
		brelse(bitmap_bh);
		*err = -EIO;
		goto failed_alloc_blocks;
	}

	return ret;

failed_alloc_blocks:
	return ret;
}

void vsfs_free_blocks(struct inode *inode, unsigned int block, unsigned int count)
{
#if 0
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh;
	unsigned int bit, i;
	struct super_block *sb = inode->i_sb;
	struct vsfs_sb_info *sbi = VSFS_SB(sb);
#endif

//	Not implemented yet.. I'll do it later..
}

static int vsfs_alloc_branch(struct inode *inode, Indirect *branch, int indirect_blks, unsigned int *offsets, int *count)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	unsigned int new_blocks[4];
	unsigned i, n, num;
	int err;

	num = vsfs_alloc_blocks(inode, new_blocks, indirect_blks, *count, &err);
	if (err)
		return err;

	branch[0].key = cpu_to_le32(new_blocks[0]);
	
	for (n = 1; n <= indirect_blks; n++) {
		bh = sb_getblk(sb, new_blocks[n - 1]);
		if (!bh) {
			err = -ENOMEM;
			goto failed_alloc_branch;
		}
		branch[n].bh = bh;
		lock_buffer(bh);
		memset(bh->b_data, 0, VSFS_BLKSIZE);
		branch[n].p = (__le32 *)bh->b_data + offsets[n];
		branch[n].key = cpu_to_le32(new_blocks[n]);
		*branch[n].p = branch[n].key;
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);
		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
			sync_dirty_buffer(bh);
	}
	*count = num;
	return err;

failed_alloc_branch:
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < indirect_blks; i++)
		vsfs_free_blocks(inode, new_blocks[i], 1);
	vsfs_free_blocks(inode, new_blocks[i], 1);

	return err;
}

static void vsfs_splice_branch(struct inode *inode, long block, Indirect *where, int num, int blks)
{
	int i;
	unsigned int current_block;

	*where->p = where->key;

	if (num == 0 && blks > 1) {
		current_block = le32_to_cpu(where->key) + 1;
		for (i = 1; i < blks; i++) {
			*(where->p + i) = cpu_to_le32(current_block++);
		}
	}

	if (where->bh)
		mark_buffer_dirty_inode(where->bh, inode);

	inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);
}

static int vsfs_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned int offsets[4];
	unsigned int bno, indirect_blks;
	int depth = vsfs_block_to_path(iblock, offsets);
	Indirect chain[4], *partial;
	int err, count = 1;
	
	if (depth == 0)
		return -EIO;

	partial = vsfs_find_branch(inode, chain, offsets, depth, &err);
	if (!partial)
		goto done_get_block;

	if (!create || err == -EIO)
		goto done_get_block;

	indirect_blks = (chain + depth) - partial - 1;

	err = vsfs_alloc_branch(inode, partial, indirect_blks, offsets, &count);
	if (err)
		vsfs_msg(KERN_ERR, "vsfs_get_block", "failed to alloc_brach");


	vsfs_splice_branch(inode, iblock, partial, indirect_blks, count);
		
	partial = chain + depth - 1;

done_get_block:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	bno = le32_to_cpu(chain[depth - 1].key);
	if (bno)
		map_bh(bh_result, sb, bno);
	else
		bh_result = NULL;

	return err;
}

static int vsfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, vsfs_get_block, wbc);
}

static int vsfs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, vsfs_get_block);
}

int vsfs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
	return __block_write_begin(page, pos, len, vsfs_get_block);
}

static inline void vsfs_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr;

	for (; p < q; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {
			*p = 0;
			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				vsfs_free_blocks(inode, block_to_free, count);
				mark_inode_dirty(inode);
			free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}

	if (count > 0) {
		vsfs_free_blocks(inode, block_to_free, count);
		mark_inode_dirty(inode);
	}
}

static void vsfs_free_full_branch(struct inode *inode, unsigned long ind_block, int depth)
{
	return;
}	

static void vsfs_truncate_blocks(struct inode * inode)
{
	struct vsfs_inode_info *vsi = VSFS_I(inode);
	unsigned int offsets[4];
//	__le32 *i_data = VSFS_I(inode)->i_data;
	int depth, depth2;
	unsigned long block;
	unsigned i;
	void *p;
	sector_t last = 0;

	if (inode->i_size) {
		last = (inode->i_size - 1) >> VSFS_BLKSHIFT;
		depth = vsfs_block_to_path(last, offsets);
		if (!depth)
			return;
	} else {
		depth = 1;
	}

	for (depth2 = depth - 1; depth2; depth2--)
		if (offsets[depth2] != VSFS_NODE_PER_BLK - 1)
			break;
	
	if (depth == 1) {
		//vsfs_trunc_direct(inode);
		offsets[0] = VSFS_IND_BLK;
	} else {
		p = &vsi->i_data[offsets[0]++];
		for (i = 0; i < depth2; i++) {
			block = le32_to_cpu(*(__le32 *)p);
			if (!block)
				break;
		
		}
	}
	for (i = offsets[0]; i <= VSFS_TIND_BLK; i++) {
		p = &vsi->i_data[i];
		block = le32_to_cpu(*(__le32 *)p);
		if (block) {
			*(__le32 *)p = 0;
			vsfs_free_full_branch(inode, block, i - VSFS_IND_BLK + 1);
		}
	}
	mark_inode_dirty(inode);
}

static void vsfs_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		truncate_pagecache(inode, inode->i_size);
		vsfs_truncate_blocks(inode);
	}
}

static int vsfs_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned flags,
		struct page **pagep, void **fsdata)
{
	int ret;

	ret = block_write_begin(mapping, pos, len, flags, pagep,
			vsfs_get_block);
	if (unlikely(ret))
		vsfs_write_failed(mapping, pos + len);

	return ret;
}

static int vsfs_write_end(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned copied,
		struct page *page, void *fsdata)
{
	int ret;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len)
		vsfs_write_failed(mapping, pos + len);
	return ret;
}

static sector_t vsfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, vsfs_get_block);
}

const struct address_space_operations vsfs_aops = {
	.readpage	= vsfs_readpage,
	.writepage	= vsfs_writepage,
	.write_begin	= vsfs_write_begin,
	.write_end	= vsfs_write_end,
	.bmap		= vsfs_bmap,
};

static int vsfs_read_inode(struct inode *inode, struct vsfs_inode *vsfs_inode)
{       
	struct vsfs_inode_info *vsi = VSFS_I(inode);
	

	if (inode->i_nlink == 0)
		return -ESTALE;

	inode->i_mode = le16_to_cpu(vsfs_inode->i_mode);
	i_uid_write(inode, le32_to_cpu(vsfs_inode->i_uid));
	i_gid_write(inode, le32_to_cpu(vsfs_inode->i_gid));
	set_nlink(inode, le16_to_cpu(vsfs_inode->i_links));
	inode->i_size = le32_to_cpu(vsfs_inode->i_size);
	inode->i_atime.tv_sec = (signed)le32_to_cpu(vsfs_inode->i_atime);
	inode->i_ctime.tv_sec = (signed)le32_to_cpu(vsfs_inode->i_ctime);
	inode->i_mtime.tv_sec = (signed)le32_to_cpu(vsfs_inode->i_mtime);
	inode->i_atime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	inode->i_blocks = le32_to_cpu(vsfs_inode->i_blocks);

	memcpy(vsi->i_data, vsfs_inode->i_daddr, sizeof(vsi->i_data));

	return 0;
}

static void vsfs_set_inode_ops(struct inode *inode)
{
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &vsfs_file_inode_operations;
		inode->i_fop = &vsfs_file_operations;
		inode->i_mapping->a_ops = &vsfs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &vsfs_dir_inode_operations;
		inode->i_fop = &vsfs_dir_operations;
		inode->i_mapping->a_ops = &vsfs_aops;
	} 
}

struct inode *vsfs_iget(struct super_block *sb, unsigned long ino)
{
	struct vsfs_inode *vsfs_inode;
	struct buffer_head *bh;
	struct inode *inode;
	int err = -EIO;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_sb = sb;

	bh = sb_bread(sb, vsfs_inotoba(ino));
	if (!bh) {
		vsfs_msg(KERN_ERR, "vsfs_iget", "Failed to read inode %d\n", ino);
		goto bad_inode;
	}
	vsfs_inode = (struct vsfs_inode *)bh->b_data;
	err = vsfs_read_inode(inode, vsfs_inode);
	brelse(bh);
	if (err)
		goto bad_inode;

	inode_inc_iversion(inode);

	vsfs_set_inode_ops(inode);

	unlock_new_inode(inode);

	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(err);
}

static void vsfs_fill_inode(struct inode *inode, struct vsfs_inode *vsfs_inode)
{
	struct vsfs_inode_info *vsi = VSFS_I(inode);

	vsfs_inode->i_mode = cpu_to_le16(inode->i_mode);
	vsfs_inode->i_links = cpu_to_le16(inode->i_nlink);

	vsfs_inode->i_uid = cpu_to_le32(i_uid_read(inode));
	vsfs_inode->i_gid = cpu_to_le32(i_gid_read(inode));

	vsfs_inode->i_size = cpu_to_le64(inode->i_size);
	vsfs_inode->i_atime_nsec = cpu_to_le32(inode->i_atime.tv_sec);
	vsfs_inode->i_ctime_nsec = cpu_to_le32(inode->i_ctime.tv_sec);
	vsfs_inode->i_mtime_nsec = cpu_to_le32(inode->i_mtime.tv_sec);
	vsfs_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	vsfs_inode->i_flags = cpu_to_le32(vsi->i_flags);

	memcpy(&vsfs_inode->i_daddr, vsi->i_data, sizeof(vsi->i_data));
	
	if (!inode->i_nlink)
		memset(vsfs_inode, 0, sizeof(struct vsfs_inode));
}

static int vsfs_update_inode(struct inode *inode, int do_sync) {
	struct super_block *sb = inode->i_sb;
	struct vsfs_inode *vsfs_inode;
	struct buffer_head *bh;

	bh = sb_bread(sb, vsfs_inotoba(inode->i_ino));
	if (!bh) {
		vsfs_msg(KERN_ERR, "vsfs_update_inode", "Failed to read inode %lu\n", inode->i_ino);
		return -1;
	}

	vsfs_inode = (struct vsfs_inode *)bh->b_data;

	vsfs_fill_inode(inode, vsfs_inode);

	mark_buffer_dirty(bh);
	if (do_sync)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int vsfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return vsfs_update_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

void vsfs_evict_inode(struct inode *inode)
{
	int want_delete = 0;


	if (!inode->i_nlink && !is_bad_inode(inode))
		want_delete = 1;
	
	truncate_inode_pages_final(&inode->i_data);
	if (want_delete) {
		inode->i_size = 0;
		if (inode->i_blocks)
			vsfs_truncate_blocks(inode);
		vsfs_update_inode(inode, inode_needs_sync(inode));
	}

	invalidate_inode_buffers(inode);
	clear_inode(inode);

}

struct inode *vsfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb;
	struct vsfs_sb_info *sbi;
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh;
	unsigned i;
	ino_t ino = 0;
	struct inode *inode;
	struct vsfs_inode_info *vsi;
	struct vsfs_inode *vsfs_inode;
	struct timespec64 ts;
	int err = -ENOSPC;


	if (!dir || !dir->i_nlink)
		return ERR_PTR(-EPERM);

	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	vsi = VSFS_I(inode);
	sbi = VSFS_SB(sb);

	for (i = 0; i < VSFS_GET_SB(blkcnt_imap); i++) {
		brelse(bitmap_bh);
		bitmap_bh = sb_bread(sb, VSFS_GET_SB(imap_blkaddr) + i);
		if (!bitmap_bh) {
			err = -EIO;
			goto failed;
		}
		ino = 0;

		ino = find_next_zero_bit_le(bitmap_bh->b_data, VSFS_BLKSIZE, 0);
		if (ino >= vsfs_max_bit(i + 1))
			continue;
		if (!test_and_set_bit_le(ino, bitmap_bh->b_data)) {
			ino += vsfs_max_bit(i) + VSFS_ROOT_INO;
			goto got;
		}
	}
	brelse(bitmap_bh);
	err = -ENOSPC;
	goto failed;

got:
	mark_buffer_dirty(bitmap_bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);

	if (ino < VSFS_ROOT_INO || ino > VSFS_GET_SB(blkcnt_inode) + VSFS_ROOT_INO) {
		vsfs_msg(KERN_ERR, "vsfs_new_inode", "rserved inode or inode > inodes count");
		err = -EIO;
		goto failed;
	}

	inode->i_ino = ino;
	inode_init_owner(inode, dir, mode);
	inode->i_blocks = 0;
	inode->i_generation = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	vsi->i_flags = VSFS_I(dir)->i_flags;
	vsi->i_dir_start_lookup = 0;
	memset(&vsi->i_data, 0, sizeof(vsi->i_data));
	if (insert_inode_locked(inode) < 0) {
		err = -EIO;
		goto failed;
	}

	mark_inode_dirty(inode);

	bh = sb_bread(sb, vsfs_inotoba(inode->i_ino));
	if (!bh) {
		vsfs_msg(KERN_ERR, "vsfs_new_inode", "Failed to read inode %lu\n", inode->i_ino);
		err = -EIO;
		goto fail_remove_inode;
	}
	vsfs_inode = (struct vsfs_inode *)bh->b_data;
	ktime_get_real_ts64(&ts);
	vsfs_inode->i_ctime = cpu_to_le64(ts.tv_sec);
	vsfs_inode->i_ctime_nsec = cpu_to_le32(ts.tv_nsec);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bh);
	brelse(bh);

	return inode;

fail_remove_inode:
	clear_nlink(inode);
	discard_new_inode(inode);
	return ERR_PTR(err);

failed:
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(err);
}

int vsfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid = attr->ia_valid;
	int err;

	err= setattr_prepare(dentry, attr);
	if (err)
		return err;

	if (ia_valid & ATTR_SIZE && attr->ia_size != inode->i_size) {
		//err = sfs_truncate(inode, attr->ia_size);
		if (err)
			return err;
	}

	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return err;
}

const struct inode_operations vsfs_file_inode_operations = {
		.setattr = vsfs_setattr,
};

const struct file_operations vsfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.open		= generic_file_open,
	.fsync		= generic_file_fsync,
	.splice_read	= generic_file_splice_read,
};
