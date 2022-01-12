#include <linux/fs.h>

#include "sfs_fs.h"
#include "sfs.h"

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

static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;

	if (dentry->d_name.len > SFS_MAXNAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = sfs_inode_by_name(dir, &dentry->d_name);
	if (ino)
		inode = sfs_iget(dir->i_sb, ino);
	return d_splice_alias(inode, dentry);
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

static int sfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	inode_inc_link_count(dir);

	inode = sfs_new_inode(dir, S_IFDIR|mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;
	
	inode->i_op = &sfs_dir_inode_operations;
	inode->i_fop = &sfs_dir_operations;
	inode->i_mapping->a_ops = &sfs_aops;

	inode_inc_link_count(inode);

	err = sfs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = sfs_add_link(dentry, inode);
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

static int sfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct sfs_dir_entry *de;
	struct page *page;
	int err = -ENOENT;

	de = sfs_find_entry(dir, &dentry->d_name, &page);
	if (!de)
		goto out_unlink;
	
	err = sfs_delete_entry(dir, de, page);
	if (err)
		goto out_unlink;
	
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	err = 0;

out_unlink:
	return err;
}

static int sfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err = -ENOTEMPTY;

	if (sfs_empty_dir(inode)) {
		err = sfs_unlink(dir, dentry);
		if (!err) {
			inode->i_size = 0;
			inode_dec_link_count(inode);
			inode_dec_link_count(dir);
		}
	}
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


