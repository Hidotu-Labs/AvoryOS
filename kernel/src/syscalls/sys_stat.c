// sys_stat.c — File metadata syscalls:
//   stat, fstat, lstat, newfstatat, statx, statfs, fstatfs,
//   getdents, getdents64
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../drivers/storage/block.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

/* Block-backed filesystem statfs handlers. When a file/dir carries one of
 * these, node->device points at a mount struct whose first word is the
 * backing struct block_device* (ext2_mount_t::dev, fat32_mount_t::dev). */
extern int ext4_statfs_impl(vfs_node_t *, struct statfs_buf *);
extern int ext2_statfs_impl(vfs_node_t *, struct statfs_buf *);
extern int fat32_statfs_impl(vfs_node_t *, struct statfs_buf *);

// ---------------------------------------------------------------------------
// fill_kstat — convert a vfs_node into a kstat structure
// ---------------------------------------------------------------------------

void fill_kstat(struct kstat *ks, vfs_node_t *node) {
    ks->st_dev   = 0;
    ks->st_ino   = (uint64_t)node->inode;
    ks->st_nlink = node->get_nlink ? node->get_nlink(node) : 1;

    uint32_t mode = node->mask & 07777;
    switch (node->flags & FS_TYPE_MASK) {
    case FS_FILE:      mode |= 0100000; break;
    case FS_DIRECTORY: mode |= 0040000; break;
    case FS_CHARDEV:   mode |= 0020000; break;
    case FS_BLOCKDEV:  mode |= 0060000; break;
    case FS_SYMLINK:   mode |= 0120000; break;
    case FS_SOCKET:    mode |= 0140000; break;
    case FS_PIPE:      mode |= 0010000; break;
    default:           mode |= 0100000; break;
    }
    ks->st_mode  = mode;
    ks->st_uid   = node->uid;
    ks->st_gid   = node->gid;
    ks->__pad0   = 0;
    ks->st_rdev  = 0;
    if ((node->flags & FS_TYPE_MASK) == FS_CHARDEV)
        ks->st_rdev = (uint64_t)node->inode;
    if ((node->flags & FS_TYPE_MASK) == FS_BLOCKDEV && node->device) {
        /* /dev/<disk> nodes: report the same major:minor that /sys/block,
         * /proc/partitions and /proc/self/mountinfo advertise. The inode
         * already carries the userspace dev_t (set in block_publish_node),
         * but older nodes registered before that change have inode 0, so
         * derive it from the block device when needed. */
        struct block_device *bd = (struct block_device *)node->device;
        uint32_t rdev = block_device_userspace_rdev(bd);
        ks->st_rdev = (uint64_t)(rdev ? rdev : node->inode);
    }
    /* st_dev for files on block filesystems: the filesystem's device number.
     * Qt/UDisks match stat().st_dev against mountinfo's devno to find the
     * mount a path belongs to; with st_dev 0 every file looks like it lives
     * on no device and the match falls back to pure path prefixing. */
    if (((node->flags & FS_TYPE_MASK) == FS_FILE ||
         (node->flags & FS_TYPE_MASK) == FS_DIRECTORY) &&
        node->device && node->statfs &&
        (node->statfs == ext4_statfs_impl ||
         node->statfs == ext2_statfs_impl ||
         node->statfs == fat32_statfs_impl)) {
        struct block_device *bd = *(struct block_device **)node->device;
        if (bd && (bd->linux_major || bd->linux_minor))
            ks->st_dev = (uint64_t)block_device_userspace_rdev(bd);
    }

    ks->st_size    = (int64_t)node->length;
    ks->st_blksize = 4096;
    ks->st_blocks  = ((int64_t)node->length + 511) / 512;

    ks->st_atim_sec  = (int64_t)node->atime;
    ks->st_atim_nsec = 0;
    ks->st_mtim_sec  = (int64_t)node->mtime;
    ks->st_mtim_nsec = 0;
    ks->st_ctim_sec  = (int64_t)node->ctime;
    ks->st_ctim_nsec = 0;
    ks->__unused[0] = ks->__unused[1] = ks->__unused[2] = 0;
}

// ---------------------------------------------------------------------------
// stat / lstat / fstat
// ---------------------------------------------------------------------------

