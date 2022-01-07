/*
 * sfs.h
 *
 * Copyright 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#ifndef _SFS_H
#define _SFS_H

#include "sfs_fs.h"

struct sfs_sb_info {
	struct super_block *sb;				/* pointer to VFS super block */
	struct sfs_super_block *raw_super;		/* raw super block pointer */

	unsigned int dmap_start_lookup;
	unsigned int imap_start_lookup;

        unsigned int imap_blkaddr;
        unsigned int dmap_blkaddr;
        unsigned int data_blkaddr;
	unsigned int inode_blkaddr;
        unsigned int blkcnt_imap;
        unsigned int blkcnt_dmap;
        unsigned int blkcnt_inode;
        unsigned int blkcnt_data;
	unsigned long total_blkcnt;
};

#define SFS_GET_SB(i)			(sbi->i)
#define sfs_inotoba(x)			(((struct sfs_sb_info *)(sb->s_fs_info))->inode_blkaddr + x - SFS_ROOT_INO)
#define SFS_BITS_PER_BLK		BITS_PER_BYTE * SFS_BLKSIZE
#define sfs_max_bit(x)			(SFS_BITS_PER_BLK * (x))

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

#endif /* _SFS_H */
