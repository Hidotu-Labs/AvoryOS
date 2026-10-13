#include "af_unix_internal.h"
#include "../apic/lapic_timer.h"
#include "../syscalls/sys_io_shared.h"
#include "arch/uaccess.h"

/* Pin and snapshot under the sender's file-table lock. A CLONE_FILES
 * sibling can close/reuse the descriptor while sendmsg is in progress. */
static bool unix_scm_capture_fd(struct thread *sender, int fd,
                                unix_scm_fd_t *saved) {
  if (!sender || !sender->files || fd < 0 || fd >= MAX_FDS)
    return false;
  spinlock_acquire(&sender->files->lock);
  vfs_node_t *node = sender->fds[fd];
  if (!fd_is_valid_node(node)) {
    spinlock_release(&sender->files->lock);
    return false;
  }
  vfs_node_ref(node);
  saved->node = node;
  saved->offset = sender->fd_offsets[fd];
  saved->flags = sender->fd_flags[fd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  const char *path = fd_path_value(sender, fd);
  saved->path[0] = '\0';
  if (path) {
    strncpy(saved->path, path, sizeof(saved->path) - 1);
    saved->path[sizeof(saved->path) - 1] = '\0';
  }
  spinlock_release(&sender->files->lock);
  vfs_open(node); /* queued message owns this open reference */
  vfs_close(node); /* release the snapshot's temporary pin */
  return true;
}

static int unix_scm_install_fd(struct thread *receiver,
                               unix_scm_fd_t *saved, int flags) {
  int fd = alloc_fd(receiver);
  if (fd < 0)
    return fd;
  spinlock_acquire(&receiver->files->lock);
  receiver->fd_offsets[fd] = saved->offset;
  receiver->fd_flags[fd] = saved->flags |
      ((flags & MSG_CMSG_CLOEXEC) ? FD_FLAGS_CLOEXEC_BIT : 0);
  if (!fd_path_set(receiver, fd, saved->path)) {
    spinlock_release(&receiver->files->lock);
    fd_release_reserved(receiver, fd);
    return -12;
  }
  receiver->fds[fd] = saved->node; /* publish after all metadata */
  spinlock_release(&receiver->files->lock);
  return fd;
}

static void unix_scm_free_msg(unix_scm_msg_t *msg) {
  for (int i = 0; i < msg->count; i++) {
    if (msg->fds[i].node)
      vfs_close(msg->fds[i].node);
  }
  kfree(msg);
}

/* bytes_read names the next unread byte. Reaching the first byte carrying
 * SCM does not consume it; only a read past that byte discards its rights. */
static void unix_discard_consumed_scm(unix_sock_t *usk) {
  while (usk->scm_queue_head &&
         usk->bytes_read > usk->scm_queue_head->stream_offset) {
    unix_scm_msg_t *discard = usk->scm_queue_head;
    for (int i = 0; i < discard->count; i++) {
      if (discard->fds[i].node)
        vfs_close(discard->fds[i].node);
    }
    usk->scm_queue_head = discard->next;
    if (!usk->scm_queue_head)
      usk->scm_queue_tail = NULL;
    kfree(discard);
  }
}

static bool unix_user_range_valid(uint64_t addr, size_t len) {
  if (!addr)
    return false;
  return is_user_range((const void *)addr, len);
}

struct unix_ucred {
  int pid;
  int uid;
  int gid;
};

// Called with peer->recv_lock held. Linux attaches the credentials of the
// sending process to data received on an SO_PASSCRED socket, including data
// sent through write(2)/send(2), not only explicit sendmsg(2) control data.
static void unix_record_sender_credentials(unix_sock_t *peer,
                                           struct thread *sender) {
  if (!peer->passcred || !sender)
    return;

  peer->scm_cred_pid = (int)sender->tgid;
  peer->scm_cred_uid = (int)sender->uid;
  peer->scm_cred_gid = (int)sender->gid;
  peer->scm_cred_pending = true;
}

static socket_t *unix_get_live_peer(socket_t *sock, unix_sock_t **peer_out) {
  if (peer_out)
    *peer_out = NULL;
  if (!sock || !peer_out)
    return NULL;

  spinlock_acquire(&sock->lock);
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  unix_sock_t *peer = usk ? usk->peer : NULL;
  socket_t *peer_sock = peer ? peer->parent : NULL;

  if (!peer_sock || !socket_try_get(peer_sock)) {
    spinlock_release(&sock->lock);
    return NULL;
  }

  if (peer_sock->closing || !peer_sock->sk) {
    spinlock_release(&sock->lock);
    socket_put(peer_sock);
    return NULL;
  }

  *peer_out = (unix_sock_t *)peer_sock->sk;
  spinlock_release(&sock->lock);
  return peer_sock;
}

// A socket's epoll watchers register on its VFS node, so notifying the node
// reaches every epoll instance watching this fd.  The by-fd fallback is only
// needed for a socket that somehow has no node.
static void unix_notify_epoll(socket_t *s, uint32_t events) {
  if (!s)
    return;
  if (s->node)
    epoll_notify_event(s->node, events);
  else if (s->fd >= 0)
    epoll_notify_socket(s->fd, events);
}

// usk->wait is initialised from the socket's own wait queue, so the second
// wake in the old code walked the same list again.  Keep both calls but skip
// the duplicate queue.
static void unix_wake_recv_waiters(unix_sock_t *peer) {
  if (!peer)
    return;
  wait_queue_t *sock_wq =
      peer->parent ? (wait_queue_t *)peer->parent->wait_queue : NULL;
  wait_queue_wake_all(peer->wait);
  if (sock_wq && sock_wq != peer->wait)
    wait_queue_wake_all(sock_wq);
}

static bool unix_ensure_recv_buf(unix_sock_t *usk, size_t needed) {
  if (!usk) return false;
  if (!usk->recv_buf) {
    size_t sz = (usk->parent && usk->parent->rcvbuf > 0) ? (size_t)usk->parent->rcvbuf : SOCKET_DEFAULT_RCVBUF;
    if (needed > 0 && needed + 1 > sz)
      sz = needed + 4096;
    if (sz < 65536) sz = 65536;
    usk->recv_buf = kmalloc(sz);
    if (!usk->recv_buf) return false;
    usk->recv_buf_size = sz;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = 0;
  } else if (needed > 0 && needed + 1 > usk->recv_buf_size && usk->recv_buf_size < 4 * 1024 * 1024) {
    size_t new_sz = usk->recv_buf_size * 2;
    while (new_sz < needed + 4096 && new_sz < 4 * 1024 * 1024)
      new_sz *= 2;
    uint8_t *new_buf = kmalloc(new_sz);
    if (new_buf) {
      size_t head = usk->recv_buf_head;
      size_t tail = usk->recv_buf_tail;
      size_t old_size = usk->recv_buf_size;
      size_t available = (old_size > 0) ? (tail - head + old_size) % old_size : 0;
      unix_ring_consume(usk->recv_buf, old_size, head, new_buf, available);
      kfree(usk->recv_buf);
      usk->recv_buf = new_buf;
      usk->recv_buf_size = new_sz;
      usk->recv_buf_head = 0;
      usk->recv_buf_tail = available;
      if (usk->parent)
        usk->parent->rcvbuf = (int)new_sz;
    }
  }
  return true;
}

ssize_t unix_send_impl(socket_t *sock, const void *buf, size_t len, int flags) {
  KTRACK(KSUBSYS_AF_UNIX);
  (void)flags;
  if (!sock || !sock->sk) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -9);
    return -9; // EBADF
  }

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (sock->closing || usk->write_shutdown) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -32);
    return -32; // EPIPE
  }

  if (sock->state != SS_CONNECTED && sock->state != SS_CONNECTING) {
    KTRACK_ERR(KSUBSYS_AF_UNIX, -107);
    if (usk->was_connected)
      return -32; // EPIPE
    return -107; // ENOTCONN
  }

  // If still CONNECTING, block until peer is set by accept()
  while (usk->peer == NULL && sock->state == SS_CONNECTING) {
    if (sock->closing)
      return -32; // EPIPE
    if (usk->listener == NULL || usk->orphaned)
      return -107; // ENOTCONN

    struct thread *current = sched_get_current();
    wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};

    current->state = THREAD_BLOCKED;
    wait_queue_add(usk->wait, &entry);

    if (usk->peer != NULL || sock->state != SS_CONNECTING) {
      current->state = THREAD_RUNNING;
    } else {
      sched_yield();
    }

    wait_queue_remove(usk->wait, &entry);
    current->state = THREAD_RUNNING;
  }

  unix_sock_t *peer = NULL;
  socket_t *peer_sock = unix_get_live_peer(sock, &peer);
  if (!peer_sock || !peer) {
    if (usk->was_connected)
      return -32; // EPIPE
    return -107; // ENOTCONN
  }

  uint64_t sndtimeo_deadline = 0;
  if (usk->sndtimeo_ms > 0)
    sndtimeo_deadline = lapic_timer_get_ticks() + (uint64_t)usk->sndtimeo_ms;

  if (peer && peer->is_syslog_sink) {
    socket_put(peer_sock);
    return (ssize_t)len;
  }

  if (sock->type == SOCK_SEQPACKET || sock->type == SOCK_DGRAM) {
    while (1) {
      if (sock->closing || peer_sock->closing || peer->read_shutdown) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      spinlock_acquire(&peer->recv_lock);
      if (peer->packet_queue_bytes + len > 4 * 1024 * 1024 && peer->packet_queue_bytes > 0) {
        if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
          spinlock_release(&peer->recv_lock);
          socket_put(peer_sock);
          return -11; // EAGAIN
        }

        struct thread *current = sched_get_current();
        wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
        current->state = THREAD_BLOCKED;
        wait_queue_add(peer->wait, &entry);
        if (sndtimeo_deadline != 0)
          current->wakeup_ticks = sndtimeo_deadline;
        /* Recheck under recv_lock: a consume+wake between the first check
         * and the add would otherwise be lost (wake sees RUNNING). */
        if (!(peer->packet_queue_bytes + len > 4 * 1024 * 1024 && peer->packet_queue_bytes > 0)) {
          current->state = THREAD_RUNNING;
          current->wakeup_ticks = 0;
          spinlock_release(&peer->recv_lock);
          wait_queue_remove(peer->wait, &entry);
          current->state = THREAD_RUNNING;
          if (sndtimeo_deadline != 0 && lapic_timer_get_ticks() >= sndtimeo_deadline) {
            socket_put(peer_sock);
            return -11; // EAGAIN
          }
          continue;
        }

        spinlock_release(&peer->recv_lock);
        sched_yield();
        wait_queue_remove(peer->wait, &entry);
        current->state = THREAD_RUNNING;
        current->wakeup_ticks = 0;
        if (sndtimeo_deadline != 0 && lapic_timer_get_ticks() >= sndtimeo_deadline) {
          socket_put(peer_sock);
          return -11; // EAGAIN
        }
        continue;
      }
      break;
    }

    unix_packet_t *pkt = kmalloc(sizeof(unix_packet_t) + len);
    if (!pkt) {
      spinlock_release(&peer->recv_lock);
      socket_put(peer_sock);
      return -12; // ENOMEM
    }

    pkt->next = NULL;
    pkt->data_len = len;
    pkt->scm = NULL;
    pkt->cred_pending = false;
    if (len > 0 && buf)
      memcpy(pkt->data, buf, len);

    if (peer->passcred) {
      struct thread *sender = sched_get_current();
      if (sender) {
        pkt->cred_pending = true;
        pkt->cred_pid = (int)sender->tgid;
        pkt->cred_uid = (int)sender->uid;
        pkt->cred_gid = (int)sender->gid;
      }
    }

    if (!peer->packet_queue_tail) {
      peer->packet_queue_head = pkt;
      peer->packet_queue_tail = pkt;
    } else {
      peer->packet_queue_tail->next = pkt;
      peer->packet_queue_tail = pkt;
    }
    peer->packet_queue_bytes += len;

    spinlock_release(&peer->recv_lock);

    unix_wake_recv_waiters(peer);
    unix_notify_epoll(peer->parent, EPOLLIN | EPOLLRDNORM);

    socket_put(peer_sock);
    return (ssize_t)len;
  }

  size_t sent = 0;
  const uint8_t *src = (const uint8_t *)buf;

  while (sent < len) {
    if (sock->closing || peer_sock->closing)
      break;

    spinlock_acquire(&peer->recv_lock);

    if (!unix_ensure_recv_buf(peer, len)) {
      spinlock_release(&peer->recv_lock);
      if (sent > 0) break;
      socket_put(peer_sock);
      return -12; // ENOMEM
    }

    size_t head  = peer->recv_buf_head;
    size_t tail  = peer->recv_buf_tail;
    size_t size  = peer->recv_buf_size;
    size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

    if (space == 0) {
      if (sent > 0) {
        spinlock_release(&peer->recv_lock);
        break; // Return what we've sent so far
      }

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) { // MSG_DONTWAIT
        spinlock_release(&peer->recv_lock);
        socket_put(peer_sock);
        return -11; // EAGAIN
      }

      struct thread *current = sched_get_current();
      wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};

      current->state = THREAD_BLOCKED;
      wait_queue_add(peer->wait, &entry);
      if (sndtimeo_deadline != 0)
        current->wakeup_ticks = sndtimeo_deadline;
      /* Recheck space under recv_lock before parking. */
      {
        size_t rh = peer->recv_buf_head;
        size_t rt = peer->recv_buf_tail;
        size_t rs = peer->recv_buf_size;
        size_t rspace = (rs > 0) ? (rh - rt - 1 + rs) % rs : 0;
        if (rspace != 0 || sent > 0 || sock->closing || peer_sock->closing || sock->error) {
          current->state = THREAD_RUNNING;
          current->wakeup_ticks = 0;
          spinlock_release(&peer->recv_lock);
          wait_queue_remove(peer->wait, &entry);
          current->state = THREAD_RUNNING;
          if (sndtimeo_deadline != 0 && lapic_timer_get_ticks() >= sndtimeo_deadline) {
            socket_put(peer_sock);
            return sent > 0 ? (ssize_t)sent : -11; // EAGAIN
          }
          if (sock->closing || peer_sock->closing) {
            socket_put(peer_sock);
            return -32; // EPIPE
          }
          if (sock->error) {
            int err = sock->error;
            socket_put(peer_sock);
            return -err;
          }
          if (rspace != 0 || sent > 0)
            continue;
          continue;
        }
      }

      spinlock_release(&peer->recv_lock);
      sched_yield();
      wait_queue_remove(peer->wait, &entry);
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;

      if (sndtimeo_deadline != 0 && lapic_timer_get_ticks() >= sndtimeo_deadline) {
        socket_put(peer_sock);
        return sent > 0 ? (ssize_t)sent : -11; // EAGAIN
      }

      if (sock->closing || peer_sock->closing) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      if (sock->error) {
        int err = sock->error;
        socket_put(peer_sock);
        return -err;
      }
      continue;
    }

    size_t to_copy = (len - sent < space) ? len - sent : space;

    tail = unix_ring_append(peer->recv_buf, size, tail, src + sent, to_copy);
    peer->recv_buf_tail = tail;
    sent += to_copy;
    peer->bytes_written += to_copy;

    unix_record_sender_credentials(peer, sched_get_current());

    spinlock_release(&peer->recv_lock);

    unix_wake_recv_waiters(peer);
    unix_notify_epoll(peer->parent, EPOLLIN | EPOLLRDNORM);
  }

  bool closed = sock->closing || peer_sock->closing;
  socket_put(peer_sock);
  if (sent == 0 && closed)
    return -32; // EPIPE
  return (ssize_t)sent;
}