static uint64_t sys_stat(uint64_t path_ptr, uint64_t statbuf_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)path_ptr;
    struct kstat *ks  = (struct kstat *)statbuf_ptr;
    if (!path || !ks || !is_user_ptr((uint64_t)ks))
        return (uint64_t)-14;

    struct thread *t = sched_get_current();
    vfs_node_t *node  = NULL;
    bool node_owned   = false;

    if (strncmp(path, "/dev/", 5) == 0)
        node = fb_lookup_device((char *)path + 5);

    if (!node) {
        vfs_node_t *cwd = (path[0] == '/') ? fs_root
                          : (t->cwd_node ? t->cwd_node : fs_root);
        node = vfs_resolve_path_at(cwd, path);
        node_owned = node != NULL;
    }
    if (!node) {
        return (uint64_t)-2;
    }
    fill_kstat(ks, node);
    if (node_owned)
        vfs_close(node);
    return 0;
}

static uint64_t sys_lstat(uint64_t path_ptr, uint64_t statbuf_ptr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)path_ptr;
    struct kstat *ks  = (struct kstat *)statbuf_ptr;
    if (!path || !ks || !is_user_ptr((uint64_t)ks))
        return (uint64_t)-14;

    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    vfs_node_t *cwd   = NULL;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        cwd = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (cwd) base = cwd;
    }

    vfs_node_t *node = vfs_resolve_symlink_node(base, path);
    if (!node) {
        if (cwd) vfs_close(cwd);
        return sys_stat(path_ptr, statbuf_ptr, a2, a3, a4, a5);
    }

    fill_kstat(ks, node);
    vfs_close(node);
    if (cwd) vfs_close(cwd);
    return 0;
}

static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf_ptr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct kstat *ks = (struct kstat *)statbuf_ptr;
    if (!ks || !is_user_ptr((uint64_t)ks)) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd])) return (uint64_t)-9;

    fill_kstat(ks, t->fds[fd]);
    return 0;
}

// ---------------------------------------------------------------------------
// statx
// ---------------------------------------------------------------------------

