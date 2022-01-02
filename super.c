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


void sfs_msg(const char *level, const char *funtion, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sSFS (%s): %pV\n", level, funtion, &vaf);
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
	printk(KERN_ERR "jy: free_inode\n");
	kmem_cache_free(sfs_inode_cachep, SFS_I(inode));
}

static void init_once(void *foo)
{
	struct sfs_inode_info *si = (struct sfs_inode_info *) foo;

	inode_init_once(&si->vfs_inode);
}

static int __init init_inode_cache(void)
{
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
				goto fail;
		}
		printk(KERN_ERR "jy: get_page1\n");
	}
	return page;

fail:
	printk(KERN_ERR "jy: get_page2\n");
	sfs_put_page(page);
	return ERR_PTR(-EIO);
}

static int sfs_block_to_path(sector_t iblock, unsigned int offsets[4])
{
	int ptrs = DEF_ADDRS_PER_BLOCK;
	int ptrs_bits = DEF_APB_SHIFT;
	const long direct_blocks = DEF_ADDRS_PER_INODE,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	int n = 0;

	if (iblock < direct_blocks) {
		offsets[n++] = iblock;
	} else if ((iblock -= direct_blocks) < indirect_blocks) {
		offsets[n++] = SFS_IND_BLOCK;
		offsets[n++] = iblock;
	} else if ((iblock -= indirect_blocks) < double_blocks) {
		offsets[n++] = SFS_DIND_BLOCK;
		offsets[n++] = iblock >> ptrs_bits;
		offsets[n++] = iblock & (ptrs - 1);
	} else if (((iblock -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = SFS_TIND_BLOCK;
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

static unsigned int sfs_map(struct inode *inode, unsigned int offsets[4], int depth)
{
	struct sfs_inode_info *si = SFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct sfs_sb_info *sbi = SFS_SB(sb);
	struct buffer_head *bh;
	unsigned int ret, *p;
	Indirect chain[4], *q = chain;

	if (depth == 0)
		goto no_block;

	p = offsets;

	ret = si->i_data[*p++];

	while (--depth) {
		bh = sb_bread(sb, sbi->data_blkaddr + *p++);
		if (!bh)
			goto no_block;
		
	}

no_block:
	return le32_to_cpu(ret);
}

static int sfs_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned int offsets[4];
	unsigned int blkno;
	int depth;
	
	depth = sfs_block_to_path(iblock, offsets);
	if (depth == 0)
		return -EIO;

	blkno = sfs_map(inode, offsets, depth);
	printk(KERN_ERR "jy: get_block %lld, %d\n", iblock, create);

	if (!create)
		goto done;
	

		
done:
	map_bh(bh_result, sb, 0x80a);
//	map_bh(bh_result, sb, blkno);

	return 0;
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

#if 0
int sfs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
		return __block_write_begin(page, pos, len, sfs_get_block);
}
#endif

static void sfs_truncate_blocks(struct inode * inode) {

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
	if (unlikely(ret))
		sfs_write_failed(mapping, pos + len);
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
	struct super_block *sb = dir->i_sb;
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
		goto out;

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
					goto out;
				}
				printk(KERN_ERR "jy: find_entry4: %d %s\n", de->inode, de->name);
				if (sfs_match(namelen, name, de))
					goto found;
				de = sfs_next_entry(de);
			}
			sfs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);
	printk(KERN_ERR "jy: find_entry5\n");

out:
	return NULL;

found:
	printk(KERN_ERR "jy: find_entry6\n");
	*res_page = page;
	si->i_dir_start_lookup = n;
	return de;
}


void sbh_sync_block(struct sfs_buffer_head *sbh)
{
	unsigned i;

	if (sbh) {
		for (i = 0; i < sbh->count; i++)
			write_dirty_buffer(sbh->bh[i], 0);

		for (i = 0; i < sbh->count; i++)
			wait_on_buffer(sbh->bh[i]);
	}
}

