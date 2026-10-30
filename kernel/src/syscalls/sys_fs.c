// sys_fs.c — Filesystem namespace syscalls:
//   mkdir, mkdirat, unlink, unlinkat, rmdir, rename, symlink, readlink,
//   link, linkat, chmod, chown, fchmod, fchmodat, fchmodat2, fchownat, access,
//   faccessat2, getcwd, chdir, fchdir, utimensat, futimesat, utimes, readlinkat
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/af_unix.h"
#include "syscall.h"
#include "../lib/tsc.h"
#include <stdint.h>

#include "../fs/mount_namespace.h"
#include "../sched/capability.h"
#define fs_root vfs_task_root()

#define AT_REMOVEDIR        0x200
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100  /* fchmodat2 / fstatat: do not follow symlinks */
#endif
#define AT_SYMLINK_FOLLOW   0x400  /* linkat: follow symlinks on old path */
#define AT_EMPTY_PATH       0x1000 /* linkat: use olddirfd as the source if oldpath is "" */
/* faccessat2 flag namespace (shares the 0x200 value with AT_REMOVEDIR,
 * which lives in the unlinkat namespace): use effective IDs for the check. */
#define AT_EACCESS          0x200

// ---------------------------------------------------------------------------
// vfs_resolve_symlink_node — resolve WITHOUT following the final symlink
// ---------------------------------------------------------------------------

vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *base, const char *path) {
    if (!path || !path[0]) return NULL;
    /* Dot components denote directories, not literal ramfs entries. */
    size_t path_len = strlen(path);
    const char *final = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') final = p + 1;
    if (!strcmp(final, ".") || !strcmp(final, "..") ||
        (path_len && path[path_len - 1] == '/'))
        return vfs_resolve_path_at(base ? base : fs_root, path);

    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') last_slash = p;

    vfs_node_t *parent;
    const char *last_comp;
    bool parent_owned = false;

    if (!last_slash) {
        parent    = base ? base : fs_root;
        last_comp = path;
    } else if (last_slash == path) {
        parent    = fs_root;
        last_comp = last_slash + 1;
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        char parent_path[512];
        if (parent_len >= sizeof(parent_path)) return NULL;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        parent = vfs_resolve_path_at(base ? base : fs_root, parent_path);
        if (!parent) return NULL;
        parent_owned = true;
        last_comp = last_slash + 1;
    }

    if (!last_comp || !last_comp[0]) {
        if (!parent_owned)
            vfs_node_ref(parent); // keep the "always owned" contract uniform
        return parent;
    }
    vfs_node_t *node = vfs_finddir(parent, (char *)last_comp);
    if (parent_owned) vfs_close(parent);
    return node;
}

// ---------------------------------------------------------------------------
// resolve_parent_and_name helper
//
// Returns an owned reference to the parent directory (even when the parent is
// fs_root or the caller's cwd, which is reffed here), or NULL.  Callers must
// vfs_close() it on every path.
// ---------------------------------------------------------------------------

vfs_node_t *resolve_parent_and_name(const char *path, char *name_out,
                                     size_t name_size) {
    if (!path) return NULL;
    size_t len = strlen(path);
    if (len == 0 || len >= 256) return NULL;

    struct thread *t = sched_get_current();
    vfs_node_t *base  = (path[0] == '/') ? fs_root
                       : (t->cwd_node ? t->cwd_node : fs_root);

    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') last_slash = p;

    vfs_node_t *parent;
    const char *basename;

    if (last_slash) {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len == 0) {
            parent = fs_root;
            vfs_node_ref(parent);
        } else {
            char parent_path[256];
            if (parent_len >= sizeof(parent_path)) return NULL;
            memcpy(parent_path, path, parent_len);
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base, parent_path);
        }
        basename = last_slash + 1;
    } else {
        parent   = base;
        vfs_node_ref(parent);
        basename = path;
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent) vfs_close(parent);
        return NULL;
    }

    size_t blen = strlen(basename);
    if (blen == 0 || blen >= name_size) {
        vfs_close(parent);
        return NULL;
    }
    strcpy(name_out, basename);
    return parent;
}

// ---------------------------------------------------------------------------
// mkdir / mkdirat
// ---------------------------------------------------------------------------

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                             uint64_t a3, uint64_t a4, uint64_t a5);

static uint64_t sys_mkdir(uint64_t pathname, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_mkdirat((uint64_t)AT_FDCWD, pathname, mode, 0, 0, 0);
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    char clean_path[512];
    if (!is_user_range(path, 1)) return (uint64_t)-14;
    long copied = strncpy_from_user(clean_path, path, sizeof(clean_path));
    if (copied < 0) return (uint64_t)-14;
    if ((size_t)copied >= sizeof(clean_path)) return (uint64_t)-36;
    size_t len = (size_t)copied;
    if (len == 0) return (uint64_t)-2;
    while (len > 1 && clean_path[len - 1] == '/') { clean_path[--len] = '\0'; }
    if (strcmp(clean_path, "/") == 0) return (uint64_t)-17;

    vfs_node_t *base_dir = fs_root;
    vfs_node_t *base_owned = NULL;
    if (clean_path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            base_dir = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else if (FD_IN_RANGE(t, (int)dirfd) && fd_is_valid_node(t->fds[(int)dirfd])) {
            base_dir = t->fds[(int)dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        } else {
            return (uint64_t)-9;
        }
    }

    char parent_path[512], dir_name[256];

    TSC_BEGIN(mkdir_resolve);
    const char *slash = 0;
    for (const char *p = clean_path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base_dir;
    bool parent_owned = false;
    if (slash) {
        size_t parent_len = (size_t)(slash - clean_path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) {
                if (base_owned) vfs_close(base_owned);
                TSC_END(mkdir_resolve);
                return (uint64_t)-14;
            }
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = clean_path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base_dir, parent_path);
            parent_owned = parent != NULL;
        }
        size_t dlen = strlen(slash + 1);
        if (dlen == 0 || dlen >= sizeof(dir_name)) {
            if (parent_owned) vfs_close(parent);
            if (base_owned) vfs_close(base_owned);
            TSC_END(mkdir_resolve);
            return (uint64_t)-22;
        }
        strcpy(dir_name, slash + 1);
    } else {
        if (len >= sizeof(dir_name)) {
            if (base_owned) vfs_close(base_owned);
            TSC_END(mkdir_resolve);
            return (uint64_t)-36; // ENAMETOOLONG
        }
        strcpy(dir_name, clean_path);
    }
    TSC_END(mkdir_resolve);

    if (!parent) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2; // ENOENT
    }
    TSC_BEGIN(mkdir_op);
    if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(mkdir_op);
        return (uint64_t)-20; // ENOTDIR
    }
    vfs_node_t *existing = vfs_finddir(parent, dir_name);
    if (existing) {
        vfs_close(existing);
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(mkdir_op);
        return (uint64_t)-17;
    }
    if (!vfs_access(parent, 3)) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(mkdir_op);
        return (uint64_t)-13;
    }
    mode &= ~t->umask;
    if (parent->mask & 02000) mode |= 02000;
    int mkdir_rc = vfs_mkdir(parent, dir_name, (uint16_t)mode);
    if (mkdir_rc != 0) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(mkdir_op);
        return (uint64_t)(mkdir_rc < 0 ? mkdir_rc : -17);
    }
    vfs_node_t *created = vfs_finddir(parent, dir_name);
    if (created) {
        vfs_chown(created, t->fsuid,
            (parent->mask & 02000) ? parent->gid : t->fsgid);
        vfs_close(created);
    }
    if (parent_owned) vfs_close(parent);
    if (base_owned) vfs_close(base_owned);
    TSC_END(mkdir_op);
    return 0;
}

