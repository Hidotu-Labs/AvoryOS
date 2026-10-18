
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "arch/uaccess.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

#define XATTR_ENOENT 2
#define XATTR_EBADF 9
#define XATTR_EFAULT 14
#define XATTR_EINVAL 22
#define XATTR_ENOMEM 12
#define XATTR_EOPNOTSUPP 95

#define XATTR_NAME_MAX 255
#define XATTR_PATH_MAX 4096
/* ssize_t range cap inherited from the stub layer; the store itself caps
 * single values at 64 KiB (fs/xattr.c) and reports E2BIG above that. */
#define XATTR_SIZE_CAP 0x7FFFFFFFULL

static bool user_string_ok(const char *s, long bound) {
  if (!is_user_ptr((uint64_t)(uintptr_t)s))
    return false;
  return strnlen_user(s, bound) > 0;
}

static bool xattr_fd_ok(uint64_t fd) {
  struct thread *t = sched_get_current();
  return FD_IN_RANGE(t, fd) && fd_is_valid_node(t->fds[fd]);
}

/* Resolve `path` to an owned node, honouring the caller's cwd. Returns NULL
 * when the path does not resolve; `*cwd_out` (when non-NULL) receives an
 * owned cwd node the caller must close, or NULL. */
static vfs_node_t *xattr_resolve_path(const char *path, vfs_node_t **cwd_out) {
  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  vfs_node_t *cwd = NULL;
  if (cwd_out)
    *cwd_out = NULL;
  if (path[0] != '/' && t && t->cwd_path[0]) {
    cwd = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (cwd)
      base = cwd;
  }
  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node) {
    if (cwd)
      vfs_close(cwd);
    return NULL;
  }
  if (cwd_out)
    *cwd_out = cwd;
  else if (cwd)
    vfs_close(cwd);
  return node;
}

/* Copy a NUL-terminated user name into kname (256 bytes). 0 on success,
 * -EFAULT when unreadable, -EINVAL when empty or over XATTR_NAME_MAX. */
static int64_t xattr_copy_name(uint64_t name_ptr, char *kname) {
  if (!user_string_ok((const char *)name_ptr, XATTR_NAME_MAX + 1))
    return -XATTR_EFAULT;
  long n = strncpy_from_user(kname, (const char *)name_ptr, XATTR_NAME_MAX + 1);
  if (n < 0)
    return -XATTR_EFAULT;
  if (n == 0 || n > XATTR_NAME_MAX)
    return -XATTR_EINVAL;
  kname[XATTR_NAME_MAX] = '\0';
  return 0;
}

static uint64_t sys_setxattr(uint64_t path, uint64_t name, uint64_t value,
                             uint64_t size, uint64_t flags, uint64_t a5) {
  (void)a5;
  if (!user_string_ok((const char *)path, XATTR_PATH_MAX))
    return (uint64_t)-XATTR_EFAULT;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;
  if (size > 0 && value != 0 &&
      !is_user_range((const void *)value, (size_t)size))
    return (uint64_t)-XATTR_EFAULT;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  vfs_node_t *cwd = NULL;
  vfs_node_t *node = xattr_resolve_path((const char *)path, &cwd);
  if (!node)
    return (uint64_t)-XATTR_ENOENT;

  uint8_t *kvalue = NULL;
  if (size > 0) {
    if (value == 0) {
      vfs_close(node);
      if (cwd)
        vfs_close(cwd);
      return (uint64_t)-XATTR_EFAULT;
    }
    kvalue = kmalloc((size_t)size);
    if (!kvalue) {
      vfs_close(node);
      if (cwd)
        vfs_close(cwd);
      return (uint64_t)-XATTR_ENOMEM;
    }
    if (copy_from_user(kvalue, (const void *)value, (unsigned long)size) != 0) {
      kfree(kvalue);
      vfs_close(node);
      if (cwd)
        vfs_close(cwd);
      return (uint64_t)-XATTR_EFAULT;
    }
  }

  rc = vfs_setxattr(node, kname, kvalue, (uint32_t)size, (int)flags);
  if (kvalue)
    kfree(kvalue);
  vfs_close(node);
  if (cwd)
    vfs_close(cwd);
  return (uint64_t)rc;
}

static uint64_t sys_lsetxattr(uint64_t path, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t flags, uint64_t a5) {
  /* No-follow variant not implemented: symlinks resolve to their target,
   * same as before this store existed. */
  return sys_setxattr(path, name, value, size, flags, a5);
}

static uint64_t sys_fsetxattr(uint64_t fd, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t flags, uint64_t a5) {
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;
  if (size > 0 && value != 0 &&
      !is_user_range((const void *)value, (size_t)size))
    return (uint64_t)-XATTR_EFAULT;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  uint8_t *kvalue = NULL;
  if (size > 0) {
    if (value == 0)
      return (uint64_t)-XATTR_EFAULT;
    kvalue = kmalloc((size_t)size);
    if (!kvalue)
      return (uint64_t)-XATTR_ENOMEM;
    if (copy_from_user(kvalue, (const void *)value, (unsigned long)size) != 0) {
      kfree(kvalue);
      return (uint64_t)-XATTR_EFAULT;
    }
  }

  struct thread *t = sched_get_current();
  rc = vfs_setxattr(t->fds[fd], kname, kvalue, (uint32_t)size, (int)flags);
  if (kvalue)
    kfree(kvalue);
  return (uint64_t)rc;
}

