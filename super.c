/*
 * sfs.c
 *
 * Copyright 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/mount.h>
#include <linux/iversion.h>
#include <linux/writeback.h>

#include "sfs.h"

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

static const struct super_operations sfs_sops;

static void sfs_put_super(struct super_block *sb)
{
	struct sfs_sb_info *sbi = SFS_SB(sb);

	printk(KERN_ERR "jy: put_super\n");
	
	kvfree(sbi->raw_super);
	kvfree(sbi);

	return;
}

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

static const struct super_operations sfs_sops = {
	.alloc_inode    = sfs_alloc_inode,
	.free_inode     = sfs_free_inode,
	.write_inode    = sfs_write_inode,
	.put_super      = sfs_put_super,
	.evict_inode    = sfs_evict_inode,
/*
	.sync_fs        = sfs_sync_fs,
	.statfs         = sfs_statfs,
	.remount_fs     = sfs_remount,
	.show_options   = sfs_show_options,
*/	
};

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
