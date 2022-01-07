/*
 * sfs.c
 *
 * Copyright 2020 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/log2.h>
#include <linux/quotaops.h>
#include <linux/uaccess.h>
#include <linux/dax.h>
#include <linux/iversion.h>
#include <linux/writeback.h>

//#include <linux/sfs_fs.h>
#include "sfs.h"

struct inode *sfs_iget(struct super_block *sb, unsigned long ino);

const struct inode_operations sfs_file_inode_operations;
const struct file_operations sfs_file_operations;
const struct inode_operations sfs_dir_inode_operations;
const struct file_operations sfs_dir_operations;

static int sfs_commit_chunk(struct page *page, loff_t pos, unsigned len);

void sfs_msg(const char *level, const char *funtion, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sSFS(%s): %pV\n", level, funtion, &vaf);
	va_end(args);
}

static struct kmem_cache *sfs_inode_cachep;

static struct inode *sfs_alloc_inode(struct super_block *sb)
{
	struct sfs_inode_info *si;
	printk(KERN_ERR "jy: alloc inode\n");

	si = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
	if (!si)
		return NULL;

	inode_set_iversion(&si->vfs_inode, 1);

	return &si->vfs_inode;
}

static void sfs_free_inode(struct inode *inode)
{
	printk(KERN_ERR "jy: free_inode %lu\n", inode->i_ino);
	kmem_cache_free(sfs_inode_cachep, SFS_I(inode));
}

static void init_once(void *foo)
{
	struct sfs_inode_info *si = (struct sfs_inode_info *) foo;

	printk(KERN_ERR "jy: init_once\n");
	inode_init_once(&si->vfs_inode);
}

static int __init init_inode_cache(void)
{
	printk(KERN_ERR "jy: init_inode_cache\n");
        sfs_inode_cachep = kmem_cache_create_usercopy("sfs_inode_cache",
				sizeof(struct sfs_inode_info), 0,
				(SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_ACCOUNT),
				offsetof(struct sfs_inode_info, i_data),
				sizeof_field(struct sfs_inode_info, i_data),
				init_once);
	if (sfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inode_cache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	printk(KERN_ERR "jy: destroy_inode_cache\n");
	rcu_barrier();
	kmem_cache_destroy(sfs_inode_cachep);
}





static inline void sfs_put_page(struct page *page)
{
	kunmap(page);
	put_page(page);
}

static struct page *sfs_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
//	struct page *page = read_mapping_page(mapping, n, NULL);
	struct page *page;
	printk(KERN_ERR "jy: get_page %ld %ld\n", dir->i_ino, n);
 	page = read_mapping_page(mapping, n, NULL);

	if (!IS_ERR(page)) {
		printk(KERN_ERR "jy: get_page0\n");
		kmap(page);
		if (unlikely(!PageChecked(page))) {
			if (PageError(page))// || !sfs_check_page(page))
				goto get_page_fail;
		}
		printk(KERN_ERR "jy: get_page1\n");
	}
	return page;

get_page_fail:
	printk(KERN_ERR "jy: get_page2\n");
	sfs_put_page(page);
	return ERR_PTR(-EIO);
}

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

	printk(KERN_ERR "jy: find_branch %x %x\n", *offsets, *(si->i_data + *offsets));
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

	for (i = 0; i < sfs_get(blkcnt_dmap); i++) {
		brelse(bitmap_bh);
		printk(KERN_ERR "jy: alloc_blocks0 %d\n", sfs_get(dmap_blkaddr) + i);
		bitmap_bh = sb_bread(sb, sfs_get(dmap_blkaddr) + i);
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
			*new_blocks++ = sfs_get(data_blkaddr) + sfs_max_bit(i) + bno;
			printk(KERN_ERR "jy: alloc_blocks1.2 %x\n", *(new_blocks - 1));
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
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh;
	unsigned int bit, i;
	struct super_block *sb = inode->i_sb;
	struct sfs_sb_info *sbi = SFS_SB(sb);

	
}

static int sfs_alloc_branch(struct inode *inode, Indirect *branch, int indirect_blks, unsigned int *offsets, int *count)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	unsigned int new_blocks[4] = {0}; //
	unsigned int current_block;
	unsigned i, n, num;
	int err;

	printk(KERN_ERR "jy: alloc_branch\n");
	num = sfs_alloc_blocks(inode, new_blocks, indirect_blks, *count, &err);
	if (err)
		return err;

	printk(KERN_ERR "jy: alloc_branch0 %x, %x, %x, %x, %x\n", num, new_blocks[0], new_blocks[1], new_blocks[2], new_blocks[3]);
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
	int i;
	unsigned int current_block;

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
	printk(KERN_ERR "jy: get_block0 %x %x %x %x %u\n", depth, offsets[0], offsets[1], offsets[2], offsets[3]);
	if (depth == 0)
		return -EIO;

	partial = sfs_find_branch(inode, chain, offsets, depth, &err);
	printk(KERN_ERR "jy: get_block1\n");
		//no allocation needed
	if (!partial) {

		goto done_get_block;
	}

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
	printk(KERN_ERR "jy: get_block5 %x\n", bno);
	map_bh(bh_result, sb, bno);

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
	printk(KERN_ERR "jy: prepare_chunk\n");
	return __block_write_begin(page, pos, len, sfs_get_block);
}

static void sfs_truncate_blocks(struct inode * inode)
{
	printk(KERN_ERR "jy: truncate_blocks\n");
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
	return generic_block_bmap(mapping,block, sfs_get_block);
}

const struct address_space_operations sfs_aops = {
	.readpage	= sfs_readpage,
	.writepage	= sfs_writepage,
	.write_begin	= sfs_write_begin,
	.write_end	= sfs_write_end,
	.bmap		= sfs_bmap,
};







static unsigned sfs_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static inline int sfs_match(int len, const unsigned char *name, struct sfs_dir_entry *de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

static inline struct sfs_dir_entry *sfs_next_entry(struct sfs_dir_entry *p)
{
	return (struct sfs_dir_entry *)((char *)p + le16_to_cpu(p->rec_len));
}

struct sfs_dir_entry *sfs_find_entry(struct inode *dir, const struct qstr *qstr, struct page **res_page)
{
	const unsigned char *name = qstr->name;
	int namelen = qstr->len;
	unsigned reclen = SFS_DIR_REC_LEN(namelen);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct sfs_inode_info *si = SFS_I(dir);
	struct sfs_dir_entry *de;

	printk(KERN_ERR "jy: find_entry %s, %ld\n", name, dir->i_ino);

	if (npages == 0 || namelen > SFS_MAXNAME_LEN)
		goto out_find_entry;

	*res_page = NULL;

	start = si->i_dir_start_lookup;

	if (start >= npages)
		start = 0;
	n = start;
	do {
		char *kaddr;
		page = sfs_get_page(dir, n);
		if (!IS_ERR(page)) {
			kaddr = page_address(page);
			de = (struct sfs_dir_entry *)(kaddr + SFS_DENTRY_OFFSET);
			kaddr += sfs_last_byte(dir, n) - reclen;
			while ((char *) de <= kaddr) {
				if (de->rec_len == 0) {
					sfs_msg(KERN_ERR, "sfs_find_entry", "zero-length diretory entry");
					sfs_put_page(page);
					goto out_find_entry;
				}
				printk(KERN_ERR "jy: find_entry4: %d, %s\n", de->inode, de->name);
				if (sfs_match(namelen, name, de))
					goto entry_found;
				de = sfs_next_entry(de);
			}
			sfs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);
	printk(KERN_ERR "jy: find_entry5\n");

out_find_entry:
	return NULL;

entry_found:
	printk(KERN_ERR "jy: find_entry6\n");
	*res_page = page;
	si->i_dir_start_lookup = n;
	return de;
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

	for (i = 0; i < sfs_get(blkcnt_imap); i++) {
		brelse(bitmap_bh);
		printk(KERN_ERR "jy: new_inode0.5 %d\n", sfs_get(imap_blkaddr) + i);
		bitmap_bh = sb_bread(sb, sfs_get(imap_blkaddr) + i);
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
	if (ino < SFS_ROOT_INO || ino > sfs_get(blkcnt_inode) + SFS_ROOT_INO) {
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

int sfs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned reclen = SFS_DIR_REC_LEN(namelen);
	unsigned short rec_len, name_len;
	struct page *page = NULL;
	struct sfs_dir_entry *de;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	char *kaddr;
	loff_t pos;
	int err;

	printk(KERN_ERR "jy: add_link %s, %ld\n", name, dir->i_ino);
	for (n = 0; n <= npages; n++) {
		char *dir_end;

		printk(KERN_ERR "jy: add_link1\n");
		page = sfs_get_page(dir, n);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out_add_link;
		lock_page(page);
		printk(KERN_ERR "jy: add_link2\n");
		kaddr = page_address(page);
		dir_end = kaddr + sfs_last_byte(dir, n);
		de = (struct sfs_dir_entry *)kaddr;
		kaddr += PAGE_SIZE - reclen;
		while ((char *)de <= kaddr) {
			printk(KERN_ERR "jy: add_link3 %d %s\n", de->inode, de->name);
			if ((char *)de == dir_end) {
				name_len = 0;
				rec_len = SFS_BLKSIZE;
				de->rec_len = cpu_to_le16(SFS_BLKSIZE);
				de->inode = 0;
				goto got_it;
			}
			if (de->rec_len == 0) {
				sfs_msg(KERN_ERR, "sfs_add_link", "zero-length directory entry");
				err = -EIO;
				goto out_unlock;
			}	
			err = -EEXIST;
			printk(KERN_ERR "jy: add_link4\n");
			if (sfs_match(namelen, name, de))
				goto out_unlock;
			name_len = SFS_DIR_REC_LEN(de->name_len);
			rec_len = le16_to_cpu(de->rec_len);
			if (!de->inode && rec_len >= reclen)
				goto got_it;
			printk(KERN_ERR "jy: add_link5\n");
			if (rec_len >= name_len + reclen)
				goto got_it;
			de = (struct sfs_dir_entry *)((char *)de + rec_len);
		}
		unlock_page(page);
		sfs_put_page(page);
	}
	printk(KERN_ERR "jy: add_link6\n");
	return -EINVAL;

got_it:
	pos = page_offset(page) + (char *)de - (char *)page_address(page);
	err = sfs_prepare_chunk(page, pos, rec_len);
	printk(KERN_ERR "jy: add_link7 %lld\n", pos);
	if (err)
		goto out_unlock;
	printk(KERN_ERR "jy: add_link8 %u\n", de->inode);
	if (de->inode) {
		struct sfs_dir_entry *del = (struct sfs_dir_entry *)((char *)de + name_len);
		del->rec_len = cpu_to_le16(rec_len - name_len);
		de->rec_len = cpu_to_le16(name_len);

		de = del;
	}

	de->name_len = namelen;
	memcpy(de->name, name, namelen + 1);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = inode->i_mode;

	err = sfs_commit_chunk(page, pos, rec_len);
	dir->i_mtime = dir->i_ctime = current_time(dir);

	mark_inode_dirty(dir);

out_put:
	printk(KERN_ERR "jy: add_link9\n");
	sfs_put_page(page);

out_add_link:
	printk(KERN_ERR "jy: add_link10\n");
	return err;

out_unlock:
	printk(KERN_ERR "jy: add_link11\n");
	unlock_page(page);
	goto out_put;
}

static inline int sfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
//	int err = sfs_add_link(dentry, inode);
	int err;
	printk(KERN_ERR "jy: add_nondir\n");
	err = sfs_add_link(dentry, inode);
	if (!err) {
		d_instantiate_new(dentry, inode);
		return 0;
	}
	printk(KERN_ERR "jy: add_nondir1\n");
	inode_dec_link_count(inode);
	printk(KERN_ERR "jy: add_nondir2\n");
	discard_new_inode(inode);
	printk(KERN_ERR "jy: add_nondir3\n");
	return err;
}

static int sfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;

	printk(KERN_ERR "jy: create %s\n", dentry->d_name.name);
	inode = sfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	printk(KERN_ERR "jy: create0\n");

	inode->i_op = &sfs_file_inode_operations;
	inode->i_fop = &sfs_file_operations;
	inode->i_mapping->a_ops = &sfs_aops;
	mark_inode_dirty(inode);
	printk(KERN_ERR "jy: create1\n");
	return sfs_add_nondir(dentry, inode);
}

static int sfs_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;

	printk(KERN_ERR "jy: commit_chunk %llu, %lu\n", pos, page->mapping->host->i_ino);
	inode_inc_iversion(dir);
	block_write_end(NULL, mapping, pos, len, len, page, NULL);
	printk(KERN_ERR "jy: commit_chunk0\n");

	if (pos + len > dir->i_size) {
		printk(KERN_ERR "jy: commit_chunk1\n");
		i_size_write(dir, pos + len);
		mark_inode_dirty(dir);
	}

	if (IS_DIRSYNC(dir)) {
		printk(KERN_ERR "jy: commit_chunk2\n");
		err = write_one_page(page);
		if (!err)
			err = sync_inode_metadata(dir, 1);
		printk(KERN_ERR "jy: commit_chunk3\n");
	} else {
		printk(KERN_ERR "jy: commit_chunk4\n");
		unlock_page(page);
	}
	printk(KERN_ERR "jy: commit_chunk5\n");

	return err;
}

int sfs_make_empty(struct inode *inode, struct inode *dir)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	struct sfs_dir_entry *de;
	void *kaddr;
	int err;

	printk(KERN_ERR "jy: make_empty\n");
	if (!page)
		return -ENOMEM;

	printk(KERN_ERR "jy: make_empty0 %lu, %lu\n", inode->i_ino, dir->i_ino);
	err = sfs_prepare_chunk(page, 0, SFS_BLKSIZE);
	if (err) {
		unlock_page(page);
		goto fail;
	}

	printk(KERN_ERR "jy: make_empty1\n");
	kaddr = kmap_atomic(page);
	memset(kaddr, 0, PAGE_SIZE);

	de = (struct sfs_dir_entry *)kaddr;
	de->name_len = 1;
	de->rec_len = cpu_to_le16(SFS_DIR_REC_LEN(1));
	memcpy(de->name, ".\0\0", 4);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = fs_umode_to_ftype(inode->i_mode);

	de = (struct sfs_dir_entry *)(kaddr + SFS_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = cpu_to_le16(SFS_BLKSIZE - SFS_DIR_REC_LEN(2));
	memcpy(de->name, "..\0", 4);
	de->inode = cpu_to_le32(dir->i_ino);
	de->file_type = fs_umode_to_ftype(inode->i_mode);

	printk(KERN_ERR "jy: make_empty2\n");
	kunmap_atomic(kaddr);
	err = sfs_commit_chunk(page, 0, SFS_BLKSIZE);
fail:
	printk(KERN_ERR "jy: make_empty3\n");
	put_page(page);
	return err;
}

static int sfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	printk(KERN_ERR "jy: mkdir %ld, %s\n", dir->i_ino, dentry->d_name.name);
	inode_inc_link_count(dir);

	inode = sfs_new_inode(dir, S_IFDIR|mode);
	printk(KERN_ERR "jy: mkdir0 %ld\n", inode->i_ino);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;
	
	printk(KERN_ERR "jy: mkdir1\n");
	inode->i_op = &sfs_dir_inode_operations;
	inode->i_fop = &sfs_dir_operations;
	inode->i_mapping->a_ops = &sfs_aops;

	inode_inc_link_count(inode);

	printk(KERN_ERR "jy: mkdir2\n");
	err = sfs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	printk(KERN_ERR "jy: mkdir3\n");
	err = sfs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	printk(KERN_ERR "jy: mkdir4\n");
	d_instantiate_new(dentry, inode);
	return 0;

out_fail:
	printk(KERN_ERR "jy: mkdir5\n");
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);

out_dir:
	printk(KERN_ERR "jy: mkdir6\n");
	inode_dec_link_count(dir);
	return err;
}

ino_t sfs_inode_by_name(struct inode *dir, const struct qstr *qstr)
{
	ino_t ret = 0;
	struct sfs_dir_entry *de;
	struct page *page;

	printk(KERN_ERR "jy: inode_by_name\n");
	de = sfs_find_entry(dir, qstr, &page);
	if (de) {
		printk(KERN_ERR "jy: inode_by_name0\n");
		ret = le32_to_cpu(de->inode);
		sfs_put_page(page);
	}
	printk(KERN_ERR "jy: inode_by_name1\n");
	return ret;
}

static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	char *name = kzalloc(20, GFP_KERNEL);
	memcpy(name, dentry->d_name.name, dentry->d_name.len);
	printk(KERN_ERR "jy: lookup:%s(%d)\n", name, dentry->d_name.len);
	if (dentry->d_name.len > SFS_MAXNAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	printk(KERN_ERR "jy: lookup0");
	ino = sfs_inode_by_name(dir, &dentry->d_name);
	if (ino) {
		printk(KERN_ERR "jy: lookup1: %ld", ino);
		inode = sfs_iget(dir->i_sb, ino);
	}
	printk(KERN_ERR "jy: lookup2");
	return d_splice_alias(inode, dentry);
}

int sfs_getattr(const struct path *path, struct kstat *stat,
		u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(inode, stat);

	return 0;
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

const struct inode_operations sfs_dir_inode_operations = {
	.lookup         = sfs_lookup,
	.create		= sfs_create,
	.mkdir          = sfs_mkdir,
/*
	.link           = sfs_link,
	.unlink         = sfs_unlink,
	.symlink        = sfs_symlink,
	.rmdir          = sfs_rmdir,
	.mknod          = sfs_mknod,
	.rename         = sfs_rename,
*/	
};