static uint64_t sys_statx(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                           uint64_t mask, uint64_t statxbuf_ptr, uint64_t a5) {
    (void)a5; (void)mask;
    const char *path = (const char *)path_ptr;
    if (!path_ptr) return (uint64_t)-14; /* NULL pathname: EFAULT like Linux */
    if (!statxbuf_ptr ||
        !vmm_is_user_addr_range_valid(statxbuf_ptr, sizeof(struct statx)))
        return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *node = NULL;
    bool node_owned = false;
    vfs_node_t *base_owned = NULL;
    /* Fault-safe empty check: a user string running into an unmapped page
     * still yields its valid prefix via strncpy_from_user. */
    char kpath_head[1];
    if (strncpy_from_user(kpath_head, path, 1) < 0)
        return (uint64_t)-14;
    if (kpath_head[0] != '\0') {
        /* Copy the full pathname into a kernel buffer so the VFS walk below
         * never dereferences user memory directly. */
        char kpath[512];
        long klen = strncpy_from_user(kpath, path, (long)sizeof(kpath));
        if (klen < 0)
            return (uint64_t)-14;
        if ((size_t)klen >= sizeof(kpath))
            return (uint64_t)-36; /* ENAMETOOLONG */
        kpath[sizeof(kpath) - 1] = '\0';
        const char *kpathp = kpath;
        /* Dynamic /dev nodes (ptmx, pts/N, dri/card0) are not in the VFS
         * tree: mirror newfstatat's fb_lookup_device fast path so statx on
         * /dev/pts/N agrees with fstatat instead of ENOENT. */
        if (strncmp(kpath, "/dev/", 5) == 0) {
            node = fb_lookup_device(kpath + 5);
            node_owned = false; /* fb node: no extra ref to drop */
        }
        if (!node) {
            vfs_node_t *base_dir = fs_root;
            if (kpathp[0] != '/') {
                if ((int)dirfd == AT_FDCWD) {
                    base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
                    if (!base_dir) base_dir = fs_root;
                    else base_owned = base_dir;
                } else {
                    if (!FD_IN_RANGE(t, dirfd) || !t->fds[dirfd]) {
                        if (base_owned) vfs_close(base_owned);
                        return (uint64_t)-9;
                    }
                    base_dir = t->fds[dirfd];
                }
            }
            if ((flags & AT_SYMLINK_NOFOLLOW) && kpathp[0] != '/') {
                node = vfs_resolve_symlink_node(base_dir, kpathp);
            } else if ((flags & AT_SYMLINK_NOFOLLOW) && kpathp[0] == '/') {
                node = vfs_resolve_symlink_node(fs_root, kpathp);
            } else {
                node = vfs_resolve_path_at(base_dir, kpathp);
            }
            node_owned = node != NULL;
        }
    } else {
        if (flags & AT_EMPTY_PATH) {
            if ((int)dirfd == AT_FDCWD) {
                node = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!node) node = fs_root;
                else node_owned = true;
            } else {
                if (!FD_IN_RANGE(t, dirfd) || !t->fds[dirfd])
                    return (uint64_t)-9;
                node = t->fds[dirfd];
            }
        } else {
            /* Empty string without AT_EMPTY_PATH: ENOENT like Linux, not
             * EFAULT (which is reserved for a NULL pointer). */
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-2;
        }
    }
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }

    /* Build the result in kernel memory, then copy it out: direct stores to
     * the user buffer fault without extable cover on some paths. */
    struct statx kstx;
    memset(&kstx, 0, sizeof(kstx));
    kstx.stx_mask    = STATX_BASIC_STATS;
    /* libudev's path canonicalization requires mount identity. IDs are
     * never reused, so the same VFS identity also satisfies UNIQUE. */
    kstx.stx_mnt_id = node->mount_id ? node->mount_id : 1;
    if (mask & STATX_MNT_ID_UNIQUE)
        kstx.stx_mask |= STATX_MNT_ID_UNIQUE;
    else
        kstx.stx_mask |= STATX_MNT_ID;
    kstx.stx_blksize = 4096;
    kstx.stx_nlink   = node->get_nlink ? node->get_nlink(node) : 1;
    kstx.stx_uid     = node->uid;
    kstx.stx_gid     = node->gid;

    uint32_t mode = node->mask & 07777;
    switch (node->flags & FS_TYPE_MASK) {
    case FS_FILE:      mode |= 0100000; break;
    case FS_DIRECTORY: mode |= 0040000; break;
    case FS_CHARDEV:   mode |= 0020000; break;
    case FS_BLOCKDEV:  mode |= 0060000; break;
    case FS_SYMLINK:   mode |= 0120000; break;
    default:           mode |= 0100000; break;
    }
    kstx.stx_mode   = (uint16_t)mode;
    kstx.stx_ino    = node->inode;
    kstx.stx_size   = node->length;
    kstx.stx_blocks = (node->length + 511) / 512;
    kstx.stx_atime.tv_sec = (int64_t)node->atime;
    kstx.stx_mtime.tv_sec = (int64_t)node->mtime;
    kstx.stx_ctime.tv_sec = (int64_t)node->ctime;
    /* Device numbers for Qt's mount matching (see fill_kstat): block devices
     * report their own number as rdev, files on block filesystems report
     * their filesystem's number as dev. */
    if ((node->flags & FS_TYPE_MASK) == FS_CHARDEV) {
        kstx.stx_rdev_major = (node->inode >> 8) & 0xfffU;
        kstx.stx_rdev_minor = (node->inode & 0xffU) |
                             ((node->inode >> 12) & 0xfff00U);
    } else if ((node->flags & FS_TYPE_MASK) == FS_BLOCKDEV && node->device) {
        struct block_device *bd = (struct block_device *)node->device;
        if (bd && (bd->linux_major || bd->linux_minor)) {
            kstx.stx_rdev_major = bd->linux_major;
            kstx.stx_rdev_minor = bd->linux_minor;
        }
    } else if (((node->flags & FS_TYPE_MASK) == FS_FILE ||
                (node->flags & FS_TYPE_MASK) == FS_DIRECTORY) &&
               node->device && node->statfs &&
               (node->statfs == ext4_statfs_impl ||
                node->statfs == ext2_statfs_impl ||
                node->statfs == fat32_statfs_impl)) {
        struct block_device *bd = *(struct block_device **)node->device;
        if (bd && (bd->linux_major || bd->linux_minor)) {
            kstx.stx_dev_major = bd->linux_major;
            kstx.stx_dev_minor = bd->linux_minor;
        }
    }
    if (node_owned) vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    if (copy_to_user((void *)statxbuf_ptr, &kstx, sizeof(kstx)) != 0)
        return (uint64_t)-14;
    return 0;
}

