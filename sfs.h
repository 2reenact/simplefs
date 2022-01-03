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

struct sfs_bmap_info;

/*
 * sfs super-block data in memory
 */
struct sfs_sb_info {
	struct super_block *sb;				/* pointer to VFS super block */
	struct sfs_super_block *raw_super;		/* raw super block pointer */
	struct sfs_bmap_info *dm_i;
	struct sfs_bmap_info *im_i;

        unsigned int imap_blkaddr;
        unsigned int dmap_blkaddr;
        unsigned int data_blkaddr;
	unsigned int inode_blkaddr;
        unsigned int blkcnt_imap;
        unsigned int blkcnt_dmap;
        unsigned int blkcnt_inode;
        unsigned int blkcnt_data;
};

#define SFS_GET_SB(s, i)		(SFS_SB(s)->raw_super->i)

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

static inline struct sfs_bmap_info *SFS_DM_I(struct sfs_sb_info *sbi)
{
	return sbi->dm_i;
}

static inline struct sfs_bmap_info *SFS_IM_I(struct sfs_sb_info *sbi)
{
	return sbi->im_i;
}

#define sfs_get(x)			(sbi->x)
#define sfs_inotoba(x)			(((struct sfs_sb_info *)(sb->s_fs_info))->inode_blkaddr + x - SFS_ROOT_INO)

#define SBH_MAX_BH		32

struct sfs_buffer_head {
	unsigned int count;

	struct buffer_head *bh[SBH_MAX_BH];
};

struct sfs_bmap_info {
	struct sfs_buffer_head b_sbh;

	unsigned int b_start_lookup;
};




#endif /* _SFS_H */