// ---------------------------------------------------------------------------
// unlink / unlinkat / rmdir
// ---------------------------------------------------------------------------

static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    TSC_BEGIN(unlink_resolve);
    vfs_node_t *base_dir = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            base_dir = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else if (FD_IN_RANGE(t, (int)dirfd) && fd_is_valid_node(t->fds[(int)dirfd])) {
            base_dir = t->fds[(int)dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
                if (base_owned) vfs_close(base_owned);
                TSC_END(unlink_resolve);
                return (uint64_t)-20;
            }
        } else {
            TSC_END(unlink_resolve);
            return (uint64_t)-9;
        }
    }

    char parent_path[256], file_name[128];
    size_t len = strlen(path);
    if (len == 0) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-14;
    }

    const char *slash = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base_dir;
    bool parent_owned = false;
    if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) {
                if (base_owned) vfs_close(base_owned);
                return (uint64_t)-14;
            }
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base_dir, parent_path);
            parent_owned = parent != NULL;
        }
        size_t flen = strlen(slash + 1);
        if (flen == 0 || flen >= sizeof(file_name)) {
            if (parent_owned) vfs_close(parent);
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-22;
        }
        strcpy(file_name, slash + 1);
    } else {
        if (len >= sizeof(file_name)) {
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-36; // ENAMETOOLONG
        }
        strcpy(file_name, path);
    }
    TSC_END(unlink_resolve);

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    TSC_BEGIN(unlink_op);
    vfs_node_t *victim = vfs_finddir(parent, file_name);
    if (!victim) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(unlink_op);
        return (uint64_t)-2;
    }
    if (flags & AT_REMOVEDIR) {
        if ((victim->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
            vfs_close(victim);
            if (parent_owned) vfs_close(parent);
            if (base_owned) vfs_close(base_owned);
            TSC_END(unlink_op);
            return (uint64_t)-20; // ENOTDIR
        }
    } else {
        if ((victim->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
            vfs_close(victim);
            if (parent_owned) vfs_close(parent);
            if (base_owned) vfs_close(base_owned);
            TSC_END(unlink_op);
            return (uint64_t)-21; // EISDIR
        }
    }
    if (!vfs_may_remove(parent, victim)) {
        vfs_close(victim);
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        TSC_END(unlink_op);
        return (uint64_t)-13;
    }

    // Build full path for socket unbinding
    char full_path[256];
    if (path[0] == '/') {
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        if (t->cwd_path[0]) {
            strncpy(full_path, t->cwd_path, sizeof(full_path) - 1);
            full_path[sizeof(full_path) - 1] = '\0';
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
            strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
        } else {
            strncpy(full_path, "/", sizeof(full_path) - 1);
            strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
        }
    }

    unix_unbind_by_path(full_path);
    /* Release the check reference before the backend frees the node: ramfs and
     * tmpfs kfree() the directory entry outright on unlink. */
    vfs_close(victim);
    int result = (flags & AT_REMOVEDIR) ? vfs_rmdir(parent, file_name)
                                        : vfs_unlink(parent, file_name);
    if (result == 0) {
        /* Release the exact (base_dir, path) fast-path entry as well. The VFS
         * layer already dropped (parent, file_name); this covers the aliased
         * key used when the path was resolved (absolute "/tmp/x" via fs_root
         * or multi-component "a/b" via base_dir), so the deleted node+pages
         * are freed immediately instead of lingering until eviction. */
        vfs_path_cache_drop(base_dir, path);
    }
    if (parent_owned) vfs_close(parent);
    if (base_owned) vfs_close(base_owned);
    TSC_END(unlink_op);
    return result == 0 ? 0 : (uint64_t)-2;
}

static uint64_t sys_unlink(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_unlinkat((uint64_t)AT_FDCWD, pathname_ptr, 0, 0, 0, 0);
}

static uint64_t sys_rmdir(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_unlinkat((uint64_t)AT_FDCWD, pathname_ptr, AT_REMOVEDIR, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------

static uint64_t sys_renameat(uint64_t olddfd, uint64_t oldname_ptr,
                             uint64_t newdfd, uint64_t newname_ptr,
                             uint64_t a4, uint64_t a5);

static uint64_t sys_rename(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_renameat((uint64_t)AT_FDCWD, oldpath_ptr, (uint64_t)AT_FDCWD, newpath_ptr, 0, 0);
}

static uint64_t sys_renameat(uint64_t olddfd, uint64_t oldname_ptr,
                             uint64_t newdfd, uint64_t newname_ptr,
                             uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    const char *oldname = (const char *)oldname_ptr;
    const char *newname = (const char *)newname_ptr;
    if (!oldname || !newname) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *old_base = fs_root;
    if (oldname[0] != '/') {
        if ((int)olddfd == AT_FDCWD) {
            old_base = (t->cwd_node ? t->cwd_node : fs_root);
        } else if (FD_IN_RANGE(t, (int)olddfd) && fd_is_valid_node(t->fds[(int)olddfd])) {
            old_base = t->fds[(int)olddfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    }

    vfs_node_t *new_base = fs_root;
    if (newname[0] != '/') {
        if ((int)newdfd == AT_FDCWD) {
            new_base = (t->cwd_node ? t->cwd_node : fs_root);
        } else if (FD_IN_RANGE(t, (int)newdfd) && fd_is_valid_node(t->fds[(int)newdfd])) {
            new_base = t->fds[(int)newdfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    }

    char old_bn[128], new_bn[128];
    TSC_BEGIN(rename_resolve);
    char old_pdir[256];
    size_t old_plen = 0;
    bool old_had_slash = false;
    const char *old_slash = strrchr(oldname, '/');
    vfs_node_t *old_parent = old_base;
    bool old_parent_owned = false;
    if (old_slash) {
        old_had_slash = true;
        size_t plen = (size_t)(old_slash - oldname);
        if (plen == 0) {
            old_parent = fs_root;
        } else {
            if (plen >= sizeof(old_pdir)) return (uint64_t)-14;
            memcpy(old_pdir, oldname, plen);
            old_pdir[plen] = '\0';
            old_plen = plen;
            old_parent = vfs_resolve_path_at(old_base, old_pdir);
            old_parent_owned = old_parent != NULL;
        }
        strncpy(old_bn, old_slash + 1, sizeof(old_bn) - 1);
    } else {
        strncpy(old_bn, oldname, sizeof(old_bn) - 1);
    }

    const char *new_slash = strrchr(newname, '/');
    vfs_node_t *new_parent = new_base;
    bool new_parent_owned = false;
    if (new_slash) {
        char new_pdir[256];
        size_t plen = (size_t)(new_slash - newname);
        if (plen == 0) {
            new_parent = fs_root;
        } else {
            if (plen >= sizeof(new_pdir)) {
                if (old_parent_owned) vfs_close(old_parent);
                return (uint64_t)-14;
            }
            memcpy(new_pdir, newname, plen);
            new_pdir[plen] = '\0';
            /* Same-directory fast path: identical parent dirs share one
             * resolution (the common rename-in-place case).  Bases match
             * whenever both paths are absolute or were resolved from the
             * same base above; comparing the dir strings covers both. */
            if (old_had_slash && old_parent && plen == old_plen &&
                old_plen > 0 && memcmp(new_pdir, old_pdir, plen) == 0 &&
                old_base == new_base) {
                new_parent = old_parent;
            } else {
                new_parent = vfs_resolve_path_at(new_base, new_pdir);
                new_parent_owned = new_parent != NULL;
            }
        }
        strncpy(new_bn, new_slash + 1, sizeof(new_bn) - 1);
    } else {
        strncpy(new_bn, newname, sizeof(new_bn) - 1);
    }
    TSC_END(rename_resolve);

    if (!old_parent || !new_parent) {
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-2; // ENOENT
    }
    TSC_BEGIN(rename_op);
    vfs_node_t *old_node = vfs_finddir(old_parent, old_bn);
    if (!old_node) {
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        TSC_END(rename_op);
        return (uint64_t)-2;
    }

    /* Same directory: classic in-place rename (see sys_rename: inode
     * numbers alone cannot prove two resolutions share a filesystem). */
    if (old_parent == new_parent) {
        int rc = vfs_rename(old_parent, old_bn, new_bn);
        if (rc == 0) {
            vfs_path_cache_drop(old_base, oldname);
            if (new_base != old_base || strcmp(oldname, newname) != 0)
                vfs_path_cache_drop(new_base, newname);
        }
        vfs_close(old_node);
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        TSC_END(rename_op);
        return rc < 0 ? (uint64_t)rc : 0;
    }

    /* Different directories: same-filesystem move when the backend
     * supports it, EXDEV otherwise (see sys_rename). */
    {
        int rc = vfs_rename_cross(old_parent, old_bn, new_parent, new_bn);
        if (rc == 0) {
            vfs_path_cache_drop(old_base, oldname);
            if (new_base != old_base || strcmp(oldname, newname) != 0)
                vfs_path_cache_drop(new_base, newname);
        }
        vfs_close(old_node);
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        TSC_END(rename_op);
        return rc < 0 ? (uint64_t)rc : 0;
    }
}

static uint64_t sys_renameat2(uint64_t olddfd, uint64_t oldname_ptr,
                              uint64_t newdfd, uint64_t newname_ptr,
                              uint64_t flags, uint64_t a5) {
    (void)flags; (void)a5;
    return sys_renameat(olddfd, oldname_ptr, newdfd, newname_ptr, 0, 0);
}

// ---------------------------------------------------------------------------
// symlink / readlink / readlinkat
// ---------------------------------------------------------------------------

static uint64_t do_sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (!target || !linkpath) return (uint64_t)-14;
    if (!linkpath[0]) return (uint64_t)-2;

    struct thread *t = sched_get_current();
    vfs_node_t *base_dir = fs_root;
    if (linkpath[0] != '/') {
        if (newdirfd == AT_FDCWD) {
            base_dir = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else if (FD_IN_RANGE(t, newdirfd) && fd_is_valid_node(t->fds[newdirfd])) {
            base_dir = t->fds[newdirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        } else {
            return (uint64_t)-9;
        }
    }

    char link_name[128];
    TSC_BEGIN(symlink_resolve);
    const char *last_slash = NULL;
    for (const char *p = linkpath; *p; p++)
        if (*p == '/') last_slash = p;

    vfs_node_t *parent = NULL;
    bool parent_owned = false;
    if (last_slash) {
        size_t parent_len = (size_t)(last_slash - linkpath);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            char parent_path[256];
            if (parent_len >= sizeof(parent_path)) {
                TSC_END(symlink_resolve);
                return (uint64_t)-14;
            }
            memcpy(parent_path, linkpath, parent_len);
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base_dir, parent_path);
            parent_owned = parent != NULL;
        }
        size_t nlen = strlen(last_slash + 1);
        if (nlen == 0 || nlen >= sizeof(link_name)) {
            if (parent_owned) vfs_close(parent);
            TSC_END(symlink_resolve);
            return (uint64_t)-22;
        }
        strcpy(link_name, last_slash + 1);
    } else {
        if (strlen(linkpath) >= sizeof(link_name)) {
            TSC_END(symlink_resolve);
            return (uint64_t)-36;
        }
        strcpy(link_name, linkpath);
        parent = base_dir;
    }
    TSC_END(symlink_resolve);

    if (!parent) return (uint64_t)-2;
    TSC_BEGIN(symlink_op);
    if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        TSC_END(symlink_op);
        return (uint64_t)-20;
    }
    if (!vfs_access(parent, 3)) {
        if (parent_owned) vfs_close(parent);
        TSC_END(symlink_op);
        return (uint64_t)-13;
    }
    vfs_node_t *existing = vfs_finddir(parent, link_name);
    if (existing) {
        vfs_close(existing);
        if (parent_owned) vfs_close(parent);
        TSC_END(symlink_op);
        return (uint64_t)-17;
    }

    char target_buf[256];
    size_t t_len = strlen(target);
    if (t_len == 0 || t_len >= sizeof(target_buf)) {
        if (parent_owned) vfs_close(parent);
        TSC_END(symlink_op);
        return (uint64_t)-14;
    }
    strcpy(target_buf, target);

    int rc = vfs_symlink(parent, link_name, target_buf) == 0 ? 0 : (uint64_t)-1;
    if (parent_owned) vfs_close(parent);
    TSC_END(symlink_op);
    return rc;
}

static uint64_t sys_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                             uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return do_sys_symlinkat((const char *)target_ptr, AT_FDCWD, (const char *)linkpath_ptr);
}

static uint64_t sys_symlinkat(uint64_t target_ptr, uint64_t newdirfd,
                              uint64_t linkpath_ptr, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return do_sys_symlinkat((const char *)target_ptr, (int)newdirfd, (const char *)linkpath_ptr);
}

// ---------------------------------------------------------------------------
// mknod
// ---------------------------------------------------------------------------

/* Linux mknod(2).  AvoryOS has no major/minor driver registry: the created
 * node carries the type so stat() reports it and the rdev is kept in inode
 * for userland that inspects it.  Services such as dmesg create /dev/kmsg
 * this way; without the syscall they fail with ENOSYS. */
static uint64_t sys_mknod(uint64_t pathname_ptr, uint64_t mode, uint64_t dev,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;

    uint32_t type;
    switch ((uint32_t)mode & 0170000u) {
        case 0000000u: type = FS_FILE;     break; // regular
        case 0010000u: type = FS_PIPE;     break; // S_IFIFO
        case 0020000u: type = FS_CHARDEV;  break; // S_IFCHR
        case 0060000u: type = FS_BLOCKDEV; break; // S_IFBLK
        default:       return (uint64_t)-22;      // EINVAL (dirs use mkdir)
    }

    char name[128];
    vfs_node_t *parent =
        resolve_parent_and_name(path, name, sizeof(name));
    if (!parent) return (uint64_t)-2;
    if (!vfs_access(parent, 3)) {
        vfs_close(parent);
        return (uint64_t)-13;
    }
    vfs_node_t *existing = vfs_finddir(parent, name);
    if (existing) {
        vfs_close(existing);
        vfs_close(parent);
        return (uint64_t)-17; // EEXIST
    }

    int rc = vfs_mknod(parent, name, (uint16_t)(mode & 07777), type, NULL);
    if (rc == 0 && (type == FS_CHARDEV || type == FS_BLOCKDEV)) {
        vfs_node_t *created = vfs_finddir(parent, name);
        if (created) {
            created->inode = (uint32_t)dev; // st_rdev for stat()
            vfs_close(created);
        }
    }
    vfs_close(parent);
    return rc == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_mknodat(uint64_t dirfd, uint64_t pathname_ptr, uint64_t mode,
                            uint64_t dev, uint64_t a4, uint64_t a5) {
    (void)dirfd; (void)a4; (void)a5;
    return sys_mknod(pathname_ptr, mode, dev, 0, 0, 0);
}

static uint64_t do_sys_readlink_at(vfs_node_t *base, const char *path,
                                    char *buf, uint64_t bufsiz);

static uint64_t sys_readlink(uint64_t pathname_ptr, uint64_t buf_ptr,
                              uint64_t bufsiz, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    char *buf        = (char *)buf_ptr;
    if (!path || !buf) return (uint64_t)-14;
    if (bufsiz == 0)   return (uint64_t)-22;

    if (strcmp(path, "/proc/self/exe") == 0) {
        struct thread *ct = sched_get_current();
        const char *exe_path = (ct && ct->exe_path[0]) ? ct->exe_path : "/init";
        size_t len = strlen(exe_path);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, exe_path, len);
        return len;
    }

    struct thread *t = sched_get_current();
    vfs_node_t *base  = (t && t->cwd_node && path[0] != '/') ? t->cwd_node : fs_root;

    return do_sys_readlink_at(base, path, buf, bufsiz);
}

// Shared resolver for readlink/readlinkat: base is already selected
// (fs_root, cwd, or an open dirfd). Returns byte count or a negative errno
// (propagated verbatim: dead-pid/fd ENOENT must not become EINVAL).
static uint64_t do_sys_readlink_at(vfs_node_t *base, const char *path,
                                    char *buf, uint64_t bufsiz) {
    vfs_node_t *node = vfs_resolve_symlink_node(base, path);
    if (!node) {
        return (uint64_t)-2;
    }
    if ((node->flags & FS_TYPE_MASK) != FS_SYMLINK) {
        vfs_close(node);
        return (uint64_t)-22;
    }

    int ret = vfs_readlink(node, buf, (uint32_t)bufsiz);
    vfs_close(node);
    if (ret < 0) return (uint64_t)ret;

    return (uint64_t)ret;
}

static uint64_t sys_readlinkat(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t buf_ptr, uint64_t bufsiz,
                                uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    char *buf        = (char *)buf_ptr;
    if (!path || !buf) return (uint64_t)-14;
    if (bufsiz == 0)   return (uint64_t)-22;

    // Absolute paths ignore dirfd, exactly like Linux.
    if (path[0] == '/' || (int)dirfd == AT_FDCWD)
        return sys_readlink(pathname_ptr, buf_ptr, bufsiz, 0, 0, 0);

    // Relative to an open directory fd (libudev/libdrm pass device-dir fds
    // with relative names like "subsystem" or "driver").
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd]))
        return (uint64_t)-9; // EBADF
    vfs_node_t *base = t->fds[dirfd];
    /* Linux readlinkat(fd, "", ...) reads an O_PATH|O_NOFOLLOW link.
     * libudev uses this while canonicalizing /sys/class aliases. */
    if (path[0] == '\0') {
        if ((base->flags & FS_TYPE_MASK) != FS_SYMLINK)
            return (uint64_t)-22;
        return (uint64_t)vfs_readlink(base, buf, (uint32_t)bufsiz);
    }
    if ((base->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    return do_sys_readlink_at(base, path, buf, bufsiz);
}

// ---------------------------------------------------------------------------
// link
// ---------------------------------------------------------------------------

static uint64_t sys_link(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *oldpath = (const char *)oldpath_ptr;
    const char *newpath = (const char *)newpath_ptr;
    if (!oldpath || !newpath) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    vfs_node_t *base = (oldpath[0] == '/') ? fs_root
                       : (t && t->cwd_node ? t->cwd_node : fs_root);
    vfs_node_t *src = vfs_resolve_path_at(base, oldpath);
    if (!src) return (uint64_t)-2;
    if ((src->flags & FS_TYPE_MASK) != FS_FILE) {
        vfs_close(src);
        return (uint64_t)-1;
    }

    // Only the destination *basename* has to fit in file_name[].  The old
    // check compared strlen(newpath) against sizeof(file_name), so any link
    // whose full destination path was >= 128 bytes failed with ENAMETOOLONG
    // even though the name itself was short.  That broke every WebKit cache
    // link: .../WebKitCache/Version 17/Records/<40 hex>/Resource/<40
    // hex>-blob is ~150 bytes with a 45-byte basename.
    char file_name[128];
    const char *dst_slash = NULL;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') dst_slash = p;
    const char *dst_base = dst_slash ? dst_slash + 1 : newpath;
    if (!dst_base[0] || strlen(dst_base) >= sizeof(file_name)) {
        vfs_close(src);
        return (uint64_t)-36; // ENAMETOOLONG
    }

    vfs_node_t *parent =
        resolve_parent_and_name(newpath, file_name, sizeof(file_name));

    if (!parent) {
        vfs_close(src);
        return (uint64_t)-2; // ENOENT
    }
    if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        vfs_close(parent);
        vfs_close(src);
        return (uint64_t)-20; // ENOTDIR
    }
    if (!vfs_access(parent, 3)) {
        vfs_close(parent);
        vfs_close(src);
        return (uint64_t)-13;
    }
    vfs_node_t *existing = vfs_finddir(parent, file_name);
    if (existing) {
        vfs_close(existing);
        vfs_close(parent);
        vfs_close(src);
        return (uint64_t)-17;
    }
    if (vfs_create(parent, file_name, src->mask & 0777) != 0) {
        vfs_close(parent);
        vfs_close(src);
        return (uint64_t)-1;
    }

    vfs_node_t *dst = vfs_finddir(parent, file_name);
    if (dst && src->length > 0) {
        uint8_t buf[512];
        uint32_t offset = 0;
        while (offset < src->length) {
            uint32_t chunk = src->length - offset;
            if (chunk > sizeof(buf)) chunk = sizeof(buf);
            uint32_t rd = vfs_read(src, offset, chunk, buf);
            if (rd == 0) break;
            vfs_write(dst, offset, rd, buf);
            offset += rd;
        }
    }
    if (dst) vfs_close(dst);
    vfs_close(parent);
    vfs_close(src);
    return 0;
}

// ---------------------------------------------------------------------------
// linkat (265)
// linkat(olddirfd, oldpath, newdirfd, newpath, flags)
//   flags: AT_SYMLINK_FOLLOW (0x400) – follow symlinks on oldpath (default)
//          AT_EMPTY_PATH     (0x1000) – use olddirfd node directly when oldpath==""
// ---------------------------------------------------------------------------

static uint64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath_ptr,
                            uint64_t newdirfd, uint64_t newpath_ptr,
                            uint64_t flags, uint64_t a5) {
    (void)a5;
    const char *oldpath = (const char *)oldpath_ptr;
    const char *newpath = (const char *)newpath_ptr;
    if (!oldpath || !newpath) return (uint64_t)-14; // EFAULT

    struct thread *t = sched_get_current();

    // ---- Resolve source node -----------------------------------------------
    vfs_node_t *src = NULL;
    bool src_owned = false;

    if ((flags & AT_EMPTY_PATH) && oldpath[0] == '\0') {
        // Use the fd referred to by olddirfd directly as the source.
        if ((int32_t)olddirfd == AT_FDCWD) {
            src = t ? t->cwd_node : fs_root;
        } else if (FD_IN_RANGE(t, olddirfd) && fd_is_valid_node(t->fds[olddirfd])) {
            src = t->fds[olddirfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    } else {
        // Normal path resolution relative to olddirfd.
        vfs_node_t *old_base;
        if (oldpath[0] == '/') {
            old_base = fs_root;
        } else if ((int32_t)olddirfd == AT_FDCWD) {
            old_base = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else if (FD_IN_RANGE(t, olddirfd) && fd_is_valid_node(t->fds[olddirfd])) {
            old_base = t->fds[olddirfd];
            if ((old_base->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20; // ENOTDIR
        } else {
            return (uint64_t)-9; // EBADF
        }

        // AT_SYMLINK_FOLLOW: follow the final symlink (vfs_resolve_path_at already does).
        // Without AT_SYMLINK_FOLLOW we'd need to avoid following the final component,
        // but hard-linking a symlink is EPERM on Linux anyway, so either way we
        // resolve the symlink target for the source node.
        src = vfs_resolve_path_at(old_base, oldpath);
        if (!src) return (uint64_t)-2; // ENOENT
        src_owned = true;
    }

    // Hard-linking directories is not allowed.
    if ((src->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
        if (src_owned) vfs_close(src);
        return (uint64_t)-1; // EPERM
    }

    // ---- Resolve destination parent + name ---------------------------------
    vfs_node_t *new_base;
    if (newpath[0] == '/') {
        new_base = fs_root;
    } else if ((int32_t)newdirfd == AT_FDCWD) {
        new_base = (t && t->cwd_node) ? t->cwd_node : fs_root;
    } else if (FD_IN_RANGE(t, newdirfd) && fd_is_valid_node(t->fds[newdirfd])) {
        new_base = t->fds[newdirfd];
        if ((new_base->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
            if (src_owned) vfs_close(src);
            return (uint64_t)-20; // ENOTDIR
        }
    } else {
        if (src_owned) vfs_close(src);
        return (uint64_t)-9; // EBADF
    }

    // file_name[] only has to hold the destination *basename*; the full
    // newpath may be much longer (see the comment in sys_link).  The length
    // is validated below, right before the basename is copied into file_name.
    char file_name[128];

    // Temporarily override the cwd so resolve_parent_and_name picks up new_base.
    // Instead, parse the new path manually the same way sys_link does.
    const char *slash = NULL;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *new_parent;
    const char *basename;
    bool new_parent_owned = false;

    if (slash) {
        size_t parent_len = (size_t)(slash - newpath);
        if (parent_len == 0) {
            new_parent = fs_root;
        } else {
            char parent_path[256];
            if (parent_len >= sizeof(parent_path)) {
                if (src_owned) vfs_close(src);
                return (uint64_t)-36;
            }
            memcpy(parent_path, newpath, parent_len);
            parent_path[parent_len] = '\0';
            new_parent = vfs_resolve_path_at(new_base, parent_path);
            new_parent_owned = new_parent != NULL;
        }
        basename = slash + 1;
    } else {
        new_parent = new_base;
        basename   = newpath;
    }

    size_t blen = strlen(basename);
    if (blen == 0 || blen >= sizeof(file_name)) {
        if (src_owned) vfs_close(src);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-36;
    }
    memcpy(file_name, basename, blen + 1);

    if (!new_parent) {
        if (src_owned) vfs_close(src);
        return (uint64_t)-2; // ENOENT
    }
    if ((new_parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (src_owned) vfs_close(src);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-20; // ENOTDIR
    }
    if (!vfs_access(new_parent, 3)) {
        if (src_owned) vfs_close(src);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-13;  // EACCES
    }
    vfs_node_t *existing = vfs_finddir(new_parent, file_name);
    if (existing) {
        vfs_close(existing);
        if (src_owned) vfs_close(src);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-17; // EEXIST
    }

    // ---- Create the destination entry and copy data ------------------------
    if (vfs_create(new_parent, file_name, src->mask & 0777) != 0) {
        if (src_owned) vfs_close(src);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-1;
    }

    vfs_node_t *dst = vfs_finddir(new_parent, file_name);
    if (dst && src->length > 0) {
        uint8_t buf[512];
        uint32_t offset = 0;
        while (offset < src->length) {
            uint32_t chunk = src->length - offset;
            if (chunk > sizeof(buf)) chunk = sizeof(buf);
            uint32_t rd = vfs_read(src, offset, chunk, buf);
            if (rd == 0) break;
            vfs_write(dst, offset, rd, buf);
            offset += rd;
        }
    }
    if (dst) vfs_close(dst);
    if (src_owned) vfs_close(src);
    if (new_parent_owned) vfs_close(new_parent);
    return 0;
}

// ---------------------------------------------------------------------------
// chmod / chown / fchmod / fchmodat / fchownat
// ---------------------------------------------------------------------------

static uint64_t sys_chmod(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = (t && t->cwd_node && path[0] != '/') ? t->cwd_node : fs_root;
    vfs_node_t *base_owned = NULL;
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    if (t && !capability_has(t, CAP_FOWNER) && t->fsuid != node->uid) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-1;
    }
    int rc = vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

/* Return an owned node for metadata operations. Empty paths address the fd
 * itself (including O_PATH handles); nofollow applies only to the final link. */
static vfs_node_t *metadata_node_at(int dirfd, const char *path, unsigned flags,
                                    int *error) {
    struct thread *t = sched_get_current();
    if (!path) { *error = -14; return NULL; }
    if (!path[0] && !(flags & AT_EMPTY_PATH)) {
        *error = -2;
        return NULL;
    }
    vfs_node_t *base;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        base = path[0] == '/' ? fs_root
               : (t && t->cwd_node ? t->cwd_node : fs_root);
        vfs_node_ref(base);
    } else {
        base = fd_node_get(t, (uint64_t)(unsigned)dirfd);
        if (!base) { *error = -9; return NULL; }
    }
    if (!path[0]) return base;
    if ((base->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        vfs_close(base);
        *error = -20;
        return NULL;
    }
    vfs_node_t *node = (flags & AT_SYMLINK_NOFOLLOW)
        ? vfs_resolve_symlink_node(base, path)
        : vfs_resolve_path_at(base, path);
    vfs_close(base);
    if (!node) *error = -2;
    return node;
}

static uint64_t chown_node(vfs_node_t *node, uint32_t owner, uint32_t group) {
    struct thread *t = sched_get_current();
    uint32_t uid = owner == UINT32_MAX ? node->uid : owner;
    uint32_t gid = group == UINT32_MAX ? node->gid : group;
    if (t && !capability_has(t, CAP_CHOWN) &&
        (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid)))
        return (uint64_t)-1;
    if ((uid != node->uid || gid != node->gid) && (node->mask & 06000) &&
        (node->flags & FS_TYPE_MASK) != FS_SYMLINK)
        if (vfs_chmod(node, (uint16_t)(node->mask & ~06000)) != 0)
            return (uint64_t)-1;
    return vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t do_chownat(int dirfd, const char *path, uint32_t owner,
                            uint32_t group, unsigned flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return (uint64_t)-22;
    int error = 0;
    vfs_node_t *node = metadata_node_at(dirfd, path, flags, &error);
    if (!node) return (uint64_t)error;
    uint64_t result = chown_node(node, owner, group);
    vfs_close(node);
    return result;
}

static uint64_t sys_chown(uint64_t pathname_ptr, uint64_t owner,
                           uint64_t group, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return do_chownat(AT_FDCWD, (const char *)pathname_ptr, owner, group, 0);
}

static uint64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    vfs_node_t *node = fd_node_get(sched_get_current(), fd);
    if (!node) return (uint64_t)-9;
    uint64_t result = chown_node(node, owner, group);
    vfs_close(node);
    return result;
}

static uint64_t sys_lchown(uint64_t pathname_ptr, uint64_t owner,
                            uint64_t group, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return do_chownat(AT_FDCWD, (const char *)pathname_ptr, owner, group,
                      AT_SYMLINK_NOFOLLOW);
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd])) return (uint64_t)-9;
    if (!capability_has(t, CAP_FOWNER) && t->fsuid != t->fds[fd]->uid) return (uint64_t)-1;
    if (vfs_chmod(t->fds[fd], (uint16_t)mode) != 0) return (uint64_t)-1;
    return 0;
}

static uint64_t sys_fchmodat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t mode, uint64_t flags, uint64_t a4,
                              uint64_t a5) {
    (void)flags; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            base = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else {
            if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd])) return (uint64_t)-9;
            base = t->fds[dirfd];
        }
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    if (t && !capability_has(t, CAP_FOWNER) && t->fsuid != node->uid) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-1;
    }
    int rc = vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

// fchmodat2 — Linux 6.6+ (syscall 452)
// Identical to fchmodat(268) but the flags argument is officially supported.
// AT_SYMLINK_NOFOLLOW (0x100): do not chmod a symlink's target; instead operate
// on the symlink node itself (which on most filesystems is a no-op/EOPNOTSUPP).
static uint64_t sys_fchmodat2(uint64_t dirfd, uint64_t pathname_ptr,
                               uint64_t mode, uint64_t flags,
                               uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return (uint64_t)-22;
    int error = 0;
    vfs_node_t *node = metadata_node_at((int)dirfd,
        (const char *)pathname_ptr, (unsigned)flags, &error);
    if (!node) return (uint64_t)error;
    struct thread *t = sched_get_current();
    uint64_t result;
    if ((node->flags & FS_TYPE_MASK) == FS_SYMLINK)
        result = (uint64_t)-95; // Linux does not change symlink permissions.
    else if (t && !capability_has(t, CAP_FOWNER) && t->fsuid != node->uid)
        result = (uint64_t)-1;
    else
        result = vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    return result;
}

static uint64_t sys_fchownat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t owner, uint64_t group, uint64_t flags,
                              uint64_t a5) {
    (void)a5;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return (uint64_t)-22;
    return do_chownat((int)dirfd, (const char *)pathname_ptr, owner, group,
                      (unsigned)flags);
}

