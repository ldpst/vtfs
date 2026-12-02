// SPDX-License-Identifier: GPL-2.0
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "vtfs.h"

struct inode *vtfs_get_inode(struct super_block *sb,
				    const struct inode *parent, umode_t mode)
{
	struct vtfs_fs *fs = sb->s_fs_info;
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;
	inode->i_private = kzalloc(sizeof(struct vtfs_data), GFP_KERNEL);
	if (!inode->i_private) {
		iput(inode);
		return NULL;
	}
	INIT_LIST_HEAD(&((struct vtfs_data *)inode->i_private)->children);
	((struct vtfs_data *)inode->i_private)->next_cookie = 2;
	inode->i_ino = atomic_long_inc_return(&fs->next_ino);
	inode_init_owner(&nop_mnt_idmap, inode, parent, mode);
	simple_inode_init_ts(inode);
	if (S_ISDIR(mode)) {
		inode->i_op = &vtfs_dir_inode_ops;
		inode->i_fop = &vtfs_dir_ops;
		inc_nlink(inode);
	} else {
		inode->i_op = &vtfs_file_inode_ops;
		inode->i_fop = &vtfs_file_ops;
	}
	return inode;
}

void vtfs_evict_inode(struct inode *inode)
{
	struct vtfs_data *data = inode->i_private;

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (data) {
		kvfree(data->bytes);
		kfree(data);
	}
}