static int sfs_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	unsigned int offset = pos & ~PAGE_MASK;
	unsigned long n = pos >> PAGE_SHIFT;
	unsigned long npages = dir_pages(inode);
	bool need_revalidate = !inode_eq_iversion(inode, file->f_version);

	printk(KERN_ERR "jy: readdir:%s %ld %ld %lld\n",
			file->f_path.dentry->d_name.name, file->f_inode->i_ino, npages, pos);

	if (pos > inode->i_size - SFS_DIR_REC_LEN(1))
		return 0;

	for ( ; n < npages; n++, offset = 0) {
		char *kaddr, *limit;
		struct sfs_dir_entry *de;

		struct page *page = sfs_get_page(inode, n);

		if (IS_ERR(page)) {
			sfs_msg(KERN_ERR, "sfs_readdir", "bad page in %lu", inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return -EIO;
		}
		printk(KERN_ERR "jy: readdir1\n");
		kaddr = page_address(page);
		if (need_revalidate) {
			if (offset) {
				printk(KERN_ERR "jy: readdir1.5\n");
				//jy
				ctx->pos = (n << PAGE_SHIFT) + offset;
			}
			file->f_version = inode_query_iversion(inode);
			need_revalidate = false;
		}
		de = (struct sfs_dir_entry *)(kaddr + offset);
		limit = kaddr + sfs_last_byte(inode, n) - SFS_DIR_REC_LEN(1);
		for ( ; (char *)de <= limit; de = sfs_next_entry(de)) {
			if (de->rec_len == 0) {
				sfs_msg(KERN_ERR, "sfs_find_entry", "zero-length diretory entry");
				sfs_put_page(page);
				return -EIO;
			}
			printk(KERN_ERR "jy: readdir2: %d %s\n", de->inode, de->name);
			if (de->inode) {
				unsigned char d_type = DT_UNKNOWN;

				d_type = fs_ftype_to_dtype(de->file_type);
				if (!dir_emit(ctx, de->name, de->name_len,
						le32_to_cpu(de->inode),
						d_type)) {
					sfs_put_page(page);
					return 0;
				}
			}
			ctx->pos += le16_to_cpu(de->rec_len);
		}
		sfs_put_page(page);
	}
	return 0;
}

