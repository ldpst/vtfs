// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

#include "vtfs.h"

static int vtfs_reserve(struct inode *inode, size_t needed)
{
	struct vtfs_data *data = inode->i_private;
	size_t capacity;
	char *bytes;

	if (needed <= data->capacity)
		return 0;
	capacity = max(needed, data->capacity <= SIZE_MAX / 2 ?
			data->capacity * 2 : needed);
	bytes = kvzalloc(capacity, GFP_KERNEL);
	if (!bytes)
		return -ENOMEM;
	if (inode->i_size)
		memcpy(bytes, data->bytes, inode->i_size);
	kvfree(data->bytes);
	data->bytes = bytes;
	data->capacity = capacity;
	return 0;
}

static ssize_t vtfs_read(struct file *file, char __user *buffer,
			 size_t count, loff_t *position)
{
	struct inode *inode = file_inode(file);
	struct vtfs_data *data = inode->i_private;
	ssize_t result;

	inode_lock(inode);
	result = simple_read_from_buffer(buffer, count, position,
					 data->bytes, inode->i_size);
	inode_unlock(inode);
	if (result > 0)
		file_accessed(file);
	return result;
}

/* iocb: открытый файл, позиция, флаги. from: итератор по данным для записи */
static ssize_t vtfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct vtfs_data *data = inode->i_private;
	loff_t position;
	size_t copied;
	ssize_t result;

	inode_lock(inode);
	result = generic_write_checks(iocb, from);
	if (result <= 0)
		goto out;
	position = iocb->ki_pos;
	result = vtfs_reserve(inode, position + iov_iter_count(from));
	if (result)
		goto out;
	result = file_modified(file);
	if (result)
		goto out;
	if (position > inode->i_size)
		memset(data->bytes + inode->i_size, 0, position - inode->i_size);
	copied = copy_from_iter(data->bytes + position, iov_iter_count(from), from);
	if (!copied) {
		result = -EFAULT;
		goto out;
	}
	iocb->ki_pos = position + copied;
	if (iocb->ki_pos > inode->i_size)
		i_size_write(inode, iocb->ki_pos);
	result = copied;
out:
	inode_unlock(inode);
	return result;
}

static int vtfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct vtfs_data *data = inode->i_private;
	int result = setattr_prepare(idmap, dentry, attr);

	if (result)
		return result;
	if (attr->ia_valid & ATTR_SIZE) {
		result = vtfs_reserve(inode, attr->ia_size);
		if (result)
			return result;
		if (!attr->ia_size) {
			kvfree(data->bytes);
			data->bytes = NULL;
			data->capacity = 0;
		} else if (attr->ia_size < inode->i_size) {
			memset(data->bytes + attr->ia_size, 0,
			       inode->i_size - attr->ia_size);
		}
		truncate_setsize(inode, attr->ia_size);
	}
	setattr_copy(idmap, inode, attr);
	return 0;
}

const struct inode_operations vtfs_file_inode_ops = {
	.setattr = vtfs_setattr,
	.getattr = simple_getattr,
};

const struct file_operations vtfs_file_ops = {
	.owner = THIS_MODULE,
	.read = vtfs_read,
	.write_iter = vtfs_write_iter,
	.llseek = generic_file_llseek,
	.fsync = noop_fsync,
};