struct sfs_bmap_info *sfs_load_imap(struct super_block *sb)
{
	struct sfs_sb_info *sbi = SFS_SB(sb);
	struct sfs_bmap_info *im_i = SFS_IM_I(sbi);
	struct sfs_buffer_head *sbh = &im_i->b_sbh;
	unsigned int i, j, count;

	if (!sbh->count)
		goto load;


load:
	count = min(sbi->blkcnt_imap, SBH_MAX_BH);
	sbh->count = count;

	for (i = 0; i < count; i++) {
		
	}

	
}

struct inode *sfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb;
	struct sfs_sb_info *sbi;
	struct inode *inode;
	struct sfs_inode_info *si;
	struct sfs_inode *sfs_inode;
	struct sfs_bmap_info *im_i;
	struct buffer_head *bh;
	struct timespec64 ts;
	unsigned cg, bit, i, j, start;
	int err = -ENOSPC;

	if (!dir || !dir->i_nlink)
		return ERR_PTR(-EPERM);
	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	si = SFS_I(inode);
	sbi = SFS_SB(sb);

	
	//jy

	im_i = sfs_load_imap(sb);

	if (S_ISDIR(mode)) {
		
	}

/* jy
	if (sb->s_flags & SB_SYNCHRONOU)
		sbh_sync_block();
*/
//jy	
//	sfs_mark_sb_dirty(sb);

	inode->i_ino = bit;
	inode_init_owner(inode, dir, mode);
	inode->i_blocks = 0;
	inode->i_generation = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	si->i_flags = SFS_I(dir)->i_flags;
	si->i_dir_start_lookup = 0;
	memset(&si->i_data, 0, sizeof(si->i_data));
	if (insert_inode_locked(inode) < 0) {
		err = -EIO;
		goto failed;
	}
	mark_inode_dirty(inode);

	bh = sb_bread(sb, sfs_inotoba(inode->i_ino));
	if (!bh) {
		sfs_msg(KERN_ERR, "sfs_new_inode", "Failed to read inode %lu\n", inode->i_ino);
		err = -EIO;
		goto fail_remove_inode;
	}
	sfs_inode = (struct sfs_inode *)bh->b_data;
	ktime_get_real_ts64(&ts);
	sfs_inode->i_ctime = cpu_to_le64(ts.tv_sec);
	sfs_inode->i_ctime_nsec = cpu_to_le32(ts.tv_nsec);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
// jy	
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

int sfs_add_link(struct dentry *dentry, struct inode *inode)
{
	//jy
	return 0;
}

static inline int sfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = sfs_add_link(dentry, inode);
	if (!err) {
		d_instantiate_new(dentry, inode);
		return 0;
	}
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return err;
}

static int sfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;

	inode = sfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &sfs_file_inode_operations;
	inode->i_fop = &sfs_file_operations;
	inode->i_mapping->a_ops = &sfs_aops;
	mark_inode_dirty(inode);
	return sfs_add_nondir(dentry, inode);
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

struct inode_operations sfs_dir_inode_operations = {
/*
	.create		= sfs_create,
*/	
	.lookup         = sfs_lookup,
/*
	.link           = sfs_link,
	.unlink         = sfs_unlink,
	.symlink        = sfs_symlink,
	.mkdir          = sfs_mkdir,
	.rmdir          = sfs_rmdir,
	.mknod          = sfs_mknod,
	.rename         = sfs_rename,
	.getattr        = sfs_getattr,
*/	
	.setattr        = sfs_setattr,
/*
	.get_acl        = sfs_get_acl,
	.set_acl        = sfs_set_acl,
	.tmpfile        = sfs_tmpfile,
*/	
};











static int sfs_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
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

struct file_operations sfs_dir_operations = {
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
	struct super_block *sb = inode->i_sb;
	struct sfs_inode_info *si = SFS_I(inode);

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

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		sfs_inode->i_daddr[0] = si->i_data[0];
	} else if (inode->i_blocks) {
		memcpy(&sfs_inode->i_daddr, si->i_data, sizeof(sfs_inode->i_daddr));
	}

	if (!inode->i_nlink)
		memset(sfs_inode, 0, sizeof(struct sfs_inode));
}

