#ifndef LINUXKPI_NATIVE_VFS_H
#define LINUXKPI_NATIVE_VFS_H

/* Native VFS bridge between imported Linux file operations and AvoryOS's
 * vfs_node descriptor table.
 *
 * Layout:
 *   native_vfs.c (kernel/src/linuxkpi)  -- native side: creates vfs_node
 *       descriptors whose callbacks forward into the Linux API TU, and wraps
 *       the native fd table.
 *   file.c (kernel/linuxkpi/src)        -- Linux side: struct file lifetime,
 *       fget/fput, anon_inode_getfile, f_op dispatch.
 *
 * Like the other native_*.h headers, only builtin types are used so this can
 * be included next to the real Linux headers. */

/* ── native side (native_vfs.c) ─────────────────────────────────────────── */

/* Allocate a vfs_node_t with the Linux-file callbacks installed and one
 * reference.  Returns 0 on failure. */
void *asc_vfs_anon_node(void);
void asc_vfs_node_set_name(void *node, const char *name);
void asc_vfs_node_set_device(void *node, void *file);
void *asc_vfs_node_device(void *node);
/* Bind a native wait queue (opaque `wait_queue_t *`) to a node so sys_poll()
 * blocks on it and drivers can wake pollers through it. */
void asc_vfs_node_set_wait_queue(void *node, void *wq);
void asc_vfs_node_ref(void *node);
void asc_vfs_node_unref(void *node);
/* Release a node whose only owner is a kernel-internal Linux `struct file`
 * (e.g. the per-GEM-object shmem file).  Unlike asc_vfs_node_unref() this
 * detaches the node's file pointer and close callback first, so freeing the
 * file through the node's own close path cannot recurse.  Nodes already torn
 * down by an fd close (refcount 0) are left alone. */
void asc_vfs_node_release_kernel(void *node);
/* The node's rdev/inode word, used to hand a char device's dev_t to a Linux
 * open path (DRM stores MKDEV(DRM_MAJOR, minor->index) here). */
__UINT32_TYPE__ asc_vfs_node_inode(void *node);

/* Register /dev/<name> as a character device whose per-open native node is
 * produced by `open_fn` (a Linux-API TU callback).  The metadata node is
 * persistent; every open() calls open_fn and adopts the returned node with
 * the descriptor owning the initial reference, so close/fork behave like any
 * other char device.  Returns 0 or a negative error. */
int asc_vfs_register_devnode(const char *name, void *(*open_fn)(void *));

/* Same, but for a nested path below /dev (e.g. dir="dri", name="card1").
 * The metadata node is kept in a small registry; directories that own a
 * custom finddir/readdir (like /dev/dri) enumerate it with
 * asc_vfs_devnode_lookup()/asc_vfs_devnode_name_at().  `rdev` is exported to
 * the open callback through asc_vfs_node_inode().  Returns 0, -EEXIST if the
 * path is already registered, or another negative errno. */
int asc_vfs_register_devnode_at(const char *dir, const char *name,
                                __UINT32_TYPE__ rdev,
                                void *(*open_fn)(void *));
/* Remove a nested devnode registered with asc_vfs_register_devnode_at() (e.g.
 * when the underlying Linux device is deleted).  Drops the metadata node's
 * persistent reference; open descriptors keep their own per-open nodes. */
void asc_vfs_unregister_devnode_at(const char *dir, const char *name);
void *asc_vfs_devnode_lookup(const char *dir, const char *name);
const char *asc_vfs_devnode_name_at(const char *dir, unsigned int index,
                                    __UINT32_TYPE__ *rdev_out);

/* Allocate/free a native poll wait queue (`wait_queue_t`) for the file
 * bridge; attach it to a node with asc_vfs_node_set_wait_queue(). */
void *asc_vfs_poll_wq_alloc(void);
void asc_vfs_poll_wq_free(void *wq);

/* Kernel-side (fd-less) open of a native path.  Runs open_instance() like
 * sys_open() and returns the node with one reference, or NULL.  Used by
 * kernel self-tests and in-kernel consumers; release with
 * asc_vfs_kernel_close(). */
void *asc_vfs_kernel_open(const char *path);
int asc_vfs_kernel_ioctl(void *node, unsigned int request,
                         __UINT64_TYPE__ arg);
int asc_vfs_kernel_poll(void *node, int events);
unsigned int asc_vfs_kernel_read(void *node, unsigned int offset,
                                 unsigned int size, unsigned char *buffer);
void asc_vfs_kernel_close(void *node);

/* Native fd table.  asc_vfs_fd_alloc reserves a descriptor (returns < 0 on
 * failure); asc_vfs_fd_install publishes the node into it, applying
 * O_CLOEXEC from `flags`; asc_vfs_fd_release_reserved cancels a reservation;
 * asc_vfs_fd_lookup returns the node without taking a reference. */
int asc_vfs_fd_alloc(unsigned int flags);
void asc_vfs_fd_install(int fd, void *node, unsigned int flags);
void asc_vfs_fd_release_reserved(int fd);
void *asc_vfs_fd_lookup(int fd);

/* ── Linux side (file.c) ────────────────────────────────────────────────── */

int linuxkpi_file_ioctl(void *node, unsigned int request,
                        __UINT64_TYPE__ arg);
__UINT64_TYPE__ linuxkpi_file_mmap(void *node, __UINT64_TYPE__ addr,
                                   __UINT64_TYPE__ length,
                                   __UINT64_TYPE__ prot,
                                   __UINT64_TYPE__ flags,
                                   __UINT64_TYPE__ offset);
int linuxkpi_file_poll(void *node, int events);
unsigned int linuxkpi_file_read(void *node, unsigned int offset,
                                unsigned int size, unsigned char *buffer);
unsigned int linuxkpi_file_write(void *node, unsigned int offset,
                                 unsigned int size, unsigned char *buffer);
void linuxkpi_file_close(void *node);

#endif /* LINUXKPI_NATIVE_VFS_H */