// ---------------------------------------------------------------------------
// access / faccessat2
// ---------------------------------------------------------------------------

static uint64_t do_sys_access(int dirfd, const char *path, uint64_t mode,
                               int flags) {
    struct thread *t = sched_get_current();
    if (t) t->last_access_nonexec_probe = false;
    // faccessat2 accepts only these flag bits (access/faccessat pass 0);
    // anything else is EINVAL, matching Linux.
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH))
        return (uint64_t)-22;
    if (!path) return (uint64_t)-14;
    // AT_EACCESS selects effective-ID semantics, which is what vfs_access()
    // already keys off (fsuid/fsgid). Plain access() per POSIX uses real
    // IDs; for setuid processes this is a known minor deviation (behavior
    // matches EACCESS). Non-setuid processes are unaffected (ids equal).

    vfs_node_t *node  = NULL;
    bool node_owned = false;
    vfs_node_t *base_owned = NULL;

    // AT_EMPTY_PATH with an empty path operates on dirfd itself — the O_PATH
    // fd accessibility probe. AT_FDCWD + "" means the cwd. The node is
    // borrowed (fd table / fs_root) except for the resolved-cwd case.
    if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
        if (dirfd == AT_FDCWD) {
            node = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else {
            if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd]))
                return (uint64_t)-9; // EBADF
            node = t->fds[dirfd]; // borrowed; must not close
        }
    } else if (path[0] == '/') {
        node = (flags & AT_SYMLINK_NOFOLLOW)
               ? vfs_resolve_symlink_node(fs_root, path)
               : vfs_resolve_path_at(fs_root, path);
        node_owned = node != NULL;
    } else if (strncmp(path, "/dev/", 5) == 0) {
        node = fb_lookup_device((char *)path + 5);
    }

    if (!node) {
        vfs_node_t *base_dir = fs_root;
        if (dirfd == AT_FDCWD) {
            base_dir = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else {
            if (!FD_IN_RANGE(t, dirfd) || !fd_is_valid_node(t->fds[dirfd]))
                return (uint64_t)-9;
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        }
        node = (flags & AT_SYMLINK_NOFOLLOW)
               ? vfs_resolve_symlink_node(base_dir, path)
               : vfs_resolve_path_at(base_dir, path);
        node_owned = node != NULL;
    }
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }

    if (mode & ~7) {
        if (node_owned) vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-22;
    }
    if (mode && !vfs_access(node, (uint32_t)mode)) {
        /* A negative executable-file query is normal Qt metadata collection.
         * Tag only a resolved regular file with no execute bits, never a
         * directory search denial or a read/write failure. Diagnostics must
         * not resolve paths again to infer this after releasing the node. */
        if (t && mode == 1 && (node->flags & FS_TYPE_MASK) == FS_FILE &&
            !(node->mask & 0111))
            t->last_access_nonexec_probe = true;
        if (node_owned) vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-13;
    }
    if (node_owned) vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return 0;
}

