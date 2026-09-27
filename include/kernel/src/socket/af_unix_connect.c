// AF_UNIX – Listen / connect / accept

#include "af_unix_internal.h"

int unix_listen_impl(socket_t *sock, int backlog) {
  if (!sock)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  spinlock_acquire(&sock->lock);

  usk->is_listener    = true;
  usk->backlog        = (backlog > 0) ? backlog : 128;
  usk->accept_queue_len = 0;
  usk->accept_next    = NULL;
  sock->state         = SS_LISTENING;

  spinlock_release(&sock->lock);

  klog_puts("[OK] unix_listen: socket is now listening (backlog=");
  klog_uint64(usk->backlog);
  klog_puts(")\n");
  return 0;
}

int unix_connect_impl(socket_t *sock, struct sockaddr *addr, int addrlen) {
  KTRACK(KSUBSYS_AF_UNIX);
  if (!sock || !addr) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -22);
    return -22; // EINVAL
  }

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -22);
    return -22;
  }

  if (sock->state == SS_CONNECTED) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -106);
    return -106; // EISCONN
  }
  if (sock->state == SS_LISTENING) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -22);
    return -22; // EINVAL
  }

  struct sockaddr_un *sun = (struct sockaddr_un *)addr;

  unix_sock_t *dusk = unix_find_socket_by_addr_ref(sun, addrlen);
  if (!dusk) {
    klog_puts("[WARN] unix_connect: destination not found: \"");
    if (sun->sun_path[0] == '\0') {
      klog_puts("@");
      klog_puts(sun->sun_path + 1);
      klog_puts("\"\n");
      KTRACK_ERR(KSUBSYS_AF_UNIX, -111);
      return -111; // ECONNREFUSED
    } else {
      klog_puts(sun->sun_path);
      klog_puts("\"\n");
      KTRACK_ERR(KSUBSYS_AF_UNIX, -2);
      return -2; // ENOENT
    }
  }

  socket_t *listener_sock = dusk->parent;
  if (!af_unix_sock_live(dusk, &listener_sock) || listener_sock->closing) {
    socket_put(listener_sock);
    klog_puts("[WARN] unix_connect: destination is closing or stale\n");
    KTRACK_ERR(KSUBSYS_AF_UNIX, -111);
    return -111; // ECONNREFUSED
  }

  if (listener_sock->state != SS_LISTENING) {
    klog_puts("[WARN] unix_connect: destination is not listening\n");
    socket_put(listener_sock);
    KTRACK_ERR(KSUBSYS_AF_UNIX, -111);
    return -111; // ECONNREFUSED
  }

  // Create the server-side socket early so the peer link is established
  // before accept() is called.
  socket_t *server_sock =
      socket_create(sock->domain, sock->type, sock->protocol);
  if (!server_sock) {
    socket_put(listener_sock);
    KTRACK_ERR(KSUBSYS_AF_UNIX, -12);
    return -12; // ENOMEM
  }

  unix_sock_t *server_usk = (unix_sock_t *)server_sock->sk;
  server_usk->passcred = dusk->passcred;

  usk->peer        = server_usk;
  server_usk->peer = usk;

  sock->state        = SS_CONNECTED;
  server_sock->state = SS_CONNECTED;
  usk->was_connected        = true;
  server_usk->was_connected = true;

  spinlock_acquire(&listener_sock->lock);

  if (listener_sock->closing || listener_sock->state != SS_LISTENING) {
    spinlock_release(&listener_sock->lock);
    usk->peer = NULL;
    server_usk->peer = NULL;
    socket_put(server_sock);
    socket_put(listener_sock);
    return -111; // ECONNREFUSED
  }

  if (dusk->accept_queue_len >= dusk->backlog) {
    spinlock_release(&listener_sock->lock);
    klog_puts("[WARN] unix_connect: listener backlog full\n");
    usk->peer        = NULL;
    server_usk->peer = NULL;
    socket_put(server_sock);
    socket_put(listener_sock);
    return -111; // ECONNREFUSED
  }

  // Enqueue server socket in listener's accept queue
  server_usk->accept_next = NULL;
  if (dusk->accept_next == NULL) {
    dusk->accept_next = server_usk;
  } else {
    unix_sock_t *curr = dusk->accept_next;
    while (curr->accept_next)
      curr = curr->accept_next;
    curr->accept_next = server_usk;
  }
  dusk->accept_queue_len++;

  // Notify listener
  epoll_notify_socket(listener_sock->fd, POLLIN);
  if (listener_sock->node) {
    epoll_notify_event(listener_sock->node, POLLIN);
  }
  if (listener_sock->wait_queue) {
    wait_queue_wake_all((wait_queue_t *)listener_sock->wait_queue);
  }
  wait_queue_wake_all(dusk->wait);

  spinlock_release(&listener_sock->lock);
  socket_put(listener_sock);
  return 0;
}

int unix_accept_impl(socket_t *sock, socket_t **newsock) {
  if (!sock || !newsock)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk || !usk->is_listener)
    return -22; // EINVAL

  spinlock_acquire(&sock->lock);

  while (usk->accept_next == NULL) {
    if (sock->flags & SOCK_NONBLOCK) {
      spinlock_release(&sock->lock);
      return -11; // EAGAIN
    }

    struct thread *current = sched_get_current();
    wait_queue_entry_t entry;
    entry.thread = current;
    entry.next   = NULL;

    wait_queue_add(usk->wait, &entry);
    current->state = THREAD_BLOCKED;

    spinlock_release(&sock->lock);
    sched_yield();
    wait_queue_remove(usk->wait, &entry);
    current->state = THREAD_RUNNING;

    spinlock_acquire(&sock->lock);
  }

  unix_sock_t *new_usk   = usk->accept_next;
  usk->accept_next       = new_usk->accept_next;
  usk->accept_queue_len--;

  spinlock_release(&sock->lock);

  socket_t *new_sock    = new_usk->parent;
  new_usk->is_accepted  = true;

  struct thread *current_thread = sched_get_current();
  if (current_thread) {
    new_usk->owner_pid = current_thread->tgid;
    new_usk->owner_uid = current_thread->euid;
    new_usk->owner_gid = current_thread->egid;
  }

  *newsock = new_sock;

  klog_puts("[OK] unix_accept: retrieved early-linked connection\n");
  return 0;
}