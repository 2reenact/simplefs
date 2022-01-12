#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/iversion.h>

#include "sfs_fs.h"
#include "sfs.h"

static int sfs_block_to_path(sector_t iblock, unsigned int offsets[4])
{
	int ptrs = SFS_NODE_PER_BLK;
	int ptrs_bits = SFS_NODE_PER_BLK_BIT;
	const long direct_blocks = SFS_DIR_BLK_CNT,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	int n = 0;

	if (iblock < direct_blocks) {
		offsets[n++] = iblock;
	} else if ((iblock -= direct_blocks) < indirect_blocks) {
		offsets[n++] = SFS_IND_BLK;
		offsets[n++] = iblock;
	} else if ((iblock -= indirect_blocks) < double_blocks) {
		offsets[n++] = SFS_DIND_BLK;
		offsets[n++] = iblock >> ptrs_bits;
		offsets[n++] = iblock & (ptrs - 1);
	} else if (((iblock -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = SFS_TIND_BLK;
		offsets[n++] = iblock >> (ptrs_bits * 2);
		offsets[n++] = (iblock >> ptrs_bits) & (ptrs - 1);
		offsets[n++] = iblock & (ptrs - 1);
	} else {
		sfs_msg(KERN_ERR, "sfs_block_to_path", "block > big");
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

static Indirect *sfs_find_branch(struct inode *inode, Indirect *chain, unsigned int *offsets, int depth, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct sfs_inode_info *si = SFS_I(inode);
	Indirect *p = chain;
	struct buffer_head *bh;

	printk(KERN_ERR "jy: find_branch 0x%x 0x%x\n", *offsets, *(si->i_data + *offsets));
	*err = 0;

	add_chain(chain, NULL, si->i_data + *offsets);
	if (!p->key)
		goto no_block;
	while (--depth) {
		printk(KERN_ERR "jy: find_branch0\n");
		bh = sb_bread(sb, le32_to_cpu(p->key));
		if (!bh)
			goto failure;
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (__le32*)bh->b_data + *++offsets);
		printk(KERN_ERR "jy: find_branch1 %n\n", (__le32 *)bh->b_data + *offsets);
		if (!p->key)
			goto no_block;
	}
	printk(KERN_ERR "jy: find_branch2\n");
	return NULL;

changed:
	printk(KERN_ERR "jy: find_branch3\n");
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;

failure:
	printk(KERN_ERR "jy: find_branch4\n");
	*err = -EIO;

no_block:
	printk(KERN_ERR "jy: find_branch5\n");
	return p;
}

static int sfs_alloc_blocks(struct inode *inode, unsigned int *new_blocks, int indirect_blks, int blks, int *err)
{
	struct super_block *sb = inode->i_sb;
	struct sfs_sb_info *sbi = SFS_SB(sb);
	struct buffer_head *bitmap_bh = NULL;
	unsigned i, target;
	int bno, ret = 0;

	printk(KERN_ERR "jy: alloc_blocks %d %d\n", indirect_blks, blks);
	*err = 0;
	target = blks + indirect_blks;

	for (i = 0; i < SFS_GET_SB(blkcnt_dmap); i++) {
		brelse(bitmap_bh);
		printk(KERN_ERR "jy: alloc_blocks0 %d\n", SFS_GET_SB(dmap_blkaddr) + i);
		bitmap_bh = sb_bread(sb, SFS_GET_SB(dmap_blkaddr) + i);
		if (!bitmap_bh) {
			*err = -EIO;
			goto failed_alloc_blocks;
		}
find_next:
		bno = 0;

		bno = find_next_zero_bit_le(bitmap_bh->b_data, SFS_BLKSIZE, 0);
		printk(KERN_ERR "jy: alloc_blocks1 %d\n", bno);
		if (bno >= sfs_max_bit(i + 1))
			continue;
		if (!test_and_set_bit_le(bno, bitmap_bh->b_data)) {
			*new_blocks++ = SFS_GET_SB(data_blkaddr) + sfs_max_bit(i) + bno;
			printk(KERN_ERR "jy: alloc_blocks1.2 0x%x\n", *(new_blocks - 1));
			ret++;
			goto got_alloc_blocks;
		}
	}
	printk(KERN_ERR "jy: alloc_blocks2\n");
	brelse(bitmap_bh);
	*err = -ENOSPC;
	goto failed_alloc_blocks;

got_alloc_blocks:
	printk(KERN_ERR "jy: alloc_blocks3\n");
	mark_buffer_dirty(bitmap_bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	if (--target > 0)
		goto find_next;
	brelse(bitmap_bh);

#if 0
	if (*(new_block - 1) > sfs_get(total_blkcnt)) {
		brelse(bitmap_bh);
		err = -EIO;
		goto failed_alloc_branch;
	}
#endif
	return ret;

failed_alloc_blocks:
	printk(KERN_ERR "jy: alloc_blocks4\n");
	return ret;
}

void sfs_free_blocks(struct inode *inode, unsigned int block, unsigned int count)
{
#if 0
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh;
	unsigned int bit, i;
	struct super_block *sb = inode->i_sb;
	struct sfs_sb_info *sbi = SFS_SB(sb);
#endif

	printk(KERN_ERR "jy: free_blocks\n");
}

static int sfs_alloc_branch(struct inode *inode, Indirect *branch, int indirect_blks, unsigned int *offsets, int *count)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	unsigned int new_blocks[4];
	unsigned i, n, num;
	int err;

	printk(KERN_ERR "jy: alloc_branch\n");
	num = sfs_alloc_blocks(inode, new_blocks, indirect_blks, *count, &err);
	if (err)
		return err;

	printk(KERN_ERR "jy: alloc_branch0 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", num, new_blocks[0], new_blocks[1], new_blocks[2], new_blocks[3]);
	branch[0].key = cpu_to_le32(new_blocks[0]);
	
	for (n = 1; n <= indirect_blks; n++) {
		printk(KERN_ERR "jy: alloc_branch1\n");
		bh = sb_getblk(sb, new_blocks[n - 1]);
		if (!bh) {
			err = -ENOMEM;
			goto failed_alloc_branch;
		}
		printk(KERN_ERR "jy: alloc_branch2\n");
		branch[n].bh = bh;
		lock_buffer(bh);
		memset(bh->b_data, 0, SFS_BLKSIZE);
		branch[n].p = (__le32 *)bh->b_data + offsets[n];
		branch[n].key = cpu_to_le32(new_blocks[n]);
		*branch[n].p = branch[n].key;
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);
		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
			sync_dirty_buffer(bh);
	}
	printk(KERN_ERR "jy: alloc_branch4\n");
	*count = num;
	return err;

failed_alloc_branch:
	printk(KERN_ERR "jy: alloc_branch5\n");
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < indirect_blks; i++)
		sfs_free_blocks(inode, new_blocks[i], 1);
	sfs_free_blocks(inode, new_blocks[i], 1);
	printk(KERN_ERR "jy: alloc_branch6\n");

	return err;
}

static void sfs_splice_branch(struct inode *inode, long block, Indirect *where, int num, int blks)
{
#if 0
	int i;
	unsigned int current_block;
#endif

	printk(KERN_ERR "jy: splice_branch\n");
	*where->p = where->key;

#if 0
	if (num == 0 && blks > 1) {
		printk(KERN_ERR "jy: splice_branch0\n");
		current_block = le32_to_cpu(where->key) + 1;
		for (i = 1; i < blks; i++) {
			printk(KERN_ERR "jy: splice_branch1\n");
			*(where->p + i) = cpu_to_le32(current_block++);
		}
	}
#endif

	if (where->bh) {
		printk(KERN_ERR "jy: splice_branch2\n");
		mark_buffer_dirty_inode(where->bh, inode);
	}

	printk(KERN_ERR "jy: splice_branch3\n");
	inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);
}

