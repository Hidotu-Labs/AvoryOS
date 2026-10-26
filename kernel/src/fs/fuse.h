#ifndef AVORY_FS_FUSE_H
#define AVORY_FS_FUSE_H
#include "vfs.h"
void fuse_init(void);
bool fuse_is_device(vfs_node_t *node);
void fuse_abort_device(vfs_node_t *node);
/* Caller has checked CAP_SYS_ADMIN; mount options are a kernel string. */
int fuse_mount_at(vfs_node_t *point, const char *source, const char *type,
                    const char *path, uint64_t flags, const char *options);
/* Only called for the root of a FUSE mount, after namespace/global detach. */
bool fuse_is_root(vfs_node_t *node);
bool fuse_mount_busy(vfs_node_t *node);
void fuse_detach_mount(vfs_node_t *node);
#endif