static uint64_t sys_access(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return do_sys_access(AT_FDCWD, (const char *)pathname_ptr, mode, 0);
}

static uint64_t sys_faccessat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t mode, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return do_sys_access((int)dirfd, (const char *)pathname_ptr, mode, 0);
}

static uint64_t sys_faccessat2(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t mode, uint64_t flags, uint64_t a4,
                                uint64_t a5) {
    (void)a4; (void)a5;
    return do_sys_access((int)dirfd, (const char *)pathname_ptr, mode, (int)flags);
}

// ---------------------------------------------------------------------------
// chdir / fchdir
// ---------------------------------------------------------------------------

static uint64_t sys_fchdir(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd])) return (uint64_t)-9;
    if ((t->fds[fd]->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;
    if (!vfs_access(t->fds[fd], 1)) return (uint64_t)-13;

    const char *fd_path = fd_path_value(t, (int)fd);
    if (fd_path && fd_path[0]) {
        strncpy(t->cwd_path, fd_path, sizeof(t->cwd_path) - 1);
        t->cwd_path[sizeof(t->cwd_path) - 1] = '\0';
        if (t->cwd_node) vfs_close(t->cwd_node);
        t->cwd_node = t->fds[fd];
        vfs_open(t->cwd_node);
        return 0;
    }
    return (uint64_t)-9;
}

// ---------------------------------------------------------------------------
// chroot: change the task root used by every absolute and symlink lookup.
// ---------------------------------------------------------------------------

static uint64_t sys_chroot(uint64_t path_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    if (!path_ptr) return (uint64_t)-14; // EFAULT
    char path[512];
    if (!is_user_range((void *)path_ptr, 1)) return (uint64_t)-14;
    long copied = strncpy_from_user(path, (void *)path_ptr, sizeof(path));
    if (copied < 0) return (uint64_t)-14;
    if ((uint64_t)copied >= sizeof(path)) return (uint64_t)-36;
    size_t len = (size_t)copied;
    if (len == 0) return (uint64_t)-2; // ENOENT
    if (len >= 4096) return (uint64_t)-36; // ENAMETOOLONG
    struct thread *task = sched_get_current();
    if (!capability_has(task, CAP_SYS_CHROOT)) return (uint64_t)-1;
    vfs_node_t *node = vfs_resolve_path_at(task && task->cwd_node ? task->cwd_node : fs_root, path);
    if (!node) return (uint64_t)-2;
    int result = (node->flags & FS_TYPE_MASK) == FS_DIRECTORY ? vfs_namespace_chroot(node) : -20;
    vfs_close(node);
    return (uint64_t)result;
}

// ---------------------------------------------------------------------------
// pivot_root: change the root mount
// ---------------------------------------------------------------------------
static uint64_t sys_pivot_root(uint64_t new_root_ptr, uint64_t put_old_ptr,
                               uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!new_root_ptr || !put_old_ptr) return (uint64_t)-14; // EFAULT
    char new_root[512], put_old[512];
    if (!is_user_range((void *)new_root_ptr, 1) || !is_user_range((void *)put_old_ptr, 1)) return (uint64_t)-14;
    long n = strncpy_from_user(new_root, (void *)new_root_ptr, sizeof(new_root));
    if (n < 0) return (uint64_t)-14;
    if ((uint64_t)n >= sizeof(new_root)) return (uint64_t)-36;
    n = strncpy_from_user(put_old, (void *)put_old_ptr, sizeof(put_old));
    if (n < 0) return (uint64_t)-14;
    if ((uint64_t)n >= sizeof(put_old)) return (uint64_t)-36;

    struct thread *task = sched_get_current();
    if (!capability_has(task, CAP_SYS_ADMIN)) return (uint64_t)-1;
    vfs_node_t *base = task && task->cwd_node ? task->cwd_node : fs_root;
    vfs_node_t *n_node = vfs_resolve_path_at(base, new_root);
    if (!n_node) return (uint64_t)-2;
    vfs_node_t *o_node = vfs_resolve_path_at(base, put_old);
    if (!o_node) { vfs_close(n_node); return (uint64_t)-2; }
    int result = ((n_node->flags & FS_TYPE_MASK) != FS_DIRECTORY ||
                  (o_node->flags & FS_TYPE_MASK) != FS_DIRECTORY) ? -20
                   : vfs_namespace_pivot(n_node, o_node);
    vfs_close(n_node); vfs_close(o_node);
    return (uint64_t)result;
}

