#ifndef LINUXKPI_NATIVE_SYSFS_H
#define LINUXKPI_NATIVE_SYSFS_H

/* Native sysfs bridge for LinuxKPI implementation files.
 *
 * The native sysfs tree (kernel/src/fs/sysfs.c) was static; these entry points
 * let the device-core shim materialize dynamic class/device directories and
 * attribute files.  Only builtin types are used so this header can be included
 * next to the real Linux headers.
 *
 * Text attribute callbacks receive the opaque `ctx` registered with the file
 * and fill/consume a kernel buffer; they return the number of bytes produced
 * or -errno.  Binary attributes get the file offset and buffer size.  All
 * records carry a release callback that runs when the file is removed, so the
 * LinuxKPI shim can free its per-attribute context.  Paths/dirs are opaque
 * `vfs_node_t *` handles. */

/* /sys/class/<name>, created on first use. */
void *asc_sysfs_class_dir(const char *name);

/* A directory under a class dir (e.g. /sys/class/drm/card1). */
void *asc_sysfs_device_dir(void *class_dir, const char *dev_name);

/* An existing sysfs directory by absolute path (e.g.
 * "/sys/bus/pci/devices/0000:00:03.0"), or NULL.  The returned tree pointer
 * stays valid while the directory exists (sysfs directories are permanent). */
void *asc_sysfs_dir_by_path(const char *path);

/* Best-effort removal of a child file/dir (used by device_del). */
void asc_sysfs_remove(void *dir, const char *name);

/* Create an attribute file whose read() calls show(ctx, buf, size) and whose
 * write() calls store(ctx, buf, size). */
int asc_sysfs_attr_file(void *dir, const char *name, unsigned int mode,
                        void *ctx,
                        int (*show)(void *ctx, char *buf, unsigned int size),
                        int (*store)(void *ctx, const char *buf,
                                     unsigned int size),
                        void (*release)(void *ctx));

/* Create a binary attribute file.  read()/write() receive the file offset
 * (partial reads) and return the bytes produced/consumed, or -errno. */
int asc_sysfs_bin_file(void *dir, const char *name, unsigned int mode,
                       void *ctx,
                       int (*read)(void *ctx, unsigned int offset, char *buf,
                                   unsigned int size),
                       int (*write)(void *ctx, unsigned int offset,
                                    const char *buf, unsigned int size),
                       void (*release)(void *ctx));

#endif /* LINUXKPI_NATIVE_SYSFS_H */