ssize_t unix_recv_impl(socket_t *sock, void *buf, size_t len, int flags) {
  if (!sock || !sock->sk)
    return -9; // EBADF

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  uint64_t rcvtimeo_deadline = 0;
  if (usk->rcvtimeo_ms > 0)
    rcvtimeo_deadline = lapic_timer_get_ticks() + (uint64_t)usk->rcvtimeo_ms;

  if (sock->type == SOCK_SEQPACKET || sock->type == SOCK_DGRAM) {
    while (1) {
      spinlock_acquire(&usk->recv_lock);
      if (usk->packet_queue_head != NULL)
        break;

      if (sock->closing || usk->read_shutdown) {
        spinlock_release(&usk->recv_lock);
        return 0; // EOF
      }

      /* SEQPACKET is connection-oriented; DGRAM is connectionless! */
      if (sock->type == SOCK_SEQPACKET && sock->state != SS_CONNECTED) {
        spinlock_release(&usk->recv_lock);
        return usk->was_connected ? 0 : -107; // ENOTCONN
      }

      bool peer_shut = false;
      if (sock->type == SOCK_SEQPACKET) {
        spinlock_acquire(&sock->lock);
        if (usk->peer && usk->peer->write_shutdown)
          peer_shut = true;
        spinlock_release(&sock->lock);

        if (peer_shut) {
          spinlock_release(&usk->recv_lock);
          return 0; // EOF
        }
      }

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) { // MSG_DONTWAIT
        spinlock_release(&usk->recv_lock);
        return -11; // EAGAIN
      }

      struct thread *current = sched_get_current();
      if (thread_has_pending_signal(current)) {
        spinlock_release(&usk->recv_lock);
        return -4; // EINTR
      }

      wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
      current->state = THREAD_BLOCKED;
      wait_queue_add(usk->wait, &entry);
      if (rcvtimeo_deadline != 0)
        current->wakeup_ticks = rcvtimeo_deadline;
      /* Recheck under recv_lock: packet may have arrived between the
       * first empty check and the add (wake lost, sees RUNNING). */
      if (usk->packet_queue_head != NULL || sock->closing || usk->read_shutdown || sock->error) {
        current->state = THREAD_RUNNING;
        current->wakeup_ticks = 0;
        spinlock_release(&usk->recv_lock);
        wait_queue_remove(usk->wait, &entry);
        current->state = THREAD_RUNNING;
        if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
          return -11; // EAGAIN
        if (sock->closing || usk->read_shutdown)
          return 0;
        if (sock->error)
          return -(int)sock->error;
        continue;
      }

      spinlock_release(&usk->recv_lock);
      sched_yield();
      wait_queue_remove(usk->wait, &entry);
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;

      if (thread_has_pending_signal(current))
        return -4; // EINTR

      if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
        return -11; // EAGAIN

      if (sock->closing || usk->read_shutdown)
        return 0;
      if (sock->error)
        return -(int)sock->error;
    }

    unix_packet_t *pkt = usk->packet_queue_head;
    size_t to_copy = (len < pkt->data_len) ? len : pkt->data_len;
    if (to_copy > 0 && buf)
      memcpy(buf, pkt->data, to_copy);

    if (pkt->sender_addr_len > 0) {
      memcpy(&usk->last_sender_addr, &pkt->sender_addr, sizeof(struct sockaddr_un));
      usk->last_sender_addr_len = pkt->sender_addr_len;
    } else {
      usk->last_sender_addr_len = 0;
    }

    if (!(flags & 0x02)) { // MSG_PEEK
      usk->packet_queue_head = pkt->next;
      if (!usk->packet_queue_head)
        usk->packet_queue_tail = NULL;
      usk->packet_queue_bytes -= pkt->data_len;

      if (pkt->scm) {
        for (int i = 0; i < pkt->scm->count; i++) {
          if (pkt->scm->fds[i].node) vfs_close(pkt->scm->fds[i].node);
        }
        kfree(pkt->scm);
      }
      kfree(pkt);

      wait_queue_wake_all(usk->wait);
    }

    spinlock_release(&usk->recv_lock);

    unix_sock_t *notify_peer = NULL;
    socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
    if (notify_peer_sock) {
      unix_notify_epoll(notify_peer_sock, EPOLLOUT | EPOLLWRNORM);
      socket_put(notify_peer_sock);
    }

    return (ssize_t)to_copy;
  }

  if ((sock->closing || usk->read_shutdown) && usk->recv_buf_head == usk->recv_buf_tail)
    return 0; // EOF

  if (sock->state != SS_CONNECTED &&
      usk->recv_buf_head == usk->recv_buf_tail) {
    return usk->was_connected ? 0 : -107; // ENOTCONN
  }

  uint8_t *dest     = (uint8_t *)buf;
  size_t  received  = 0;

  while (received < len) {
    spinlock_acquire(&usk->recv_lock);

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available == 0) {
      if (sock->closing || usk->read_shutdown) {
        spinlock_release(&usk->recv_lock);
        return (ssize_t)received;
      }

      if (sock->state != SS_CONNECTED) {
        usk->accepted_orphaned = false;
        spinlock_release(&usk->recv_lock);
        if (!usk->was_connected && received == 0)
          return -107; // ENOTCONN
        return (ssize_t)received;
      }

      bool peer_shut = false;
      spinlock_acquire(&sock->lock);
      if (usk->peer && usk->peer->write_shutdown)
        peer_shut = true;
      spinlock_release(&sock->lock);

      if (peer_shut) {
        spinlock_release(&usk->recv_lock);
        return (ssize_t)received;
      }

      if (received > 0) {
        spinlock_release(&usk->recv_lock);
        break;
      }

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) { // MSG_DONTWAIT
        spinlock_release(&usk->recv_lock);
        return -11; // EAGAIN
      }

      struct thread *current = sched_get_current();
      wait_queue_entry_t entry = {.thread = current, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};

      current->state = THREAD_BLOCKED;
      wait_queue_add(usk->wait, &entry);
      if (rcvtimeo_deadline != 0)
        current->wakeup_ticks = rcvtimeo_deadline;

      /* Recheck under recv_lock before parking. */
      {
        size_t rh = usk->recv_buf_head;
        size_t rt = usk->recv_buf_tail;
        size_t rs = usk->recv_buf_size;
        size_t ravail = (rs > 0) ? (rt - rh + rs) % rs : 0;
        if (ravail != 0 || sock->closing || usk->read_shutdown || sock->error) {
          current->state = THREAD_RUNNING;
          current->wakeup_ticks = 0;
          spinlock_release(&usk->recv_lock);
          wait_queue_remove(usk->wait, &entry);
          current->state = THREAD_RUNNING;
          if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
            return received > 0 ? (ssize_t)received : -11; // EAGAIN
          if (sock->closing || usk->read_shutdown)
            return (ssize_t)received; // EOF
          if (sock->error)
            return -sock->error;
          continue;
        }
      }

      spinlock_release(&usk->recv_lock);
      sched_yield();
      wait_queue_remove(usk->wait, &entry);
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;

      if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
        return received > 0 ? (ssize_t)received : -11; // EAGAIN

      if (sock->closing || usk->read_shutdown)
        return (ssize_t)received; // EOF

      spinlock_acquire(&sock->lock);
      bool peer_shut_after_wait = (usk->peer && usk->peer->write_shutdown);
      spinlock_release(&sock->lock);
      if (peer_shut_after_wait && usk->recv_buf_head == usk->recv_buf_tail)
        return (ssize_t)received; // EOF

      if (sock->error)
        return -sock->error;
      continue;
    }

    size_t to_copy = (len - received < available) ? len - received : available;

    size_t next_head = unix_ring_consume(usk->recv_buf, size, head, dest + received, to_copy);

    if (!(flags & 0x02)) { // MSG_PEEK
      usk->recv_buf_head = (size > 0) ? next_head : 0;
      usk->bytes_read += to_copy;
      usk->scm_cred_pending = false;

      unix_discard_consumed_scm(usk);
    }

    spinlock_release(&usk->recv_lock);
    received += to_copy;

    if (sock->type == SOCK_DGRAM)
      break;
  }

  wait_queue_wake_all(usk->wait);

  unix_sock_t *notify_peer = NULL;
  socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
  if (notify_peer_sock) {
    unix_notify_epoll(notify_peer_sock, EPOLLOUT | EPOLLWRNORM);
    socket_put(notify_peer_sock);
  }

  return (ssize_t)received;
}

