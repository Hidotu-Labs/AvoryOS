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

static int unix_opt_get_len(const int *optlen, int *klen) {
  if (!is_user_range(optlen, sizeof(int)) ||
      copy_from_user(klen, optlen, sizeof(int)) != 0)
    return -14;
  return 0;
}

static int unix_opt_put_int(void *optval, int *optlen, int klen, int val) {
  if (klen < (int)sizeof(int))
    return -22;
  int out_len = (int)sizeof(int);
  if (!is_user_range(optval, sizeof(int)) ||
      copy_to_user(optval, &val, sizeof(int)) != 0 ||
      copy_to_user(optlen, &out_len, sizeof(int)) != 0)
    return -14;
  return 0;
}

static int unix_opt_get_int(const void *optval, int optlen, int *val) {
  if (optlen < (int)sizeof(int))
    return -22;
  if (!is_user_range(optval, sizeof(int)) ||
      copy_from_user(val, optval, sizeof(int)) != 0)
    return -14;
  return 0;
}

int unix_getsockopt_impl(socket_t *sock, int level, int optname,
                         void *optval, int *optlen) {
  if (!sock || !sock->sk || !optval || !optlen)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (level != SOL_SOCKET)
    return -92; // ENOPROTOOPT

  int klen = 0;
  int lerr = unix_opt_get_len(optlen, &klen);
  if (lerr < 0)
    return lerr;

  switch (optname) {
  case SO_ERROR:
    return unix_opt_put_int(optval, optlen, klen, socket_get_error(sock));

  case SO_TYPE:
    return unix_opt_put_int(optval, optlen, klen, sock->type);

  case SO_ACCEPTCONN:
    return unix_opt_put_int(optval, optlen, klen, usk->is_listener ? 1 : 0);

  case SO_PASSCRED:
    return unix_opt_put_int(optval, optlen, klen, usk->passcred ? 1 : 0);

  case SO_PEERCRED: {
    if (klen < (int)UCRED_SIZE) {
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED buffer too small\n");
      return -22;
    }
    struct ucred_local kcred;
    spinlock_acquire(&sock->lock);
    if (!usk->peer) {
      // Peer gone: serve the cached cred from when it was connected, like
      // Linux. Only never-connected sockets get ENOTCONN.
      if (usk->was_connected && usk->peer_cred_valid) {
        kcred.pid = (int)usk->peer_cred_pid;
        kcred.uid = usk->peer_cred_uid;
        kcred.gid = usk->peer_cred_gid;
        spinlock_release(&sock->lock);
      } else {
        spinlock_release(&sock->lock);
        klog_puts("[WARN] unix_getsockopt: SO_PEERCRED but no peer\n");
        return -107; // ENOTCONN
      }
    } else {
      kcred.pid = usk->peer->owner_pid;
      kcred.uid = usk->peer->owner_uid;
      kcred.gid = usk->peer->owner_gid;
      // Refresh the disconnect cache on every successful live query.
      usk->peer_cred_pid = usk->peer->owner_pid;
      usk->peer_cred_uid = usk->peer->owner_uid;
      usk->peer_cred_gid = usk->peer->owner_gid;
      usk->peer_cred_valid = true;
      spinlock_release(&sock->lock);
    }
    int out_len = (int)UCRED_SIZE;
    if (!is_user_range(optval, UCRED_SIZE) ||
        copy_to_user(optval, &kcred, UCRED_SIZE) != 0 ||
        copy_to_user(optlen, &out_len, sizeof(int)) != 0)
      return -14;
    return 0;
  }

  case SO_RCVTIMEO:
    if (klen < (int)sizeof(int))
      return -22;
    if (klen >= (int)TIMEVAL_SIZE) {
      struct timeval_local tv = {
        .tv_sec = usk->rcvtimeo_ms / 1000,
        .tv_usec = (usk->rcvtimeo_ms % 1000) * 1000,
      };
      int out_len = (int)TIMEVAL_SIZE;
      if (!is_user_range(optval, TIMEVAL_SIZE) ||
          copy_to_user(optval, &tv, TIMEVAL_SIZE) != 0 ||
          copy_to_user(optlen, &out_len, sizeof(int)) != 0)
        return -14;
      return 0;
    }
    return unix_opt_put_int(optval, optlen, klen, usk->rcvtimeo_ms);

  case SO_SNDTIMEO:
    if (klen < (int)sizeof(int))
      return -22;
    if (klen >= (int)TIMEVAL_SIZE) {
      struct timeval_local tv = {
        .tv_sec = usk->sndtimeo_ms / 1000,
        .tv_usec = (usk->sndtimeo_ms % 1000) * 1000,
      };
      int out_len = (int)TIMEVAL_SIZE;
      if (!is_user_range(optval, TIMEVAL_SIZE) ||
          copy_to_user(optval, &tv, TIMEVAL_SIZE) != 0 ||
          copy_to_user(optlen, &out_len, sizeof(int)) != 0)
        return -14;
      return 0;
    }
    return unix_opt_put_int(optval, optlen, klen, usk->sndtimeo_ms);

  case 9: { // SO_KEEPALIVE
    int r = unix_opt_put_int(optval, optlen, klen, 0);
    if (r == 0)
      klog_debug_puts("[OK] unix_getsockopt: SO_KEEPALIVE (0)\n");
    return r;
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
    if (klen < needed_bytes) {
      if (copy_to_user(optlen, &needed_bytes, sizeof(int)) != 0)
        return -14;
      return -34; // ERANGE
    }
    if (count > 0 && (!is_user_range(optval, (size_t)needed_bytes) ||
                      copy_to_user(optval, groups, needed_bytes) != 0))
      return -14;
    if (copy_to_user(optlen, &needed_bytes, sizeof(int)) != 0)
      return -14;
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

/* Resize the receive ring to `sz` bytes (a power of two), keeping every byte
 * already queued: Linux never drops data on SO_RCVBUF(FORCE). sd-bus raises
 * the buffer of each accepted connection, and a client's AUTH line that
 * arrived first used to be discarded, deadlocking both sides (random systemd
 * start jobs hung on PID 1's private bus). */
static int unix_resize_recv_buf(socket_t *sock, unix_sock_t *usk, size_t sz) {
  for (;;) {
    uint8_t *new_buf = kmalloc(sz);
    if (!new_buf) {
      extern size_t mm_file_cache_reclaim(size_t target_pages);
      extern size_t vfs_dentry_reclaim_pages(size_t target_pages);
      mm_file_cache_reclaim(sz / 4096 + 16);
      vfs_dentry_reclaim_pages(64);
      new_buf = kmalloc(sz);
    }
    while (!new_buf && sz > UNIX_RECV_BUF_MIN && sz > usk->recv_buf_size) {
      size_t smaller = sz >> 1;
      if (smaller < UNIX_RECV_BUF_MIN || smaller <= usk->recv_buf_size)
        break;
      sz = smaller;
      new_buf = kmalloc(sz);
    }
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new recv buffer\n");
      return -12; // ENOMEM
    }
    spinlock_acquire(&usk->recv_lock);
    size_t count = usk->recv_buf_count;
    if (count > sz) {
      /* More is queued than the requested size: grow to fit and retry. */
      spinlock_release(&usk->recv_lock);
      kfree(new_buf);
      sz = unix_round_pow2(count);
      continue;
    }
    if (usk->recv_buf && count > 0)
      unix_ring_consume(usk->recv_buf, usk->recv_buf_size, usk->recv_buf_head,
                        new_buf, count);
    kfree(usk->recv_buf);
    usk->recv_buf      = new_buf;
    usk->recv_buf_size = sz;
    usk->recv_buf_mask = sz - 1;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = count;
    usk->recv_buf_count = count;
    sock->rcvbuf       = (int)sz;
    spinlock_release(&usk->recv_lock);
    return 0;
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
    int val = 0;
    int err = unix_opt_get_int(optval, optlen, &val);
    if (err < 0)
      return err;
    if (val < 256)       val = 256;
    if (val > 4*1024*1024) val = 4*1024*1024;
    int r = unix_resize_recv_buf(sock, usk, unix_round_pow2((size_t)val));
    if (r)
      return r;
    klog_debug_puts("[OK] unix_setsockopt: SO_RCVBUF set to ");
    klog_debug_uint64(val);
    klog_debug_puts("\n");
    return 0;
  }

  case SO_SNDBUF: {
    int val = 0;
    int err = unix_opt_get_int(optval, optlen, &val);
    if (err < 0)
      return err;
    if (val < 256)       val = 256;
    if (val > 1024*1024) val = 1024*1024;

    /* AF_UNIX sends land in the peer's receive ring; send_buf never holds
     * data. Allocating up to 1 MiB contiguous here only made the call fail
     * with ENOMEM under fragmentation (Linux never fails SO_SNDBUF so). */
    spinlock_acquire(&usk->send_lock);
    uint8_t *old_buf = usk->send_buf;
    usk->send_buf      = NULL;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf       = val;
    spinlock_release(&usk->send_lock);
    kfree(old_buf);

    klog_debug_puts("[OK] unix_setsockopt: SO_SNDBUF set to ");
    klog_debug_uint64(val);
    klog_debug_puts("\n");
    return 0;
  }

  case SO_RCVBUFFORCE: {
    int val = 0;
    int err = unix_opt_get_int(optval, optlen, &val);
    if (err < 0)
      return err;
    if (val < 64) val = 64;
    return unix_resize_recv_buf(sock, usk, unix_round_pow2((size_t)val));
  }

  case SO_SNDBUFFORCE: {
    int val = 0;
    int err = unix_opt_get_int(optval, optlen, &val);
    if (err < 0)
      return err;
    if (val < 64) val = 64;

    spinlock_acquire(&usk->send_lock);
    uint8_t *old_buf = usk->send_buf;
    usk->send_buf      = NULL;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf       = val;
    spinlock_release(&usk->send_lock);
    kfree(old_buf);
    return 0;
  }

  case SO_PASSCRED: {
    int val = 0;
    int err = unix_opt_get_int(optval, optlen, &val);
    if (err < 0)
      return err;
    usk->passcred = (val != 0);
    klog_debug_puts("[OK] unix_setsockopt: SO_PASSCRED set to ");
    klog_debug_uint64(usk->passcred ? 1 : 0);
    klog_debug_puts("\n");
    return 0;
  }

  case SO_RCVTIMEO: {
    if (optlen < (int)sizeof(int))
      return -22;
    if (optlen >= (int)TIMEVAL_SIZE) {
      struct timeval_local tv;
      if (!is_user_range(optval, TIMEVAL_SIZE) ||
          copy_from_user(&tv, optval, TIMEVAL_SIZE) != 0)
        return -14;
      usk->rcvtimeo_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    } else {
      int val = 0;
      int err = unix_opt_get_int(optval, optlen, &val);
      if (err < 0)
        return err;
      usk->rcvtimeo_ms = val;
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
      struct timeval_local tv;
      if (!is_user_range(optval, TIMEVAL_SIZE) ||
          copy_from_user(&tv, optval, TIMEVAL_SIZE) != 0)
        return -14;
      usk->sndtimeo_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    } else {
      int val = 0;
      int err = unix_opt_get_int(optval, optlen, &val);
      if (err < 0)
        return err;
      usk->sndtimeo_ms = val;
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

  case 29: // SO_TIMESTAMP
  case 35: // SO_TIMESTAMPNS
  case 37: // SO_TIMESTAMPING
    /* Accepted, but no SCM_TIMESTAMP* control message is delivered;
     * receivers then stamp messages themselves. systemd-journald treats a
     * failing SO_TIMESTAMP on its syslog socket as fatal and exits. */
    return 0;

  case 34: // SO_PASSSEC
  case 76: // SO_PASSPIDFD
  case 83: // SO_PASSRIGHTS
    /* Not implemented; callers (systemd) treat ENOPROTOOPT as optional.
     * Skip the WARN so boot logs stay readable. */
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
