// AF_UNIX – Bound-socket address registry
//
// Owns the global unix_bound_list / unix_bound_lock and provides the two
// address-lookup helpers used by the rest of the subsystem.

#include "af_unix_internal.h"
#include "../fs/mount_namespace.h"

// ── Global registry ───────────────────────────────────────────────────────────

struct list_head unix_bound_list;
spinlock_t       unix_bound_lock;

static bool af_unix_kernel_ptr(const void *ptr) {
  uint64_t addr = (uint64_t)ptr;
  return addr >= 0xFFFF800000000000ULL && !is_user_ptr(addr);
}

bool af_unix_sock_live(unix_sock_t *usk, socket_t **sock_out) {
  if (!usk || !af_unix_kernel_ptr(usk))
    return false;

  socket_t *sock = usk->parent;
  if (!sock || !af_unix_kernel_ptr(sock))
    return false;
  if (sock->sk != usk || sock->domain != AF_UNIX || sock->closing)
    return false;

  if (sock_out)
    *sock_out = sock;
  return true;
}

static unix_sock_t *af_unix_sock_ref(unix_sock_t *usk) {
  socket_t *sock = NULL;
  if (!af_unix_sock_live(usk, &sock))
    return NULL;
  if (!socket_try_get(sock))
    return NULL;
  return usk;
}

/* Resolve filesystem socket names before consulting the registry. A pathname
 * string is not an identity across different root/mount views. Disk-backed
 * socket inodes keep their filesystem device pointer, not a socket_t payload. */
static unix_sock_t *unix_find_filesystem_socket(struct sockaddr_un *addr, bool pin) {
  struct thread *task = sched_get_current();
  vfs_node_t *base = task && task->cwd_node ? task->cwd_node : fs_root;
  vfs_node_t *node = vfs_resolve_path_at(base, addr->sun_path);
  if (!node) return NULL;
  vfs_node_t *backing = vfs_backing_node(node);
  unix_sock_t *found = NULL;
  if ((node->flags & FS_TYPE_MASK) == FS_SOCKET) {
    spinlock_acquire(&unix_bound_lock);
    struct list_head *entry;
    list_for_each(entry, &unix_bound_list) {
      unix_sock_t *candidate = list_entry(entry, unix_sock_t, bind_node);
      vfs_node_t *bound = vfs_backing_node(candidate->bound_vnode);
      if (!bound || candidate->is_abstract) continue;
      if (bound == backing || (bound->inode && bound->inode == backing->inode &&
                               bound->device == backing->device)) {
        found = pin ? af_unix_sock_ref(candidate) : candidate;
        break;
      }
    }
    spinlock_release(&unix_bound_lock);
  }
  vfs_close(node);
  return found;
}

// ── Lookup ────────────────────────────────────────────────────────────────────

unix_sock_t *unix_find_socket_by_addr(socket_t *owner, struct sockaddr_un *addr, int addrlen) {
  if (!addr)
    return NULL;

  struct list_head *pos;

  spinlock_acquire(&unix_bound_lock);

  list_for_each(pos, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    if (usk->addr.sun_family != AF_UNIX)
      continue;

    if (usk->addr.sun_path[0] == '\0') {
      if (usk->parent->net_ns != owner->net_ns) continue;
      // Abstract socket: match exact length and bytes
      if (usk->addr_len != addrlen)
        continue;
      if (memcmp(usk->addr.sun_path, addr->sun_path,
                 addrlen - offsetof(struct sockaddr_un, sun_path)) == 0) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
    }
  }

  spinlock_release(&unix_bound_lock);

  if (addr->sun_path[0] != '\0')
    return unix_find_filesystem_socket(addr, false);

  return NULL;
}

unix_sock_t *unix_find_socket_by_addr_ref(socket_t *owner, struct sockaddr_un *addr, int addrlen) {
  if (!addr)
    return NULL;

  struct list_head *pos;

  spinlock_acquire(&unix_bound_lock);

  list_for_each(pos, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);
    bool match = false;

    if (usk->addr.sun_family != AF_UNIX)
      continue;

    if (usk->addr.sun_path[0] == '\0') {
      if (usk->parent->net_ns != owner->net_ns) continue;
      if (usk->addr_len == addrlen &&
          memcmp(usk->addr.sun_path, addr->sun_path,
                 addrlen - offsetof(struct sockaddr_un, sun_path)) == 0)
        match = true;
    }

    if (match) {
      unix_sock_t *found = af_unix_sock_ref(usk);
      if (found) {
        spinlock_release(&unix_bound_lock);
        return found;
      }
      break;
    }
  }

  spinlock_release(&unix_bound_lock);

  if (addr->sun_path[0] != '\0')
    return unix_find_filesystem_socket(addr, true);

  return NULL;
}

/**
 * Mark a socket's filesystem entry as unlinked.
 * Called when a socket file is unlinked via VFS.
 * The socket stays in the bound list so re-binding fails with EADDRINUSE.
 * Returns 0 on success, -1 if not found.
 */
int unix_unbind_by_path(const char *path) {
  if (!path || path[0] == '\0')
    return -1;

  struct list_head *pos, *n;
  int found = 0;

  spinlock_acquire(&unix_bound_lock);

  list_for_each_safe(pos, n, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    if (usk->addr.sun_path[0] == '\0')
      continue; // skip abstract sockets

    if (strcmp(usk->addr.sun_path, path) == 0) {
      list_del(&usk->bind_node);
      usk->addr_len = 0;
      /* The caller unlinks right after this, and filesystem backends free the
       * node outright, so stop pointing at it: unix_destroy() must not touch a
       * dangling vnode, and there is nothing left to vfs_close. */
      usk->bound_vnode = NULL;
      found = 1;
      break;
    }
  }

  spinlock_release(&unix_bound_lock);
  return found ? 0 : -1;
}
