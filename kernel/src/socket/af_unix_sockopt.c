// AF_UNIX – Socket options (getsockopt / setsockopt)

#include "af_unix_internal.h"

// Local types for SO_PEERCRED and SO_RCVTIMEO / SO_SNDTIMEO
struct ucred_local {
  int pid;
  int uid;
  int gid;
};

struct timeval_local {
  int64_t tv_sec;
  int64_t tv_usec;
};

#define UCRED_SIZE   sizeof(struct ucred_local)
#define TIMEVAL_SIZE sizeof(struct timeval_local)

// ── getsockname / getpeername ─────────────────────────────────────────────────

int unix_getsockname_impl(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!sock || !sock->sk || !addr || !addrlen)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (usk->addr_len > 0) {
    // Socket has a bound address — return it.
    int copy = usk->addr_len < *addrlen ? usk->addr_len : *addrlen;
    memcpy(addr, &usk->addr, (size_t)copy);
    *addrlen = usk->addr_len;
  } else {
    // Unbound: return a minimal sockaddr_un with just the family.
    struct sockaddr_un *sun = (struct sockaddr_un *)addr;
    int copy = (int)sizeof(sa_family_t) < *addrlen
                   ? (int)sizeof(sa_family_t)
                   : *addrlen;
    memset(sun, 0, (size_t)copy);
    sun->sun_family = AF_UNIX;
    *addrlen = (int)sizeof(sa_family_t);
  }

  return 0;
}

int unix_getpeername_impl(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!sock || !sock->sk || !addr || !addrlen)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  spinlock_acquire(&sock->lock);
  unix_sock_t *peer = usk->peer;
  if (!peer) {
    spinlock_release(&sock->lock);
    return -107; // ENOTCONN
  }

  if (peer->addr_len > 0) {
    int copy = peer->addr_len < *addrlen ? peer->addr_len : *addrlen;
    memcpy(addr, &peer->addr, (size_t)copy);
    *addrlen = peer->addr_len;
  } else {
    // Peer is unbound (e.g. anonymous socketpair end or connect() client).
    struct sockaddr_un *sun = (struct sockaddr_un *)addr;
    int copy = (int)sizeof(sa_family_t) < *addrlen
                   ? (int)sizeof(sa_family_t)
                   : *addrlen;
    memset(sun, 0, (size_t)copy);
    sun->sun_family = AF_UNIX;
    *addrlen = (int)sizeof(sa_family_t);
  }

  spinlock_release(&sock->lock);
  return 0;
}

// ── getsockopt / setsockopt ───────────────────────────────────────────────────

