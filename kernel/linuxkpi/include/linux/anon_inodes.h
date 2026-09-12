#ifndef __AVORY_LINUXKPI_ANON_INODES_H
#define __AVORY_LINUXKPI_ANON_INODES_H

/* AvoryOS overlay for <linux/anon_inodes.h>.  The "inode" is the bridge in
 * linuxkpi/src/file.c; no VFS filesystem is involved. */

#include <linux/types.h>
#include <linux/fs.h>

struct file *anon_inode_getfile(const char *name,
                                const struct file_operations *fops, void *priv,
                                int flags);
int anon_inode_getfd(const char *name, const struct file_operations *fops,
                     void *priv, int flags);

#endif /* __AVORY_LINUXKPI_ANON_INODES_H */
