/*
 * namei.c
 *
 * 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#include <linux/fs.h>

#include "vsfs_fs.h"
#include "vsfs.h"

static inline int vsfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = vsfs_add_link(dentry, inode);
	if (!err) {
		d_instantiate_new(dentry, inode);
		return 0;
	}
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return err;
}

static struct dentry *vsfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	if (dentry->d_name.len > VSFS_MAXNAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = vsfs_inode_by_name(dir, &dentry->d_name);
	if (ino)
		inode = vsfs_iget(dir->i_sb, ino);
	return d_splice_alias(inode, dentry);
}

static int vsfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;

	inode = vsfs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &vsfs_file_inode_operations;
	inode->i_fop = &vsfs_file_operations;
	inode->i_mapping->a_ops = &vsfs_aops;
	mark_inode_dirty(inode);
	return vsfs_add_nondir(dentry, inode);
}

static int vsfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	inode_inc_link_count(dir);

	inode = vsfs_new_inode(dir, S_IFDIR|mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;
	
	inode->i_op = &vsfs_dir_inode_operations;
	inode->i_fop = &vsfs_dir_operations;
	inode->i_mapping->a_ops = &vsfs_aops;

	inode_inc_link_count(inode);

	err = vsfs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = vsfs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate_new(dentry, inode);
	return 0;

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);

out_dir:
	inode_dec_link_count(dir);
	return err;
}

static int vsfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct vsfs_dir_entry *de;
	struct page *page;
	int err = -ENOENT;

	de = vsfs_find_entry(dir, &dentry->d_name, &page);
	if (!de)
		goto out_unlink;
	
	err = vsfs_delete_entry(dir, de, page);
	if (err)
		goto out_unlink;
	
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	err = 0;

out_unlink:
	return err;
}

static int vsfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err = -ENOTEMPTY;

	if (vsfs_empty_dir(inode)) {
		err = vsfs_unlink(dir, dentry);
		if (!err) {
			inode->i_size = 0;
			inode_dec_link_count(inode);
			inode_dec_link_count(dir);
		}
	}
	return err;
}

const struct inode_operations vsfs_dir_inode_operations = {
	.lookup         = vsfs_lookup,
	.create		= vsfs_create,
	.mkdir          = vsfs_mkdir,
	.rmdir          = vsfs_rmdir,
	.unlink         = vsfs_unlink,
};