ssize_t unix_sendmsg_impl(socket_t *sock, struct msghdr *msg, int flags);

ssize_t unix_sendto_impl(socket_t *sock, const void *buf, size_t len,
                         int flags, struct sockaddr *dest_addr, int addrlen) {
  if (!dest_addr)
    return unix_send_impl(sock, buf, len, flags);

  struct iovec iov = {
    .iov_base = (void *)buf,
    .iov_len = len,
  };
  struct msghdr msg = {
    .msg_name = dest_addr,
    .msg_namelen = (uint32_t)addrlen,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = NULL,
    .msg_controllen = 0,
    .msg_flags = 0,
  };
  return unix_sendmsg_impl(sock, &msg, flags);
}

ssize_t unix_recvfrom_impl(socket_t *sock, void *buf, size_t len, int flags,
                           struct sockaddr *src_addr, int *addrlen) {
  ssize_t ret = unix_recv_impl(sock, buf, len, flags);
  if (ret >= 0 && src_addr && addrlen) {
    unix_sock_t *usk = sock ? (unix_sock_t *)sock->sk : NULL;
    unix_sock_t *peer = NULL;
    socket_t *peer_sock = unix_get_live_peer(sock, &peer);
    if (peer_sock && peer) {
      if (peer->addr_len > 0) {
        int to_copy = peer->addr_len < *addrlen ? peer->addr_len : *addrlen;
        memcpy(src_addr, &peer->addr, (size_t)to_copy);
        *addrlen = peer->addr_len;
      } else {
        struct sockaddr_un *sun = (struct sockaddr_un *)src_addr;
        int copy = (int)sizeof(sa_family_t) < *addrlen
                       ? (int)sizeof(sa_family_t)
                       : *addrlen;
        memset(sun, 0, (size_t)copy);
        sun->sun_family = AF_UNIX;
        *addrlen = (int)sizeof(sa_family_t);
      }
      socket_put(peer_sock);
    } else if (sock && sock->type == SOCK_DGRAM && usk && usk->last_sender_addr_len > 0) {
      int to_copy = usk->last_sender_addr_len < *addrlen ? usk->last_sender_addr_len : *addrlen;
      memcpy(src_addr, &usk->last_sender_addr, (size_t)to_copy);
      *addrlen = usk->last_sender_addr_len;
    } else {
      struct sockaddr_un *sun = (struct sockaddr_un *)src_addr;
      int copy = (int)sizeof(sa_family_t) < *addrlen
                     ? (int)sizeof(sa_family_t)
                     : *addrlen;
      memset(sun, 0, (size_t)copy);
      sun->sun_family = AF_UNIX;
      *addrlen = (int)sizeof(sa_family_t);
    }
  }
  return ret;
}