// ---------------------------------------------------------------------------
// newfstatat
// ---------------------------------------------------------------------------

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t statbuf_ptr, uint64_t flags,
                                uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    struct kstat *ks  = (struct kstat *)statbuf_ptr;
    if (!ks) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
        if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd]))
            return (uint64_t)-9;
        fill_kstat(ks, t->fds[dirfd]);
        return 0;
    }
    if (!path) return (uint64_t)-14;

    vfs_node_t *base_dir = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            if (t->cwd_path[0]) {
                base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base_dir) base_dir = fs_root;
                else base_owned = base_dir;
            }
        } else {
            if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd]))
                return (uint64_t)-9;
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        }
    }

    vfs_node_t *node = NULL;
    bool node_owned = false;
    if (strncmp(path, "/dev/", 5) == 0)
        node = fb_lookup_device((char *)path + 5);

    if (!node) {
        node = (flags & AT_SYMLINK_NOFOLLOW)
               ? vfs_resolve_symlink_node(base_dir, path)
               : vfs_resolve_path_at(base_dir, path);
        node_owned = node != NULL;
        if (!node) {
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-2;
        }
    }

    fill_kstat(ks, node);
    if (node_owned) vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return 0;
}

// ---------------------------------------------------------------------------
// getdents / getdents64
// ---------------------------------------------------------------------------

struct linux_dirent64 {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

struct linux_dirent {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    char     d_name[];
};

static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
    if (t->fd_flags[fd] & O_PATH) return (uint64_t)-9; // EBADF: O_PATH fds are not readable
    if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;

    uint8_t *buf = (uint8_t *)dirp;
    if (!buf || !is_user_ptr((uint64_t)buf)) return (uint64_t)-14;

    size_t   written = 0;
    uint32_t index   = t->fd_offsets[fd];

    while (1) {
        struct dirent *de = vfs_readdir(node, index);
        if (!de) break;

        size_t name_len   = strlen(de->name);
        size_t entry_size = sizeof(struct linux_dirent64) + name_len + 1;
        entry_size = (entry_size + 7) & ~7;
        if (written + entry_size > count) break;

        struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + written);
        memset(buf + written, 0, entry_size);
        entry->d_ino    = de->ino;
        entry->d_off    = (uint64_t)(index + 1);
        entry->d_reclen = (uint16_t)entry_size;
        /* The backend already classified the entry; asking the VFS to resolve
         * every name again was the quadratic part of a directory listing. */
        entry->d_type   = de->d_type;

        strcpy(entry->d_name, de->name);
        written += entry_size;
        index++;
    }

    t->fd_offsets[fd] = index;
    return written;
}

static uint64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
    if (t->fd_flags[fd] & O_PATH) return (uint64_t)-9; // EBADF: O_PATH fds are not readable
    if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;

    uint8_t *buf = (uint8_t *)dirp;
    if (!buf || !vmm_is_user_addr_range_valid(dirp, count)) return (uint64_t)-14;

    size_t   written = 0;
    uint32_t index   = t->fd_offsets[fd];

    while (1) {
        struct dirent *de = vfs_readdir(node, index);
        if (!de) break;
        size_t name_len   = strlen(de->name);
        size_t entry_size = 8 + 8 + 2 + name_len + 2;
        entry_size = (entry_size + 7) & ~7;
        if (written + entry_size > count) break;

        struct linux_dirent *entry = (struct linux_dirent *)(buf + written);
        memset(buf + written, 0, entry_size);
        entry->d_ino    = de->ino;
        entry->d_off    = (uint64_t)(index + 1);
        entry->d_reclen = (uint16_t)entry_size;
        strcpy(entry->d_name, de->name);
        written += entry_size;
        index++;
    }

    t->fd_offsets[fd] = index;
    return written;
}

// ---------------------------------------------------------------------------
// statfs / fstatfs
// ---------------------------------------------------------------------------

#define PIPEFS_MAGIC 0x50495045
#define SOCKFS_MAGIC 0x534F434B
#define ANON_INODE_FS_MAGIC 0x09041934

/* Non-storage nodes (pipes, sockets, epoll) carry no filesystem handler.
 * Linux reports them as 0-block pseudo filesystems instead of failing, so
 * do the same: truthful "no storage here", not an error retry loop. */
static void statfs_pseudo(struct statfs_buf *buf, int64_t magic) {
    buf->f_type    = magic;
    buf->f_bsize   = 4096;
    buf->f_blocks  = 0;
    buf->f_bfree   = 0;
    buf->f_bavail  = 0;
    buf->f_files   = 0;
    buf->f_ffree   = 0;
    buf->f_fsid[0] = 0;
    buf->f_fsid[1] = 0;
    buf->f_namelen = 255;
    buf->f_frsize  = 4096;
    buf->f_flags   = 0;
}

