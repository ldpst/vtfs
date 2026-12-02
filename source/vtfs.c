// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "vtfs.h"

#define VTFS_MAGIC 0x76746673

static void vtfs_kill_sb(struct super_block *sb)
{
	vtfs_clear_namespace(sb);
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
	kill_anon_super(sb);
}

static const struct super_operations vtfs_super_ops = {
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = vtfs_evict_inode,
};

static int vtfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct vtfs_fs *fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	struct inode *root;

	if (!fs)
		return -ENOMEM;
	atomic_long_set(&fs->next_ino, 999);
	mutex_init(&fs->namespace_lock);
	INIT_LIST_HEAD(&fs->entries);
	sb->s_fs_info = fs;
	sb->s_magic = VTFS_MAGIC;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_time_gran = 1;
	sb->s_op = &vtfs_super_ops;
	root = vtfs_get_inode(sb, NULL, S_IFDIR | 0777);
	if (!root)
		return -ENOMEM;
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;
	return 0;
}

static struct dentry *vtfs_mount(struct file_system_type *type, int flags,
				const char *device, void *data)
{
	return mount_nodev(type, flags, data, vtfs_fill_super);
}

static struct file_system_type vtfs_type = {
	.owner = THIS_MODULE,
	.name = "vtfs",
	.mount = vtfs_mount,
	.kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void)
{
	int result = register_filesystem(&vtfs_type);

	if (!result)
		pr_info("vtfs: filesystem registered\n");
	return result;
}

static void __exit vtfs_exit(void)
{
	unregister_filesystem(&vtfs_type);
	pr_info("vtfs: filesystem unregistered\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VTFS lab: RAM filesystem with file I/O and hard links");
MODULE_ALIAS_FS("vtfs");
