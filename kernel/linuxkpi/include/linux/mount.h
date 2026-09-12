#ifndef __AVORY_LINUXKPI_MOUNT_H
#define __AVORY_LINUXKPI_MOUNT_H

/* AvoryOS overlay for <linux/mount.h>.
 *
 * Enough fake VFS to let pseudo filesystems (dma-buf, DRM's internal fs)
 * work: a synthetic superblock/mount pair per kern_mount() call, anon inodes
 * from alloc_anon_inode(), and files from alloc_file_pseudo().  None of it is
 * reachable through paths; the objects exist so imported code can keep its
 * dentry/inode bookkeeping.  The fs_context callback runs at mount time so
 * the filesystem's dentry_operations can be captured per superblock; that
 * keeps dma-buf's d_release from being clobbered when DRM mounts its own
 * pseudo fs.  Implementations: linuxkpi/src/file.c. */

#include <linux/fs.h>

struct super_block {
  unsigned long s_magic;
  struct file_system_type *s_type;
  struct dentry *s_root;
  void *s_fs_info;
  const struct dentry_operations *s_dops;
};

struct vfsmount {
  struct super_block *mnt_sb;
  struct dentry *mnt_root;
};

struct file_system_type {
  const char *name;
  int fs_flags;
  int (*init_fs_context)(struct fs_context *);
  void (*kill_sb)(struct super_block *);
  struct module *owner;
  struct file_system_type *next;
};

void kill_anon_super(struct super_block *sb);
struct vfsmount *kern_mount(struct file_system_type *type);
void kern_unmount(struct vfsmount *mnt);

struct inode *alloc_anon_inode(struct super_block *sb);
struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt,
                               const char *name, int flags,
                               const struct file_operations *fops);
struct dentry *dget(struct dentry *dentry);
void dput(struct dentry *dentry);
void iput(struct inode *inode);
struct inode *igrab(struct inode *inode);

char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
                  int *count);
void simple_release_fs(struct vfsmount **mount, int *count);

#endif /* __AVORY_LINUXKPI_MOUNT_H */
