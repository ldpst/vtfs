// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/slab.h>

#include "vtfs.h"

struct vtfs_entry {
	struct list_head sibling;
	struct list_head all;
	struct inode *inode;
	loff_t cookie;
	unsigned int length;
	char name[];
};

static struct vtfs_entry *vtfs_find(struct inode *parent, const struct qstr *name)
{
	struct vtfs_data *dir = parent->i_private;
	struct vtfs_entry *entry;

	list_for_each_entry(entry, &dir->children, sibling) {
		if (entry->length == name->len &&
		    !memcmp(entry->name, name->name, name->len))
			return entry;
	}
	return NULL;
}

static int vtfs_insert(struct inode *parent, const struct qstr *name,
		       struct inode *inode)
{
	struct vtfs_fs *fs = parent->i_sb->s_fs_info;
	struct vtfs_data *dir = parent->i_private;
	struct vtfs_entry *entry;

	if (name->len > NAME_MAX)
		return -ENAMETOOLONG;
	if (vtfs_find(parent, name))
		return -EEXIST;
	if (dir->next_cookie == LLONG_MAX)
		return -ENOSPC;
	entry = kmalloc(struct_size(entry, name, name->len + 1), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->inode = inode;
	entry->cookie = dir->next_cookie++;
	entry->length = name->len;
	memcpy(entry->name, name->name, name->len);
	entry->name[name->len] = '\0';
	ihold(inode);
	list_add_tail(&entry->sibling, &dir->children);
	list_add_tail(&entry->all, &fs->entries);
	return 0;
}

static struct dentry *vtfs_lookup(struct inode *parent, struct dentry *dentry,
				 unsigned int flags)
{
	struct vtfs_fs *fs = parent->i_sb->s_fs_info;
	struct vtfs_entry *entry;
	struct inode *inode = NULL;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	mutex_lock(&fs->namespace_lock);
	entry = vtfs_find(parent, &dentry->d_name);
	if (entry) {
		inode = entry->inode;
		ihold(inode);
	}
	mutex_unlock(&fs->namespace_lock);
	return d_splice_alias(inode, dentry);
}

static int vtfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct vtfs_fs *fs = inode->i_sb->s_fs_info;
	struct vtfs_data *dir = inode->i_private;
	struct vtfs_entry *entry;

	mutex_lock(&fs->namespace_lock);
	if (!dir_emit_dots(file, ctx))
		goto out;
	list_for_each_entry(entry, &dir->children, sibling) {
		if (entry->cookie < ctx->pos)
			continue;
		ctx->pos = entry->cookie;
		if (!dir_emit(ctx, entry->name, entry->length, entry->inode->i_ino,
			      S_ISDIR(entry->inode->i_mode) ? DT_DIR : DT_REG))
			goto out;
		ctx->pos = entry->cookie + 1;
	}
out:
	mutex_unlock(&fs->namespace_lock);
	return 0;
}

static int vtfs_add(struct inode *parent, struct dentry *dentry, umode_t type)
{
	struct vtfs_fs *fs = parent->i_sb->s_fs_info;
	struct inode *inode = vtfs_get_inode(parent->i_sb, parent, type | 0777);
	int result;

	if (!inode)
		return -ENOMEM;
	mutex_lock(&fs->namespace_lock);
	result = vtfs_insert(parent, &dentry->d_name, inode);
	if (!result) {
		d_instantiate(dentry, inode);
		inode_set_mtime_to_ts(parent, inode_set_ctime_current(parent));
	}
	mutex_unlock(&fs->namespace_lock);
	if (result)
		iput(inode);
	return result;
}

static int vtfs_create(struct mnt_idmap *idmap, struct inode *parent,
		       struct dentry *dentry, umode_t mode, bool exclusive)
{
	return vtfs_add(parent, dentry, S_IFREG);
}

static int vtfs_mkdir(struct mnt_idmap *idmap, struct inode *parent,
		      struct dentry *dentry, umode_t mode)
{
	int result = vtfs_add(parent, dentry, S_IFDIR);

	if (!result)
		inc_nlink(parent);
	return result;
}

static int vtfs_link(struct dentry *old, struct inode *parent,
		     struct dentry *new)
{
	struct inode *inode = d_inode(old);
	struct vtfs_fs *fs = parent->i_sb->s_fs_info;
	int result;

	if (!S_ISREG(inode->i_mode))
		return -EPERM;
	if (inode->i_sb != parent->i_sb)
		return -EXDEV;
	mutex_lock(&fs->namespace_lock);
	result = vtfs_insert(parent, &new->d_name, inode);
	if (!result) {
		inc_nlink(inode);
		ihold(inode);
		d_instantiate(new, inode);
		inode_set_ctime_current(inode);
		inode_set_mtime_to_ts(parent, inode_set_ctime_current(parent));
	}
	mutex_unlock(&fs->namespace_lock);
	return result;
}

static int vtfs_remove(struct inode *parent, struct dentry *dentry, bool directory)
{
	struct vtfs_fs *fs = parent->i_sb->s_fs_info;
	struct inode *inode = d_inode(dentry);
	struct vtfs_data *data = inode->i_private;
	struct vtfs_entry *entry;
	int result = 0;

	if (directory != S_ISDIR(inode->i_mode))
		return directory ? -ENOTDIR : -EISDIR;
	mutex_lock(&fs->namespace_lock);
	entry = vtfs_find(parent, &dentry->d_name);
	if (!entry) {
		result = -ENOENT;
		goto out;
	}
	if (directory && !list_empty(&data->children)) {
		result = -ENOTEMPTY;
		goto out;
	}
	list_del(&entry->sibling);
	list_del(&entry->all);
	kfree(entry);
	if (directory) {
		clear_nlink(inode);
		drop_nlink(parent);
	} else {
		drop_nlink(inode);
	}
	inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(parent, inode_set_ctime_current(parent));
	iput(inode);
out:
	mutex_unlock(&fs->namespace_lock);
	return result;
}

static int vtfs_unlink(struct inode *parent, struct dentry *dentry)
{
	return vtfs_remove(parent, dentry, false);
}

static int vtfs_rmdir(struct inode *parent, struct dentry *dentry)
{
	return vtfs_remove(parent, dentry, true);
}

void vtfs_clear_namespace(struct super_block *sb)
{
	struct vtfs_fs *fs = sb->s_fs_info;
	struct vtfs_entry *entry, *next;

	if (fs) {
		list_for_each_entry(entry, &fs->entries, all)
			list_del(&entry->sibling);
		list_for_each_entry_safe(entry, next, &fs->entries, all) {
			list_del(&entry->all);
			iput(entry->inode);
			kfree(entry);
		}
	}
}

const struct inode_operations vtfs_dir_inode_ops = {
	.lookup = vtfs_lookup,
	.create = vtfs_create,
	.unlink = vtfs_unlink,
	.mkdir = vtfs_mkdir,
	.rmdir = vtfs_rmdir,
	.link = vtfs_link,
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

const struct file_operations vtfs_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = vtfs_iterate,
	.llseek = generic_file_llseek,
};

