// AF_UNIX – Bind logic (abstract and filesystem namespaces)

#include "af_unix_internal.h"

static int unix_bind_abstract(unix_sock_t *usk, struct sockaddr_un *sun,
                              int addrlen) {
  if (unix_find_socket_by_addr(sun, addrlen))
    return -EADDRINUSE;

  size_t copy = (size_t)addrlen < sizeof(usk->addr) ? (size_t)addrlen : sizeof(usk->addr);
  memcpy(&usk->addr, sun, copy);
  usk->addr_len = addrlen;
  usk->is_abstract = true;

  spinlock_acquire(&unix_bound_lock);
  list_add(&usk->bind_node, &unix_bound_list);
  spinlock_release(&unix_bound_lock);

  usk->parent->state = SS_UNCONNECTED;

  klog_debug_puts("[OK] unix_bind: bound to abstract address\n");
  return 0;
}

static int unix_bind_fs(unix_sock_t *usk, struct sockaddr_un *sun, int addrlen) {
  if (unix_find_socket_by_addr(sun, addrlen)) {
    klog_puts("[WARN] unix_bind_fs: address already in internal bound list\n");
    return -EADDRINUSE;
  }

  // Split sun_path into parent directory + basename
  char parent_path[UNIX_PATH_MAX];
  char name[UNIX_PATH_MAX];

  const char *last_slash = strrchr(sun->sun_path, '/');
  if (!last_slash) {
    strcpy(parent_path, ".");
    strcpy(name, sun->sun_path);
  } else {
    int dir_len = last_slash - sun->sun_path;
    if (dir_len == 0) {
      strcpy(parent_path, "/");
    } else {
      memcpy(parent_path, sun->sun_path, dir_len);
      parent_path[dir_len] = '\0';
    }
    strcpy(name, last_slash + 1);
  }

  struct thread *current_thread = sched_get_current();
  vfs_node_t *cwd_node = fs_root;
  vfs_node_t *cwd_owned = NULL;
  /* Absolute socket paths ignore the base entirely
   * (vfs_resolve_path_at() anchors them at fs_root), so only pay for a CWD
   * resolution when the parent is actually relative. */
  if (parent_path[0] != '/' && current_thread && current_thread->cwd_path[0]) {
    cwd_node = vfs_resolve_path(current_thread->cwd_path);
    if (!cwd_node)
      cwd_node = fs_root;
    else
      cwd_owned = cwd_node;
  }

  vfs_node_t *parent = vfs_resolve_path_at(cwd_node, parent_path);
  if (!parent) {
    if (cwd_owned) vfs_close(cwd_owned);
    klog_puts("[WARN] unix_bind_fs: parent directory not found\n");
    return -2; // ENOENT
  }

  vfs_node_t *existing = vfs_finddir(parent, name);
  if (existing) {
    if (!unix_find_socket_by_addr(sun, addrlen)) {
      // Stale socket file – unlink and recreate
      klog_puts("[INFO] unix_bind_fs: unlinking stale socket node: ");
      klog_puts(sun->sun_path);
      klog_puts("\n");
      /* vfs_unlink frees the node outright in ramfs/tmpfs; do not touch
       * `existing` after this. */
      vfs_unlink(parent, name);
      existing = NULL;
    } else {
      klog_puts("[WARN] unix_bind_fs: address already in use: ");
      klog_puts(sun->sun_path);
      klog_puts("\n");
      vfs_close(existing);
      vfs_close(parent);
      if (cwd_owned) vfs_close(cwd_owned);
      return -EADDRINUSE;
    }
  }

  if (!current_thread || !vfs_access(parent, 3)) {
    vfs_close(parent);
    if (cwd_owned) vfs_close(cwd_owned);
    return -13; // EACCES
  }

  uint16_t mode = (uint16_t)(0777 & ~current_thread->umask);
  int ret = vfs_mknod(parent, name, mode, FS_SOCKET, usk->parent);
  if (ret < 0) {
    vfs_close(parent);
    if (cwd_owned) vfs_close(cwd_owned);
    return ret;
  }

  vfs_node_t *fs_node = vfs_finddir(parent, name);
  if (!fs_node) {
    vfs_close(parent);
    if (cwd_owned) vfs_close(cwd_owned);
    return -2; // ENOENT
  }
  vfs_chown(fs_node, current_thread->fsuid,
            current_thread->fsgid);

  usk->bound_vnode = fs_node; // reference transferred to the socket

  {
    size_t copy = (size_t)addrlen < sizeof(usk->addr) ? (size_t)addrlen : sizeof(usk->addr);
    memcpy(&usk->addr, sun, copy);
  }
  usk->addr_len = addrlen;
  usk->is_abstract = false;

  spinlock_acquire(&unix_bound_lock);
  list_add(&usk->bind_node, &unix_bound_list);
  spinlock_release(&unix_bound_lock);

  usk->parent->state = SS_UNCONNECTED;

  klog_debug_puts("[OK] unix_bind: bound to filesystem path: ");
  klog_debug_puts(sun->sun_path);
  klog_debug_puts("\n");
  vfs_close(parent);
  if (cwd_owned) vfs_close(cwd_owned);
  return 0;
}

// Entry point – registered in unix_ops (af_unix.c)
int unix_bind_impl(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !addr)
    return -22; // EINVAL
  if (addr->sa_family != AF_UNIX)
    return -97; // EAFNOSUPPORT
  if ((size_t)addrlen < offsetof(struct sockaddr_un, sun_path))
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  if (usk->addr_len > 0)
    return -22; // EINVAL – already bound

  struct sockaddr_un *sun = (struct sockaddr_un *)addr;

  if (sun->sun_path[0] == '\0')
    return unix_bind_abstract(usk, sun, addrlen);
  else
    return unix_bind_fs(usk, sun, addrlen);
}