/*
 * super.c
 *
 * 2021 Lee JeYeon., Dankook Univ.
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

#include "vsfs.h"

void vsfs_msg(const char *level, const char *funtion, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sVSFS(%s): %pV\n", level, funtion, &vaf);
	va_end(args);
}

static const struct super_operations vsfs_sops;

static void vsfs_put_super(struct super_block *sb)
{
	struct vsfs_sb_info *sbi = VSFS_SB(sb);

	kvfree(sbi->raw_super);
	kvfree(sbi);

	return;
}

static int vsfs_read_raw_super(struct vsfs_sb_info *sbi, struct vsfs_super_block **raw_super, int *valid_super_block)
{
	struct super_block *sb = sbi->sb;
	struct vsfs_super_block *super;
	struct buffer_head *bh;
	int block;
	int err = 0;

	super = kzalloc(sizeof(struct vsfs_super_block), GFP_KERNEL);
	if (!super)
		return -ENOMEM;
	
	for (block = 0; block < 2; block++) {
		bh = sb_bread(sb, block);
		if (!bh) {
			vsfs_msg(KERN_ERR, "read_raw_super_block", "Failed to read %th superblock", block + 1);
			err = -EIO;
			continue;
		}

		if (!*raw_super) {
			memcpy(super, bh->b_data + VSFS_SUPER_OFFSET, sizeof(*super));
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

static void vsfs_init_sb_info(struct vsfs_sb_info *sbi, struct vsfs_super_block *raw_super)
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

static int vsfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct vsfs_super_block *raw_super;
	struct vsfs_sb_info *sbi;
	struct inode *root;
	int valid_super_block;
	int ret = -EIO;

	raw_super = NULL;

	sbi = kzalloc(sizeof(struct vsfs_sb_info), GFP_KERNEL);
	if (!sbi)
		goto failed_nomem;
	sb->s_fs_info = sbi;
	sbi->sb = sb;

	if (!sb_set_blocksize(sb, VSFS_BLKSIZE)) {
		vsfs_msg(KERN_ERR, "vsfs_fill_super", "Failed to set blocksize");
		goto free_sbi;
	}

	ret = vsfs_read_raw_super(sbi, &raw_super, &valid_super_block);
	if (ret) {
		vsfs_msg(KERN_ERR, "vsfs_fill_super", "Failed to read superblock");
		goto free_sbi;
	}

	if (le32_to_cpu(raw_super->magic) != VSFS_SUPER_MAGIC) {
		vsfs_msg(KERN_ERR, "vsfs_fill_super", "Failed to get magic");
		goto free_raw_super;
	}

	sbi->raw_super = raw_super;
	sb->s_op = &vsfs_sops;
	sb->s_magic = le64_to_cpu(raw_super->magic);	

	vsfs_init_sb_info(sbi, raw_super);

	//flag operation

	root = vsfs_iget(sb, VSFS_ROOT_INO);
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
	vsfs_msg(KERN_ERR, "vsfs_fill_super", "ENOMEM");
	return -ENOMEM;
}

static struct kmem_cache *vsfs_inode_cachep;

static struct inode *vsfs_alloc_inode(struct super_block *sb)
{
	struct vsfs_inode_info *vsi;

	vsi = kmem_cache_alloc(vsfs_inode_cachep, GFP_KERNEL);
	if (!vsi)
		return NULL;

	inode_set_iversion(&vsi->vfs_inode, 1);

	return &vsi->vfs_inode;
}

static void vsfs_free_inode(struct inode *inode)
{
	kmem_cache_free(vsfs_inode_cachep, VSFS_I(inode));
}

static void init_once(void *foo)
{
	struct vsfs_inode_info *vsi = (struct vsfs_inode_info *) foo;

	inode_init_once(&vsi->vfs_inode);
}

static int __init init_inode_cache(void)
{
        vsfs_inode_cachep = kmem_cache_create_usercopy("vsfs_inode_cache",
				sizeof(struct vsfs_inode_info), 0,
				(SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_ACCOUNT),
				offsetof(struct vsfs_inode_info, i_data),
				sizeof_field(struct vsfs_inode_info, i_data),
				init_once);
	if (vsfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inode_cache(void)
{
	rcu_barrier();
	kmem_cache_destroy(vsfs_inode_cachep);
}

static const struct super_operations vsfs_sops = {
	.alloc_inode    = vsfs_alloc_inode,
	.free_inode     = vsfs_free_inode,
	.write_inode    = vsfs_write_inode,
	.put_super      = vsfs_put_super,
	.evict_inode    = vsfs_evict_inode,
};

static struct dentry *vsfs_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, vsfs_fill_super);
}

static struct file_system_type vsfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "vsfs",
	.mount		= vsfs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_vsfs_fs(void)
{
	int err;

	err = init_inode_cache();
	if (err)
		goto out1;
	err = register_filesystem(&vsfs_fs_type);
	if (err)
		goto out2;
	
	return 0;

out2:
	destroy_inode_cache();

out1:
	return err;
}

static void __exit exit_vsfs_fs(void)
{
	unregister_filesystem(&vsfs_fs_type);
	destroy_inode_cache();
}

module_init(init_vsfs_fs)
module_exit(exit_vsfs_fs)

MODULE_AUTHOR("Jeyeon Lee from Dankook Univ");
MODULE_DESCRIPTION("Very Simple File System");
MODULE_LICENSE("GPL");
