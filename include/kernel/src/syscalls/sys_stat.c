// sys_stat.c — File metadata syscalls:
//   stat, fstat, lstat, newfstatat, statx, statfs, fstatfs,
//   getdents, getdents64
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// fill_kstat — convert a vfs_node into a kstat structure
// ---------------------------------------------------------------------------

void fill_kstat(struct kstat *ks, vfs_node_t *node) {
    ks->st_dev   = 0;
    ks->st_ino   = (uint64_t)node->inode;
    ks->st_nlink = 1;

    uint32_t mode = node->mask & 07777;
    switch (node->flags & FS_TYPE_MASK) {
    case FS_FILE:      mode |= 0100000; break;
    case FS_DIRECTORY: mode |= 0040000; break;
    case FS_CHARDEV:   mode |= 0020000; break;
    case FS_BLOCKDEV:  mode |= 0060000; break;
    case FS_SYMLINK:   mode |= 0120000; break;
    case FS_SOCKET:    mode |= 0140000; break;
    default:           mode |= 0100000; break;
    }
    ks->st_mode  = mode;
    ks->st_uid   = node->uid;
    ks->st_gid   = node->gid;
    ks->__pad0   = 0;
    ks->st_rdev  = 0;
    if ((node->flags & FS_TYPE_MASK) == FS_CHARDEV ||
        (node->flags & FS_TYPE_MASK) == FS_BLOCKDEV)
        ks->st_rdev = (uint64_t)node->inode;

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
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

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
    struct statx *stx = (struct statx *)statxbuf_ptr;
    if (!stx || !is_user_ptr((uint64_t)stx)) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *node = NULL;
    bool node_owned = false;
    vfs_node_t *base_owned = NULL;
    if (path && path[0] != '\0') {
        vfs_node_t *base_dir = fs_root;
        if (path[0] != '/') {
            if ((int)dirfd == AT_FDCWD) {
                base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base_dir) base_dir = fs_root;
                else base_owned = base_dir;
            } else {
                if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
                    return (uint64_t)-9;
                base_dir = t->fds[dirfd];
            }
        }
        node = vfs_resolve_path_at(base_dir, path);
        node_owned = node != NULL;
    } else {
        if (flags & AT_EMPTY_PATH) {
            if ((int)dirfd == AT_FDCWD) {
                node = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!node) node = fs_root;
                else node_owned = true;
            } else {
                if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
                    return (uint64_t)-9;
                node = t->fds[dirfd];
            }
        } else {
            return (uint64_t)-14;
        }
    }
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }

    memset(stx, 0, sizeof(struct statx));
    stx->stx_mask    = STATX_BASIC_STATS;
    stx->stx_blksize = 4096;
    stx->stx_nlink   = 1;
    stx->stx_uid     = node->uid;
    stx->stx_gid     = node->gid;

    uint32_t mode = node->mask & 07777;
    switch (node->flags & FS_TYPE_MASK) {
    case FS_FILE:      mode |= 0100000; break;
    case FS_DIRECTORY: mode |= 0040000; break;
    case FS_CHARDEV:   mode |= 0020000; break;
    case FS_BLOCKDEV:  mode |= 0060000; break;
    case FS_SYMLINK:   mode |= 0120000; break;
    default:           mode |= 0100000; break;
    }
    stx->stx_mode   = (uint16_t)mode;
    stx->stx_ino    = node->inode;
    stx->stx_size   = node->length;
    stx->stx_blocks = (node->length + 511) / 512;
    stx->stx_atime.tv_sec = (int64_t)node->atime;
    stx->stx_mtime.tv_sec = (int64_t)node->mtime;
    stx->stx_ctime.tv_sec = (int64_t)node->ctime;
    if (node_owned) vfs_close(node);
    if (base_owned) vfs_close(base_owned);
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
        if ((int)dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
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
            if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
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
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
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
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
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

static uint64_t sys_statfs(uint64_t path_ptr, uint64_t buf_ptr, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path_ptr || !buf_ptr ||
        !is_user_ptr(path_ptr) || !is_user_ptr(buf_ptr))
        return (uint64_t)-14;

    struct statfs_buf *buf = (struct statfs_buf *)buf_ptr;
    vfs_node_t *node = vfs_resolve_path((const char *)path_ptr);
    if (!node) return (uint64_t)-2;

    int ret = vfs_statfs(node, buf);
    if (ret != 0) {
        /* Fallback: 2 GiB total disk, ~200 MiB used (rough estimate).
         * f_bsize = 4096, f_blocks = 2 GiB / 4096 = 524288 blocks
         * f_bfree / f_bavail = (2 GiB - 200 MiB) / 4096 ~ 471040 blocks */
        buf->f_type    = 0x61657673;
        buf->f_bsize   = 4096;
        buf->f_blocks  = 524288;   /* 2 GiB total */
        buf->f_bfree   = 471040;   /* ~1.8 GiB free */
        buf->f_bavail  = 471040;
        buf->f_files   = 10000;
        buf->f_ffree   = 5000;
        buf->f_fsid[0] = 1;
        buf->f_fsid[1] = 0;
        buf->f_namelen = 255;
        buf->f_frsize  = 4096;
        buf->f_flags   = 0;
    }
    vfs_close(node);
    return 0;
}

static uint64_t sys_fstatfs(uint64_t fd, uint64_t buf_ptr, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    if (!buf_ptr || !is_user_ptr(buf_ptr))  return (uint64_t)-14;

    struct statfs_buf *buf  = (struct statfs_buf *)buf_ptr;
    vfs_node_t        *node = t->fds[fd];
    int ret = vfs_statfs(node, buf);
    if (ret != 0) {
        /* Fallback: 2 GiB total disk, ~200 MiB used (rough estimate).
         * f_bsize = 4096, f_blocks = 2 GiB / 4096 = 524288 blocks
         * f_bfree / f_bavail = (2 GiB - 200 MiB) / 4096 ~ 471040 blocks */
        buf->f_type    = 0x61657673;
        buf->f_bsize   = 4096;
        buf->f_blocks  = 524288;   /* 2 GiB total */
        buf->f_bfree   = 471040;   /* ~1.8 GiB free */
        buf->f_bavail  = 471040;
        buf->f_files   = 10000;
        buf->f_ffree   = 5000;
        buf->f_fsid[0] = 1;
        buf->f_fsid[1] = 0;
        buf->f_namelen = 255;
        buf->f_frsize  = 4096;
        buf->f_flags   = 0;
    }
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