// ---------------------------------------------------------------------------
// utimensat / futimesat / utimes / utime
// ---------------------------------------------------------------------------

struct sys_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct sys_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct sys_utimbuf {
    int64_t actime;
    int64_t modtime;
};

#define UTIMENS_NOW  ((1l << 30) - 1l)
#define UTIMENS_OMIT ((1l << 30) - 2l)

static int vfs_set_node_times(vfs_node_t *node, uint32_t atime, uint32_t mtime) {
    if (!node) return -1;
    if (vfs_mount_readonly(node)) return -30;
    struct thread *task = sched_get_current();
    if (task && task->fsuid != node->uid && !capability_has(task, CAP_FOWNER) && !vfs_access(node, 2)) return -13;
    node->atime = atime;
    node->mtime = mtime;
    node = vfs_backing_node(node);
    node->atime = atime; node->mtime = mtime;

    extern int ext2_statfs_impl(vfs_node_t *, struct statfs_buf *);
    extern int ext4_statfs_impl(vfs_node_t *, struct statfs_buf *);
    if (node->statfs && (node->statfs == ext2_statfs_impl || node->statfs == ext4_statfs_impl)) {
        extern int ext2_utime_impl(vfs_node_t *node, uint32_t atime, uint32_t mtime);
        ext2_utime_impl(node, atime, mtime);
    }
    return 0;
}

