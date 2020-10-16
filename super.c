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
	rcu_barrier();
	kmem_cache_destroy(sfs_inode_cachep);
}














struct inode *sfs_iget(struct super_block *sb, unsigned long ino);

struct inode_operations sfs_dir_inode_operations = {
/*
	.create		= sfs_create,
	.lookup         = sfs_lookup,
*/	
/*
	.link           = sfs_link,
	.unlink         = sfs_unlink,
	.symlink        = sfs_symlink,
	.mkdir          = sfs_mkdir,
	.rmdir          = sfs_rmdir,
	.mknod          = sfs_mknod,
	.rename         = sfs_rename,
*/	
/*
	.getattr        = sfs_getattr,
	.setattr        = sfs_setattr,
	.get_acl        = sfs_get_acl,
	.set_acl        = sfs_set_acl,
	.tmpfile        = sfs_tmpfile,
*/	
};
















struct file_operations sfs_dir_operations = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
/*
	.iterate_shared = sfs_readdir,
	.unlocked_ioctl = sfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = sfs_compat_ioctl,
#endif
	.fsync          = sfs_fsync,
*/	
};




















const struct address_space_operations sfs_aops = {
/*
	.readpage       = ext2_readpage, 
	.readpages      = ext2_readpages,
	.writepage      = ext2_writepage,
	.write_begin    = ext2_write_begin
	.write_end      = ext2_write_end, 
	.bmap           = ext2_bmap,
	.direct_IO      = ext2_direct_IO, 
	.writepages     = ext2_writepages,
	.migratepage    = buffer_migrate_page,
	.is_partially_uptodate  = block_is_partially_uptodate,
*/
	.error_remove_page  = generic_error_remove_page,
};

struct inode *sfs_iget(struct super_block *sb, unsigned long ino)
{
	struct sfs_inode_info *si;
	struct buffer_head *bh;
	struct sfs_inode *raw_inode;
	struct inode *inode;
	int n;
	uid_t i_uid;
	gid_t i_gid;

	unsigned long block;
	unsigned long offset;

	inode = iget_locked(sb, ino);		
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;
		
	si = SFS_I(inode);

//sfs_get_inode {
	offset = (ino - SFS_ROOT_INO);
	block = SFS_GET_SB(sb, inodes_blkaddr) + offset;
	if (!(bh = sb_bread(sb, block))) {
		sfs_msg(sb, KERN_ERR, "unable to read inode inode");
		return ERR_PTR(-EIO);	
	}
	raw_inode = (struct sfs_inode *)bh->b_data;
// }

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
	si->i_flags = le32_to_cpu(raw_inode->i_flags);

	si->i_dir_start_lookup = 0;

	for (n = 0; n < DEF_ADDRS_PER_INODE; n++)
		si->i_data[n] = raw_inode->d_addr[n];
	for (; n < DEF_SFS_N_BLOCKS; n++)
		si->i_data[n] = raw_inode->i_addr[n - DEF_ADDRS_PER_BLOCK];

	if (S_ISREG(inode->i_mode)) {

	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &sfs_dir_inode_operations;
		inode->i_fop = &sfs_dir_operations;
		inode->i_mapping->a_ops = &sfs_aops;
	} else {

	}	
	brelse(bh);
	return inode;
}



















const struct file_operations sfs_file_operations = {
	.llseek		= generic_file_llseek,
/*
	.read_iter	= sfs_file_read_iter,
	.write_iter	= sfs_file_write_iter,
	.unlocked_ioctl = sfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= sfs_compat_ioctl,
#endif
*/
	.mmap		= generic_file_mmap,
	.open		= dquot_file_open,
/*
	.release	= sfs_release_file,
	.fsync		= sfs_fsync,
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

static int sfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh;
	struct sfs_sb_info *sbi;
	struct sfs_super_block *raw_super;
	struct inode *root;
	ino_t ino = SFS_ROOT_INO;
	unsigned long block;

	sbi = kzalloc(sizeof(struct sfs_sb_info), GFP_KERNEL);
	if (!sbi) {
		sfs_msg(sb, KERN_ERR, "unable to alloc sbi");
		goto free_sbi;
	}

	//sbi->sb = sb;

	if (unlikely(!sb_set_blocksize(sb, SFS_BLKSIZE))) {
		sfs_msg(sb, KERN_ERR, "unable to set blocksize");
		goto free_sbi;
	}

// read_raw_super_block {
	raw_super = kzalloc(sizeof(struct sfs_super_block), GFP_KERNEL);
	if (!raw_super) {
		sfs_msg(sb, KERN_ERR, "unable to alloc super");
		goto free_raw_super;
	}

	block = 0;
	if (!(bh = sb_bread(sb, block))) {
		sfs_msg(sb, KERN_ERR, "unable to read superblock");
		goto brelse_bh;
	}

	memcpy(raw_super, bh->b_data + SFS_SUPER_OFFSET, sizeof(*raw_super));
// }

	sb->s_fs_info = sbi;
	sbi->raw_super = raw_super;
	sb->s_magic = le64_to_cpu(raw_super->magic);	

	if (sb->s_magic != SFS_SUPER_MAGIC) {
		sfs_msg(sb, KERN_ERR, "unable to get magic");
		goto failed;
	}

	sb->s_op = &sfs_sops;

	root = sfs_iget(sb, ino);

	sb->s_root = d_make_root(root);

	brelse(bh);
	
	return 0;

failed:


brelse_bh:
	brelse(bh);

free_raw_super:
	kvfree(raw_super);

free_sbi:
	kvfree(sbi);

	return -1;
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

MODULE_AUTHOR("Lee Jeyeon at DanKook Univ");
MODULE_DESCRIPTION("Simple File System");
MODULE_LICENSE("GPL");