static int sfs_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned int offsets[4] = {0};//
	unsigned int bno, indirect_blks;
	int depth; // = sfs_block_to_path(iblock, offsets);
	Indirect chain[4], *partial;
	int err, count = 1;
	
//	printk(KERN_ERR "jy: get_block %lu, %lld, %ld, %d\n", inode->i_ino, iblock, bh_result->b_size, create);
	printk(KERN_ERR "jy: get_block %lu, %lld, %d\n", inode->i_ino, iblock, create);
	depth = sfs_block_to_path(iblock, offsets);
	printk(KERN_ERR "jy: get_block0 0x%x 0x%x 0x%x 0x%x %u\n", depth, offsets[0], offsets[1], offsets[2], offsets[3]);
	if (depth == 0)
		return -EIO;

	partial = sfs_find_branch(inode, chain, offsets, depth, &err);
	printk(KERN_ERR "jy: get_block1 0x%x\n", le32_to_cpu(chain[depth - 1].key));
		//no allocation needed
	if (!partial)
		goto done_get_block;

	if (!create || err == -EIO)
		goto done_get_block;

	indirect_blks = (chain + depth) - partial - 1;
	printk(KERN_ERR "jy: get_block2 %d\n", indirect_blks);

	err = sfs_alloc_branch(inode, partial, indirect_blks, offsets, &count);
	printk(KERN_ERR "jy: get_block3\n");
	if (err) {
	}

	sfs_splice_branch(inode, iblock, partial, indirect_blks, count);
	printk(KERN_ERR "jy: get_block3.5\n");
		
	partial = chain + depth - 1;

