#include <linux/fs.h>

#include "sfs_fs.h"
#include "sfs.h"

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

static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	printk(KERN_ERR "jy: lookup:%s(%d)\n", dentry->d_name.name, dentry->d_name.len);
	if (dentry->d_name.len > SFS_MAXNAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	printk(KERN_ERR "jy: lookup0");
	ino = sfs_inode_by_name(dir, &dentry->d_name);
	if (ino)
		inode = sfs_iget(dir->i_sb, ino);
	printk(KERN_ERR "jy: lookup2");
	return d_splice_alias(inode, dentry);
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

static int sfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct sfs_dir_entry *de;
	struct page *page;
	int err = -ENOENT;

	printk(KERN_ERR "jy: unlink %ld, %s\n", dir->i_ino, dentry->d_name.name);
	de = sfs_find_entry(dir, &dentry->d_name, &page);
	if (!de)
		goto out_unlink;
	
	printk(KERN_ERR "jy: unlink0 %d\n", de->inode);
	err = sfs_delete_entry(dir, de, page);
	if (err)
		goto out_unlink;
	
	printk(KERN_ERR "jy: unlink1\n");
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	err = 0;

out_unlink:
	printk(KERN_ERR "jy: unlink2\n");
	return err;
}

static int sfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err = -ENOTEMPTY;

	printk(KERN_ERR "jy: rmdir %s\n", dentry->d_name.name);
	if (sfs_empty_dir(inode)) {
		printk(KERN_ERR "jy: rmdir0\n");
		err = sfs_unlink(dir, dentry);
		if (!err) {
			printk(KERN_ERR "jy: rmdir1\n");
			inode->i_size = 0;
			inode_dec_link_count(inode);
			inode_dec_link_count(dir);
		}
	}
	printk(KERN_ERR "jy: rmdir2\n");
	return err;
}

const struct inode_operations sfs_dir_inode_operations = {
	.lookup         = sfs_lookup,
	.create		= sfs_create,
	.mkdir          = sfs_mkdir,
	.rmdir          = sfs_rmdir,
	.unlink         = sfs_unlink,
/*
	.rename         = sfs_rename,
	.link           = sfs_link,
	.symlink        = sfs_symlink,
	.mknod          = sfs_mknod,
*/	
};


