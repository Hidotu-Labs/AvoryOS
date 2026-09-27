
#include "../fs/vfs.h"
#include "../sched/sched.h"
#include "arch/uaccess.h"
#include "syscall.h"
#include <stdint.h>

#define XATTR_ENOENT     2
#define XATTR_EBADF      9
#define XATTR_EFAULT    14
#define XATTR_EINVAL    22
#define XATTR_ENODATA   61
#define XATTR_EOPNOTSUPP 95

#define XATTR_NAME_MAX 255 
#define XATTR_PATH_MAX 4096

static bool user_string_ok(const char *s, long bound) {
  if (!is_user_ptr((uint64_t)(uintptr_t)s))
    return false;
  return strnlen_user(s, bound) > 0;
}

static uint64_t xattr_list_result(uint64_t list_ptr, uint64_t size) {
  if (size > 0x7FFFFFFFULL)
    return (uint64_t)-XATTR_EINVAL; /* the return value is an ssize_t */
  if (list_ptr != 0 && !is_user_range((const void *)list_ptr, size))
    return (uint64_t)-XATTR_EFAULT;
  return 0;
}

static bool xattr_fd_ok(uint64_t fd) {
  struct thread *t = sched_get_current();
  return t && fd < MAX_FDS && t->fds[fd] != NULL;
}

static uint64_t xattr_check_path(const char *path) {
  if (!user_string_ok(path, XATTR_PATH_MAX))
    return (uint64_t)-XATTR_EFAULT;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  vfs_node_t *cwd = NULL;
  if (path[0] != '/' && t && t->cwd_path[0]) {
    cwd = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (cwd)
      base = cwd;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node) {
    if (cwd) vfs_close(cwd);
    return (uint64_t)-XATTR_ENOENT;
  }
  vfs_close(node);
  if (cwd) vfs_close(cwd);
  return 0;
}

static uint64_t xattr_get_result(uint64_t name_ptr, uint64_t value_ptr,
                                 uint64_t size) {
  if (!user_string_ok((const char *)name_ptr, XATTR_NAME_MAX + 1))
    return (uint64_t)-XATTR_EFAULT;
  if (size > 0x7FFFFFFFULL)
    return (uint64_t)-XATTR_EINVAL;
  if (value_ptr != 0 && !is_user_range((const void *)value_ptr, size))
    return (uint64_t)-XATTR_EFAULT;
  return (uint64_t)-XATTR_ENODATA;
}

static uint64_t xattr_set_result(uint64_t name_ptr) {
  if (!user_string_ok((const char *)name_ptr, XATTR_NAME_MAX + 1))
    return (uint64_t)-XATTR_EFAULT;
  return (uint64_t)-XATTR_EOPNOTSUPP;
}

static uint64_t xattr_remove_result(uint64_t name_ptr) {
  if (!user_string_ok((const char *)name_ptr, XATTR_NAME_MAX + 1))
    return (uint64_t)-XATTR_EFAULT;
  return (uint64_t)-XATTR_ENODATA;
}



static uint64_t sys_setxattr(uint64_t path, uint64_t name, uint64_t value,
                             uint64_t size, uint64_t flags, uint64_t a5) {
  (void)value;
  (void)size;
  (void)flags;
  (void)a5;
  uint64_t rc = xattr_check_path((const char *)path);
  if (rc != 0)
    return rc;
  return xattr_set_result(name);
}

static uint64_t sys_lsetxattr(uint64_t path, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t flags, uint64_t a5) {
  return sys_setxattr(path, name, value, size, flags, a5);
}

static uint64_t sys_fsetxattr(uint64_t fd, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t flags, uint64_t a5) {
  (void)value;
  (void)size;
  (void)flags;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  return xattr_set_result(name);
}

static uint64_t sys_getxattr(uint64_t path, uint64_t name, uint64_t value,
                             uint64_t size, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  uint64_t rc = xattr_check_path((const char *)path);
  if (rc != 0)
    return rc;
  return xattr_get_result(name, value, size);
}

static uint64_t sys_lgetxattr(uint64_t path, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t a4, uint64_t a5) {
  return sys_getxattr(path, name, value, size, a4, a5);
}

static uint64_t sys_fgetxattr(uint64_t fd, uint64_t name, uint64_t value,
                              uint64_t size, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  return xattr_get_result(name, value, size);
}



static uint64_t sys_listxattr(uint64_t path, uint64_t list, uint64_t size,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t rc = xattr_check_path((const char *)path);
  if (rc != 0)
    return rc;
  return xattr_list_result(list, size);
}

static uint64_t sys_llistxattr(uint64_t path, uint64_t list, uint64_t size,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  return sys_listxattr(path, list, size, a3, a4, a5);
}

static uint64_t sys_flistxattr(uint64_t fd, uint64_t list, uint64_t size,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!xattr_fd_ok(fd))
    return (uint64_t)-XATTR_EBADF;
  return xattr_list_result(list, size);
}


static uint64_t sys_removexattr(uint64_t path, uint64_t name, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t rc = xattr_check_path((const char *)path);
  if (rc != 0)
    return rc;
  return xattr_remove_result(name);
}

static uint64_t sys_lremovexattr(uint64_t path, uint64_t name, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
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
  return xattr_remove_result(name);
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
