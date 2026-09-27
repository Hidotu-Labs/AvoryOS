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
    if (!usk->peer) {
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED but no peer\n");
      return -107; // ENOTCONN
    }
    struct ucred_local *cred = (struct ucred_local *)optval;
    cred->pid = usk->peer->owner_pid;
    cred->uid = usk->peer->owner_uid;
    cred->gid = usk->peer->owner_gid;
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
    klog_puts("[OK] unix_getsockopt: SO_KEEPALIVE (0)\n");
    return 0;
  }

  case 31: // SO_PEERSEC
  case 59: // SO_PEERGROUPS
    return -92; // ENOPROTOOPT (unsupported security context / groups)

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

  if (level != SOL_SOCKET)
    return -92; // ENOPROTOOPT

  switch (optname) {
  case SO_RCVBUF: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 256)       val = 256;
    if (val > 1024*1024) val = 1024*1024;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new recv buffer\n");
      return -12; // ENOMEM
    }

    spinlock_acquire(&usk->recv_lock);
    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t old_size  = usk->recv_buf_size;
    size_t available = (tail - head + old_size) % old_size;

    size_t keep = available < (size_t)val ? available : (size_t)val;
    unix_ring_consume(usk->recv_buf, old_size, head, new_buf, keep);

    usk->recv_buf_head = 0;
    usk->recv_buf_tail = keep;
    kfree(usk->recv_buf);
    usk->recv_buf      = new_buf;
    usk->recv_buf_size = (size_t)val;
    sock->rcvbuf       = val;
    spinlock_release(&usk->recv_lock);

    klog_puts("[OK] unix_setsockopt: SO_RCVBUF set to ");
    klog_uint64(val);
    klog_puts("\n");
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

    klog_puts("[OK] unix_setsockopt: SO_SNDBUF set to ");
    klog_uint64(val);
    klog_puts("\n");
    return 0;
  }

  case SO_RCVBUFFORCE: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 64) val = 64;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf)
      return -12;

    spinlock_acquire(&usk->recv_lock);
    kfree(usk->recv_buf);
    usk->recv_buf      = new_buf;
    usk->recv_buf_size = (size_t)val;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = 0;
    sock->rcvbuf       = val;
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
    klog_puts("[OK] unix_setsockopt: SO_PASSCRED set to ");
    klog_uint64(usk->passcred ? 1 : 0);
    klog_puts("\n");
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
    klog_puts("[OK] unix_setsockopt: SO_RCVTIMEO set to ");
    klog_uint64(usk->rcvtimeo_ms);
    klog_puts(" ms\n");
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
    klog_puts("[OK] unix_setsockopt: SO_SNDTIMEO set to ");
    klog_uint64(usk->sndtimeo_ms);
    klog_puts(" ms\n");
    return 0;
  }

  case 9: // SO_KEEPALIVE
    klog_puts("[OK] unix_setsockopt: SO_KEEPALIVE\n");
    return 0;

  case 12: // SO_PRIORITY
    /* Linux accepts SO_PRIORITY for every socket family; AF_UNIX has no
     * priority to apply, so it is accepted and ignored.  libpulse sets it on
     * its native-protocol socket; rejecting it only produced noise. */
    klog_puts("[OK] unix_setsockopt: SO_PRIORITY\n");
    return 0;

  case 31: // SO_PEERSEC
  case 59: // SO_PEERGROUPS
    return -92; // ENOPROTOOPT

  default:
    klog_puts("[WARN] unix_setsockopt: unknown optname ");
    klog_uint64(optname);
    klog_puts("\n");
    return -92; // ENOPROTOOPT
  }
}
