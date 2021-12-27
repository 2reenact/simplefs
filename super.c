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
	unsigned max_blocks;

	return 0;
}

static int sfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, sfs_get_block, wbc);
}

static int sfs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, sfs_get_block);
}

#if 0
int sfs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
		return __block_write_begin(page, pos, len, sfs_get_block);
}
#endif

static void sfs_truncate_blocks(struct inode *);

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

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len)
		sfs_write_failed(mapping, pos + len);
	return ret;
}

const struct address_space_operations sfs_aops = {
	.readpage	= sfs_readpage,
/*
	.writepage	= sfs_writepage,
	.write_begin	= sfs_write_begin,
	.write_end	= sfs_write_end,
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
	unsigned reclen = sizeof(struct sfs_dir_entry);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct sfs_inode_info *si = SFS_I(dir);
	struct sfs_dir_entry *de;

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
			de = (struct sfs_dir_entry *) kaddr;
			kaddr += sfs_last_byte(dir, n) - reclen;
			while ((char *) de <= kaddr) {
				if (sfs_match(sb, name, de))
					goto found;
				de = de + reclen;
			}
			sfs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);

	return NULL;

found:
	*res_page = page;
	si->i_dir_start_lookup = n;
	return de;
}

ino_t sfs_inode_by_name(struct inode *dir, const struct qstr *qstr)
{
	ino_t ret = 0;
	struct sfs_dir_entry *de;
	struct page *page;

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

	printk(KERN_ERR "sfs: lookup\n");
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
//	struct ext2_inode_info ei = SFS_I(inode);

	generic_fillattr(inode, stat);

	return 0;
}

int sfs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(dentry, iattr);
	if (error)
		return error;

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
*/	
	.getattr        = sfs_getattr,
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



}

struct file_operations sfs_dir_operations = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
	.fsync          = generic_file_fsync,
	.iterate_shared	= sfs_readdir,
/*
	.unlocked_ioctl = sfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = sfs_compat_ioctl,
#endif
*/	
};














const struct inode_operations sfs_file_inode_operations = {
		.setattr = sfs_setattr,
};

const struct file_operations sfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
/*
	.unlocked_ioctl = sfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sfs_compat_ioctl,
#endif
*/
	.mmap		= generic_file_mmap,
	.open		= generic_file_open,
	.fsync		= generic_file_fsync,
/*
	.release	= sfs_release_file,
	.get_unmapped_area = thp_get_unmapped_area,
*/	
	.splice_read	= generic_file_splice_read,
/*
	.splice_write	= iter_file_splice_write,
*/	
};













static struct inode *sfs_alloc_inode(struct super_block *sb)
{
	struct sfs_inode_info *si;
	si = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
	if (!si)
		return NULL;
	inode_set_iversion(&si->vfs_inode, 1);

	return &si->vfs_inode;
}

static const struct super_operations sfs_sops = {
	.alloc_inode    = sfs_alloc_inode,
/*
	.free_inode     = sfs_free_inode,
	.write_inode    = sfs_write_inode,
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

static int sfs_get_inode(struct inode *inode, ino_t ino,
		struct buffer_head **p)
{       
	struct super_block *sb = inode->i_sb;
	struct sfs_inode *raw_inode;
	struct buffer_head * bh;
	unsigned long block;
	unsigned long offset;
	uid_t i_uid;
	gid_t i_gid;

/*
	 *p = NULL;
	 if ((ino != SFS_ROOT_INO && ino < SFS_FIRST_INO(sb)) ||
	 ino > le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count))
	 goto Einval;
*/

	offset = (ino - SFS_ROOT_INO);
	block = SFS_GET_SB(sb, inodes_blkaddr) + offset;
	if (!(bh = sb_bread(sb, block)))
		goto Eio;

	*p = bh;
	raw_inode = (struct sfs_inode *)bh->b_data;
/*
Einval:
	sfs_error(sb, "sfs_get_inode", "bad inode number: %lu",
			(unsigned long) ino);
	return ERR_PTR(-EINVAL);
*/
	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	i_uid = (uid_t)le16_to_cpu(raw_inode->i_uid);
	i_gid = (gid_t)le16_to_cpu(raw_inode->i_gid);
	i_uid_write(inode, i_uid);
	i_gid_write(inode, i_gid);
	set_nlink(inode, le16_to_cpu(raw_inode->i_links));
	inode->i_size = le32_to_cpu(raw_inode->i_size);
	inode->i_atime.tv_sec = (signed)le32_to_cpu(raw_inode->i_atime);
	inode->i_ctime.tv_sec = (signed)le32_to_cpu(raw_inode->i_ctime);
	inode->i_mtime.tv_sec = (signed)le32_to_cpu(raw_inode->i_mtime);
	inode->i_atime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);

	if (S_ISREG(inode->i_mode)) {

	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &sfs_dir_inode_operations;
		inode->i_fop = &sfs_dir_operations;
	} else {

	}

	return 0;

Eio:
	sfs_msg(sb, KERN_ERR, "sfs_get_inode", "unable to read inode block - inode=%lu, block=%lu",
				(unsigned long) ino, block);
	return -EIO;

/*
Egdp:
	return ERR_PTR(-EIO);
*/
}

static void sfs_set_inode_ops(struct inode *inode)
{
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
	struct buffer_head *bh;
	struct inode *inode;
	int err;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	inode->i_sb = sb;

	err = sfs_get_inode(inode, ino, &bh);
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
		return err;
	err = register_filesystem(&sfs_fs_type);
	if (err)
		goto fail;
	
	return 0;

fail:
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