/* A regular file/dir without a handler is a real coverage gap. The old code
 * filled in fake 2 GiB disk values and returned success, which made df lie.
 * Be loud instead: rate-limited klog plus ENOSYS so the gap gets fixed. */
static uint32_t statfs_nohandler_count;

static int64_t statfs_handle_missing(vfs_node_t *node, struct statfs_buf *buf) {
    uint32_t type = node ? (node->flags & FS_TYPE_MASK) : 0;
    if (type == FS_PIPE)   { statfs_pseudo(buf, PIPEFS_MAGIC); return 0; }
    if (type == FS_SOCKET) { statfs_pseudo(buf, SOCKFS_MAGIC); return 0; }
    if (type == FS_EPOLL)  { statfs_pseudo(buf, ANON_INODE_FS_MAGIC); return 0; }
    if (__atomic_add_fetch(&statfs_nohandler_count, 1, __ATOMIC_RELAXED) <= 8) {
        klog_puts("[STATFS] no handler: type=");
        klog_uint64(type);
        klog_puts(" inode=");
        klog_uint64(node ? (uint64_t)node->inode : 0);
        klog_puts("\n");
    }
    return -38; /* ENOSYS */
}

static uint64_t sys_statfs(uint64_t path_ptr, uint64_t buf_ptr, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path_ptr || !buf_ptr || !is_user_ptr(path_ptr))
        return (uint64_t)-14;
    if (!vmm_is_user_addr_range_valid(buf_ptr, sizeof(struct statfs_buf)))
        return (uint64_t)-14;

    /* Copy the pathname into kernel memory first: the VFS walk below
     * (strlen/strstr per component) must never dereference user memory
     * directly, and the result must be built in a kernel buffer and
     * copied out (see sys_statx) — direct stores to the user buffer
     * fault without extable cover on some paths, which is exactly how
     * Qt's QStorageInfo (statvfs → statfs) ended up with Total 0 B. */
    char kpath[512];
    long klen = strncpy_from_user(kpath, (const char *)path_ptr,
                                  (long)sizeof(kpath));
    if (klen < 0)
        return (uint64_t)-14;
    if ((size_t)klen >= sizeof(kpath))
        return (uint64_t)-36; /* ENAMETOOLONG */
    kpath[sizeof(kpath) - 1] = '\0';

    vfs_node_t *node = vfs_resolve_path(kpath);
    if (!node) return (uint64_t)-2;

    struct statfs_buf kbuf;
    memset(&kbuf, 0, sizeof(kbuf));
    int ret = vfs_statfs(node, &kbuf);
    if (ret != 0)
        ret = (int)statfs_handle_missing(node, &kbuf);
    vfs_close(node);
    if (ret != 0)
        return (uint64_t)ret;
    if (copy_to_user((void *)buf_ptr, &kbuf, sizeof(kbuf)) != 0)
        return (uint64_t)-14;
    return 0;
}

static uint64_t sys_fstatfs(uint64_t fd, uint64_t buf_ptr, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !t->fds[fd]) return (uint64_t)-9;
    if (!buf_ptr ||
        !vmm_is_user_addr_range_valid(buf_ptr, sizeof(struct statfs_buf)))
        return (uint64_t)-14;

    vfs_node_t        *node = t->fds[fd];
    struct statfs_buf kbuf;
    memset(&kbuf, 0, sizeof(kbuf));
    int ret = vfs_statfs(node, &kbuf);
    if (ret != 0)
        ret = (int)statfs_handle_missing(node, &kbuf);
    if (ret != 0)
        return (uint64_t)ret;
    if (copy_to_user((void *)buf_ptr, &kbuf, sizeof(kbuf)) != 0)
        return (uint64_t)-14;
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void syscall_register_stat(void) {
    syscall_register(SYS_STAT,       sys_stat);
    syscall_register(SYS_FSTAT,      sys_fstat);
    syscall_register(SYS_LSTAT,      sys_lstat);
    syscall_register(SYS_NEWFSTATAT, sys_newfstatat);
    syscall_register(SYS_STATX,      sys_statx);
    syscall_register(SYS_GETDENTS64, sys_getdents64);
    syscall_register(SYS_GETDENTS,   sys_getdents);
    syscall_register(SYS_STATFS,     sys_statfs);
    syscall_register(SYS_FSTATFS,    sys_fstatfs);
}