static uint64_t do_utimensat_impl(uint64_t dirfd, uint64_t pathname,
                                  const struct sys_timespec *ts_in,
                                  bool have_times, uint64_t flags) {
    if (pathname && !is_user_ptr(pathname))
        return (uint64_t)-14; // EFAULT
    const char *path = (const char *)pathname;
    struct thread *t = sched_get_current();
    vfs_node_t *node = NULL;
    vfs_node_t *base_owned = NULL;

    if (!path || path[0] == '\0') {
        if ((int)dirfd == AT_FDCWD) {
            node = (t && t->cwd_node) ? t->cwd_node : fs_root;
            vfs_node_ref(node);
        } else if (FD_IN_RANGE(t, (int)dirfd) && fd_is_valid_node(t->fds[(int)dirfd])) {
            node = t->fds[(int)dirfd];
            vfs_node_ref(node);
        } else {
            return (uint64_t)-9; // EBADF
        }
    } else {
        vfs_node_t *base = fs_root;
        if (path[0] != '/') {
            if ((int)dirfd == AT_FDCWD) {
                base = (t && t->cwd_node) ? t->cwd_node : fs_root;
            } else {
                if (!FD_IN_RANGE(t, (int)dirfd) || !fd_is_valid_node(t->fds[(int)dirfd]))
                    return (uint64_t)-9; // EBADF
                base = t->fds[(int)dirfd];
            }
        }
        if (flags & AT_SYMLINK_NOFOLLOW) {
            node = vfs_resolve_symlink_node(base, path);
        } else {
            node = vfs_resolve_path_at(base, (char *)path);
        }
        if (base_owned)
            vfs_close(base_owned);
        if (!node)
            return (uint64_t)-2; // ENOENT
    }

    extern uint64_t rtc_get_boot_timestamp(void);
    extern uint64_t lapic_timer_get_ms(void);
    uint64_t now_sec = rtc_get_boot_timestamp() + (lapic_timer_get_ms() / 1000);
    uint32_t atime = node->atime;
    uint32_t mtime = node->mtime;

    if (!have_times || !ts_in) {
        atime = (uint32_t)now_sec;
        mtime = (uint32_t)now_sec;
    } else {
        if (ts_in[0].tv_nsec == UTIMENS_NOW) {
            atime = (uint32_t)now_sec;
        } else if (ts_in[0].tv_nsec != UTIMENS_OMIT) {
            atime = (uint32_t)ts_in[0].tv_sec;
        }
        if (ts_in[1].tv_nsec == UTIMENS_NOW) {
            mtime = (uint32_t)now_sec;
        } else if (ts_in[1].tv_nsec != UTIMENS_OMIT) {
            mtime = (uint32_t)ts_in[1].tv_sec;
        }
    }

    int result = vfs_set_node_times(node, atime, mtime);
    vfs_close(node);
    return (uint64_t)result;
}

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t flags, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (times) {
        if (!is_user_ptr(times)) return (uint64_t)-14;
        struct sys_timespec ts[2];
        if (copy_from_user(ts, (const void *)times, sizeof(ts)) != 0)
            return (uint64_t)-14;
        return do_utimensat_impl(dirfd, pathname, ts, true, flags);
    }
    return do_utimensat_impl(dirfd, pathname, NULL, false, flags);
}

