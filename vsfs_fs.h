/*
 * vvsfs_fs.h
 *
 * Copyright 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#ifndef _VSFS_FS_H
#define _VSFS_FS_H

/* these are defined in kernel */
#ifndef PAGE_SIZE
#define PAGE_SIZE		4096
#endif
#define BITS_PER_BYTE		8
#define VSFS_SUPER_MAGIC	0x202105F5	/* VSFS Magic Number */

struct vsfs_configuration {
	int heap;
	int dbg_lv;
	int trim;

	int32_t fd;
	u_int32_t sector_size;
	u_int32_t sectors_per_block;
	u_int64_t start_blkaddr;
	u_int64_t end_blkaddr;
	u_int64_t total_sectors;
	u_int32_t total_blocks;

	char *vol_label;
	char *path;

	u_int32_t root_uid;
	u_int32_t root_gid;
} __attribute__((packed));

#define set_sb_le64(member, val)		(sb->member = cpu_to_le64(val))
#define set_sb_le32(member, val)		(sb->member = cpu_to_le32(val))
#define set_sb_le16(member, val)		(sb->member = cpu_to_le16(val))
#define get_sb_le64(member)			le64_to_cpu(sb->member)
#define get_sb_le32(member)			le32_to_cpu(sb->member)
#define get_sb_le16(member)			le16_to_cpu(sb->member)

#define set_sb(member, val)						 \
			do {						 \
				typeof(sb->member) t;			 \
				switch (sizeof(t)) {			 \
				case 8: set_sb_le64(member, val); break; \
				case 4: set_sb_le32(member, val); break; \
				case 2: set_sb_le16(member, val); break; \
				}					 \
			} while(0)

#define get_sb(member)							\
			({						\
				typeof(sb->member) t;			\
				switch (sizeof(t)) {			\
				case 8: t = get_sb_le64(member); break; \
				case 4: t = get_sb_le32(member); break; \
				case 2: t = get_sb_le16(member); break; \
				} 					\
				t;					\
			})

#define VSFS_SUPER_OFFSET		1024	/* byte-size offset */
#define VSFS_BLKSIZE			4096	/* support only 4KB block */
#define VSFS_BLKSHIFT			12	/* support only 4KB block */
#define MAX_PATH_LEN			32

#define VSFS_ROOT_INO			 3	/* Root inode */

struct vsfs_super_block {
        __le32 magic;                   /* Magic Number */
        __le32 sector_size;		/* sector size in bytes */
        __le32 sectors_per_block;       /* # of sectors per block */
        __le32 block_size;		/* block size in bytes */
        __le64 block_count;             /* total # of user blocks */
	__le32 start_block_addr;	/* block 0 byte address  */
        __le32 imap_blkaddr;            /* start block address of inode bmap */
        __le32 dmap_blkaddr;            /* start block address of data bmap */
        __le32 inodes_blkaddr;          /* start block address of inodes */
        __le32 data_blkaddr;            /* start block address of data */
        __le32 block_count_imap;        /* # of blocks for inode bmap */
        __le32 block_count_dmap;        /* # of blocks for data bmap */
        __le32 block_count_inodes;      /* # of blocks for inode */
        __le32 block_count_data;        /* # of blocks for data */
        __le32 root_addr;               /* root inode blkaddr */
	char path[MAX_PATH_LEN];
} __attribute__((packed));

#define VSFS_DIR_BLK_CNT		12      /* Address Pointers in Inode */
#define VSFS_IND_BLK_CNT		3       /* Indirect Pointers in Inode */
#define VSFS_NODE_PER_BLK		1024    /* Address Pointers in an Indirect Block */
#define VSFS_NODE_PER_BLK_BIT		10
#define VSFS_IND_BLK			VSFS_DIR_BLK_CNT + 0
#define VSFS_DIND_BLK			VSFS_DIR_BLK_CNT + 1
#define VSFS_TIND_BLK			VSFS_DIR_BLK_CNT + 2

#define VSFS_MAXNAME_LEN		255

struct vsfs_inode {
        __le16 i_mode;                  /* file mode */
        __u8 i_advise;                  /* file hints */
        __u8 i_inline;                  /* file inline flags */
        __le32 i_uid;                   /* user ID */
        __le32 i_gid;                   /* group ID */
        __le32 i_links;                 /* links count */
        __le64 i_size;                  /* file size in bytes */
        __le64 i_blocks;                /* file size in blocks */
        __le64 i_atime;                 /* access time */
        __le64 i_ctime;                 /* creation time */
        __le64 i_mtime;                 /* modification time */
        __le32 i_atime_nsec;            /* access time in nano scale */
        __le32 i_ctime_nsec;            /* creation time in nano scale */
        __le32 i_mtime_nsec;            /* modification time in nano scale */
        __le32 i_flags;                 /* file attributes */
        __le32 i_pino;                  /* parent inode number */

        __le32 i_daddr[VSFS_DIR_BLK_CNT];     /* Pointers to data blocks */
        __le32 i_iaddr[VSFS_IND_BLK_CNT];      /* indirect, double indirect,
                                                triple_indirect block address*/
} __attribute__((packed));

struct indirect_node {
        __le32 addr[VSFS_NODE_PER_BLK];       /* array of data block address */
} __attribute__((packed));


struct vsfs_dir_entry {
        __le32 inode;			/* inode number */
	__le16 rec_len;
	__u8 name_len;
	__u8 file_type;
        __u8 name[VSFS_MAXNAME_LEN];	 /* file name */
} __attribute__((packed));

#define VSFS_DIR_PAD			4
#define VSFS_DIR_ROUND			(VSFS_DIR_PAD - 1)
#define VSFS_DIR_REC_LEN(name_len)	(((name_len) + 8 + VSFS_DIR_ROUND) & ~VSFS_DIR_ROUND)

#endif /* _VSFS_FS_H */

