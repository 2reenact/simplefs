/*
 * vsfs.h
 *
 * Copyright 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#ifndef _VSFS_H
#define _VSFS_H

#include "vsfs_fs.h"

struct vsfs_sb_info {
	struct super_block *sb;				/* pointer to VFS super block */
	struct vsfs_super_block *raw_super;		/* raw super block pointer */

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

#define VSFS_GET_SB(i)			(sbi->i)
#define vsfs_inotoba(x)			(((struct vsfs_sb_info *)(sb->s_fs_info))->inode_blkaddr + x - VSFS_ROOT_INO)
#define VSFS_BITS_PER_BLK		BITS_PER_BYTE * VSFS_BLKSIZE
#define vsfs_max_bit(x)			(VSFS_BITS_PER_BLK * (x))

struct vsfs_inode_info {
	__le32 i_data[15];
	__u32 i_flags;

	__u32 i_dir_start_lookup;

	struct inode vfs_inode;
};

static inline struct vsfs_inode_info *VSFS_I(struct inode *inode)
{
	return container_of(inode, struct vsfs_inode_info, vfs_inode);
}

static inline struct vsfs_sb_info *VSFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* super.c */
extern void vsfs_msg(const char *, const char *, const char *, ...);

/* inode.c */
extern struct inode *vsfs_iget(struct super_block *, unsigned long);
extern int vsfs_write_inode(struct inode *, struct writeback_control *);
extern void vsfs_evict_inode(struct inode *);
extern int vsfs_prepare_chunk(struct page *, loff_t, unsigned);
extern struct inode *vsfs_new_inode(struct inode *, umode_t);
extern int vsfs_setattr(struct dentry *, struct iattr *);
extern const struct inode_operations vsfs_file_inode_operations;
extern const struct file_operations vsfs_file_operations;
extern const struct address_space_operations vsfs_aops;

/* dir.c */
extern int vsfs_add_link(struct dentry *, struct inode *);
extern ino_t vsfs_inode_by_name(struct inode *, const struct qstr *);
extern int vsfs_make_empty(struct inode *, struct inode *);
extern struct vsfs_dir_entry *vsfs_find_entry(struct inode *, const struct qstr *, struct page **);
extern int vsfs_delete_entry(struct inode *, struct vsfs_dir_entry *, struct page *);
extern int vsfs_empty_dir(struct inode *);
extern const struct file_operations vsfs_dir_operations;

/* namei.c */
extern const struct inode_operations vsfs_dir_inode_operations;

#endif /* _VSFS_H */