done_get_block:
	printk(KERN_ERR "jy: get_block4\n");
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	bno = le32_to_cpu(chain[depth - 1].key);
	printk(KERN_ERR "jy: get_block5 0x%x\n", bno);
	if (bno)
		map_bh(bh_result, sb, bno);
	else
		bh_result = NULL;

	return err;
}

static int sfs_writepage(struct page *page, struct writeback_control *wbc)
{
	printk(KERN_ERR "jy: writepage \n");
	return block_write_full_page(page, sfs_get_block, wbc);
}

static int sfs_readpage(struct file *file, struct page *page)
{
	printk(KERN_ERR "jy: readpage\n");
	return block_read_full_page(page, sfs_get_block);
}

int sfs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
	printk(KERN_ERR "jy: prepare_chunk %lld, %d\n", pos, len);
	return __block_write_begin(page, pos, len, sfs_get_block);
}

static inline void sfs_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr;

	printk(KERN_ERR "jy: free_data\n");
	for (; p < q; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {
			*p = 0;
			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				sfs_free_blocks(inode, block_to_free, count);
				mark_inode_dirty(inode);
			free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}

	if (count > 0) {
		sfs_free_blocks(inode, block_to_free, count);
		mark_inode_dirty(inode);
	}
}

static void sfs_free_full_branch(struct inode *inode, unsigned long ind_block, int depth)
{
	return;
}	

static void sfs_truncate_blocks(struct inode * inode)
{
	struct sfs_inode_info *si = SFS_I(inode);
	unsigned int offsets[4];
//	__le32 *i_data = SFS_I(inode)->i_data;
	int depth, depth2;
	unsigned long block;
	unsigned i;
	void *p;
	sector_t last = 0;
	printk(KERN_ERR "jy: truncate_blocks\n");

	if (inode->i_size) {
		last = (inode->i_size - 1) >> SFS_BLKSHIFT;
		depth = sfs_block_to_path(last, offsets);
		if (!depth)
			return;
	} else {
		depth = 1;
	}

	printk(KERN_ERR "jy: truncate_blocks0 %lld\n", last);
	for (depth2 = depth - 1; depth2; depth2--)
		if (offsets[depth2] != SFS_NODE_PER_BLK - 1)
			break;
	
	if (depth == 1) {
		//sfs_trunc_direct(inode);
		offsets[0] = SFS_IND_BLK;
	} else {
		p = &si->i_data[offsets[0]++];
		for (i = 0; i < depth2; i++) {
			block = le32_to_cpu(*(__le32 *)p);
			if (!block)
				break;
		
		}
	}
	for (i = offsets[0]; i <= SFS_TIND_BLK; i++) {
		p = &si->i_data[i];
		block = le32_to_cpu(*(__le32 *)p);
		if (block) {
			*(__le32 *)p = 0;
			sfs_free_full_branch(inode, block, i - SFS_IND_BLK + 1);
		}
	}
	mark_inode_dirty(inode);
}

