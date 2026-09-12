#ifndef __AVORY_LINUXKPI_PSEUDO_FS_H
#define __AVORY_LINUXKPI_PSEUDO_FS_H

/* AvoryOS overlay for <linux/pseudo_fs.h>.  dma-buf only references this
 * from its fs_context callback, which the kern_mount() shim never invokes;
 * the types and init_pseudo() exist so the file compiles. */

#include <linux/fs.h>
#include <linux/mount.h>

struct fs_context {
  unsigned long fs_type_magic;
};

struct pseudo_fs_context {
  unsigned long magic;
  const struct dentry_operations *dops;
};

struct pseudo_fs_context *init_pseudo(struct fs_context *fc,
                                      unsigned long magic);

#endif /* __AVORY_LINUXKPI_PSEUDO_FS_H */