const struct file_operations sfs_dir_operations = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
	.fsync          = generic_file_fsync,
	.iterate_shared	= sfs_readdir,
};














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
	printk(KERN_ERR "jy: update_inode0 %llu %x\n", bh->b_blocknr, ((struct sfs_inode *)bh->b_data)->i_daddr[0]);

	sfs_inode = (struct sfs_inode *)bh->b_data;

	sfs_fill_inode(inode, sfs_inode);
	printk(KERN_ERR "jy: update_inode1 %llu %x\n", bh->b_blocknr, ((struct sfs_inode *)bh->b_data)->i_daddr[0]);

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

	printk(KERN_ERR "jy: evict_inode\n");
	if (!inode->i_nlink && !is_bad_inode(inode))
		want_delete = 1;
	
	truncate_inode_pages_final(&inode->i_data);
	printk(KERN_ERR "jy: evict_inode0\n");
	if (want_delete) {
		inode->i_size = 0;
		if (inode->i_blocks &&
			(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
			S_ISLNK(inode->i_mode)))
			sfs_truncate_blocks(inode);
		sfs_update_inode(inode, inode_needs_sync(inode));
	}

	printk(KERN_ERR "jy: evict_inode1\n");
	invalidate_inode_buffers(inode);
	clear_inode(inode);

	printk(KERN_ERR "jy: evict_inode2\n");
	if (want_delete)
		sfs_free_inode(inode);
	printk(KERN_ERR "jy: evict_inode3\n");
}