static uint64_t sys_futimesat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (!pathname) return (uint64_t)-14;
    if (times) {
        if (!is_user_ptr(times)) return (uint64_t)-14;
        struct sys_timeval tv[2];
        if (copy_from_user(tv, (const void *)times, sizeof(tv)) != 0)
            return (uint64_t)-14;
        struct sys_timespec ts[2];
        ts[0].tv_sec = tv[0].tv_sec;
        ts[0].tv_nsec = tv[0].tv_usec * 1000;
        ts[1].tv_sec = tv[1].tv_sec;
        ts[1].tv_nsec = tv[1].tv_usec * 1000;
        return do_utimensat_impl(dirfd, pathname, ts, true, 0);
    }
    return do_utimensat_impl(dirfd, pathname, NULL, false, 0);
}

static uint64_t sys_utimes(uint64_t pathname, uint64_t times, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_futimesat((uint64_t)AT_FDCWD, pathname, times, 0, 0, 0);
}

static uint64_t sys_utime(uint64_t pathname, uint64_t times, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!pathname) return (uint64_t)-14;
    if (times) {
        if (!is_user_ptr(times)) return (uint64_t)-14;
        struct sys_utimbuf ub;
        if (copy_from_user(&ub, (const void *)times, sizeof(ub)) != 0)
            return (uint64_t)-14;
        struct sys_timespec ts[2];
        ts[0].tv_sec = ub.actime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = ub.modtime;
        ts[1].tv_nsec = 0;
        return sys_utimensat((uint64_t)AT_FDCWD, pathname, (uint64_t)ts, 0, 0, 0);
    }
    return sys_utimensat((uint64_t)AT_FDCWD, pathname, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// name_to_handle_at / open_by_handle_at
// Returning -EOPNOTSUPP (95) tells elogind/systemd to fallback to stat/fstat.
// ---------------------------------------------------------------------------

static uint64_t sys_name_to_handle_at(uint64_t dfd, uint64_t name, uint64_t handle,
                                      uint64_t mount_id, uint64_t flags, uint64_t a5) {
    (void)dfd; (void)name; (void)handle; (void)mount_id; (void)flags; (void)a5;
    return (uint64_t)(-(int64_t)95);
}

static uint64_t sys_open_by_handle_at(uint64_t mountdirfd, uint64_t handle, uint64_t flags,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)mountdirfd; (void)handle; (void)flags; (void)a3; (void)a4; (void)a5;
    return (uint64_t)(-(int64_t)95);
}