static void sfs_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		truncate_pagecache(inode, inode->i_size);
		sfs_truncate_blocks(inode);
	}
}

static int sfs_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned flags,
		struct page **pagep, void **fsdata)
{
	int ret;

	printk(KERN_ERR "jy: write_begin\n");
	ret = block_write_begin(mapping, pos, len, flags, pagep,
			sfs_get_block);
	if (unlikely(ret))
		sfs_write_failed(mapping, pos + len);

	return ret;
}

static int sfs_write_end(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned copied,
		struct page *page, void *fsdata)
{
	int ret;

	printk(KERN_ERR "jy: write_end\n");
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len)
		sfs_write_failed(mapping, pos + len);
	return ret;
}

static sector_t sfs_bmap(struct address_space *mapping, sector_t block)
{
	printk(KERN_ERR "jy: bmap\n");
	return generic_block_bmap(mapping, block, sfs_get_block);
}

const struct address_space_operations sfs_aops = {
	.readpage	= sfs_readpage,
	.writepage	= sfs_writepage,
	.write_begin	= sfs_write_begin,
	.write_end	= sfs_write_end,
	.bmap		= sfs_bmap,
};

static int sfs_read_inode(struct inode *inode, struct sfs_inode *sfs_inode)
{       
	struct sfs_inode_info *si = SFS_I(inode);
	
	printk(KERN_ERR "jy: read_inode\n");

	if (inode->i_nlink == 0)
		return -ESTALE;

	printk(KERN_ERR "jy: read_inode0\n");
	inode->i_mode = le16_to_cpu(sfs_inode->i_mode);
	i_uid_write(inode, le32_to_cpu(sfs_inode->i_uid));
	i_gid_write(inode, le32_to_cpu(sfs_inode->i_gid));
	set_nlink(inode, le16_to_cpu(sfs_inode->i_links));
	inode->i_size = le32_to_cpu(sfs_inode->i_size);
	inode->i_atime.tv_sec = (signed)le32_to_cpu(sfs_inode->i_atime);
	inode->i_ctime.tv_sec = (signed)le32_to_cpu(sfs_inode->i_ctime);
	inode->i_mtime.tv_sec = (signed)le32_to_cpu(sfs_inode->i_mtime);
	inode->i_atime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	inode->i_blocks = le32_to_cpu(sfs_inode->i_blocks);

	memcpy(si->i_data, sfs_inode->i_daddr, sizeof(si->i_data));

	return 0;
}

static void sfs_set_inode_ops(struct inode *inode)
{
	printk(KERN_ERR "jy: set_inode_ops\n");
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &sfs_file_inode_operations;
		inode->i_fop = &sfs_file_operations;
		inode->i_mapping->a_ops = &sfs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &sfs_dir_inode_operations;
		inode->i_fop = &sfs_dir_operations;
		inode->i_mapping->a_ops = &sfs_aops;
	} 
}

struct inode *sfs_iget(struct super_block *sb, unsigned long ino)
{
	struct sfs_inode *sfs_inode;
	struct buffer_head *bh;
	struct inode *inode;
	int err = -EIO;

	printk(KERN_ERR "jy: iget %ld\n", ino);
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_sb = sb;

	bh = sb_bread(sb, sfs_inotoba(ino));
	printk(KERN_ERR "jy: iget0 0x%lx\n", sfs_inotoba(ino));
	if (!bh) {
		sfs_msg(KERN_ERR, "sfs_iget", "Failed to read inode %d\n", ino);
		goto bad_inode;
	}
	sfs_inode = (struct sfs_inode *)bh->b_data;
	err = sfs_read_inode(inode, sfs_inode);
	brelse(bh);
	if (err)
		goto bad_inode;