static int sfs_sync_fs(struct super_block *sb, int wait)
{
	printk(KERN_ERR "jy: sync_fs\n");

	return 0;
}

static void sfs_put_super(struct super_block *sb)
{
	struct sfs_sb_info *sbi = SFS_SB(sb);

	printk(KERN_ERR "jy: put_super\n");
	
	kvfree(sbi->raw_super);
	kvfree(sbi);

	return;
}

static const struct super_operations sfs_sops = {
	.alloc_inode    = sfs_alloc_inode,
	.free_inode     = sfs_free_inode,
	.write_inode    = sfs_write_inode,
	.evict_inode    = sfs_evict_inode,
	.sync_fs        = sfs_sync_fs,
	.put_super      = sfs_put_super,
/*
	.freeze_fs      = sfs_freeze,
	.unfreeze_fs    = sfs_unfreeze,
	.statfs         = sfs_statfs,
	.remount_fs     = sfs_remount,
	.show_options   = sfs_show_options,
*/	
};

static int sfs_read_raw_super(struct sfs_sb_info *sbi, struct sfs_super_block **raw_super, int *valid_super_block)
{
	struct super_block *sb = sbi->sb;
	struct sfs_super_block *super;
	struct buffer_head *bh;
	int block;
	int err = 0;

	super = kzalloc(sizeof(struct sfs_super_block), GFP_KERNEL);
	if (!super)
		return -ENOMEM;
	
	for (block = 0; block < 2; block++) {
		bh = sb_bread(sb, block);
		if (!bh) {
			sfs_msg(KERN_ERR, "read_raw_super_block", "Failed to read %th superblock", block + 1);
			err = -EIO;
			continue;
		}

		if (!*raw_super) {
			memcpy(super, bh->b_data + SFS_SUPER_OFFSET, sizeof(*super));
			*valid_super_block = block;
			*raw_super = super;
		}
		brelse(bh);
	}

	if (!*raw_super)
		kvfree(super);
	else
		err = 0;

	return err;
}

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