// New mount API (fsopen/fsmount/fsconfig/move_mount/open_tree/fspick/
// mount_setattr): no filesystem-context infrastructure yet. Explicit
// -ENOSYS so libmount falls back to legacy mount(2); see syscall.h.
static uint64_t sys_nosys_mount_api(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)(-(int64_t)38);
}

// ---------------------------------------------------------------------------
// Disk Quota syscalls (quotactl, quotactl_fd)
// ---------------------------------------------------------------------------

// quotactl: syscall 179
static uint64_t sys_quotactl(uint64_t cmd_val, uint64_t special_ptr,
                            uint64_t id_val, uint64_t addr_ptr,
                            uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    uint32_t cmd = (uint32_t)cmd_val;
    uint32_t subcmd = cmd >> 8;
    (void)id_val;

    // Validate special pathname if provided
    if (special_ptr) {
        if (!is_user_ptr(special_ptr))
            return (uint64_t)-14; // EFAULT
    } else {
        // Q_SYNC allows a NULL special to sync all filesystems; other commands require special
        if (subcmd != 0x800001 /* Q_SYNC */ && subcmd != (('X' << 8) | 7) /* Q_XQUOTASYNC */)
            return (uint64_t)-14; // EFAULT
    }

    // Validate addr pointer if command returns data to user
    if (subcmd == 0x800007 /* Q_GETQUOTA */ ||
        subcmd == 0x800005 /* Q_GETINFO */ ||
        subcmd == 0x800009 /* Q_GETNEXTQUOTA */ ||
        subcmd == (('X' << 8) | 3) /* Q_XGETQUOTA */ ||
        subcmd == (('X' << 8) | 5) /* Q_XGETQSTAT */ ||
        subcmd == (('X' << 8) | 8) /* Q_XGETQSTATV */ ||
        subcmd == (('X' << 8) | 9) /* Q_XGETNEXTQUOTA */) {
        if (!addr_ptr || !is_user_ptr(addr_ptr))
            return (uint64_t)-14; // EFAULT
        // Quotas are not active on this filesystem.
        // Returning ESRCH indicates to callers (Firefox Cache2, glibc, coreutils)
        // that no quota limits are configured on this filesystem.
        return (uint64_t)-3; // ESRCH
    }

    if (subcmd == 0x800001 /* Q_SYNC */ || subcmd == (('X' << 8) | 7) /* Q_XQUOTASYNC */) {
        return 0; // No-op success for syncing unmetered filesystems
    }

    struct thread *current = sched_get_current();
    if (current && current->euid != 0) {
        return (uint64_t)-1; // EPERM for administrative quota commands
    }

    return (uint64_t)-95; // ENOTSUP / EOPNOTSUPP: Quota operations not supported
}

// quotactl_fd: syscall 443
static uint64_t sys_quotactl_fd(uint64_t fd_val, uint64_t cmd_val,
                               uint64_t id_val, uint64_t addr_ptr,
                               uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    struct thread *current = sched_get_current();
    if (!current)
        return (uint64_t)-9; // EBADF
    int fd = (int)fd_val;
    if (!FD_IN_RANGE(current, fd) || !fd_is_valid_node(current->fds[fd]))
        return (uint64_t)-9; // EBADF

    uint32_t cmd = (uint32_t)cmd_val;
    uint32_t subcmd = cmd >> 8;
    (void)id_val;

    if (subcmd == 0x800007 /* Q_GETQUOTA */ ||
        subcmd == 0x800005 /* Q_GETINFO */ ||
        subcmd == 0x800009 /* Q_GETNEXTQUOTA */ ||
        subcmd == (('X' << 8) | 3) /* Q_XGETQUOTA */ ||
        subcmd == (('X' << 8) | 5) /* Q_XGETQSTAT */ ||
        subcmd == (('X' << 8) | 8) /* Q_XGETQSTATV */ ||
        subcmd == (('X' << 8) | 9) /* Q_XGETNEXTQUOTA */) {
        if (!addr_ptr || !is_user_ptr(addr_ptr))
            return (uint64_t)-14; // EFAULT
        return (uint64_t)-3; // ESRCH
    }

    if (subcmd == 0x800001 /* Q_SYNC */ || subcmd == (('X' << 8) | 7) /* Q_XQUOTASYNC */) {
        return 0;
    }

    if (current->euid != 0) {
        return (uint64_t)-1; // EPERM
    }

    return (uint64_t)-95; // ENOTSUP
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void syscall_register_fs(void) {
    syscall_register(SYS_MKDIR,      sys_mkdir);
    syscall_register(SYS_MKDIRAT,    sys_mkdirat);
    syscall_register(SYS_MKNOD,      sys_mknod);
    syscall_register(SYS_MKNODAT,    sys_mknodat);
    syscall_register(SYS_UNLINK,     sys_unlink);
    syscall_register(SYS_UNLINKAT,   sys_unlinkat);
    syscall_register(SYS_RMDIR,      sys_rmdir);
    syscall_register(SYS_RENAME,     sys_rename);
    syscall_register(SYS_RENAMEAT,   sys_renameat);
    syscall_register(SYS_RENAMEAT2,  sys_renameat2);
    syscall_register(SYS_SYMLINK,    sys_symlink);
    syscall_register(SYS_SYMLINKAT,  sys_symlinkat);
    syscall_register(SYS_READLINK,   sys_readlink);
    syscall_register(SYS_READLINKAT, sys_readlinkat);
    syscall_register(SYS_LINK,       sys_link);
    syscall_register(SYS_LINKAT,     sys_linkat);
    syscall_register(SYS_CHMOD,      sys_chmod);
    syscall_register(SYS_CHOWN,      sys_chown);
    syscall_register(SYS_FCHOWN,     sys_fchown);
    syscall_register(SYS_LCHOWN,     sys_lchown);
    syscall_register(SYS_FCHMOD,     sys_fchmod);
    syscall_register(SYS_FCHMODAT,   sys_fchmodat);
    syscall_register(SYS_FCHMODAT2,  sys_fchmodat2);
    syscall_register(SYS_FCHOWNAT,   sys_fchownat);
    syscall_register(SYS_ACCESS,     sys_access);
    syscall_register(SYS_FACCESSAT,  sys_faccessat);
    syscall_register(SYS_FACCESSAT2, sys_faccessat2);
    syscall_register(SYS_FCHDIR,     sys_fchdir);
    syscall_register(SYS_PIVOT_ROOT, sys_pivot_root);
    syscall_register(SYS_CHROOT,     sys_chroot);
    syscall_register(SYS_UTIMENSAT,  sys_utimensat);
    syscall_register(SYS_FUTIMESAT,  sys_futimesat);
    syscall_register(SYS_UTIMES,     sys_utimes);
    syscall_register(SYS_UTIME,      sys_utime);
    syscall_register(SYS_NAME_TO_HANDLE_AT, sys_name_to_handle_at);
    syscall_register(SYS_OPEN_BY_HANDLE_AT, sys_open_by_handle_at);
    syscall_register(SYS_OPEN_TREE,   sys_nosys_mount_api);
    syscall_register(SYS_MOVE_MOUNT,  sys_nosys_mount_api);
    syscall_register(SYS_FSOPEN,      sys_nosys_mount_api);
    syscall_register(SYS_FSCONFIG,    sys_nosys_mount_api);
    syscall_register(SYS_FSMOUNT,     sys_nosys_mount_api);
    syscall_register(SYS_FSPICK,      sys_nosys_mount_api);
    syscall_register(SYS_QUOTACTL,    sys_quotactl);
    syscall_register(SYS_QUOTACTL_FD, sys_quotactl_fd);
}
