// SPDX-License-Identifier: GPL-2.0
#ifndef VTFS_H
#define VTFS_H

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mutex.h>

struct vtfs_fs {
	atomic_long_t next_ino;
	struct mutex namespace_lock;
	struct list_head entries;
};

struct vtfs_data {
	char *bytes;
	size_t capacity;
	struct list_head children;
	loff_t next_cookie;
};

struct inode *vtfs_get_inode(struct super_block *sb,
                            const struct inode *parent, umode_t mode);
void vtfs_evict_inode(struct inode *inode);
void vtfs_clear_namespace(struct super_block *sb);

extern const struct inode_operations vtfs_dir_inode_ops;
extern const struct inode_operations vtfs_file_inode_ops;
extern const struct file_operations vtfs_dir_ops;
extern const struct file_operations vtfs_file_ops;

#endif
