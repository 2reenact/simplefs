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

/* super.c */
extern void sfs_msg(const char *, const char *, const char *, ...);

/* inode.c */
extern struct inode *sfs_iget(struct super_block *, unsigned long);
extern int sfs_write_inode(struct inode *, struct writeback_control *);
extern void sfs_evict_inode(struct inode *);
extern int sfs_prepare_chunk(struct page *, loff_t, unsigned);
extern struct inode *sfs_new_inode(struct inode *, umode_t);
extern int sfs_setattr(struct dentry *, struct iattr *);
extern const struct inode_operations sfs_file_inode_operations;
extern const struct file_operations sfs_file_operations;
extern const struct address_space_operations sfs_aops;

/* dir.c */
extern int sfs_add_link(struct dentry *, struct inode *);
extern ino_t sfs_inode_by_name(struct inode *, const struct qstr *);
extern int sfs_make_empty(struct inode *, struct inode *);
extern struct sfs_dir_entry *sfs_find_entry(struct inode *, const struct qstr *, struct page **);
extern int sfs_delete_entry(struct inode *, struct sfs_dir_entry *, struct page *);
extern int sfs_empty_dir(struct inode *);
extern const struct file_operations sfs_dir_operations;

/* namei.c */
extern const struct inode_operations sfs_dir_inode_operations;

#endif /* _SFS_H */
