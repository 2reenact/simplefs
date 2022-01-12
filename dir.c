/*
 * dir.c
 *
 * 2021 Lee JeYeon., Dankook Univ.
 *		2reenact@gmail.com
 *
 */

#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/iversion.h>

#include "vsfs_fs.h"
#include "vsfs.h"

static inline int vsfs_match(int len, const unsigned char *name, struct vsfs_dir_entry *de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

static int vsfs_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;

	inode_inc_iversion(dir);
	block_write_end(NULL, mapping, pos, len, len, page, NULL);

	if (pos + len > dir->i_size) {
		i_size_write(dir, pos + len);
		mark_inode_dirty(dir);
	}

	if (IS_DIRSYNC(dir)) {
		err = write_one_page(page);
		if (!err)
			err = sync_inode_metadata(dir, 1);
	} else {
		unlock_page(page);
	}

	return err;
}

static inline void vsfs_put_page(struct page *page)
{
	kunmap(page);
	put_page(page);
}

ino_t vsfs_inode_by_name(struct inode *dir, const struct qstr *qstr)
{
	ino_t ret = 0;
	struct vsfs_dir_entry *de;
	struct page *page;

	de = vsfs_find_entry(dir, qstr, &page);
	if (de) {
		ret = le32_to_cpu(de->inode);
		vsfs_put_page(page);
	}
	return ret;
}

static struct page *vsfs_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
	struct page *page = read_mapping_page(mapping, n, NULL);

	if (!IS_ERR(page)) {
		kmap(page);
		if (unlikely(!PageChecked(page))) {
			if (PageError(page))
				goto get_page_fail;
		}
	}
	return page;

get_page_fail:
	vsfs_put_page(page);
	return ERR_PTR(-EIO);
}

static unsigned vsfs_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static inline struct vsfs_dir_entry *vsfs_next_entry(struct vsfs_dir_entry *p)
{
	return (struct vsfs_dir_entry *)((char *)p + le16_to_cpu(p->rec_len));
}

struct vsfs_dir_entry *vsfs_find_entry(struct inode *dir, const struct qstr *qstr, struct page **res_page)
{
	const unsigned char *name = qstr->name;
	int namelen = qstr->len;
	unsigned reclen = VSFS_DIR_REC_LEN(namelen);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct vsfs_inode_info *vsi = VSFS_I(dir);
	struct vsfs_dir_entry *de;

	if (npages == 0 || namelen > VSFS_MAXNAME_LEN)
		goto out_find_entry;

	*res_page = NULL;

	start = vsi->i_dir_start_lookup;

	if (start >= npages)
		start = 0;
	n = start;
	do {
		char *kaddr;
		page = vsfs_get_page(dir, n);
		if (!IS_ERR(page)) {
			kaddr = page_address(page);
			de = (struct vsfs_dir_entry *)kaddr;
			kaddr += vsfs_last_byte(dir, n) - reclen;
			while ((char *) de <= kaddr) {
				if (de->rec_len == 0) {
					vsfs_msg(KERN_ERR, "vsfs_find_entry", "zero-length diretory entry");
					vsfs_put_page(page);
					goto out_find_entry;
				}
				if (vsfs_match(namelen, name, de))
					goto entry_found;
				de = vsfs_next_entry(de);
			}
			vsfs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);

out_find_entry:
	return NULL;

entry_found:
	*res_page = page;
	vsi->i_dir_start_lookup = n;
	return de;
}

int vsfs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned reclen = VSFS_DIR_REC_LEN(namelen);
	unsigned short rec_len, name_len;
	struct page *page = NULL;
	struct vsfs_dir_entry *de;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	char *kaddr;
	loff_t pos;
	int err;

