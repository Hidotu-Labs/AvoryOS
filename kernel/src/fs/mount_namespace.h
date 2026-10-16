#ifndef AVORY_MOUNT_NAMESPACE_H
#define AVORY_MOUNT_NAMESPACE_H
#include "vfs.h"
struct thread;
struct mount_namespace;
vfs_node_t *vfs_task_root(void);
vfs_node_t *vfs_namespace_root_view(vfs_node_t *node);
bool vfs_namespace_node_path(vfs_node_t *node, char *buffer, size_t size);
bool vfs_namespace_active(void);
struct mount_namespace *vfs_namespace_prepare(struct thread *parent);
uint32_t vfs_namespace_id(struct thread *task);
struct mount_namespace *vfs_namespace_lookup_copy(struct thread *task);
void vfs_namespace_install(struct thread *task,
                           struct mount_namespace *prepared);
void vfs_namespace_put(struct mount_namespace *ns);
int vfs_namespace_clone(struct thread *child, struct thread *parent,
                        bool new_namespace);
void vfs_namespace_release(struct thread *task);
int vfs_namespace_unshare(struct thread *task);
vfs_node_t *vfs_namespace_resolve(vfs_node_t *dir, const char *path);
vfs_node_t *vfs_namespace_finddir(vfs_node_t *dir, char *name);
int vfs_namespace_mount(vfs_node_t *point, vfs_node_t *target,
                        const char *source, const char *type, const char *path,
                        uint64_t flags);
int vfs_namespace_setattr(vfs_node_t *point, uint64_t set, uint64_t clear,
                          bool recursive);
int vfs_namespace_move(vfs_node_t *source, vfs_node_t *target);
int vfs_namespace_remount(vfs_node_t *point, uint64_t flags);
int vfs_namespace_umount(vfs_node_t *point, unsigned flags);
int vfs_namespace_pivot(vfs_node_t *newroot, vfs_node_t *oldpoint);
int vfs_namespace_chroot(vfs_node_t *root);
int vfs_namespace_mounts(vfs_mount_info_t *buffer, int count);
int vfs_get_mounts_task(struct thread *task, vfs_mount_info_t *buffer,
                        int count);
int vfs_initial_mounts_get(vfs_mount_info_t *buffer, int count);
uint64_t vfs_mount_flags(vfs_node_t *node);
bool vfs_mount_readonly(vfs_node_t *node);
vfs_node_t *vfs_backing_node(vfs_node_t *node);
/* Snapshot boot mounts when the first private mount namespace is created. */
vfs_node_t *vfs_initial_filesystem(const char *type);
int vfs_initial_mounts_copy(struct mount_namespace *ns);
int vfs_namespace_seed(struct mount_namespace *ns, vfs_node_t *point,
                       vfs_node_t *target, uint64_t id, const char *source,
                       const char *type, const char *path);
#endif