static sfs_update_inode(struct inode *inode, int do_sync) {
	struct super_block *sb = inode->i_sb;
	struct sfs_sb_info *sbi = SFS_SB(sb);
	struct sfs_inode *sfs_inode;
	struct buffer_head *bh;

	bh = sb_bread(sb, sfs_inotoba(inode->i_ino));

	sfs_inode = (struct sfs_inode *)bh->b_data;

	sfs_fill_inode(inode, sfs_inode);

	mark_buffer_dirty(bh);
	if (do_sync)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int sfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	printk(KERN_ERR "jy: write_inode\n");
	return sfs_update_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

static const struct super_operations sfs_sops = {
	.alloc_inode    = sfs_alloc_inode,
	.free_inode     = sfs_free_inode,
	.write_inode    = sfs_write_inode,
/*
	.evict_inode    = sfs_evict_inode,
	.put_super      = sfs_put_super,
	.sync_fs        = sfs_sync_fs,
	.freeze_fs      = sfs_freeze,
	.unfreeze_fs    = sfs_unfreeze,
	.statfs         = sfs_statfs,
	.remount_fs     = sfs_remount,
	.show_options   = sfs_show_options,
*/	
};

static int read_raw_super_block(struct sfs_sb_info *sbi,
			struct sfs_super_block **raw_super,
			int *valid_super_block)
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

static int init_im_info(struct sfs_sb_info *sbi)
{
	struct sfs_bmap_info *im_i = SFS_IM_I(sbi);

	sbi->im_i = kzalloc(sizeof(struct sfs_buffer_head), GFP_KERNEL);
	if (!sbi->im_i)
		return -ENOMEM;

	im_i->b_sbh.count = 0;
	if (!sfs_load_imap(sbi->sb))
		goto free_im_i;

	return 0;

free_im_i:
	kvfree(sbi->im_i);
	return -1;
}

static int sfs_read_inode(struct inode *inode, struct sfs_inode *sfs_inode)
{       
	printk(KERN_ERR "jy: read_inode\n");

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

	if (S_ISREG(inode->i_mode)) {

	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &sfs_dir_inode_operations;
		inode->i_fop = &sfs_dir_operations;
	} else {

	}

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
	int err;

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
	int ret;

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
	ret = read_raw_super_block(sbi, &raw_super, &valid_super_block);
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

	sbi->imap_blkaddr = le32_to_cpu(raw_super->imap_blkaddr);
	sbi->dmap_blkaddr = le32_to_cpu(raw_super->dmap_blkaddr);
	sbi->inode_blkaddr = le32_to_cpu(raw_super->inodes_blkaddr);
	sbi->data_blkaddr = le32_to_cpu(raw_super->data_blkaddr);
	sbi->blkcnt_imap = le32_to_cpu(raw_super->block_count_imap);
	sbi->blkcnt_dmap = le32_to_cpu(raw_super->block_count_dmap);
	sbi->blkcnt_inode = le32_to_cpu(raw_super->block_count_inodes);
	sbi->blkcnt_data = le32_to_cpu(raw_super->block_count_data);

	//flag operation

	//jy init dm_i, im_i
/*
	ret = init_im_info(sbi);
	if (ret) {
		sfs_msg(sb, KERN_ERR, "Failed to init imap info");
		goto free_raw_super;
	}
*/

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

	err = init_inode_cache();
	if (err)
		goto out1;
	err = register_filesystem(&sfs_fs_type);
	if (err)
		goto out;
	
	return 0;

out:
	destroy_inode_cache();

out1:
	return err;
}

static void __exit exit_sfs_fs(void)
{
	unregister_filesystem(&sfs_fs_type);
	destroy_inode_cache();
}

module_init(init_sfs_fs)
module_exit(exit_sfs_fs)

MODULE_AUTHOR("Jeyeon Lee at DanKook Univ");
MODULE_DESCRIPTION("Very Simple File System");
MODULE_LICENSE("GPL");