static uint64_t xattr_do_get(vfs_node_t *node, const char *kname,
                             uint64_t value_ptr, uint64_t size) {
  if (value_ptr == 0 || size == 0) {
    /* Size query: Linux returns the value length without touching the
     * buffer, so callers can size their allocation. */
    return (uint64_t)vfs_getxattr(node, kname, NULL, 0);
  }
  if (!is_user_range((const void *)value_ptr, (size_t)size))
    return (uint64_t)-XATTR_EFAULT;
  uint8_t *tmp = kmalloc((size_t)size);
  if (!tmp)
    return (uint64_t)-XATTR_ENOMEM;
  int64_t rc = vfs_getxattr(node, kname, tmp, (uint32_t)size);
  if (rc >= 0) {
    if (copy_to_user((void *)value_ptr, tmp, (unsigned long)rc) != 0)
      rc = -XATTR_EFAULT;
  }
  kfree(tmp);
  return (uint64_t)rc;
}

static uint64_t sys_getxattr(uint64_t path, uint64_t name, uint64_t value,
                             uint64_t size, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!user_string_ok((const char *)path, XATTR_PATH_MAX))
    return (uint64_t)-XATTR_EFAULT;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  vfs_node_t *cwd = NULL;
  vfs_node_t *node = xattr_resolve_path((const char *)path, &cwd);
  if (!node)
    return (uint64_t)-XATTR_ENOENT;

  uint64_t ret = xattr_do_get(node, kname, value, size);
  vfs_close(node);
  if (cwd)
    vfs_close(cwd);
  return ret;
}

static uint64_t sys_lgetxattr(uint64_t path, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t a4, uint64_t a5) {
  /* See sys_lsetxattr: nofollow not implemented. */
  return sys_getxattr(path, name, value, size, a4, a5);
}

static uint64_t sys_fgetxattr(uint64_t fd, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  struct thread *t = sched_get_current();
  return xattr_do_get(t->fds[fd], kname, value, size);
}

static uint64_t xattr_do_list(vfs_node_t *node, uint64_t list_ptr,
                              uint64_t size) {
  if (list_ptr == 0 || size == 0)
    return (uint64_t)vfs_listxattr(node, NULL, 0);
  if (!is_user_range((const void *)list_ptr, (size_t)size))
    return (uint64_t)-XATTR_EFAULT;
  char *tmp = kmalloc((size_t)size);
  if (!tmp)
    return (uint64_t)-XATTR_ENOMEM;
  int64_t rc = vfs_listxattr(node, tmp, (uint32_t)size);
  if (rc >= 0) {
    if (copy_to_user((void *)list_ptr, tmp, (unsigned long)rc) != 0)
      rc = -XATTR_EFAULT;
  }
  kfree(tmp);
  return (uint64_t)rc;
}

static uint64_t sys_listxattr(uint64_t path, uint64_t list, uint64_t size,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_string_ok((const char *)path, XATTR_PATH_MAX))
    return (uint64_t)-XATTR_EFAULT;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;

  vfs_node_t *cwd = NULL;
  vfs_node_t *node = xattr_resolve_path((const char *)path, &cwd);
  if (!node)
    return (uint64_t)-XATTR_ENOENT;

  uint64_t ret = xattr_do_list(node, list, size);
  vfs_close(node);
  if (cwd)
    vfs_close(cwd);
  return ret;
}

static uint64_t sys_llistxattr(uint64_t path, uint64_t list, uint64_t size,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  /* See sys_lsetxattr: nofollow not implemented. */
  return sys_listxattr(path, list, size, a3, a4, a5);
}

static uint64_t sys_flistxattr(uint64_t fd, uint64_t list, uint64_t size,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  if (size > XATTR_SIZE_CAP)
    return (uint64_t)-XATTR_EINVAL;

  struct thread *t = sched_get_current();
  return xattr_do_list(t->fds[fd], list, size);
}

static uint64_t sys_removexattr(uint64_t path, uint64_t name, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_string_ok((const char *)path, XATTR_PATH_MAX))
    return (uint64_t)-XATTR_EFAULT;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  vfs_node_t *cwd = NULL;
  vfs_node_t *node = xattr_resolve_path((const char *)path, &cwd);
  if (!node)
    return (uint64_t)-XATTR_ENOENT;

  rc = vfs_removexattr(node, kname);
  vfs_close(node);
  if (cwd)
    vfs_close(cwd);
  return (uint64_t)rc;
}

static uint64_t sys_lremovexattr(uint64_t path, uint64_t name, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  /* See sys_lsetxattr: nofollow not implemented. */
  return sys_removexattr(path, name, a2, a3, a4, a5);
}

static uint64_t sys_fremovexattr(uint64_t fd, uint64_t name, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;

  char kname[XATTR_NAME_MAX + 1];
  int64_t rc = xattr_copy_name(name, kname);
  if (rc != 0)
    return (uint64_t)rc;

  struct thread *t = sched_get_current();
  return (uint64_t)vfs_removexattr(t->fds[fd], kname);
}

void syscall_register_xattr(void) {
  syscall_register(SYS_SETXATTR, sys_setxattr);
  syscall_register(SYS_LSETXATTR, sys_lsetxattr);
  syscall_register(SYS_FSETXATTR, sys_fsetxattr);
  syscall_register(SYS_GETXATTR, sys_getxattr);
  syscall_register(SYS_LGETXATTR, sys_lgetxattr);
  syscall_register(SYS_FGETXATTR, sys_fgetxattr);
  syscall_register(SYS_LISTXATTR, sys_listxattr);
  syscall_register(SYS_LLISTXATTR, sys_llistxattr);
  syscall_register(SYS_FLISTXATTR, sys_flistxattr);
  syscall_register(SYS_REMOVEXATTR, sys_removexattr);
  syscall_register(SYS_LREMOVEXATTR, sys_lremovexattr);
  syscall_register(SYS_FREMOVEXATTR, sys_fremovexattr);
}