int unix_getsockopt_impl(socket_t *sock, int level, int optname,
                         void *optval, int *optlen) {
  if (!sock || !sock->sk || !optval || !optlen)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (level != SOL_SOCKET)
    return -92; // ENOPROTOOPT

  switch (optname) {
  case SO_ERROR: {
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = socket_get_error(sock);
    *optlen = sizeof(int);
    return 0;
  }

  case SO_TYPE: {
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = sock->type;
    *optlen = sizeof(int);
    return 0;
  }

  case SO_ACCEPTCONN:
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = usk->is_listener ? 1 : 0;
    *optlen = sizeof(int);
    return 0;

  case SO_PASSCRED:
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = usk->passcred ? 1 : 0;
    *optlen = sizeof(int);
    return 0;

  case SO_PEERCRED: {
    if (*optlen < (int)UCRED_SIZE) {
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED buffer too small\n");
      return -22;
    }
    spinlock_acquire(&sock->lock);
    if (!usk->peer) {
      // Peer gone: serve the cached cred from when it was connected, like
      // Linux. Only never-connected sockets get ENOTCONN.
      if (usk->was_connected && usk->peer_cred_valid) {
        struct ucred_local *cached = (struct ucred_local *)optval;
        cached->pid = (int)usk->peer_cred_pid;
        cached->uid = usk->peer_cred_uid;
        cached->gid = usk->peer_cred_gid;
        spinlock_release(&sock->lock);
        *optlen = (int)UCRED_SIZE;
        return 0;
      }
      spinlock_release(&sock->lock);
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED but no peer\n");
      return -107; // ENOTCONN
    }
    struct ucred_local *cred = (struct ucred_local *)optval;
    cred->pid = usk->peer->owner_pid;
    cred->uid = usk->peer->owner_uid;
    cred->gid = usk->peer->owner_gid;
    // Refresh the disconnect cache on every successful live query.
    usk->peer_cred_pid = usk->peer->owner_pid;
    usk->peer_cred_uid = usk->peer->owner_uid;
    usk->peer_cred_gid = usk->peer->owner_gid;
    usk->peer_cred_valid = true;
    spinlock_release(&sock->lock);
    *optlen = (int)UCRED_SIZE;
    return 0;
  }

  case SO_RCVTIMEO:
    if (*optlen < (int)sizeof(int))
      return -22;
    if (*optlen >= (int)TIMEVAL_SIZE) {
      struct timeval_local *tv = (struct timeval_local *)optval;
      tv->tv_sec  = usk->rcvtimeo_ms / 1000;
      tv->tv_usec = (usk->rcvtimeo_ms % 1000) * 1000;
      *optlen = (int)TIMEVAL_SIZE;
    } else {
      *(int *)optval = usk->rcvtimeo_ms;
      *optlen = sizeof(int);
    }
    return 0;

  case SO_SNDTIMEO:
    if (*optlen < (int)sizeof(int))
      return -22;
    if (*optlen >= (int)TIMEVAL_SIZE) {
      struct timeval_local *tv = (struct timeval_local *)optval;
      tv->tv_sec  = usk->sndtimeo_ms / 1000;
      tv->tv_usec = (usk->sndtimeo_ms % 1000) * 1000;
      *optlen = (int)TIMEVAL_SIZE;
    } else {
      *(int *)optval = usk->sndtimeo_ms;
      *optlen = sizeof(int);
    }
    return 0;

  case 9: { // SO_KEEPALIVE
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = 0;
    *optlen = sizeof(int);
    klog_debug_puts("[OK] unix_getsockopt: SO_KEEPALIVE (0)\n");
    return 0;
  }

  case 59: { // SO_PEERGROUPS
    uint32_t groups[32], count;
    spinlock_acquire(&sock->lock);
    if (usk->peer) {
      unix_cache_peer_groups(usk, usk->peer);
    } else if (usk->was_connected && usk->peer_cred_valid) {
      // Use the cached connection credentials after disconnect.
    } else {
      spinlock_release(&sock->lock);
      return -107; // ENOTCONN
    }
    count = usk->peer_group_count;
    memcpy(groups, usk->peer_groups, count * sizeof(uint32_t));
    spinlock_release(&sock->lock);
    int needed_bytes = (int)(count * sizeof(uint32_t));
    if (*optlen < needed_bytes) {
      *optlen = needed_bytes;
      return -34; // ERANGE
    }
    if (count > 0 && copy_to_user(optval, groups, needed_bytes) != 0)
      return -14;
    *optlen = needed_bytes;
    return 0;
  }

  case 31: // SO_PEERSEC
  case 77: // SO_PEERPIDFD
    return -92; // ENOPROTOOPT (unsupported security context / pidfd)

  default:
    klog_puts("[WARN] unix_getsockopt: unknown optname ");
    klog_uint64(optname);
    klog_puts("\n");
    return -92; // ENOPROTOOPT
  }
}

