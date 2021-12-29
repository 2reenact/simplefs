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



void sfs_msg(struct super_block *sb, const char *level, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sSFS (%s): %pV\n", level, sb->s_id, &vaf);
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
	struct page *page = read_mapping_page(mapping, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
		if (unlikely(!PageChecked(page))) {
			if (PageError(page))// || !sfs_check_page(page))
				goto fail;
		}
	}
	return page;

fail:
	sfs_put_page(page);
	return ERR_PTR(-EIO);
}

static int sfs_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;

	unsigned max_blocks;
	int err;

	printk(KERN_ERR "jy: get_block %lld\n", iblock);	

	return 0;
}

static int sfs_writepage(struct page *page, struct writeback_control *wbc)
{
	printk(KERN_ERR "jy: writepage \n");
	return block_write_full_page(page, sfs_get_block, wbc);
}

static int sfs_readpage(struct file *file, struct page *page)
{
	printk(KERN_ERR "jy: readpage \n");
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

static sector_t ufs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block, sfs_get_block);
}

const struct address_space_operations sfs_aops = {
	.readpage	= sfs_readpage,
	.writepage	= sfs_writepage,
	.write_begin	= sfs_write_begin,
	.write_end	= sfs_write_end,
/*
	.bmap		= sfs_bmap,
*/
};







static unsigned sfs_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static inline int sfs_match(struct super_block *sb, const unsigned char *name, struct sfs_dir_entry *de)
{
	if (!de->i_no)
		return 0;
	return !memcmp(name, de->filename, SFS_NAME_LEN);
}

struct sfs_dir_entry *sfs_find_entry(struct inode *dir, const struct qstr *qstr, struct page **res_page)
{
	struct super_block *sb = dir->i_sb;
	const unsigned char *name = qstr->name;
	int namelen = qstr->len;
	unsigned reclen = SFS_REC_LEN;
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct sfs_inode_info *si = SFS_I(dir);
	struct sfs_dir_entry *de;

	printk(KERN_ERR "jy: find_entry %s\n", name);
	*res_page = NULL;

	start = si->i_dir_start_lookup;

	if (start >= npages)
		start = 0;

	n = start;
	printk(KERN_ERR "jy: find_entry0\n");
	do {
		char *kaddr;
		printk(KERN_ERR "jy: find_entry1: %ld\n", n);
		page = sfs_get_page(dir, n);
		if (!IS_ERR(page)) {
			kaddr = page_address(page);
			de = (struct sfs_dir_entry *) kaddr;
			printk(KERN_ERR "jy: find_entry2 %p\n", kaddr);
			kaddr += sfs_last_byte(dir, n) - reclen;
			printk(KERN_ERR "jy: find_entry3 %p\n", kaddr);
			while ((char *) de <= kaddr) {
				printk(KERN_ERR "jy: find_entry4\n");
				if (sfs_match(sb, name, de))
					goto found;
				de = de + reclen;
			}
			sfs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);
	printk(KERN_ERR "jy: find_entry5\n");

	return NULL;

found:
	printk(KERN_ERR "jy: find_entry6\n");
	*res_page = page;
	si->i_dir_start_lookup = n;
	return de;
}

ino_t sfs_inode_by_name(struct inode *dir, const struct qstr *qstr)
{
	ino_t ret = 0;
	struct sfs_dir_entry *de;
	struct page *page;

	printk(KERN_ERR "jy: inode_by_name\n");
	de = sfs_find_entry(dir, qstr, &page);
	if (de) {
		ret = le32_to_cpu(de->i_no);
		sfs_put_page(page);
	}
	return ret;
}

static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	printk(KERN_ERR "jy: lookup\n");
	if (dentry->d_name.len > SFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = sfs_inode_by_name(dir, &dentry->d_name);
	if (ino)
		inode = sfs_iget(dir->i_sb, ino);
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

	printk(KERN_ERR "jy: readdir\n");
	if (pos > inode->i_size - SFS_REC_LEN)
		return 0;

	for ( ; n < npages; n++, offset = 0) {
		char *kaddr, *limit;
		struct sfs_dir_entry *de;

		struct page *page = sfs_get_page(inode, n);

		printk(KERN_ERR "jy: readdir 0\n");
		if (IS_ERR(page)) {
			sfs_msg(sb, KERN_ERR, "bad page in %lu", inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return -EIO;
		}
		printk(KERN_ERR "jy: readdir 1\n");
		kaddr = page_address(page);
		if (need_revalidate) {
			if (offset) {


			}
		}
		de = (struct sfs_dir_entry *)(kaddr + offset);
		limit = kaddr + sfs_last_byte(inode, n) - SFS_REC_LEN;
		for ( ; (char *)de <= limit; de += SFS_REC_LEN) {
			printk(KERN_ERR "jy: readdir 2: %d-%s\n", de->i_no, de->filename);
			if (de->i_no) {
				unsigned char d_type = DT_UNKNOWN;

				d_type = fs_ftype_to_dtype(de->file_type);
				printk(KERN_ERR "jy: readdir 3\n");
				if (!dir_emit(ctx, de->filename, SFS_NAME_LEN,
						le32_to_cpu(de->i_no),
						d_type)) {
					sfs_put_page(page);
					return 0;
				}
			}
			ctx->pos += SFS_REC_LEN;
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
		sfs_inode->i_dblock[0] = si->i_data[0];
	} else if (inode->i_blocks) {
		memcpy(&sfs_inode->i_dblock, si->i_data, sizeof(sfs_inode->i_dblock));
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
			sfs_msg(sb, KERN_ERR, "Failed to read %th superblock", block + 1);
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
	printk(KERN_ERR "jy: iget0 %ld\n", sfs_inotoba(ino));
	if (!bh) {
		sfs_msg(sb, "Failed to read inode %d\n", ino);
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
	int i;

	printk(KERN_ERR "jy: fill_super\n");
	raw_super = NULL;

	sbi = kzalloc(sizeof(struct sfs_sb_info), GFP_KERNEL);
	if (!sbi)
		goto failed_nomem;

	sb->s_fs_info = sbi;
	sbi->sb = sb;

	if (!sb_set_blocksize(sb, SFS_BLKSIZE)) {
		sfs_msg(sb, KERN_ERR, "Failed to set blocksize");
		goto free_sbi;
	}

	printk(KERN_ERR "jy: fill_super0\n");
	ret = read_raw_super_block(sbi, &raw_super, &valid_super_block);
	if (ret) {
		sfs_msg(sb, KERN_ERR, "Failed to read superblock");
		goto free_sbi;
	}

	if (le32_to_cpu(raw_super->magic) != SFS_SUPER_MAGIC) {
		sfs_msg(sb, KERN_ERR, "Failed to get magic");
		goto free_raw_super;
	}

	sbi->raw_super = raw_super;
	sb->s_op = &sfs_sops;
	sb->s_magic = le64_to_cpu(raw_super->magic);	

	sbi->inode_blkaddr = le32_to_cpu(raw_super->inodes_blkaddr);

	printk(KERN_ERR "jy: fill_super1\n");
	root = sfs_iget(sb, SFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto free_raw_super;
	}
	printk(KERN_ERR "jy: fill_super2\n");
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto free_raw_super;
	}
	printk(KERN_ERR "jy: fill_super3\n");

	return 0;

free_raw_super:
	kvfree(raw_super);

free_sbi:
	kvfree(sbi);
	sb->s_fs_info = NULL;
	return ret;

failed_nomem:
	printk(KERN_ERR "jy: fill_super4\n");
	sfs_msg(sb, KERN_ERR, "ENOMEM");
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