ssize_t unix_sendmsg_impl(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;
  if (sock->closing)
    return -32; // EPIPE

  unix_sock_t *peer = NULL;
  socket_t *peer_sock = unix_get_live_peer(sock, &peer);
  if (!peer_sock || !peer) {
    if (msg->msg_name && msg->msg_namelen >= sizeof(sa_family_t)) {
      struct sockaddr_un *sun = (struct sockaddr_un *)msg->msg_name;
      unix_sock_t *dusk = unix_find_socket_by_addr_ref(sun, msg->msg_namelen);
      if (dusk) {
        if (dusk->is_syslog_sink) {
          if (dusk->parent)
            socket_put(dusk->parent);
          size_t tlen = 0;
          for (size_t i = 0; i < msg->msg_iovlen; i++)
            tlen += msg->msg_iov[i].iov_len;
          return (ssize_t)tlen;
        }
        socket_t *dsock = dusk->parent;
        if (dsock && af_unix_sock_live(dusk, &dsock)) {
          peer = dusk;
          peer_sock = dsock;
        } else if (dsock) {
          socket_put(dsock);
        }
      }
    }
    if (!peer_sock || !peer) {
      if (usk->was_connected)
        return -32; // EPIPE
      return -107; // ENOTCONN
    }
  }

  size_t total_len = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++)
    total_len += msg->msg_iov[i].iov_len;

  if (peer && peer->is_syslog_sink) {
    socket_put(peer_sock);
    return (ssize_t)total_len;
  }

  struct thread *current = sched_get_current();

  if (sock->type == SOCK_SEQPACKET || sock->type == SOCK_DGRAM) {
    while (1) {
      if (sock->closing || peer_sock->closing || peer->read_shutdown) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      spinlock_acquire(&peer->recv_lock);
      if (peer->packet_queue_bytes + total_len > 4 * 1024 * 1024 && peer->packet_queue_bytes > 0) {
        if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
          spinlock_release(&peer->recv_lock);
          socket_put(peer_sock);
          return -11; // EAGAIN
        }

        struct thread *ct = sched_get_current();
        wait_queue_entry_t entry = {.thread = ct, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
        ct->state = THREAD_BLOCKED;
        wait_queue_add(peer->wait, &entry);
        /* Recheck under recv_lock before parking. */
        if (!(peer->packet_queue_bytes + total_len > 4 * 1024 * 1024 && peer->packet_queue_bytes > 0)) {
          ct->state = THREAD_RUNNING;
          spinlock_release(&peer->recv_lock);
          wait_queue_remove(peer->wait, &entry);
          ct->state = THREAD_RUNNING;
          continue;
        }

        spinlock_release(&peer->recv_lock);
        sched_yield();
        wait_queue_remove(peer->wait, &entry);
        ct->state = THREAD_RUNNING;
        continue;
      }
      break;
    }

    unix_scm_msg_t *scm_item = NULL;
    int total_fds = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
    while (cmsg) {
      if (cmsg->cmsg_len < sizeof(struct cmsghdr))
        break;
      if (cmsg->cmsg_len >= sizeof(struct cmsghdr) &&
          cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int count = (int)((cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int));
        if (count > 0) total_fds += count;
      }
      cmsg = CMSG_NXTHDR(msg, cmsg);
    }

    if (total_fds > 0) {
      scm_item = kmalloc(sizeof(unix_scm_msg_t) + total_fds * sizeof(unix_scm_fd_t));
      if (!scm_item) {
        spinlock_release(&peer->recv_lock);
        socket_put(peer_sock);
        return -12;
      }
      if (scm_item) {
        scm_item->next = NULL;
        scm_item->stream_offset = 0;
        scm_item->count = 0;

        cmsg = CMSG_FIRSTHDR(msg);
        while (cmsg) {
          if (cmsg->cmsg_len < sizeof(struct cmsghdr))
            break;
          if (cmsg->cmsg_len >= sizeof(struct cmsghdr) &&
              cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int *fds = (int *)CMSG_DATA(cmsg);
            int count = (int)((cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int));
            for (int i = 0; i < count; i++) {
              if (scm_item->count >= total_fds)
                break;
              int fd = fds[i];
              if (!unix_scm_capture_fd(current, fd, &scm_item->fds[scm_item->count])) {
                spinlock_release(&peer->recv_lock);
                unix_scm_free_msg(scm_item);
                socket_put(peer_sock);
                return -9;
              }
              scm_item->count++;
            }
          }
          cmsg = CMSG_NXTHDR(msg, cmsg);
        }

        if (scm_item->count == 0) {
          kfree(scm_item);
          scm_item = NULL;
        }
      }
    }

    unix_packet_t *pkt = kmalloc(sizeof(unix_packet_t) + total_len);
    if (!pkt) {
      if (scm_item) {
        for (int i = 0; i < scm_item->count; i++) {
          if (scm_item->fds[i].node) vfs_close(scm_item->fds[i].node);
        }
        kfree(scm_item);
      }
      spinlock_release(&peer->recv_lock);
      socket_put(peer_sock);
      return -12; // ENOMEM
    }

    pkt->next = NULL;
    pkt->data_len = total_len;
    pkt->scm = scm_item;
    pkt->cred_pending = false;
    if (usk->addr_len > 0) {
      memcpy(&pkt->sender_addr, &usk->addr, sizeof(struct sockaddr_un));
      pkt->sender_addr_len = usk->addr_len;
    } else {
      pkt->sender_addr_len = 0;
    }

    size_t offset = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
      if (msg->msg_iov[i].iov_base && msg->msg_iov[i].iov_len > 0) {
        memcpy(pkt->data + offset, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        offset += msg->msg_iov[i].iov_len;
      }
    }

    if (peer->passcred) {
      pkt->cred_pending = true;
      pkt->cred_pid = (int)current->tgid;
      pkt->cred_uid = (int)current->uid;
      pkt->cred_gid = (int)current->gid;
    }

    if (!peer->packet_queue_tail) {
      peer->packet_queue_head = pkt;
      peer->packet_queue_tail = pkt;
    } else {
      peer->packet_queue_tail->next = pkt;
      peer->packet_queue_tail = pkt;
    }
    peer->packet_queue_bytes += total_len;

    spinlock_release(&peer->recv_lock);

    unix_wake_recv_waiters(peer);
    unix_notify_epoll(peer->parent, EPOLLIN | EPOLLRDNORM);

    socket_put(peer_sock);
    return (ssize_t)total_len;
  }

  // Hold peer->recv_lock for the entire sendmsg so SCM delivery is atomic.
  spinlock_acquire(&peer->recv_lock);

  if (!unix_ensure_recv_buf(peer, total_len)) {
    spinlock_release(&peer->recv_lock);
    socket_put(peer_sock);
    return -12; // ENOMEM
  }

  while (total_len > 0) {
    size_t head  = peer->recv_buf_head;
    size_t tail  = peer->recv_buf_tail;
    size_t size  = peer->recv_buf_size;
    size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

    if (space >= total_len)
      break;

    if (space == 0) {
      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
        spinlock_release(&peer->recv_lock);
        socket_put(peer_sock);
        return -11; // EAGAIN
      }

      struct thread *ct = sched_get_current();
      wait_queue_entry_t entry = {.thread = ct, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
      ct->state = THREAD_BLOCKED;
      wait_queue_add(peer->wait, &entry);
      /* Recheck under recv_lock: space may have freed between the first
       * check and the add. */
      {
        size_t rh = peer->recv_buf_head;
        size_t rt = peer->recv_buf_tail;
        size_t rs = peer->recv_buf_size;
        size_t rspace = (rs > 0) ? (rh - rt - 1 + rs) % rs : 0;
        if (rspace != 0 || sock->closing || peer_sock->closing) {
          ct->state = THREAD_RUNNING;
          spinlock_release(&peer->recv_lock);
          wait_queue_remove(peer->wait, &entry);
          ct->state = THREAD_RUNNING;
          if (sock->closing || peer_sock->closing) {
            socket_put(peer_sock);
            return -32; // EPIPE
          }
          spinlock_acquire(&peer->recv_lock);
          continue;
        }
      }

      spinlock_release(&peer->recv_lock);
      sched_yield();
      wait_queue_remove(peer->wait, &entry);
      ct->state = THREAD_RUNNING;

      if (sock->closing || peer_sock->closing) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      spinlock_acquire(&peer->recv_lock);
      continue;
    }
    break; // Partial space – write what fits
  }

  // Deliver SCM_RIGHTS nodes atomically (lock order: recv_lock → parent->lock)
  spinlock_acquire(&peer_sock->lock);

  int total_fds = 0;
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
  while (cmsg) {
    if (cmsg->cmsg_len < sizeof(struct cmsghdr))
      break;
    if (cmsg->cmsg_len >= sizeof(struct cmsghdr) &&
        cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int count = (int)((cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) /
                        sizeof(int));
      if (count > 0)
        total_fds += count;
    }
    cmsg = CMSG_NXTHDR(msg, cmsg);
  }

  if (total_fds > 0) {
    unix_scm_msg_t *scm_item = kmalloc(sizeof(unix_scm_msg_t) + total_fds * sizeof(unix_scm_fd_t));
    if (!scm_item) {
      spinlock_release(&peer_sock->lock);
      spinlock_release(&peer->recv_lock);
      socket_put(peer_sock);
      return -12;
    }
    if (scm_item) {
      scm_item->next = NULL;
      scm_item->stream_offset = peer->bytes_written;
      scm_item->count = 0;

      cmsg = CMSG_FIRSTHDR(msg);
      while (cmsg) {
        if (cmsg->cmsg_len < sizeof(struct cmsghdr))
          break;
        if (cmsg->cmsg_len >= sizeof(struct cmsghdr) &&
            cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
          int *fds = (int *)CMSG_DATA(cmsg);
          int count = (int)((cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) /
                            sizeof(int));
          for (int i = 0; i < count; i++) {
            if (scm_item->count >= total_fds)
              break;
            int fd = fds[i];
            if (!unix_scm_capture_fd(current, fd, &scm_item->fds[scm_item->count])) {
              spinlock_release(&peer_sock->lock);
              spinlock_release(&peer->recv_lock);
              unix_scm_free_msg(scm_item);
              socket_put(peer_sock);
              return -9;
            }
            scm_item->count++;
          }
        }
        cmsg = CMSG_NXTHDR(msg, cmsg);
      }

      if (scm_item->count > 0) {
        if (!peer->scm_queue_tail) {
          peer->scm_queue_head = scm_item;
          peer->scm_queue_tail = scm_item;
        } else {
          peer->scm_queue_tail->next = scm_item;
          peer->scm_queue_tail = scm_item;
        }
      } else {
        kfree(scm_item);
      }
    }
  }

  spinlock_release(&peer_sock->lock);

  // Write iovec data into peer's ring buffer
  ssize_t total_sent = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    const uint8_t *src = (const uint8_t *)msg->msg_iov[i].iov_base;
    size_t len  = msg->msg_iov[i].iov_len;
    size_t sent = 0;

    while (sent < len) {
      size_t head  = peer->recv_buf_head;
      size_t tail  = peer->recv_buf_tail;
      size_t size  = peer->recv_buf_size;
      size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

      if (space == 0)
        break;

      size_t to_copy = (len - sent < space) ? len - sent : space;
      tail = unix_ring_append(peer->recv_buf, size, tail, src + sent, to_copy);
      peer->recv_buf_tail = tail;
      sent += to_copy;
    }
    total_sent += (ssize_t)sent;
  }

  if (total_sent > 0) {
    peer->bytes_written += (size_t)total_sent;
    unix_record_sender_credentials(peer, current);
  }

  spinlock_release(&peer->recv_lock);

  // Wake receiver
  unix_wake_recv_waiters(peer);
  unix_notify_epoll(peer->parent, EPOLLIN | EPOLLRDNORM);

  socket_put(peer_sock);
  return total_sent;
}