	inode_inc_iversion(inode);

	sfs_set_inode_ops(inode);

	unlock_new_inode(inode);

	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(err);
}

static void sfs_fill_inode(struct inode *inode, struct sfs_inode *sfs_inode)
{
	struct sfs_inode_info *si = SFS_I(inode);

	printk(KERN_ERR "jy: fill_inode\n");
	sfs_inode->i_mode = cpu_to_le16(inode->i_mode);
	sfs_inode->i_links = cpu_to_le16(inode->i_nlink);

	sfs_inode->i_uid = cpu_to_le32(i_uid_read(inode));
	sfs_inode->i_gid = cpu_to_le32(i_gid_read(inode));

	sfs_inode->i_size = cpu_to_le64(inode->i_size);
	sfs_inode->i_atime_nsec = cpu_to_le32(inode->i_atime.tv_sec);
	sfs_inode->i_ctime_nsec = cpu_to_le32(inode->i_ctime.tv_sec);
	sfs_inode->i_mtime_nsec = cpu_to_le32(inode->i_mtime.tv_sec);
	sfs_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	sfs_inode->i_flags = cpu_to_le32(si->i_flags);

	memcpy(&sfs_inode->i_daddr, si->i_data, sizeof(si->i_data));
	
	if (!inode->i_nlink)
		memset(sfs_inode, 0, sizeof(struct sfs_inode));
}