static void sfs_init_sb_info(struct sfs_sb_info *sbi, struct sfs_super_block *raw_super)
{
	sbi->dmap_start_lookup = 0;
	sbi->imap_start_lookup = 0;
	sbi->imap_blkaddr = le32_to_cpu(raw_super->imap_blkaddr);
	sbi->dmap_blkaddr = le32_to_cpu(raw_super->dmap_blkaddr);
	sbi->inode_blkaddr = le32_to_cpu(raw_super->inodes_blkaddr);
	sbi->data_blkaddr = le32_to_cpu(raw_super->data_blkaddr);
	sbi->blkcnt_imap = le32_to_cpu(raw_super->block_count_imap);
	sbi->blkcnt_dmap = le32_to_cpu(raw_super->block_count_dmap);
	sbi->blkcnt_inode = le32_to_cpu(raw_super->block_count_inodes);
	sbi->blkcnt_data = le32_to_cpu(raw_super->block_count_data);
	sbi->total_blkcnt = le64_to_cpu(raw_super->block_count);
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
	printk(KERN_ERR "jy: iget0 %lx\n", sfs_inotoba(ino));
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

static int sfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct sfs_super_block *raw_super;
	struct sfs_sb_info *sbi;
	struct inode *root;
	int valid_super_block;
	int ret = -EIO;

	raw_super = NULL;

	sbi = kzalloc(sizeof(struct sfs_sb_info), GFP_KERNEL);
	if (!sbi)
		goto failed_nomem;
	sb->s_fs_info = sbi;
	sbi->sb = sb;

	if (!sb_set_blocksize(sb, SFS_BLKSIZE)) {
		sfs_msg(KERN_ERR, "sfs_fill_super", "Failed to set blocksize");
		goto free_sbi;
	}

	printk(KERN_ERR "jy: fill_super0\n");
	ret = sfs_read_raw_super(sbi, &raw_super, &valid_super_block);
	if (ret) {
		sfs_msg(KERN_ERR, "sfs_fill_super", "Failed to read superblock");
		goto free_sbi;
	}

	if (le32_to_cpu(raw_super->magic) != SFS_SUPER_MAGIC) {
		sfs_msg(KERN_ERR, "sfs_fill_super", "Failed to get magic");
		goto free_raw_super;
	}

	sbi->raw_super = raw_super;
	sb->s_op = &sfs_sops;
	sb->s_magic = le64_to_cpu(raw_super->magic);	

	sfs_init_sb_info(sbi, raw_super);

	//flag operation

	printk(KERN_ERR "jy: fill_super1\n");
	root = sfs_iget(sb, SFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto free_raw_super;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto free_raw_super;
	}

	return 0;

free_raw_super:
	kvfree(raw_super);

free_sbi:
	kvfree(sbi);
	sb->s_fs_info = NULL;
	return ret;

failed_nomem:
	printk(KERN_ERR "jy: fill_super4\n");
	sfs_msg(KERN_ERR, "sfs_fill_super", "ENOMEM");
	return -ENOMEM;
}

static struct dentry *sfs_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, sfs_fill_super);
}

static struct file_system_type sfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "sfs",
	.mount		= sfs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_sfs_fs(void)
{
	int err;

	printk(KERN_ERR "jy: init_sfs_fs\n");
	err = init_inode_cache();
	if (err)
		goto out1;
	err = register_filesystem(&sfs_fs_type);
	if (err)
		goto out2;
	
	return 0;

out2:
	printk(KERN_ERR "jy: init_sfs_fs0\n");
	destroy_inode_cache();

out1:
	printk(KERN_ERR "jy: init_sfs_fs1\n");
	return err;
}

static void __exit exit_sfs_fs(void)
{
	printk(KERN_ERR "jy: exit_sfs_fs\n");
	unregister_filesystem(&sfs_fs_type);
	destroy_inode_cache();
}

module_init(init_sfs_fs)
module_exit(exit_sfs_fs)

MODULE_AUTHOR("Jeyeon Lee at DanKook Univ");
MODULE_DESCRIPTION("Very Simple File System");
MODULE_LICENSE("GPL");