static void unix_fill_msg_name(socket_t *sock, struct msghdr *msg) {
  if (!msg || !msg->msg_name || msg->msg_namelen == 0)
    return;

  if (!unix_user_range_valid((uint64_t)(uintptr_t)msg->msg_name,
                             msg->msg_namelen)) {
    msg->msg_namelen = 0;
    return;
  }

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk) {
    msg->msg_namelen = 0;
    return;
  }

  spinlock_acquire(&sock->lock);
  unix_sock_t *peer = usk->peer;
  if (!peer) {
    if (sock->type == SOCK_DGRAM && usk->last_sender_addr_len > 0) {
      uint32_t cap = msg->msg_namelen;
      uint32_t copy = (uint32_t)usk->last_sender_addr_len < cap ? (uint32_t)usk->last_sender_addr_len : cap;
      memcpy(msg->msg_name, &usk->last_sender_addr, copy);
      msg->msg_namelen = (uint32_t)usk->last_sender_addr_len;
      spinlock_release(&sock->lock);
      return;
    }
    spinlock_release(&sock->lock);
    msg->msg_namelen = 0;
    return;
  }

  if (peer->addr_len > 0) {
    uint32_t cap = msg->msg_namelen;
    uint32_t copy =
        (uint32_t)peer->addr_len < cap ? (uint32_t)peer->addr_len : cap;
    memcpy(msg->msg_name, &peer->addr, copy);
    msg->msg_namelen = (uint32_t)peer->addr_len;
  } else {
    struct sockaddr_un *sun = (struct sockaddr_un *)msg->msg_name;
    uint32_t cap = msg->msg_namelen;
    uint32_t copy = (uint32_t)sizeof(sa_family_t) < cap
                        ? (uint32_t)sizeof(sa_family_t)
                        : cap;
    memset(sun, 0, copy);
    sun->sun_family = AF_UNIX;
    msg->msg_namelen = (uint32_t)sizeof(sa_family_t);
  }

  spinlock_release(&sock->lock);
}