	for (n = 0; n <= npages; n++) {
		char *dir_end;

		page = vsfs_get_page(dir, n);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out_add_link;
		lock_page(page);
		kaddr = page_address(page);
		dir_end = kaddr + vsfs_last_byte(dir, n);
		de = (struct vsfs_dir_entry *)kaddr;
		kaddr += PAGE_SIZE - reclen;
		while ((char *)de <= kaddr) {
			if ((char *)de == dir_end) {
				name_len = 0;
				rec_len = VSFS_BLKSIZE;
				de->rec_len = cpu_to_le16(VSFS_BLKSIZE);
				de->inode = 0;
				goto got_it;
			}
			if (de->rec_len == 0) {
				vsfs_msg(KERN_ERR, "vsfs_add_link", "zero-length directory entry");
				err = -EIO;
				goto out_unlock;
			}	
			err = -EEXIST;
			if (vsfs_match(namelen, name, de))
				goto out_unlock;
			name_len = VSFS_DIR_REC_LEN(de->name_len);
			rec_len = le16_to_cpu(de->rec_len);
			if (!de->inode && rec_len >= reclen)
				goto got_it;
			if (rec_len >= name_len + reclen)
				goto got_it;
			de = (struct vsfs_dir_entry *)((char *)de + rec_len);
		}
		unlock_page(page);
		vsfs_put_page(page);
	}
	return -EINVAL;

got_it:
	pos = page_offset(page) + (char *)de - (char *)page_address(page);
	err = vsfs_prepare_chunk(page, pos, rec_len);
	if (err)
		goto out_unlock;
	if (de->inode) {
		struct vsfs_dir_entry *del = (struct vsfs_dir_entry *)((char *)de + name_len);
		del->rec_len = cpu_to_le16(rec_len - name_len);
		de->rec_len = cpu_to_le16(name_len);

		de = del;
	}

	de->name_len = namelen;
	memcpy(de->name, name, namelen + 1);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = inode->i_mode;

	err = vsfs_commit_chunk(page, pos, rec_len);
	dir->i_mtime = dir->i_ctime = current_time(dir);

	mark_inode_dirty(dir);

out_put:
	vsfs_put_page(page);

out_add_link:
	return err;

out_unlock:
	unlock_page(page);
	goto out_put;
}

static inline unsigned vsfs_validate_entry(char *base, unsigned offset, unsigned mask)
{
	struct vsfs_dir_entry *de = (struct vsfs_dir_entry *)(base + offset);
	struct vsfs_dir_entry *p = (struct vsfs_dir_entry *)(base + (offset & mask));
	while ((char *)p < (char *)de)
		p = vsfs_next_entry(p);
	return (char *)p - base;
}

static int vsfs_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	unsigned int offset = pos & ~PAGE_MASK;
	unsigned long n = pos >> PAGE_SHIFT;
	unsigned long npages = dir_pages(inode);
	bool need_revalidate = !inode_eq_iversion(inode, file->f_version);

	if (pos > inode->i_size - VSFS_DIR_REC_LEN(1))
		return 0;

	for ( ; n < npages; n++, offset = 0) {
		char *kaddr, *limit;
		struct vsfs_dir_entry *de;

		struct page *page = vsfs_get_page(inode, n);

		if (IS_ERR(page)) {
			vsfs_msg(KERN_ERR, "vsfs_readdir", "bad page in %lu", inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return -EIO;
		}
		kaddr = page_address(page);
		if (need_revalidate) {
			if (offset) {
				offset = vsfs_validate_entry(kaddr, offset, ~(VSFS_BLKSIZE - 1));
				ctx->pos = (n << PAGE_SHIFT) + offset;
			}
			file->f_version = inode_query_iversion(inode);
			need_revalidate = false;
		}
		de = (struct vsfs_dir_entry *)(kaddr + offset);
		limit = kaddr + vsfs_last_byte(inode, n) - VSFS_DIR_REC_LEN(1);
		for ( ; (char *)de <= limit; de = vsfs_next_entry(de)) {
			if (de->rec_len == 0) {
				vsfs_msg(KERN_ERR, "vsfs_find_entry", "zero-length diretory entry");
				vsfs_put_page(page);
				return -EIO;
			}
			if (de->inode) {
				unsigned char d_type = DT_UNKNOWN;

				d_type = fs_ftype_to_dtype(de->file_type);
				if (!dir_emit(ctx, de->name, de->name_len,
						le32_to_cpu(de->inode),
						d_type)) {
					vsfs_put_page(page);
					return 0;
				}
			}
			ctx->pos += le16_to_cpu(de->rec_len);
		}
		vsfs_put_page(page);
	}
	return 0;
}