static int sfs_update_inode(struct inode *inode, int do_sync) {
	struct super_block *sb = inode->i_sb;
	struct sfs_inode *sfs_inode;
	struct buffer_head *bh;

	printk(KERN_ERR "jy: update_inode %ld, %ld\n", inode->i_ino, sfs_inotoba(inode->i_ino));
	bh = sb_bread(sb, sfs_inotoba(inode->i_ino));
	if (!bh) {
		sfs_msg(KERN_ERR, "sfs_update_inode", "Failed to read inode %lu\n", inode->i_ino);
		return -1;
	}
	printk(KERN_ERR "jy: update_inode0 %llu 0x%x\n", bh->b_blocknr, ((struct sfs_inode *)bh->b_data)->i_daddr[0]);

	sfs_inode = (struct sfs_inode *)bh->b_data;

	sfs_fill_inode(inode, sfs_inode);
	printk(KERN_ERR "jy: update_inode1 %llu 0x%x\n", bh->b_blocknr, ((struct sfs_inode *)bh->b_data)->i_daddr[0]);

	mark_buffer_dirty(bh);
	if (do_sync)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int sfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	printk(KERN_ERR "jy: write_inode %lu\n", inode->i_ino);
	return sfs_update_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

void sfs_evict_inode(struct inode *inode)
{
	int want_delete = 0;

	printk(KERN_ERR "jy: evict_inode %ld\n", inode->i_ino);

	printk(KERN_ERR "jy: evict_inode %ld\n", inode->i_ino);
	if (!inode->i_nlink && !is_bad_inode(inode)) {
		printk(KERN_ERR "jy: evict_inode0\n");
		want_delete = 1;
	}
	
	truncate_inode_pages_final(&inode->i_data);
	if (want_delete) {
		printk(KERN_ERR "jy: evict_inode0.5\n");
		inode->i_size = 0;
		if (inode->i_blocks) {
			printk(KERN_ERR "jy: evict_inode1\n");
			sfs_truncate_blocks(inode);
		}
		sfs_update_inode(inode, inode_needs_sync(inode));
	}

	printk(KERN_ERR "jy: evict_inode1.5\n");
	invalidate_inode_buffers(inode);
	clear_inode(inode);

#if 0
	if (want_delete)
		sfs_free_inode(inode);
#endif
}

struct inode *sfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb;
	struct sfs_sb_info *sbi;
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh;
	unsigned i;
	ino_t ino = 0;
	struct inode *inode;
	struct sfs_inode_info *si;
	struct sfs_inode *sfs_inode;
	struct timespec64 ts;
	int err = -ENOSPC;

	printk(KERN_ERR "jy: new_inode\n");

	if (!dir || !dir->i_nlink)
		return ERR_PTR(-EPERM);

	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	printk(KERN_ERR "jy: new_inode0\n");
	si = SFS_I(inode);
	sbi = SFS_SB(sb);

	for (i = 0; i < SFS_GET_SB(blkcnt_imap); i++) {
		brelse(bitmap_bh);
		printk(KERN_ERR "jy: new_inode0.5 %d\n", SFS_GET_SB(imap_blkaddr) + i);
		bitmap_bh = sb_bread(sb, SFS_GET_SB(imap_blkaddr) + i);
		if (!bitmap_bh) {
			err = -EIO;
			goto failed;
		}
		ino = 0;

		ino = find_next_zero_bit_le(bitmap_bh->b_data, SFS_BLKSIZE, 0);
		printk(KERN_ERR "jy: new_inode1 %ld\n", ino);
		if (ino >= sfs_max_bit(i + 1))
			continue;
		if (!test_and_set_bit_le(ino, bitmap_bh->b_data)) {
			ino += sfs_max_bit(i) + SFS_ROOT_INO;
			goto got;
		}
	}
	printk(KERN_ERR "jy: new_inode2\n");
	brelse(bitmap_bh);
	err = -ENOSPC;
	goto failed;

got:
	printk(KERN_ERR "jy: new_inode3\n");
	mark_buffer_dirty(bitmap_bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);

	printk(KERN_ERR "jy: new_inode3.5 %ld\n", ino);
	if (ino < SFS_ROOT_INO || ino > SFS_GET_SB(blkcnt_inode) + SFS_ROOT_INO) {
		sfs_msg(KERN_ERR, "sfs_new_inode", "rserved inode or inode > inodes count");
		err = -EIO;
		goto failed;
	}

	inode->i_ino = ino;
	inode_init_owner(inode, dir, mode);
	inode->i_blocks = 0;
	inode->i_generation = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	si->i_flags = SFS_I(dir)->i_flags;
	si->i_dir_start_lookup = 0;
	memset(&si->i_data, 0, sizeof(si->i_data));
	printk(KERN_ERR "jy: new_inode4\n");
	if (insert_inode_locked(inode) < 0) {
		err = -EIO;
		goto failed;
	}

	mark_inode_dirty(inode);

	bh = sb_bread(sb, sfs_inotoba(inode->i_ino));
	printk(KERN_ERR "jy: new_inode5\n");
	if (!bh) {
		sfs_msg(KERN_ERR, "sfs_new_inode", "Failed to read inode %lu\n", inode->i_ino);
		err = -EIO;
		goto fail_remove_inode;
	}
	printk(KERN_ERR "jy: new_inode6\n");
	sfs_inode = (struct sfs_inode *)bh->b_data;
	ktime_get_real_ts64(&ts);
	sfs_inode->i_ctime = cpu_to_le64(ts.tv_sec);
	sfs_inode->i_ctime_nsec = cpu_to_le32(ts.tv_nsec);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
//	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bh);
	brelse(bh);

	printk(KERN_ERR "jy: new_inode7\n");
	return inode;

fail_remove_inode:
	printk(KERN_ERR "jy: new_inode8\n");
	clear_nlink(inode);
	discard_new_inode(inode);
	return ERR_PTR(err);

failed:
	printk(KERN_ERR "jy: new_inode9\n");
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(err);
}

int sfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid = attr->ia_valid;
	int error;

	printk(KERN_ERR "jy: setattr\n");
	error = setattr_prepare(dentry, attr);
	if (error)
		return error;

	if (ia_valid & ATTR_SIZE && attr->ia_size != inode->i_size) {
		
	}

	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return error;
}

const struct inode_operations sfs_file_inode_operations = {
		.setattr = sfs_setattr,
};

const struct file_operations sfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.open		= generic_file_open,
	.fsync		= generic_file_fsync,
	.splice_read	= generic_file_splice_read,
};
