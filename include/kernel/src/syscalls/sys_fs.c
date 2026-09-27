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
#include <stdint.h>

#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_NOFOLLOW 0x100  /* fchmodat2 / fstatat: do not follow symlinks */
#define AT_SYMLINK_FOLLOW   0x400  /* linkat: follow symlinks on old path */
#define AT_EMPTY_PATH       0x1000 /* linkat: use olddirfd as the source if oldpath is "" */

// ---------------------------------------------------------------------------
// vfs_resolve_symlink_node — resolve WITHOUT following the final symlink
// ---------------------------------------------------------------------------

vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *base, const char *path) {
    if (!path || !path[0]) return NULL;

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

static uint64_t sys_mkdir(uint64_t pathname, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    char clean_path[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(clean_path)) return (uint64_t)-14;
    strcpy(clean_path, path);
    while (len > 1 && clean_path[len - 1] == '/') { clean_path[--len] = '\0'; }
    if (strcmp(clean_path, "/") == 0) return 0;

    struct thread *t = sched_get_current();
    vfs_node_t *base  = (clean_path[0] == '/') ? fs_root
                       : (t->cwd_node ? t->cwd_node : fs_root);

    char parent_path[256], dir_name[128];
    const char *slash = 0;
    for (const char *p = clean_path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base;
    bool parent_owned = false;
    if (slash) {
        size_t parent_len = (size_t)(slash - clean_path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) return (uint64_t)-14;
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = clean_path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base, parent_path);
            if (!parent) {
                sys_mkdir((uint64_t)parent_path, mode, 0, 0, 0, 0);
                parent = vfs_resolve_path_at(base, parent_path);
            }
            parent_owned = parent != NULL;
        }
        size_t dlen = strlen(slash + 1);
        if (dlen == 0 || dlen >= sizeof(dir_name)) {
            if (parent_owned) vfs_close(parent);
            return (uint64_t)-22;
        }
        strcpy(dir_name, slash + 1);
    } else {
        strcpy(dir_name, clean_path);
    }

    if (!parent)
        return (uint64_t)-2; // ENOENT
    if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-20; // ENOTDIR
    }
    vfs_node_t *existing = vfs_finddir(parent, dir_name);
    if (existing) {
        vfs_close(existing);
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-17;
    }
    if (!vfs_access(parent, 3)) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-13;
    }
    mode &= ~t->umask;
    if (parent->mask & 02000) mode |= 02000;

    if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-17;
    }
    vfs_node_t *created = vfs_finddir(parent, dir_name);
    if (created) {
        vfs_chown(created, t->fsuid,
            (parent->mask & 02000) ? parent->gid : t->fsgid);
        vfs_close(created);
    }
    if (parent_owned) vfs_close(parent);
    return 0;
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *base_dir = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int64_t)dirfd == AT_FDCWD) {
            base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
            if (!base_dir) base_dir = fs_root;
            else base_owned = base_dir;
        } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        } else {
            return (uint64_t)-9;
        }
    }

    char parent_path[128], dir_name[128];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dir_name)) {
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
        size_t dlen = strlen(slash + 1);
        if (dlen == 0 || dlen >= sizeof(dir_name)) {
            if (parent_owned) vfs_close(parent);
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-22;
        }
        strcpy(dir_name, slash + 1);
    } else {
        strcpy(dir_name, path);
    }

    if (!parent) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2; // ENOENT
    }
    if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-20; // ENOTDIR
    }
    vfs_node_t *existing = vfs_finddir(parent, dir_name);
    if (existing) {
        vfs_close(existing);
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-17;
    }
    if (!vfs_access(parent, 3)) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-13;
    }
    mode &= ~t->umask;
    if (parent->mask & 02000) mode |= 02000;
    if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-17;
    }
    vfs_node_t *created = vfs_finddir(parent, dir_name);
    if (created) {
        vfs_chown(created, t->fsuid,
            (parent->mask & 02000) ? parent->gid : t->fsgid);
        vfs_close(created);
    }
    if (parent_owned) vfs_close(parent);
    if (base_owned) vfs_close(base_owned);
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

    vfs_node_t *base_dir = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int64_t)dirfd == AT_FDCWD) {
            base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
            if (!base_dir) base_dir = fs_root;
            else base_owned = base_dir;
        } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
                if (base_owned) vfs_close(base_owned);
                return (uint64_t)-20;
            }
        } else {
            return (uint64_t)-9;
        }
    }

    char parent_path[128], file_name[128];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(file_name)) {
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
        strcpy(file_name, path);
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    vfs_node_t *victim = vfs_finddir(parent, file_name);
    if (!victim) {
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    if (!vfs_may_remove(parent, victim)) {
        vfs_close(victim);
        if (parent_owned) vfs_close(parent);
        if (base_owned) vfs_close(base_owned);
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
    if (parent_owned) vfs_close(parent);
    if (base_owned) vfs_close(base_owned);
    return result == 0 ? 0 : (uint64_t)-2;
}

static uint64_t sys_unlink(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;

    char name[128];
    vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
    if (!parent) return (uint64_t)-2;

    vfs_node_t *target = vfs_finddir(parent, name);
    if (!target) {
        vfs_close(parent);
        return (uint64_t)-2;
    }
    if ((target->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
        vfs_close(target);
        vfs_close(parent);
        return (uint64_t)-21;
    }
    if (!vfs_may_remove(parent, target)) {
        vfs_close(target);
        vfs_close(parent);
        return (uint64_t)-13;
    }

    struct thread *t = sched_get_current();
    if (t) {
        for (int i = 0; i < MAX_FDS; i++) {
            if (t->fds[i] == target) {
                t->fds[i] = NULL;
                t->fd_offsets[i] = 0;
            }
        }
    }
    unix_unbind_by_path(path);
    /* The finddir reference must be dropped before the backend unlink: ramfs
     * and tmpfs kfree() the node outright, not via the refcount. */
    vfs_close(target);
    int rc = vfs_unlink(parent, name) == 0 ? 0 : (uint64_t)-1;
    vfs_close(parent);
    return rc;
}

static uint64_t sys_rmdir(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;

    char name[128];
    vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
    if (!parent) return (uint64_t)-2;

    vfs_node_t *target = vfs_finddir(parent, name);
    if (!target) {
        vfs_close(parent);
        return (uint64_t)-2;
    }
    if ((target->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        vfs_close(target);
        vfs_close(parent);
        return (uint64_t)-20;
    }
    if (!vfs_may_remove(parent, target)) {
        vfs_close(target);
        vfs_close(parent);
        return (uint64_t)-13;
    }
    /* Drop the check reference first: the backend frees the node on rmdir. */
    vfs_close(target);
    int rc = vfs_rmdir(parent, name) == 0 ? 0 : (uint64_t)-1;
    vfs_close(parent);
    return rc;
}

// ---------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------

static uint64_t sys_rename(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *oldpath = (const char *)oldpath_ptr;
    const char *newpath = (const char *)newpath_ptr;
    if (!oldpath || !newpath) return (uint64_t)-14;

    char old_name[128], new_name[128];
    vfs_node_t *old_parent = resolve_parent_and_name(oldpath, old_name, sizeof(old_name));
    vfs_node_t *new_parent = resolve_parent_and_name(newpath, new_name, sizeof(new_name));
    if (!old_parent) {
        if (new_parent) vfs_close(new_parent);
        return (uint64_t)-2;
    }
    if (!new_parent) {
        vfs_close(old_parent);
        return (uint64_t)-2;
    }

    vfs_node_t *old_node = vfs_finddir(old_parent, old_name);
    if (!old_node) {
        vfs_close(old_parent);
        vfs_close(new_parent);
        return (uint64_t)-2;
    }
    if (!vfs_may_remove(old_parent, old_node) || !vfs_access(new_parent, 3)) {
        vfs_close(old_node);
        vfs_close(old_parent);
        vfs_close(new_parent);
        return (uint64_t)-13;
    }

    if (old_parent == new_parent || old_parent->inode == new_parent->inode) {
        int rc = vfs_rename(old_parent, old_name, new_name) == 0 ? 0 : (uint64_t)-1;
        vfs_close(old_node);
        vfs_close(old_parent);
        vfs_close(new_parent);
        return rc;
    }
    vfs_close(old_node);
    vfs_close(old_parent);
    vfs_close(new_parent);
    return (uint64_t)-18; // EXDEV
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
        if ((int64_t)olddfd == AT_FDCWD) {
            old_base = (t->cwd_node ? t->cwd_node : fs_root);
        } else if (olddfd < MAX_FDS && t->fds[olddfd]) {
            old_base = t->fds[olddfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    }

    vfs_node_t *new_base = fs_root;
    if (newname[0] != '/') {
        if ((int64_t)newdfd == AT_FDCWD) {
            new_base = (t->cwd_node ? t->cwd_node : fs_root);
        } else if (newdfd < MAX_FDS && t->fds[newdfd]) {
            new_base = t->fds[newdfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    }

    char old_bn[128], new_bn[128];
    const char *old_slash = strrchr(oldname, '/');
    vfs_node_t *old_parent = old_base;
    bool old_parent_owned = false;
    if (old_slash) {
        char old_pdir[256];
        size_t plen = (size_t)(old_slash - oldname);
        if (plen == 0) {
            old_parent = fs_root;
        } else {
            if (plen >= sizeof(old_pdir)) return (uint64_t)-14;
            memcpy(old_pdir, oldname, plen);
            old_pdir[plen] = '\0';
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
            new_parent = vfs_resolve_path_at(new_base, new_pdir);
            new_parent_owned = new_parent != NULL;
        }
        strncpy(new_bn, new_slash + 1, sizeof(new_bn) - 1);
    } else {
        strncpy(new_bn, newname, sizeof(new_bn) - 1);
    }

    if (!old_parent || !new_parent) {
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-2; // ENOENT
    }
    vfs_node_t *old_node = vfs_finddir(old_parent, old_bn);
    if (!old_node) {
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        return (uint64_t)-2;
    }

    if (old_parent == new_parent || old_parent->inode == new_parent->inode) {
        int rc = vfs_rename(old_parent, old_bn, new_bn) == 0 ? 0 : (uint64_t)-1;
        vfs_close(old_node);
        if (old_parent_owned) vfs_close(old_parent);
        if (new_parent_owned) vfs_close(new_parent);
        return rc;
    }

    vfs_close(old_node);
    if (old_parent_owned) vfs_close(old_parent);
    if (new_parent_owned) vfs_close(new_parent);
    return (uint64_t)-18; // EXDEV
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

static uint64_t sys_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                             uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *target   = (const char *)target_ptr;
    const char *linkpath = (const char *)linkpath_ptr;
    if (!target || !linkpath) return (uint64_t)-14;

    char link_name[128];
    vfs_node_t *parent = resolve_parent_and_name(linkpath, link_name, sizeof(link_name));
    if (!parent) return (uint64_t)-2;
    if (!vfs_access(parent, 3)) {
        vfs_close(parent);
        return (uint64_t)-13;
    }
    vfs_node_t *existing = vfs_finddir(parent, link_name);
    if (existing) {
        vfs_close(existing);
        vfs_close(parent);
        return (uint64_t)-17;
    }

    char target_buf[256];
    size_t t_len = strlen(target);
    if (t_len == 0 || t_len >= sizeof(target_buf)) {
        vfs_close(parent);
        return (uint64_t)-14;
    }
    strcpy(target_buf, target);

    int rc = vfs_symlink(parent, link_name, target_buf) == 0 ? 0 : (uint64_t)-1;
    vfs_close(parent);
    return rc;
}

static uint64_t sys_symlinkat(uint64_t target_ptr, uint64_t newdirfd,
                              uint64_t linkpath_ptr, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    (void)newdirfd; (void)a3; (void)a4; (void)a5;
    return sys_symlink(target_ptr, linkpath_ptr, 0, 0, 0, 0);
}

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
    vfs_node_t *base  = fs_root;
    vfs_node_t *cwd   = NULL;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        cwd = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (cwd) base = cwd;
    }

    vfs_node_t *node = vfs_resolve_symlink_node(base, path);
    if (!node) {
        if (cwd) vfs_close(cwd);
        return (uint64_t)-2;
    }
    if ((node->flags & FS_TYPE_MASK) != FS_SYMLINK) {
        vfs_close(node);
        if (cwd) vfs_close(cwd);
        return (uint64_t)-22;
    }

    int ret = vfs_readlink(node, buf, (uint32_t)bufsiz);
    vfs_close(node);
    if (cwd) vfs_close(cwd);
    if (ret < 0) return (uint64_t)-22;

    return (uint64_t)ret;
}

static uint64_t sys_readlinkat(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t buf_ptr, uint64_t bufsiz,
                                uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    // For now, ignore dirfd and delegate to sys_readlink
    (void)dirfd;
    return sys_readlink(pathname_ptr, buf_ptr, bufsiz, 0, 0, 0);
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

    char file_name[128];
    size_t len = strlen(newpath);
    if (len == 0 || len >= sizeof(file_name)) {
        vfs_close(src);
        return (uint64_t)-36;
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
        if ((int64_t)olddirfd == AT_FDCWD) {
            src = t ? t->cwd_node : fs_root;
        } else if (olddirfd < MAX_FDS && t && t->fds[olddirfd]) {
            src = t->fds[olddirfd];
        } else {
            return (uint64_t)-9; // EBADF
        }
    } else {
        // Normal path resolution relative to olddirfd.
        vfs_node_t *old_base;
        if (oldpath[0] == '/') {
            old_base = fs_root;
        } else if ((int64_t)olddirfd == AT_FDCWD) {
            old_base = (t && t->cwd_node) ? t->cwd_node : fs_root;
        } else if (olddirfd < MAX_FDS && t && t->fds[olddirfd]) {
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
    } else if ((int64_t)newdirfd == AT_FDCWD) {
        new_base = (t && t->cwd_node) ? t->cwd_node : fs_root;
    } else if (newdirfd < MAX_FDS && t && t->fds[newdirfd]) {
        new_base = t->fds[newdirfd];
        if ((new_base->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
            if (src_owned) vfs_close(src);
            return (uint64_t)-20; // ENOTDIR
        }
    } else {
        if (src_owned) vfs_close(src);
        return (uint64_t)-9; // EBADF
    }

    char file_name[128];
    size_t len = strlen(newpath);
    if (len == 0 || len >= sizeof(file_name)) {
        if (src_owned) vfs_close(src);
        return (uint64_t)-36; // ENAMETOOLONG
    }

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
    vfs_node_t *base  = fs_root;
    vfs_node_t *base_owned = NULL;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base) base = fs_root;
        else base_owned = base;
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    if (t && t->euid != 0 && t->fsuid != node->uid) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-1;
    }
    int rc = vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

static uint64_t sys_chown(uint64_t pathname_ptr, uint64_t owner,
                           uint64_t group, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    vfs_node_t *base_owned = NULL;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base) base = fs_root;
        else base_owned = base;
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t && t->euid != 0) {
        if (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid)) {
            vfs_close(node);
            if (base_owned) vfs_close(base_owned);
            return (uint64_t)-1;
        }
    }
    if (uid != node->uid || gid != node->gid) node->mask &= ~06000;
    int rc = vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

static uint64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    vfs_node_t *node = t->fds[fd];
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t->euid != 0 &&
        (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid)))
        return (uint64_t)-1;
    if (uid != node->uid || gid != node->gid)
        vfs_chmod(node, (uint16_t)(node->mask & ~06000));
    return vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_lchown(uint64_t pathname_ptr, uint64_t owner,
                            uint64_t group, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    return sys_chown(pathname_ptr, owner, group, a3, a4, a5);
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    if (t->euid != 0 && t->fsuid != t->fds[fd]->uid) return (uint64_t)-1;
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
            if (t && t->cwd_path[0]) {
                base = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base) base = fs_root;
                else base_owned = base;
            }
        } else {
            if (dirfd >= MAX_FDS || !t->fds[dirfd]) return (uint64_t)-9;
            base = t->fds[dirfd];
        }
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    if (t && t->euid != 0 && t->fsuid != node->uid) {
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
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14; // EFAULT
    struct thread *t = sched_get_current();
    vfs_node_t *base = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base) base = fs_root;
                else base_owned = base;
            }
        } else {
            if (dirfd >= MAX_FDS || !t->fds[dirfd]) return (uint64_t)-9; // EBADF
            base = t->fds[dirfd];
        }
    }
    // If AT_SYMLINK_NOFOLLOW, resolve without following the final symlink component.
    vfs_node_t *node;
    if (flags & AT_SYMLINK_NOFOLLOW)
        node = vfs_resolve_symlink_node(base, path);
    else
        node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2; // ENOENT
    }
    // Symlink nodes themselves have no permission bits on most filesystems;
    // return EOPNOTSUPP (95) when the caller requests nofollow on a symlink.
    if ((flags & AT_SYMLINK_NOFOLLOW) && (node->flags & FS_SYMLINK)) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-95; // EOPNOTSUPP
    }
    if (t && t->euid != 0 && t->fsuid != node->uid) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-1; // EPERM
    }
    int rc = vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

