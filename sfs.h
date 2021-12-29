/*
 * sfs.h
 *
 * Copyright 2020 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#ifndef _SFS_H
#define _SFS_H

#include <linux/fs.h>
#include <linux/blockgroup_lock.h>
#include <linux/percpu_counter.h>
#include <linux/rbtree.h>

#include "sfs_fs.h"

#include <linux/dcache.h>

/*
 * sfs super-block data in memory
 */
struct sfs_sb_info {
	struct super_block *sb;				/* pointer to VFS super block */
	struct sfs_super_block *raw_super;		/* raw super block pointer */

	unsigned int inode_blkaddr;

	spinlock_t s_lock;
};

/*
 * Macro-instructions used to manage several block sizes
 */
#define SFS_BLOCK_SIZE(s)		((s)->blocksize)
#define SFS_BLOCK_SIZE_BITS(s)		((s)->blocksize_bits)
#define SFS_INODE_SIZE(s)		(SFS_SB(s)->inode_size)

struct sfs_inode_info {
	__le32 i_data[15];
	__u32 i_flags;

	__u32 i_dir_start_lookup;

	struct inode vfs_inode;
};

static inline struct sfs_inode_info *SFS_I(struct inode *inode)
{
	return container_of(inode, struct sfs_inode_info, vfs_inode);
}

static inline struct sfs_sb_info *SFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

#define SFS_GET_SB(s, i)		(SFS_SB(s)->raw_super->i)

#define SFS_ROOT_INO		 2	/* Root inode */
#define sfs_inotoba(x)			(((struct sfs_sb_info *)(sb->s_fs_info))->inode_blkaddr + x - SFS_ROOT_INO)



#endif /* _SFS_H */