int vsfs_delete_entry(struct inode *inode, struct vsfs_dir_entry *dir, struct page *page)
{
	char *kaddr = page_address(page);
	unsigned from = ((char *)dir - kaddr) & ~(VSFS_BLKSIZE - 1);
	unsigned to = ((char *)dir - kaddr) + le16_to_cpu(dir->rec_len);
	loff_t pos;
	struct vsfs_dir_entry *pde = NULL;
	struct vsfs_dir_entry *de = (struct vsfs_dir_entry *)(kaddr + from);
	int err;


	while ((char *)de < (char *)dir) {
		if (de->rec_len == 0) {
			vsfs_msg(KERN_ERR, "vsfs_delete_entry", "zero-lenth directory entry");
			err = -EIO;
			goto out_delete_entry;
		}
		pde = de;
		de = vsfs_next_entry(de);
	}
	if (pde)
		from = (char *)pde - (char *)page_address(page);
	
	pos = page_offset(page) + from;
	lock_page(page);
	err = vsfs_prepare_chunk(page, pos, to - from);
	if (pde)
		pde->rec_len = cpu_to_le16(to - from);
	dir->inode = 0;
	err = vsfs_commit_chunk(page, pos, to - from);
	inode->i_ctime = inode->i_mtime = current_time(inode);
	mark_inode_dirty(inode);
out_delete_entry:
	vsfs_put_page(page);
	return err;
}

int vsfs_make_empty(struct inode *inode, struct inode *dir)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	struct vsfs_dir_entry *de;
	void *kaddr;
	int err;

	if (!page)
		return -ENOMEM;

	err = vsfs_prepare_chunk(page, 0, VSFS_BLKSIZE);
	if (err) {
		unlock_page(page);
		goto fail;
	}

	kaddr = kmap_atomic(page);
	memset(kaddr, 0, PAGE_SIZE);

	de = (struct vsfs_dir_entry *)kaddr;
	de->name_len = 1;
	de->rec_len = cpu_to_le16(VSFS_DIR_REC_LEN(1));
	memcpy(de->name, ".\0\0", 4);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = fs_umode_to_ftype(inode->i_mode);

	de = (struct vsfs_dir_entry *)(kaddr + VSFS_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = cpu_to_le16(VSFS_BLKSIZE - VSFS_DIR_REC_LEN(2));
	memcpy(de->name, "..\0", 4);
	de->inode = cpu_to_le32(dir->i_ino);
	de->file_type = fs_umode_to_ftype(inode->i_mode);

	kunmap_atomic(kaddr);
	err = vsfs_commit_chunk(page, 0, VSFS_BLKSIZE);
fail:
	put_page(page);
	return err;
}

int vsfs_empty_dir(struct inode *inode)
{
	unsigned long i, namelen, npages = dir_pages(inode);
	struct page *page = NULL;
	struct vsfs_dir_entry *de;
	char *kaddr;

	for (i = 0; i < npages; i++) {
		page = vsfs_get_page(inode, i);

		if (IS_ERR(page))
			continue;

		kaddr = page_address(page);
		de = (struct vsfs_dir_entry *)kaddr;
		kaddr += vsfs_last_byte(inode, i) - VSFS_DIR_REC_LEN(1);

		while ((char *)de <= kaddr) {
			if (de->rec_len == 0) {
				vsfs_msg(KERN_ERR, "vsfs_empty_dir", "zero-length directory entry");
				goto not_empty;
			}
			if (de->inode) {
				namelen = de->name_len;
				if (de->name[0] != '.')
					goto not_empty;
				if (namelen > 2)
					goto not_empty;
				if (namelen < 2) {
					if (inode->i_ino != le32_to_cpu(de->inode))
						goto not_empty;
				} else if (de->name[1] != '.')
					goto not_empty;
			}
			de = vsfs_next_entry(de);
		}
		vsfs_put_page(page);
	}
	return 1;

not_empty:
	vsfs_put_page(page);
	return 0;
}

const struct file_operations vsfs_dir_operations = {
        .llseek         = generic_file_llseek,
        .read           = generic_read_dir,
	.fsync          = generic_file_fsync,
	.iterate_shared	= vsfs_readdir,
};