int unix_setsockopt_impl(socket_t *sock, int level, int optname,
                         const void *optval, int optlen) {
  if (!sock || !sock->sk || !optval)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  /* Blind TCP_NODELAY from libraries (GLib GSocket, Qt) that don't check
   * family before setting socket flags */
  if ((level == 6 /* SOL_TCP/IPPROTO_TCP */) && optname == 1 /* TCP_NODELAY */)
    return 0;

  if (level != SOL_SOCKET)
    return -92; // ENOPROTOOPT

  switch (optname) {
  case SO_RCVBUF: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 256)       val = 256;
    if (val > 4*1024*1024) val = 4*1024*1024;
    size_t sz = unix_round_pow2((size_t)val);

    uint8_t *new_buf = kmalloc(sz);
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new recv buffer\n");
      return -12; // ENOMEM
    }

    spinlock_acquire(&usk->recv_lock);
    size_t head      = usk->recv_buf_head;
    size_t old_size  = usk->recv_buf_size;
    size_t count     = usk->recv_buf_count;

    size_t keep = count < sz ? count : sz;
    if (usk->recv_buf && keep > 0) {
      unix_ring_consume(usk->recv_buf, old_size, head, new_buf, keep);
    }

    usk->recv_buf_head = 0;
    usk->recv_buf_tail = keep;
    usk->recv_buf_count = keep;
    kfree(usk->recv_buf);
    usk->recv_buf      = new_buf;
    usk->recv_buf_size = sz;
    usk->recv_buf_mask = sz - 1;
    sock->rcvbuf       = (int)sz;
    spinlock_release(&usk->recv_lock);

    klog_debug_puts("[OK] unix_setsockopt: SO_RCVBUF set to ");
    klog_debug_uint64(val);
    klog_debug_puts("\n");
    return 0;
  }

  case SO_SNDBUF: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 256)       val = 256;
    if (val > 1024*1024) val = 1024*1024;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new send buffer\n");
      return -12;
    }

    spinlock_acquire(&usk->send_lock);
    kfree(usk->send_buf);
    usk->send_buf      = new_buf;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf       = val;
    spinlock_release(&usk->send_lock);

    klog_debug_puts("[OK] unix_setsockopt: SO_SNDBUF set to ");
    klog_debug_uint64(val);
    klog_debug_puts("\n");
    return 0;
  }

  case SO_RCVBUFFORCE: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 64) val = 64;
    size_t sz = unix_round_pow2((size_t)val);

    uint8_t *new_buf = kmalloc(sz);
    if (!new_buf)
      return -12;

    spinlock_acquire(&usk->recv_lock);
    kfree(usk->recv_buf);
    usk->recv_buf      = new_buf;
    usk->recv_buf_size = sz;
    usk->recv_buf_mask = sz - 1;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = 0;
    usk->recv_buf_count = 0;
    sock->rcvbuf       = (int)sz;
    spinlock_release(&usk->recv_lock);
    return 0;
  }

  case SO_SNDBUFFORCE: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 64) val = 64;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf)
      return -12;

    spinlock_acquire(&usk->send_lock);
    kfree(usk->send_buf);
    usk->send_buf      = new_buf;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf       = val;
    spinlock_release(&usk->send_lock);
    return 0;
  }

  case SO_PASSCRED:
    if (optlen < (int)sizeof(int))
      return -22;
    usk->passcred = (*(const int *)optval != 0);
    klog_debug_puts("[OK] unix_setsockopt: SO_PASSCRED set to ");
    klog_debug_uint64(usk->passcred ? 1 : 0);
    klog_debug_puts("\n");
    return 0;

  case SO_RCVTIMEO: {
    if (optlen < (int)sizeof(int))
      return -22;
    if (optlen >= (int)TIMEVAL_SIZE) {
      const struct timeval_local *tv = (const struct timeval_local *)optval;
      usk->rcvtimeo_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    } else {
      usk->rcvtimeo_ms = *(const int *)optval;
    }
    klog_debug_puts("[OK] unix_setsockopt: SO_RCVTIMEO set to ");
    klog_debug_uint64(usk->rcvtimeo_ms);
    klog_debug_puts(" ms\n");
    return 0;
  }

  case SO_SNDTIMEO: {
    if (optlen < (int)sizeof(int))
      return -22;
    if (optlen >= (int)TIMEVAL_SIZE) {
      const struct timeval_local *tv = (const struct timeval_local *)optval;
      usk->sndtimeo_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    } else {
      usk->sndtimeo_ms = *(const int *)optval;
    }
    klog_debug_puts("[OK] unix_setsockopt: SO_SNDTIMEO set to ");
    klog_debug_uint64(usk->sndtimeo_ms);
    klog_debug_puts(" ms\n");
    return 0;
  }

  case 9: // SO_KEEPALIVE
    klog_debug_puts("[OK] unix_setsockopt: SO_KEEPALIVE\n");
    return 0;

  case 12: // SO_PRIORITY
    /* Linux accepts SO_PRIORITY for every socket family; AF_UNIX has no
     * priority to apply, so it is accepted and ignored.  libpulse sets it on
     * its native-protocol socket; rejecting it only produced noise. */
    klog_debug_puts("[OK] unix_setsockopt: SO_PRIORITY\n");
    return 0;

  case 31: // SO_PEERSEC
  case 59: // SO_PEERGROUPS
    return -92; // ENOPROTOOPT

  case 2:  // SO_REUSEADDR
  case 15: // SO_REUSEPORT
    /* Deliberately unsupported on AF_UNIX, same as Linux (ENOPROTOOPT).
     * GLib/GVFS sets these unconditionally before bind and handles the
     * error; keep the errno but skip the WARN so desktop startup stays
     * quiet. */
    return -92; // ENOPROTOOPT

  default:
    klog_puts("[WARN] unix_setsockopt: unknown optname ");
    klog_uint64(optname);
    klog_puts("\n");
    return -92; // ENOPROTOOPT
  }
}