ssize_t unix_recvmsg_impl(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;
  if (sock->closing)
    goto no_data;

  uint64_t rcvtimeo_deadline = 0;
  if (usk->rcvtimeo_ms > 0)
    rcvtimeo_deadline = lapic_timer_get_ticks() + (uint64_t)usk->rcvtimeo_ms;

  struct thread *current = sched_get_current();
  bool is_peek = (flags & 0x02) != 0; // MSG_PEEK

  if (sock->type == SOCK_SEQPACKET || sock->type == SOCK_DGRAM) {
    while (1) {
      spinlock_acquire(&usk->recv_lock);
      if (usk->packet_queue_head != NULL)
        break;

      if (sock->closing || usk->read_shutdown) {
        spinlock_release(&usk->recv_lock);
        goto no_data;
      }

      /* SEQPACKET is connection-oriented; DGRAM is connectionless! */
      if (sock->type == SOCK_SEQPACKET && sock->state != SS_CONNECTED) {
        spinlock_release(&usk->recv_lock);
        if (!usk->was_connected) return -107; // ENOTCONN
        goto no_data;
      }

      bool peer_shut = false;
      if (sock->type == SOCK_SEQPACKET) {
        spinlock_acquire(&sock->lock);
        if (usk->peer && usk->peer->write_shutdown)
          peer_shut = true;
        spinlock_release(&sock->lock);

        if (peer_shut) {
          spinlock_release(&usk->recv_lock);
          goto no_data;
        }
      }

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
        spinlock_release(&usk->recv_lock);
        return -11; // EAGAIN
      }

      struct thread *ct = sched_get_current();
      if (thread_has_pending_signal(ct)) {
        spinlock_release(&usk->recv_lock);
        return -4; // EINTR
      }

      wait_queue_entry_t entry = {.thread = ct, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
      ct->state = THREAD_BLOCKED;
      wait_queue_add(usk->wait, &entry);
      if (rcvtimeo_deadline != 0)
        ct->wakeup_ticks = rcvtimeo_deadline;
      /* Recheck under recv_lock before parking. */
      if (usk->packet_queue_head != NULL || sock->closing || usk->read_shutdown || sock->error) {
        ct->state = THREAD_RUNNING;
        ct->wakeup_ticks = 0;
        spinlock_release(&usk->recv_lock);
        wait_queue_remove(usk->wait, &entry);
        ct->state = THREAD_RUNNING;
        if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
          return -11; // EAGAIN
        if (sock->closing || usk->read_shutdown)
          goto no_data;
        if (sock->error)
          return -(int)sock->error;
        continue;
      }

      spinlock_release(&usk->recv_lock);
      sched_yield();
      wait_queue_remove(usk->wait, &entry);
      ct->state = THREAD_RUNNING;
      ct->wakeup_ticks = 0;

      if (thread_has_pending_signal(ct))
        return -4; // EINTR

      if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
        return -11; // EAGAIN

      if (sock->closing || usk->read_shutdown)
        goto no_data;
      if (sock->error)
        return -(int)sock->error;
    }

    unix_packet_t *pkt = usk->packet_queue_head;
    if (pkt->sender_addr_len > 0) {
      memcpy(&usk->last_sender_addr, &pkt->sender_addr, sizeof(struct sockaddr_un));
      usk->last_sender_addr_len = pkt->sender_addr_len;
    } else {
      usk->last_sender_addr_len = 0;
    }

    size_t control_capacity = msg->msg_controllen;
    size_t control_used = 0;
    bool have_control = msg->msg_control && control_capacity > 0;

    if (have_control &&
        !unix_user_range_valid((uint64_t)(uintptr_t)msg->msg_control,
                               control_capacity)) {
      spinlock_release(&usk->recv_lock);
      return -14; // EFAULT
    }

    if (pkt->cred_pending && have_control) {
      size_t needed = CMSG_SPACE(sizeof(struct unix_ucred));
      if (control_capacity - control_used >= needed) {
        struct cmsghdr *cmsg = (struct cmsghdr *)
            ((uint8_t *)msg->msg_control + control_used);
        cmsg->cmsg_len = CMSG_LEN(sizeof(struct unix_ucred));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_CREDENTIALS;

        struct unix_ucred *cred = (struct unix_ucred *)CMSG_DATA(cmsg);
        cred->pid = pkt->cred_pid;
        cred->uid = pkt->cred_uid;
        cred->gid = pkt->cred_gid;
        control_used += needed;
      } else {
        msg->msg_flags |= MSG_CTRUNC;
      }
    }

    if (pkt->scm != NULL && !is_peek) {
      unix_scm_msg_t *head = pkt->scm;
      if (have_control && control_capacity - control_used >= sizeof(struct cmsghdr)) {
        struct cmsghdr *cmsg = (struct cmsghdr *)
            ((uint8_t *)msg->msg_control + control_used);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;

        int *fds = (int *)CMSG_DATA(cmsg);
        size_t max_bytes_for_fds = 0;
        if (control_capacity - control_used > CMSG_ALIGN(sizeof(struct cmsghdr)))
          max_bytes_for_fds = control_capacity - control_used - CMSG_ALIGN(sizeof(struct cmsghdr));
        int max_fds = (int)(max_bytes_for_fds / sizeof(int));
        if (max_fds > head->count)
          max_fds = head->count;

        int actual_count = 0;
        for (int i = 0; i < max_fds; i++) {
          vfs_node_t *node = head->fds[i].node;
          if (!node)
            continue;
          int new_fd = unix_scm_install_fd(current, &head->fds[i], flags);
          if (new_fd >= 0) {
            fds[actual_count++] = new_fd;
          } else {
            vfs_close(node);
          }
          head->fds[i].node = NULL;
        }

        cmsg->cmsg_len = CMSG_LEN(actual_count * sizeof(int));
        control_used += CMSG_SPACE(actual_count * sizeof(int));

        if (actual_count < head->count) {
          msg->msg_flags |= MSG_CTRUNC;
          for (int i = actual_count; i < head->count; i++) {
            if (head->fds[i].node) {
              vfs_close(head->fds[i].node);
              head->fds[i].node = NULL;
            }
          }
        }
      } else if (have_control) {
        msg->msg_flags |= MSG_CTRUNC;
        for (int i = 0; i < head->count; i++) {
          if (head->fds[i].node) {
            vfs_close(head->fds[i].node);
            head->fds[i].node = NULL;
          }
        }
      }
    }

    msg->msg_controllen = control_used;

    size_t copied = 0;
    for (size_t i = 0; i < msg->msg_iovlen && copied < pkt->data_len; i++) {
      uint8_t *dest = (uint8_t *)msg->msg_iov[i].iov_base;
      size_t want = msg->msg_iov[i].iov_len;
      size_t avail = pkt->data_len - copied;
      size_t chunk = (want < avail) ? want : avail;
      if (chunk > 0 && dest) {
        memcpy(dest, pkt->data + copied, chunk);
        copied += chunk;
      }
    }

    if (pkt->data_len > copied)
      msg->msg_flags |= MSG_TRUNC;

    if (!(flags & 0x02)) { // MSG_PEEK
      usk->packet_queue_head = pkt->next;
      if (!usk->packet_queue_head)
        usk->packet_queue_tail = NULL;
      usk->packet_queue_bytes -= pkt->data_len;

      if (pkt->scm) {
        for (int i = 0; i < pkt->scm->count; i++) {
          if (pkt->scm->fds[i].node) {
            vfs_close(pkt->scm->fds[i].node);
            pkt->scm->fds[i].node = NULL;
          }
        }
        kfree(pkt->scm);
      }
      kfree(pkt);

      wait_queue_wake_all(usk->wait);
    }

    spinlock_release(&usk->recv_lock);

    unix_sock_t *notify_peer = NULL;
    socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
    if (notify_peer_sock) {
      unix_notify_epoll(notify_peer_sock, EPOLLOUT | EPOLLWRNORM);
      socket_put(notify_peer_sock);
    }

    unix_fill_msg_name(sock, msg);
    return (ssize_t)copied;
  }

  // Block until data is available; keep recv_lock held on exit from loop
  // An SCM_RIGHTS message with zero payload still makes recvmsg readable:
  // break when an SCM sits at the current stream position even with no bytes.
  while (1) {
    spinlock_acquire(&usk->recv_lock);

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available > 0)
      break; // Data ready – recv_lock stays held
    if (usk->scm_queue_head != NULL &&
        usk->bytes_read >= usk->scm_queue_head->stream_offset)
      break; // Zero-data SCM ready – deliver control without data

    if (sock->closing || (sock->state != SS_CONNECTED && usk->was_connected)) {
      spinlock_release(&usk->recv_lock);
      goto no_data;
    }
    if (sock->state != SS_CONNECTED && !usk->was_connected) {
      spinlock_release(&usk->recv_lock);
      return -107; // ENOTCONN
    }

    if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
      spinlock_release(&usk->recv_lock);
      return -11; // EAGAIN
    }

    struct thread *ct = sched_get_current();
    wait_queue_entry_t entry = {.thread = ct, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
    ct->state = THREAD_BLOCKED;
    wait_queue_add(usk->wait, &entry);
    if (rcvtimeo_deadline != 0) {
      ct->wakeup_ticks = rcvtimeo_deadline;
    }
    /* Recheck under recv_lock before parking. */
    {
      size_t rh = usk->recv_buf_head;
      size_t rt = usk->recv_buf_tail;
      size_t rs = usk->recv_buf_size;
      size_t ravail = (rs > 0) ? (rt - rh + rs) % rs : 0;
      bool scm_ready = usk->scm_queue_head != NULL &&
                       usk->bytes_read >= usk->scm_queue_head->stream_offset;
      if (ravail > 0 || scm_ready || sock->closing || sock->error) {
        ct->state = THREAD_RUNNING;
        ct->wakeup_ticks = 0;
        spinlock_release(&usk->recv_lock);
        wait_queue_remove(usk->wait, &entry);
        ct->state = THREAD_RUNNING;
        if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline)
          return -11; // EAGAIN
        if (sock->closing)
          goto no_data;
        if (sock->error)
          return -(int)sock->error;
        continue;
      }
    }

    spinlock_release(&usk->recv_lock);
    sched_yield();
    wait_queue_remove(usk->wait, &entry);
    ct->state = THREAD_RUNNING;
    ct->wakeup_ticks = 0;

    if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline) {
      return -11; // EAGAIN
    }

    if (sock->closing)
      goto no_data;

    if (sock->error)
      return -(int)sock->error;
  }

  // recv_lock held; also acquire sock->lock to dequeue SCM nodes atomically.
  // Lock order: recv_lock → parent->lock  (same as sendmsg)
  spinlock_acquire(&sock->lock);

  size_t control_capacity = msg->msg_controllen;
  size_t control_used = 0;
  bool have_control = msg->msg_control && control_capacity > 0;

  if (have_control &&
      !unix_user_range_valid((uint64_t)(uintptr_t)msg->msg_control,
                             control_capacity)) {
    spinlock_release(&sock->lock);
    spinlock_release(&usk->recv_lock);
    return -14; // EFAULT
  }

  if (usk->scm_cred_pending) {
    size_t needed = CMSG_SPACE(sizeof(struct unix_ucred));
    if (have_control) {
      if (control_capacity - control_used >= needed) {
        struct cmsghdr *cmsg = (struct cmsghdr *)
            ((uint8_t *)msg->msg_control + control_used);
        cmsg->cmsg_len = CMSG_LEN(sizeof(struct unix_ucred));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_CREDENTIALS;

        struct unix_ucred *cred = (struct unix_ucred *)CMSG_DATA(cmsg);
        cred->pid = usk->scm_cred_pid;
        cred->uid = usk->scm_cred_uid;
        cred->gid = usk->scm_cred_gid;
        control_used += needed;
      } else {
        msg->msg_flags |= MSG_CTRUNC;
      }
      if (!(flags & 0x02)) // MSG_PEEK
        usk->scm_cred_pending = false;
    }
  }

  size_t max_stream_bytes = (size_t)-1;
  if (usk->scm_queue_head != NULL && !is_peek) {
    unix_scm_msg_t *head = usk->scm_queue_head;
    if (usk->bytes_read < head->stream_offset) {
      // SCM belongs to future bytes in the stream.
      // Do not deliver SCM on this call, and cap data read at stream_offset.
      max_stream_bytes = head->stream_offset - usk->bytes_read;
    } else {
      // Stream position is at SCM boundary: deliver SCM!
      // Without a control buffer the fds are discarded (Linux: MSG_CTRUNC).
      if (!have_control) {
        msg->msg_flags |= MSG_CTRUNC;
        for (int i = 0; i < head->count; i++) {
          if (head->fds[i].node) {
            vfs_close(head->fds[i].node);
            head->fds[i].node = NULL;
          }
        }
        usk->scm_queue_head = head->next;
        if (!usk->scm_queue_head)
          usk->scm_queue_tail = NULL;
        kfree(head);
      } else if (control_capacity - control_used >= sizeof(struct cmsghdr)) {
        struct cmsghdr *cmsg = (struct cmsghdr *)
            ((uint8_t *)msg->msg_control + control_used);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;

        int *fds = (int *)CMSG_DATA(cmsg);
        size_t max_bytes_for_fds = 0;
        if (control_capacity - control_used > CMSG_ALIGN(sizeof(struct cmsghdr)))
          max_bytes_for_fds = control_capacity - control_used - CMSG_ALIGN(sizeof(struct cmsghdr));
        int max_fds = (int)(max_bytes_for_fds / sizeof(int));
        if (max_fds > head->count)
          max_fds = head->count;

        int actual_count = 0;
        for (int i = 0; i < max_fds; i++) {
          vfs_node_t *node = head->fds[i].node;
          if (!node)
            continue;
          int new_fd = unix_scm_install_fd(current, &head->fds[i], flags);
          if (new_fd >= 0) {
            fds[actual_count++] = new_fd;
          } else {
            vfs_close(node);
          }
          head->fds[i].node = NULL;
        }

        cmsg->cmsg_len = CMSG_LEN(actual_count * sizeof(int));
        control_used += CMSG_SPACE(actual_count * sizeof(int));

        if (actual_count < head->count) {
          msg->msg_flags |= MSG_CTRUNC;
          for (int i = actual_count; i < head->count; i++) {
            if (head->fds[i].node) {
              vfs_close(head->fds[i].node);
              head->fds[i].node = NULL;
            }
          }
        }

        usk->scm_queue_head = head->next;
        if (!usk->scm_queue_head)
          usk->scm_queue_tail = NULL;
        kfree(head);
      } else {
        msg->msg_flags |= MSG_CTRUNC;
        for (int i = 0; i < head->count; i++) {
          if (head->fds[i].node) {
            vfs_close(head->fds[i].node);
            head->fds[i].node = NULL;
          }
        }
        usk->scm_queue_head = head->next;
        if (!usk->scm_queue_head)
          usk->scm_queue_tail = NULL;
        kfree(head);
      }

      // If another SCM is queued after this one, cap read to that SCM's boundary
      if (usk->scm_queue_head && usk->scm_queue_head->stream_offset > usk->bytes_read) {
        max_stream_bytes = usk->scm_queue_head->stream_offset - usk->bytes_read;
      }
    }
  } else if (usk->scm_queue_head != NULL && is_peek) {
    // MSG_PEEK must not consume or install SCM fds: leave queued and cap
    // the data peek at the SCM boundary so fd/message association is kept.
    // At the boundary itself, peek the message data without consuming the SCM;
    // the real recv will deliver both together.
    unix_scm_msg_t *head = usk->scm_queue_head;
    if (usk->bytes_read < head->stream_offset) {
      max_stream_bytes = head->stream_offset - usk->bytes_read;
    } else if (head->next && head->next->stream_offset > usk->bytes_read) {
      max_stream_bytes = head->next->stream_offset - usk->bytes_read;
    }
  }

  msg->msg_controllen = control_used;

  spinlock_release(&sock->lock);

  // Read data from ring buffer (recv_lock still held)
  ssize_t total_received = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    uint8_t *dest = (uint8_t *)msg->msg_iov[i].iov_base;
    size_t   want = msg->msg_iov[i].iov_len;

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available == 0 || max_stream_bytes == 0)
      break;

    size_t to_copy = (want < available) ? want : available;
    if (to_copy > max_stream_bytes)
      to_copy = max_stream_bytes;

    if (to_copy == 0)
      break;

    size_t next_head = unix_ring_consume(usk->recv_buf, size, head, dest, to_copy);

    if (!(flags & 0x02)) { // MSG_PEEK
      usk->recv_buf_head = (size > 0) ? next_head : 0;
      usk->bytes_read += to_copy;
    }

    total_received += (ssize_t)to_copy;
    if (max_stream_bytes != (size_t)-1)
      max_stream_bytes -= to_copy;
  }

  spinlock_release(&usk->recv_lock);

  wait_queue_wake_all(usk->wait);

  unix_sock_t *notify_peer = NULL;
  socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
  if (notify_peer_sock) {
    unix_notify_epoll(notify_peer_sock, EPOLLOUT | EPOLLWRNORM);
    socket_put(notify_peer_sock);
  }

  msg->msg_flags &= ~MSG_TRUNC;
  unix_fill_msg_name(sock, msg);
  return total_received;

no_data:
  msg->msg_controllen = 0;
  msg->msg_flags      = 0;
  unix_fill_msg_name(sock, msg);
  return 0;
}