static uint64_t sys_fchownat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t owner, uint64_t group, uint64_t flags,
                              uint64_t a5) {
    (void)flags; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    vfs_node_t *base_owned = NULL;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base) base = fs_root;
                else base_owned = base;
            }
        } else {
            if (dirfd >= MAX_FDS || !t->fds[dirfd]) return (uint64_t)-9;
            base = t->fds[dirfd];
        }
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) {
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-2;
    }
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t && t->euid != 0 &&
        (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid))) {
        vfs_close(node);
        if (base_owned) vfs_close(base_owned);
        return (uint64_t)-1;
    }
    if (uid != node->uid || gid != node->gid) node->mask &= ~06000;
    int rc = vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
    vfs_close(node);
    if (base_owned) vfs_close(base_owned);
    return rc;
}

// ---------------------------------------------------------------------------
// access / faccessat2
// ---------------------------------------------------------------------------

static uint64_t do_sys_access(int dirfd, const char *path, uint64_t mode,
                               int flags) {
    (void)flags;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    vfs_node_t *node  = NULL;
    bool node_owned = false;
    vfs_node_t *base_owned = NULL;

    if (path[0] == '/') {
        node = vfs_resolve_path_at(fs_root, path);
        node_owned = node != NULL;
    } else if (strncmp(path, "/dev/", 5) == 0) {
        node = fb_lookup_device((char *)path + 5);
    }

    if (!node) {
        vfs_node_t *base_dir = fs_root;
        if (dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base_dir) base_dir = fs_root;
                else base_owned = base_dir;
            }
        } else {
            if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
                return (uint64_t)-9;
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        }
        node = vfs_resolve_path_at(base_dir, path);
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
    if (!t) return (uint64_t)-1;
    if (fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    if ((t->fds[fd]->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;
    if (!vfs_access(t->fds[fd], 1)) return (uint64_t)-13;

    const char *fd_path = fd_path_value(t, (int)fd);
    if (fd_path && fd_path[0]) {
        strncpy(t->cwd_path, fd_path, sizeof(t->cwd_path) - 1);
        t->cwd_path[sizeof(t->cwd_path) - 1] = '\0';
        if (t->cwd_node) vfs_close(t->cwd_node);
        t->cwd_node = t->fds[fd];
        vfs_open(t->cwd_node);
        klog_puts("[FCHDIR] Changed cwd to: "); klog_puts(t->cwd_path);
        klog_puts(" via fd="); klog_uint64(fd); klog_puts("\n");
        return 0;
    }
    return (uint64_t)-9;
}

// ---------------------------------------------------------------------------
// utimensat / futimesat / utimes stubs
// ---------------------------------------------------------------------------

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t flags, uint64_t a4, uint64_t a5) {
    (void)dirfd; (void)pathname; (void)times; (void)flags; (void)a4; (void)a5;
    return 0;
}

static uint64_t sys_futimesat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)dirfd; (void)pathname; (void)times; (void)a3; (void)a4; (void)a5;
    return 0;
}

static uint64_t sys_utimes(uint64_t pathname, uint64_t times, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)pathname; (void)times; (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
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

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void syscall_register_fs(void) {
    syscall_register(SYS_MKDIR,      sys_mkdir);
    syscall_register(SYS_MKDIRAT,    sys_mkdirat);
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
    syscall_register(SYS_UTIMENSAT,  sys_utimensat);
    syscall_register(SYS_FUTIMESAT,  sys_futimesat);
    syscall_register(SYS_UTIMES,     sys_utimes);
    syscall_register(SYS_NAME_TO_HANDLE_AT, sys_name_to_handle_at);
    syscall_register(SYS_OPEN_BY_HANDLE_AT, sys_open_by_handle_at);
}
