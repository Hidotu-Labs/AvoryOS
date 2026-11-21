#include "../sched/capability.h"
// Socket Syscalls: socket, socketpair, bind, connect, listen, accept, etc.

#include "../console/klog.h"
#include "../cpu/ktrack.h"
#include "../fs/vfs.h"
#include "../include/arch/uaccess.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include "../socket/socket_internal.h"
#include "syscall.h"
#include "sys_io_shared.h"
#include "sysdiag.h"
#include <stdint.h>

#define SOCKET_FD_FLAGS_CLOEXEC_BIT (1u << 24)

/* Release the descriptor lookup reference on every syscall return path. */
static void socket_release_fd(socket_t **sock) {
  if (*sock)
    socket_fd_put(*sock);
}

/* Linux distinguishes a closed/invalid descriptor (EBADF) from a valid
 * descriptor whose underlying object is not a socket (ENOTSOCK).  Libraries
 * depend on the difference: PulseAudio's pa_write() tries send() first and
 * only falls back to write() when errno is ENOTSOCK, so answering EBADF for
 * its wakeup pipe makes every libpulse mainloop wakeup fail.  GLib probes
 * stderr with getpeername() for the same reason. */
static uint64_t socket_eno_notsock(int fd) {
  struct thread *current = sched_get_current();
  bool fd_exists = false;
  if (current && current->files && fd >= 0 && fd < MAX_FDS) {
    spinlock_acquire(&current->files->lock);
    if ((uint32_t)fd < current->files->capacity)
      fd_exists = fd_is_valid_node(current->fds[fd]);
    spinlock_release(&current->files->lock);
  }
  return fd_exists ? (uint64_t)-88 : (uint64_t)-9;
}

static int copy_sockaddr_out(uint64_t addr_ptr, uint64_t addrlen_ptr,
                             const struct sockaddr *kaddr, int kaddrlen) {
  /* accept() skips this helper when the caller omits its peer address.
   * Name queries must still publish the full length for a zero-byte buffer. */
  if (!addrlen_ptr)
    return -14;

  if (!is_user_ptr(addr_ptr) || !is_user_ptr(addrlen_ptr))
    return -14; // EFAULT

  if (!is_user_range((void *)addrlen_ptr, sizeof(int)))
    return -14;

  int user_len = 0;
  if (copy_from_user(&user_len, (void *)addrlen_ptr, sizeof(int)) != 0)
    return -14;
  if (user_len < 0)
    return -22; // EINVAL

  int copy = kaddrlen < user_len ? kaddrlen : user_len;
  if (copy > 0) {
    if (!is_user_range((void *)addr_ptr, (size_t)copy))
      return -14;
    if (copy_to_user((void *)addr_ptr, kaddr, (unsigned long)copy) != 0)
      return -14;
  }

  if (copy_to_user((void *)addrlen_ptr, &kaddrlen, sizeof(int)) != 0)
    return -14;

  return 0;
}

// Syscall: socket(int domain, int type, int protocol)
// ─────────────────────── Returns: file descriptor or negative error
static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                           uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  KTRACK(KSUBSYS_SOCKET);
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int dom = (int)domain;
  int typ = (int)type;
  int proto = (int)protocol;
  if (sched_get_current() && sched_get_current()->net_ns && dom == AF_QIPCRTR)
    return (uint64_t)-EAFNOSUPPORT;
  if ((dom == AF_INET || dom == AF_INET6) &&
      ((typ & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_RAW) &&
      !capability_has(sched_get_current(), CAP_NET_RAW)) return (uint64_t)-1;

  /* There is no audit subsystem yet. Linux-PAM treats EPROTONOSUPPORT
   * from audit_open() as auditing unavailable and preserves the PAM result.
   * Accepting this socket instead makes libaudit wait for an ACK that the
   * netlink stub never produces, stalling SDDM before its greeter starts. */
  if (dom == AF_NETLINK && proto == NETLINK_AUDIT)
    return (uint64_t)-EPROTONOSUPPORT;

  // Create socket
  socket_t *sock = socket_create(dom, typ, proto);
  if (!sock) {
    // Determine error - extract base type for comparison
    int base = typ & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC;
    if (dom != AF_UNIX && dom != AF_INET && dom != AF_INET6 &&
        dom != AF_NETLINK && dom != AF_QIPCRTR) {
      char buf[32];
      snprintf(buf, sizeof(buf), "af_domain=%d", dom);
      sysdiag_record_unimplemented_feature("socket", buf, sched_get_current(), -EAFNOSUPPORT);
      return (uint64_t)-EAFNOSUPPORT;
    }
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_RAW &&
        base != SOCK_SEQPACKET) {
      char buf[32];
      snprintf(buf, sizeof(buf), "sock_type=%d", base);
      sysdiag_record_unimplemented_feature("socket", buf, sched_get_current(), -EPROTONOSUPPORT);
      return (uint64_t)-EPROTONOSUPPORT;
    }
    return (uint64_t)-12; // ENOMEM
  }

  // Allocate FD
  int fd = socket_alloc_fd(sock);
  if (fd < 0) {
    socket_put(sock);
    return (uint64_t)fd;
  }

  return (uint64_t)fd;
}

// Syscall: socketpair(int domain, int type, int protocol, int sv[2])
// ──────── Returns: 0 on success, negative error on failure
static uint64_t sys_socketpair(uint64_t domain, uint64_t type,
                               uint64_t protocol, uint64_t sv_ptr,
                               uint64_t _arg4, uint64_t _arg5) {
  (void)_arg4;
  (void)_arg5;

  int dom = (int)domain;
  int typ = (int)type;
  int proto = (int)protocol;
  if (sched_get_current() && sched_get_current()->net_ns && dom == AF_QIPCRTR)
    return (uint64_t)-EAFNOSUPPORT;
  if ((dom == AF_INET || dom == AF_INET6) &&
      ((typ & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_RAW) &&
      !capability_has(sched_get_current(), CAP_NET_RAW)) return (uint64_t)-1;

  // Validate sv pointer
  if (!sv_ptr || !is_user_range((const void *)sv_ptr, sizeof(int) * 2) || (sv_ptr & 3)) {
    return (uint64_t)-14; // EFAULT
  }

  // Create socket pair
  socket_t *socks[2] = {NULL, NULL};
  int ret = socket_create_pair(dom, typ, proto, socks);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  // Allocate FDs
  int fd0 = socket_alloc_fd(socks[0]);
  if (fd0 < 0) {
    socket_put(socks[0]);
    socket_put(socks[1]);
    return (uint64_t)fd0;
  }

  int fd1 = socket_alloc_fd(socks[1]);
  if (fd1 < 0) {
    socket_close_fd(fd0);
    socket_put(socks[1]);
    return (uint64_t)fd1;
  }

  // Write FDs to user space
  int sv[2] = {fd0, fd1};
  if (copy_to_user((void *)sv_ptr, sv, sizeof(sv)) != 0) {
    socket_close_fd(fd0);
    socket_close_fd(fd1);
    return (uint64_t)-14;
  }

  return 0;
}

// Syscall: bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
static uint64_t sys_bind(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen,
                         uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;
  struct sockaddr *addr = (struct sockaddr *)addr_ptr;

  // Validate address pointer
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  uint8_t kaddr_buf[128];
  if (addrlen > sizeof(kaddr_buf))
  return (uint64_t)-22; // EINVAL

  if (!is_user_range((const void *)addr_ptr, addrlen))
  return (uint64_t)-14; // EFAULT

  memset(kaddr_buf, 0, sizeof(kaddr_buf));
  if (copy_from_user(kaddr_buf, (const void *)addr_ptr, addrlen) != 0)
  return (uint64_t)-14; // EFAULT
  struct sockaddr *kaddr = (struct sockaddr *)kaddr_buf;
  if (kaddr->sa_family == AF_UNIX && addrlen >= (int)offsetof(struct sockaddr_un, sun_path)) {
    struct sockaddr_un *sun = (struct sockaddr_un *)kaddr;
    int path_end = (int)addrlen - (int)offsetof(struct sockaddr_un, sun_path);
    if (path_end >= 0 && path_end < (int)sizeof(sun->sun_path))
      sun->sun_path[path_end] = '\0';
  }

  int ret = socket_bind(sock, kaddr, (int)addrlen);
  return (uint64_t)ret;
}

// Syscall: connect(int sockfd, const struct sockaddr *addr, socklen_t
// addrlen)
static uint64_t sys_connect(uint64_t sockfd, uint64_t addr_ptr,
                            uint64_t addrlen, uint64_t _arg3, uint64_t _arg4,
                            uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Validate address pointer and range
  if (!is_user_ptr(addr_ptr) || !vmm_is_user_addr_range_valid(addr_ptr, addrlen)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  // Copy sockaddr safely into kernel stack buffer before reading fields
  uint8_t kaddr_buf[128];
  if (addrlen > sizeof(kaddr_buf))
  return (uint64_t)-22; // EINVAL

  memset(kaddr_buf, 0, sizeof(kaddr_buf));
  if (!is_user_range((const void *)addr_ptr, addrlen) ||
      copy_from_user(kaddr_buf, (const void *)addr_ptr, addrlen) != 0)
  return (uint64_t)-14; // EFAULT
  struct sockaddr *kaddr = (struct sockaddr *)kaddr_buf;
  if (kaddr->sa_family == AF_UNIX && addrlen >= (int)offsetof(struct sockaddr_un, sun_path)) {
    struct sockaddr_un *sun = (struct sockaddr_un *)kaddr;
    int path_end = (int)addrlen - (int)offsetof(struct sockaddr_un, sun_path);
    if (path_end >= 0 && path_end < (int)sizeof(sun->sun_path))
      sun->sun_path[path_end] = '\0';
  }

  int ret = socket_connect(sock, kaddr, (int)addrlen);
  return (uint64_t)ret;
}

// Syscall: listen(int sockfd, int backlog)
static uint64_t sys_listen(uint64_t sockfd, uint64_t backlog, uint64_t _arg2,
                           uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg2;
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;
  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  int ret = socket_listen(sock, (int)backlog);
  return (uint64_t)ret;
}

static uint64_t sys_accept_impl(uint64_t sockfd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t flags) {
  int fd = (int)sockfd;
  struct thread *t = sched_get_current();

  if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
    return (uint64_t)-22; // EINVAL
  }

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  // Accept connection
  socket_t *newsock = NULL;
  int ret = socket_accept(sock, &newsock);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  if (flags & SOCK_NONBLOCK) {
    newsock->flags |= SOCK_NONBLOCK;
  }

  // Fill peer address if the caller provided a buffer
  if (addr_ptr && addrlen_ptr) {
    uint8_t kaddr_buf[128];
    struct sockaddr *kaddr = (struct sockaddr *)kaddr_buf;
    int kaddrlen = (int)sizeof(kaddr_buf);

    if (newsock->ops && newsock->ops->getpeername &&
        newsock->ops->getpeername(newsock, kaddr, &kaddrlen) == 0) {
      int err = copy_sockaddr_out(addr_ptr, addrlen_ptr, kaddr, kaddrlen);
      if (err < 0) {
        socket_put(newsock);
        return (uint64_t)err;
      }
    } else {
      // rustix panics on addrlen > 0 with AF_UNSPEC; return anonymous unix.
      struct sockaddr_un anon;
      memset(&anon, 0, sizeof(anon));
      anon.sun_family = AF_UNIX;
      int err = copy_sockaddr_out(addr_ptr, addrlen_ptr,
                                  (struct sockaddr *)&anon,
                                  (int)sizeof(sa_family_t));
      if (err < 0) {
        socket_put(newsock);
        return (uint64_t)err;
      }
    }
  }

  // Allocate FD for new socket
  int newfd = socket_alloc_fd(newsock);
  if (newfd < 0) {
    socket_put(newsock);
    return (uint64_t)newfd;
  }

  if (FD_IN_RANGE(t, newfd)) {
    if (flags & SOCK_CLOEXEC)
      t->fd_flags[newfd] |= SOCKET_FD_FLAGS_CLOEXEC_BIT;
    if (flags & SOCK_NONBLOCK)
      t->fd_flags[newfd] |= SOCK_NONBLOCK;
  }

  return (uint64_t)newfd;
}

// Syscall: accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
static uint64_t sys_accept(uint64_t sockfd, uint64_t addr_ptr,
                           uint64_t addrlen_ptr, uint64_t _arg3, uint64_t _arg4,
                           uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;
  return sys_accept_impl(sockfd, addr_ptr, addrlen_ptr, 0);
}

// Syscall: accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
static uint64_t sys_accept4(uint64_t sockfd, uint64_t addr_ptr,
                            uint64_t addrlen_ptr, uint64_t flags, uint64_t _arg4,
                            uint64_t _arg5) {
  (void)_arg4;
  (void)_arg5;
  return sys_accept_impl(sockfd, addr_ptr, addrlen_ptr, flags);
}

/* Allocate a socket bounce buffer, capping/stepping down on SOCK_STREAM
 * to avoid large contiguous allocations and reclaim storms. */
static uint8_t *socket_alloc_bounce_buf(socket_t *sock, size_t *cap,
                                        uint8_t *stack_buf, size_t stack_cap,
                                        bool allow_stepdown) {
  bool stream_stepdown = allow_stepdown && sock && sock->type == SOCK_STREAM;
  if (stream_stepdown && *cap > 16384)
    *cap = 16384;
  if (*cap <= stack_cap)
    return stack_buf;

  uint8_t *kbuf = (uint8_t *)kmalloc(*cap);
  if (kbuf)
    return kbuf;

  if (stream_stepdown) {
    for (size_t try_cap = *cap >> 1; try_cap > stack_cap; try_cap >>= 1) {
      kbuf = (uint8_t *)kmalloc(try_cap);
      if (kbuf) {
        *cap = try_cap;
        return kbuf;
      }
    }
    *cap = stack_cap;
    return stack_buf;
  }

  extern size_t mm_file_cache_reclaim(size_t target_pages);
  extern size_t vfs_dentry_reclaim_pages(size_t target_pages);
  mm_file_cache_reclaim(*cap / 4096 + 16);
  vfs_dentry_reclaim_pages(64);
  return (uint8_t *)kmalloc_fallback(*cap);
}

// Syscall: sendto(int sockfd, const void *buf, size_t len, int flags, ...)
static uint64_t sys_sendto(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                           uint64_t flags, uint64_t dest_addr_ptr,
                           uint64_t addrlen) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (len > 0 && !is_user_range((const void *)buf_ptr, len)) {
    return (uint64_t)-14; // EFAULT
  }

  if (len == 0) {
    return 0;
  }

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (sock->closing) {
    return (uint64_t)-9;
  }

  uint8_t kaddr_buf[128];
  struct sockaddr *dest_addr = NULL;
  if (dest_addr_ptr) {
    if (addrlen > sizeof(kaddr_buf))
      return (uint64_t)-22;
    if (!is_user_range((const void *)dest_addr_ptr, addrlen) ||
        copy_from_user(kaddr_buf, (const void *)dest_addr_ptr, addrlen) != 0) {
      return (uint64_t)-14;
    }
    dest_addr = (struct sockaddr *)kaddr_buf;
  }

  uint8_t stack_buf[4096];
  size_t copy_len = (size_t)len;
  uint8_t *kbuf = socket_alloc_bounce_buf(sock, &copy_len, stack_buf,
                                          sizeof(stack_buf), true);
  if (!kbuf) {
    return (uint64_t)-12; // ENOMEM
  }
  if (copy_from_user(kbuf, (const void *)buf_ptr, copy_len) != 0) {
    if (kbuf != stack_buf)
      kfree(kbuf);
    return (uint64_t)-14;
  }

  ssize_t ret =
      socket_sendto(sock, kbuf, copy_len, (int)flags, dest_addr, (int)addrlen);
  if (kbuf != stack_buf)
    kfree(kbuf);
  return (uint64_t)ret;
}

// Syscall: recvfrom(int sockfd, void *buf, size_t len, int flags, ...)
static uint64_t sys_recvfrom(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t src_addr_ptr,
                             uint64_t addrlen_ptr) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (len > 0 && !is_user_range((const void *)buf_ptr, len)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (sock->closing) {
    return (uint64_t)-9;
  }

  uint8_t kaddr_buf[128];
  struct sockaddr *src_addr = NULL;
  int *addrlen = NULL;
  int k_addrlen = 0;

  if (addrlen_ptr) {
    if (!is_user_range((const void *)addrlen_ptr, sizeof(int)) ||
        copy_from_user(&k_addrlen, (const void *)addrlen_ptr, sizeof(int)) != 0) {
      return (uint64_t)-14;
    }
    if (k_addrlen < 0)
      return (uint64_t)-22;
    if ((size_t)k_addrlen > sizeof(kaddr_buf))
      k_addrlen = (int)sizeof(kaddr_buf);
    addrlen = &k_addrlen;
  }

  if (src_addr_ptr) {
    if (!addrlen_ptr || !is_user_range((const void *)src_addr_ptr, (size_t)k_addrlen)) {
      return (uint64_t)-14;
    }
    memset(kaddr_buf, 0, sizeof(kaddr_buf));
    src_addr = (struct sockaddr *)kaddr_buf;
  }

  uint8_t stack_buf[4096];
  size_t cap = (size_t)len;
  uint8_t *kbuf = socket_alloc_bounce_buf(sock, &cap, stack_buf,
                                          sizeof(stack_buf),
                                          (flags & 0x100) == 0);
  if (!kbuf) {
    return (uint64_t)-12; // ENOMEM
  }

  ssize_t ret = socket_recvfrom(sock, kbuf, cap, (int)flags, src_addr, addrlen);
  /* MSG_TRUNC can return the datagram length even for a zero-byte buffer. */
  size_t copied = ret > 0 ? ((size_t)ret < cap ? (size_t)ret : cap) : 0;
  if (copied && copy_to_user((void *)buf_ptr, kbuf, copied) != 0) {
    if (kbuf != stack_buf)
      kfree(kbuf);
    return (uint64_t)-14;
  }
  if (kbuf != stack_buf)
    kfree(kbuf);

  if (ret >= 0 && src_addr_ptr && k_addrlen > 0) {
    size_t copy_addr = (size_t)k_addrlen <= sizeof(kaddr_buf) ? (size_t)k_addrlen : sizeof(kaddr_buf);
    if (copy_to_user((void *)src_addr_ptr, kaddr_buf, copy_addr) != 0)
      return (uint64_t)-14;
  }
  if (ret >= 0 && addrlen_ptr) {
    if (copy_to_user((void *)addrlen_ptr, &k_addrlen, sizeof(int)) != 0)
      return (uint64_t)-14;
  }
  return (uint64_t)ret;
}

// Structures for recvmsg/sendmsg

struct compat_msghdr {
  uint32_t name, namelen, iov, iovlen, control, controllen, flags;
};
_Static_assert(sizeof(struct compat_msghdr) == 28, "i386 msghdr ABI");

/* i386 cmsghdr: {u32 len; i32 level; i32 type;} aligned to 4.
 * x86-64 cmsghdr: {u64 len; i32 level; i32 type;} aligned to 8. */
#define C32_ALIGN(x) (((x) + 3u) & ~(size_t)3u)
#define C64_ALIGN(x) (((x) + 7u) & ~(size_t)7u)

/* Convert a 32-bit control buffer (kernel copy) to native layout. */
static int compat_cmsg_to_native(const uint8_t *in, size_t in_len, uint8_t *out,
                                 size_t out_cap, size_t *out_len) {
  size_t ip = 0, op = 0;
  while (ip + 12 <= in_len) {
    uint32_t len;
    int32_t level, type;
    memcpy(&len, in + ip, 4);
    memcpy(&level, in + ip + 4, 4);
    memcpy(&type, in + ip + 8, 4);
    if (len < 12 || ip + len > in_len) return -22;
    size_t payload = len - 12;
    size_t need = C64_ALIGN(16 + payload);
    if (op + need > out_cap) return -90;
    uint64_t nlen = 16 + payload;
    memset(out + op, 0, need);
    memcpy(out + op, &nlen, 8);
    memcpy(out + op + 8, &level, 4);
    memcpy(out + op + 12, &type, 4);
    memcpy(out + op + 16, in + ip + 12, payload);
    op += need;
    ip += C32_ALIGN(len);
  }
  *out_len = op;
  return 0;
}

/* Convert native control output to 32-bit layout; sets *trunc on overflow. */
static size_t compat_cmsg_from_native(const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t out_cap, bool *trunc) {
  size_t ip = 0, op = 0;
  while (ip + 16 <= in_len) {
    uint64_t len;
    memcpy(&len, in + ip, 8);
    if (len < 16 || ip + len > in_len) break;
    size_t payload = (size_t)len - 16;
    size_t need = C32_ALIGN(12 + payload);
    if (op + 12 + payload > out_cap) { *trunc = true; break; }
    uint32_t nlen = (uint32_t)(12 + payload);
    memcpy(out + op, &nlen, 4);
    memcpy(out + op + 4, in + ip + 8, 8);
    memcpy(out + op + 12, in + ip + 16, payload);
    op = (op + need > out_cap) ? out_cap : op + need;
    ip += C64_ALIGN((size_t)len);
  }
  return op;
}

static uint64_t compat_socket_msg(int fd, uint64_t pointer, int flags, bool receive) {
  struct compat_msghdr narrow;
  if (copy_from_user(&narrow, (void *)pointer, sizeof(narrow))) return (uint64_t)-14;
  if (narrow.iovlen > 1024) return (uint64_t)-90;
  if (narrow.namelen > 4096) return (uint64_t)-22;
  if (narrow.controllen > 65536 && !receive) return (uint64_t)-90;
  socket_t *sock __attribute__((cleanup(socket_release_fd))) = socket_from_fd(fd);
  if (!sock) return socket_eno_notsock(fd);
  if (sock->closing) return (uint64_t)-9;
  if ((receive && (!sock->ops || !sock->ops->recvmsg)) ||
      (!receive && (!sock->ops || !sock->ops->sendmsg))) return (uint64_t)-95;

  /* Every buffer handed to sock->ops is a kernel bounce buffer: the family
   * implementations memcpy into msg_name/msg_control/iov directly. */
  struct iovec *uiov = NULL, *kiov = NULL;
  void *kname = NULL, *kcontrol = NULL, *ucontrol32 = NULL;
  /* AF_UNIX already reads and writes ancillary data in the caller's layout.
   * Keep that layout in its bounce buffer so SCM_RIGHTS is translated once
   * and the receiver's control capacity limits descriptor installation. */
  bool native_control = sock->domain != AF_UNIX;
  size_t control_len = narrow.controllen > 65536 ? 65536 : narrow.controllen;
  size_t kcontrol_cap = 0;
  int error = 0;
  ssize_t result = 0;

  if (narrow.iovlen) {
    uiov = kmalloc(narrow.iovlen * sizeof(*uiov));
    if (!uiov) { error = -12; goto out; }
    kiov = kmalloc(narrow.iovlen * sizeof(*kiov));
    if (!kiov) { error = -12; goto out; }
    memset(kiov, 0, narrow.iovlen * sizeof(*kiov));
  }
  for (size_t i = 0; i < narrow.iovlen; i++) {
    struct { uint32_t base, size; } v;
    uint64_t address = (uint64_t)narrow.iov + i * sizeof(v);
    if (address + sizeof(v) > 0x100000000ULL ||
        copy_from_user(&v, (void *)address, sizeof(v))) { error = -14; goto out; }
    uiov[i] = (struct iovec){ .iov_base=(void *)(uintptr_t)v.base, .iov_len=v.size };
    kiov[i].iov_len = v.size;
    if (!v.size) continue;
    if (v.size > 16u * 1024 * 1024) { error = -12; goto out; }
    if (!vmm_is_user_addr_range_valid(v.base, v.size)) { error = -14; goto out; }
    kiov[i].iov_base = kmalloc(v.size);
    if (!kiov[i].iov_base) { error = -12; goto out; }
    if (receive) memset(kiov[i].iov_base, 0, v.size);
    else if (copy_from_user(kiov[i].iov_base, uiov[i].iov_base, v.size)) { error = -14; goto out; }
  }

  if (narrow.name && narrow.namelen) {
    if (!vmm_is_user_addr_range_valid(narrow.name, narrow.namelen)) { error = -14; goto out; }
    kname = kmalloc(narrow.namelen);
    if (!kname) { error = -12; goto out; }
    if (receive) memset(kname, 0, narrow.namelen);
    else if (copy_from_user(kname, (void *)(uintptr_t)narrow.name, narrow.namelen)) { error = -14; goto out; }
  }

  if (control_len) {
    if (!narrow.control || !vmm_is_user_addr_range_valid(narrow.control, control_len)) { error = -14; goto out; }
    /* Native headers are 4 bytes larger and 8-aligned: double plus slack. */
    kcontrol_cap = native_control ? control_len * 2 + 64 : control_len;
    kcontrol = kmalloc(kcontrol_cap);
    if (!kcontrol) { error = -12; goto out; }
    memset(kcontrol, 0, kcontrol_cap);
    if (!receive && native_control) {
      ucontrol32 = kmalloc(control_len);
      if (!ucontrol32) { error = -12; goto out; }
    }
    if (!receive && copy_from_user(native_control ? ucontrol32 : kcontrol,
                                   (void *)(uintptr_t)narrow.control, control_len)) {
      error = -14;
      goto out;
    }
    if (ucontrol32) {
      error = compat_cmsg_to_native(ucontrol32, control_len, kcontrol,
                                    kcontrol_cap, &kcontrol_cap);
      if (error) goto out;
    }
  }

  struct msghdr msg = {
    .msg_name=kname, .msg_namelen=kname ? narrow.namelen : 0,
    .msg_iov=kiov, .msg_iovlen=narrow.iovlen,
    .msg_control=kcontrol_cap ? kcontrol : NULL, .msg_controllen=kcontrol_cap
  };
  result = receive ? sock->ops->recvmsg(sock, &msg, flags)
                   : sock->ops->sendmsg(sock, &msg, flags);

  if (receive && result >= 0) {
    size_t remaining = (size_t)result;
    for (size_t i = 0; i < narrow.iovlen && remaining > 0; i++) {
      size_t chunk = kiov[i].iov_len < remaining ? kiov[i].iov_len : remaining;
      if (chunk && copy_to_user(uiov[i].iov_base, kiov[i].iov_base, chunk)) { error = -14; goto out; }
      remaining -= chunk;
    }
    uint32_t length = msg.msg_namelen;
    if (kname && length) {
      uint32_t c = length < narrow.namelen ? length : narrow.namelen;
      if (copy_to_user((void *)(uintptr_t)narrow.name, kname, c)) { error = -14; goto out; }
    }
    uint32_t control = 0, output_flags = (uint32_t)msg.msg_flags;
    if (kcontrol && msg.msg_controllen > 0) {
      const void *received_control = kcontrol;
      control = (uint32_t)msg.msg_controllen;
      if (native_control) {
        ucontrol32 = kmalloc(control_len);
        if (!ucontrol32) { error = -12; goto out; }
        bool trunc = false;
        control = (uint32_t)compat_cmsg_from_native(kcontrol, msg.msg_controllen,
                                                   ucontrol32, control_len, &trunc);
        if (trunc) output_flags |= 0x8; /* MSG_CTRUNC */
        received_control = ucontrol32;
      }
      if (control && copy_to_user((void *)(uintptr_t)narrow.control,
                                  received_control, control)) {
        error = -14;
        goto out;
      }
    }
    if (copy_to_user((void *)(pointer + 4), &length, 4) ||
        copy_to_user((void *)(pointer + 20), &control, 4) ||
        copy_to_user((void *)(pointer + 24), &output_flags, 4)) { error = -14; goto out; }
  }

out:
  if (kiov) {
    for (size_t i = 0; i < narrow.iovlen; i++)
      if (kiov[i].iov_base) kfree(kiov[i].iov_base);
    kfree(kiov);
  }
  if (uiov) kfree(uiov);
  if (kname) kfree(kname);
  if (kcontrol) kfree(kcontrol);
  if (ucontrol32) kfree(ucontrol32);
  if (error) return (uint64_t)(int64_t)error;
  return (uint64_t)result;
}

struct socket_msg_bounce {
  struct msghdr kmsg;
  struct iovec *kiov;
  struct iovec *uiov;
  size_t iovlen;
  void *user_name;
  uint32_t orig_namelen;
  void *kname;
  void *user_control;
  size_t orig_controllen;
  void *kcontrol;
  size_t inline_data_used;
  struct iovec inline_kiov[4];
  struct iovec inline_uiov[4];
  uint8_t inline_name[128];
  uint8_t inline_control[128];
  uint8_t inline_data[256];
};

static inline bool socket_msg_is_inline_data(const struct socket_msg_bounce *b,
                                             const void *ptr) {
  const uint8_t *p = (const uint8_t *)ptr;
  return p >= b->inline_data && p < b->inline_data + sizeof(b->inline_data);
}

static void socket_msg_bounce_free(struct socket_msg_bounce *b) {
  if (b->kiov) {
    for (size_t i = 0; i < b->iovlen; i++) {
      if (b->kiov[i].iov_base && !socket_msg_is_inline_data(b, b->kiov[i].iov_base))
        kfree(b->kiov[i].iov_base);
    }
    if (b->kiov != b->inline_kiov)
      kfree(b->kiov);
  }
  if (b->uiov && b->uiov != b->inline_uiov)
    kfree(b->uiov);
  if (b->kname && b->kname != b->inline_name)
    kfree(b->kname);
  if (b->kcontrol && b->kcontrol != b->inline_control)
    kfree(b->kcontrol);
}

static int socket_msg_bounce_init(socket_t *sock, struct socket_msg_bounce *b,
                                  uint64_t msg_ptr, bool is_send) {
  memset(b, 0, __builtin_offsetof(struct socket_msg_bounce, inline_kiov));
  if (!is_user_range((const void *)msg_ptr, sizeof(struct msghdr)) ||
      copy_from_user(&b->kmsg, (const void *)msg_ptr, sizeof(struct msghdr)) != 0)
    return -14;

  if (b->kmsg.msg_iovlen > 1024)
    return -90; // EMSGSIZE

  b->iovlen = b->kmsg.msg_iovlen;
  b->user_name = b->kmsg.msg_name;
  b->orig_namelen = b->kmsg.msg_namelen;
  b->user_control = b->kmsg.msg_control;
  b->orig_controllen = b->kmsg.msg_controllen;

  if (b->user_name && b->orig_namelen > 0) {
    if (b->orig_namelen > 4096)
      return -22;
    if (!is_user_range(b->user_name, b->orig_namelen))
      return -14;
    b->kname = b->orig_namelen <= sizeof(b->inline_name)
                   ? (void *)b->inline_name
                   : kmalloc(b->orig_namelen);
    if (!b->kname)
      return -12;
    if (is_send) {
      if (copy_from_user(b->kname, b->user_name, b->orig_namelen) != 0) {
        socket_msg_bounce_free(b);
        return -14;
      }
    } else {
      memset(b->kname, 0, b->orig_namelen);
    }
    b->kmsg.msg_name = b->kname;
  } else {
    b->kmsg.msg_name = NULL;
  }

  if (b->user_control && b->orig_controllen > 0) {
    size_t control_capacity = b->orig_controllen;
    if (control_capacity > 65536) {
      if (is_send) {
        socket_msg_bounce_free(b);
        return -90; // EMSGSIZE
      }
      /* Receiving needs space only for the control messages we can emit.
       * Varlink supplies 64 KiB for descriptors plus the cmsghdr. */
      control_capacity = 65536;
    }
    if (!is_user_range(b->user_control, control_capacity)) {
      socket_msg_bounce_free(b);
      return -14;
    }
    b->kcontrol = control_capacity <= sizeof(b->inline_control)
                      ? (void *)b->inline_control
                      : kmalloc_fallback(control_capacity);
    if (!b->kcontrol && !is_send) {
      while (!b->kcontrol && control_capacity > 1024) {
        control_capacity >>= 1;
        b->kcontrol = control_capacity <= sizeof(b->inline_control)
                      ? (void *)b->inline_control
                      : kmalloc_fallback(control_capacity);
      }
    }
    if (!b->kcontrol) {
      socket_msg_bounce_free(b);
      return -12;
    }
    if (is_send) {
      if (copy_from_user(b->kcontrol, b->user_control, b->orig_controllen) != 0) {
        socket_msg_bounce_free(b);
        return -14;
      }
    } else {
      memset(b->kcontrol, 0, control_capacity);
    }
    b->kmsg.msg_control = b->kcontrol;
    b->kmsg.msg_controllen = control_capacity;
  } else {
    b->kmsg.msg_control = NULL;
    b->kmsg.msg_controllen = 0;
  }

  if (b->iovlen > 0) {
    size_t iov_bytes = b->iovlen * sizeof(struct iovec);
    if (!is_user_range(b->kmsg.msg_iov, iov_bytes)) {
      socket_msg_bounce_free(b);
      return -14;
    }
    b->uiov = b->iovlen <= 4 ? b->inline_uiov : kmalloc_fallback(iov_bytes);
    if (!b->uiov) {
      socket_msg_bounce_free(b);
      return -12;
    }
    b->kiov = b->iovlen <= 4 ? b->inline_kiov : kmalloc_fallback(iov_bytes);
    if (!b->kiov) {
      socket_msg_bounce_free(b);
      return -12;
    }
    memset(b->kiov, 0, iov_bytes);
    if (copy_from_user(b->uiov, b->kmsg.msg_iov, iov_bytes) != 0) {
      socket_msg_bounce_free(b);
      return -14;
    }
    bool stream_stepdown =
        sock && sock->type == SOCK_STREAM &&
        (!is_send || b->kmsg.msg_controllen == 0);
    for (size_t i = 0; i < b->iovlen; i++) {
      size_t orig_len = b->uiov[i].iov_len;
      size_t len = orig_len;
      if (len > 0) {
        if (len > (16ULL * 1024 * 1024)) {
          socket_msg_bounce_free(b);
          return -12;
        }
        if (!is_user_range(b->uiov[i].iov_base, len)) {
          socket_msg_bounce_free(b);
          return -14;
        }
        if (stream_stepdown && len > 16384)
          len = 16384;
        void *kbuf = NULL;
        if (len <= sizeof(b->inline_data) - b->inline_data_used) {
          kbuf = b->inline_data + b->inline_data_used;
          b->inline_data_used += len;
        } else {
          kbuf = stream_stepdown ? kmalloc(len) : kmalloc_fallback(len);
        }
        if (!kbuf && stream_stepdown) {
          for (size_t try_len = len >> 1; try_len >= 4096; try_len >>= 1) {
            kbuf = kmalloc(try_len);
            if (kbuf) {
              len = try_len;
              break;
            }
          }
          if (!kbuf && i > 0) {
            b->kmsg.msg_iovlen = i;
            break;
          }
        }
        if (!kbuf) {
          socket_msg_bounce_free(b);
          return -12;
        }
        if (is_send) {
          if (copy_from_user(kbuf, b->uiov[i].iov_base, len) != 0) {
            if (!socket_msg_is_inline_data(b, kbuf))
              kfree(kbuf);
            socket_msg_bounce_free(b);
            return -14;
          }
        } else {
          memset(kbuf, 0, len);
        }
        b->kiov[i].iov_len = len;
        b->kiov[i].iov_base = kbuf;
        if (len < orig_len) {
          b->kmsg.msg_iovlen = i + 1;
          break;
        }
      }
    }
    b->kmsg.msg_iov = b->kiov;
  } else {
    b->kmsg.msg_iov = NULL;
  }

  return 0;
}

// Syscall: sendmsg(int sockfd, struct msghdr *msg, int flags)
static uint64_t sys_sendmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  struct thread *caller = sched_get_current();
  if (caller && caller->seccomp_compat_dispatch)
    return compat_socket_msg((int)sockfd, msg_ptr, (int)flags, false);

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  if (sock->closing) {
    return (uint64_t)-9;
  }

  struct socket_msg_bounce b;
  int berr = socket_msg_bounce_init(sock, &b, msg_ptr, true);
  if (berr < 0)
    return (uint64_t)(int64_t)berr;

  // Use family-specific sendmsg if available
  if (sock->ops && sock->ops->sendmsg) {
    ssize_t r = sock->ops->sendmsg(sock, &b.kmsg, (int)flags);
    socket_msg_bounce_free(&b);
    return (uint64_t)r;
  }

  // Fallback to simple send from iovec array
  ssize_t total_sent = 0;
  for (size_t i = 0; i < b.iovlen; i++) {
    if (b.kiov[i].iov_len == 0) {
      continue;
    }

    ssize_t ret = socket_send(sock, b.kiov[i].iov_base, b.kiov[i].iov_len, (int)flags);
    if (ret < 0) {
      socket_msg_bounce_free(&b);
      if (total_sent > 0) {
        return (uint64_t)total_sent;
      }
      return (uint64_t)ret;
    }

    total_sent += ret;

    // If we sent less than requested, we're done (buffer full)
    if ((size_t)ret < b.kiov[i].iov_len) {
      break;
    }
  }

  socket_msg_bounce_free(&b);
  return (uint64_t)total_sent;
}

// Syscall: recvmsg(int sockfd, struct msghdr *msg, int flags)
static uint64_t sys_recvmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  struct thread *caller = sched_get_current();
  if (caller && caller->seccomp_compat_dispatch)
    return compat_socket_msg((int)sockfd, msg_ptr, (int)flags, true);

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  if (sock->closing) {
    return (uint64_t)-9;
  }

  struct socket_msg_bounce b;
  int berr = socket_msg_bounce_init(sock, &b, msg_ptr, false);
  if (berr < 0)
    return (uint64_t)(int64_t)berr;

  ssize_t total_received = 0;

  // Use family-specific recvmsg if available
  if (sock->ops && sock->ops->recvmsg) {
    total_received = sock->ops->recvmsg(sock, &b.kmsg, (int)flags);
  } else {
    /* Fill msg_name with the source address when requested.
     * Pull the first iovec as the data buffer and use recvfrom. */
    struct sockaddr *src_addr = (struct sockaddr *)b.kname;
    int src_addrlen = b.kname ? (int)b.orig_namelen : 0;

    for (size_t i = 0; i < b.iovlen; i++) {
      if (b.kiov[i].iov_len == 0) {
        continue;
      }

      ssize_t ret;
      if (i == 0 && src_addr) {
        /* First buffer: use recvfrom to capture source address */
        ret = socket_recvfrom(
            sock, b.kiov[i].iov_base, b.kiov[i].iov_len,
            (int)flags | (sock->flags & SOCK_NONBLOCK ? MSG_DONTWAIT : 0),
            src_addr, &src_addrlen);
        if (ret >= 0)
          b.kmsg.msg_namelen = (uint32_t)src_addrlen;
      } else {
        ret = socket_recv(sock, b.kiov[i].iov_base, b.kiov[i].iov_len, (int)flags);
      }

      if (ret < 0) {
        if (total_received == 0)
          total_received = ret;
        break;
      }

      total_received += ret;

      // If we received less than requested, we're done
      if ((size_t)ret < b.kiov[i].iov_len) {
        break;
      }
    }
    if (total_received >= 0) {
      b.kmsg.msg_controllen = 0;
      b.kmsg.msg_flags = 0;
    }
  }

  if (total_received >= 0) {
    size_t remaining = (size_t)total_received;
    for (size_t i = 0; i < b.iovlen && remaining > 0; i++) {
      size_t chunk = b.kiov[i].iov_len;
      if (chunk > remaining)
        chunk = remaining;
      if (chunk > 0 &&
          copy_to_user(b.uiov[i].iov_base, b.kiov[i].iov_base, chunk) != 0) {
        socket_msg_bounce_free(&b);
        return (uint64_t)-14;
      }
      remaining -= chunk;
    }
    if (b.kname && b.user_name && b.kmsg.msg_namelen > 0) {
      uint32_t copy_len = b.kmsg.msg_namelen;
      if (copy_len > b.orig_namelen)
        copy_len = b.orig_namelen;
      if (copy_to_user(b.user_name, b.kname, copy_len) != 0) {
        socket_msg_bounce_free(&b);
        return (uint64_t)-14;
      }
    }
    if (b.kcontrol && b.user_control && b.kmsg.msg_controllen > 0) {
      size_t copy_len = b.kmsg.msg_controllen;
      if (copy_len > b.orig_controllen)
        copy_len = b.orig_controllen;
      if (copy_to_user(b.user_control, b.kcontrol, copy_len) != 0) {
        socket_msg_bounce_free(&b);
        return (uint64_t)-14;
      }
    }
    struct msghdr *umsg = (struct msghdr *)msg_ptr;
    if (copy_to_user(&umsg->msg_namelen, &b.kmsg.msg_namelen, sizeof(umsg->msg_namelen)) != 0 ||
        copy_to_user(&umsg->msg_controllen, &b.kmsg.msg_controllen, sizeof(umsg->msg_controllen)) != 0 ||
        copy_to_user(&umsg->msg_flags, &b.kmsg.msg_flags, sizeof(umsg->msg_flags)) != 0) {
      socket_msg_bounce_free(&b);
      return (uint64_t)-14;
    }
  }

  socket_msg_bounce_free(&b);
  return (uint64_t)total_received;
}

struct mmsghdr {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

/* i386 mmsghdr: 28-byte compat_msghdr + u32 msg_len, no padding. The per-
 * message sendmsg/recvmsg already translate the compat msghdr for 32-bit
 * callers, so the vector walk only needs the narrower stride. */
#define COMPAT_MMSGHDR_SIZE 32u
#define COMPAT_MMSGHDR_LEN_OFF 28u
#ifndef MSG_WAITFORONE
#define MSG_WAITFORONE 0x10000
#endif

static void mmsg_layout(size_t *stride, size_t *len_off) {
  struct thread *caller = sched_get_current();
  if (caller && caller->seccomp_compat_dispatch) {
    *stride = COMPAT_MMSGHDR_SIZE;
    *len_off = COMPAT_MMSGHDR_LEN_OFF;
  } else {
    *stride = sizeof(struct mmsghdr);
    *len_off = __builtin_offsetof(struct mmsghdr, msg_len);
  }
}

// Syscall: sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags)
static uint64_t sys_sendmmsg(uint64_t sockfd, uint64_t msgvec_ptr, uint64_t vlen,
                             uint64_t flags, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg4;
  (void)_arg5;

  if (!msgvec_ptr)
    return (uint64_t)-14; // EFAULT

  unsigned int count = (unsigned int)vlen;
  if (count == 0)
    return 0;
  if (count > 1024)
    count = 1024;

  size_t stride, len_off;
  mmsg_layout(&stride, &len_off);
  if (!vmm_is_user_addr_range_writable(msgvec_ptr, count * stride))
    return (uint64_t)-14; // EFAULT

  unsigned int sent_msgs = 0;

  for (unsigned int i = 0; i < count; i++) {
    uint64_t entry = msgvec_ptr + (uint64_t)i * stride;
    uint64_t ret = sys_sendmsg(sockfd, entry, flags, 0, 0, 0);
    if ((int64_t)ret < 0) {
      if (sent_msgs > 0)
        return (uint64_t)sent_msgs;
      return ret;
    }
    unsigned int mlen = (unsigned int)ret;
    if (copy_to_user((void *)(entry + len_off), &mlen, sizeof(mlen)) != 0)
      return (uint64_t)-14;
    sent_msgs++;
  }

  return (uint64_t)sent_msgs;
}

// Syscall: recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen, int flags, struct timespec *timeout)
static uint64_t sys_recvmmsg(uint64_t sockfd, uint64_t msgvec_ptr, uint64_t vlen,
                             uint64_t flags, uint64_t timeout_ptr, uint64_t _arg5) {
  (void)timeout_ptr;
  (void)_arg5;

  if (!msgvec_ptr)
    return (uint64_t)-14; // EFAULT

  unsigned int count = (unsigned int)vlen;
  if (count == 0)
    return 0;
  if (count > 1024)
    count = 1024;

  size_t stride, len_off;
  mmsg_layout(&stride, &len_off);
  if (!vmm_is_user_addr_range_writable(msgvec_ptr, count * stride))
    return (uint64_t)-14; // EFAULT

  unsigned int recvd_msgs = 0;
  uint64_t msg_flags = flags & ~(uint64_t)MSG_WAITFORONE;

  for (unsigned int i = 0; i < count; i++) {
    uint64_t entry = msgvec_ptr + (uint64_t)i * stride;
    uint64_t ret = sys_recvmsg(sockfd, entry, msg_flags, 0, 0, 0);
    if ((int64_t)ret < 0) {
      if (recvd_msgs > 0)
        return (uint64_t)recvd_msgs;
      return ret;
    }
    unsigned int mlen = (unsigned int)ret;
    if (copy_to_user((void *)(entry + len_off), &mlen, sizeof(mlen)) != 0)
      return (uint64_t)-14;
    recvd_msgs++;
    if (flags & MSG_DONTWAIT)
      break;
    /* Linux: after the first datagram, MSG_WAITFORONE stops blocking. */
    if (flags & MSG_WAITFORONE)
      msg_flags |= MSG_DONTWAIT;
  }

  return (uint64_t)recvd_msgs;
}

// Syscall: shutdown(int sockfd, int how)
static uint64_t sys_shutdown(uint64_t sockfd, uint64_t how, uint64_t _arg2,
                             uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg2;
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (sock->closing) {
    return (uint64_t)-9;
  }

  if (!sock->ops || !sock->ops->shutdown) {
    return (uint64_t)-95; // EOPNOTSUPP
  }

  int ret = sock->ops->shutdown(sock, (int)how);
  return (uint64_t)ret;
}

// Syscall: getsockopt(int sockfd, int level, int optname, ...)
static uint64_t sys_getsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen_ptr, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock)
    return socket_eno_notsock(fd);

  if (!is_user_range((const void *)optlen_ptr, sizeof(int)))
    return (uint64_t)-14;

  int koptlen = 0;
  if (copy_from_user(&koptlen, (const void *)optlen_ptr, sizeof(int)) != 0)
    return (uint64_t)-14;

  if (koptlen < 0 || koptlen > 65536)
    return (uint64_t)-22;

  if (!is_user_ptr(optval_ptr) && !(optval_ptr == 0 && koptlen == 0))
    return (uint64_t)-14;

  if (optval_ptr != 0 && koptlen > 0 &&
      !is_user_range((const void *)optval_ptr, (size_t)koptlen))
    return (uint64_t)-14;

  int orig_optlen = koptlen;
  uint8_t stack_opt[64];
  void *koptval = NULL;
  if (optval_ptr != 0 && koptlen > 0) {
    koptval = (size_t)koptlen <= sizeof(stack_opt) ? (void *)stack_opt
                                                   : kmalloc((size_t)koptlen);
    if (!koptval)
      return (uint64_t)-12;
    if (copy_from_user(koptval, (const void *)optval_ptr, (size_t)koptlen) != 0) {
      if (koptval != stack_opt) kfree(koptval);
      return (uint64_t)-14;
    }
  }

  /* Netlink membership enumeration first queries the required buffer size
   * with a NULL value and zero length. Let the family implement that ABI. */
  if (sock->domain == AF_NETLINK && (int)level == SOL_NETLINK &&
      (int)optname == NETLINK_LIST_MEMBERSHIPS && sock->ops && sock->ops->getsockopt) {
    if (koptval && koptval != stack_opt) kfree(koptval);
    return (uint64_t)sock->ops->getsockopt(sock, (int)level, (int)optname,
                                         (void *)optval_ptr, (int *)optlen_ptr);
  }

  // Validate optlen
  if (koptlen < (int)sizeof(int) || !koptval) {
    if (koptval && koptval != stack_opt) kfree(koptval);
    return (uint64_t)-22; // EINVAL
  }

  int *val = (int *)koptval;

  // Handle socket-level options
  if ((int)level == SOL_SOCKET) {
    int handled = 1;
    switch ((int)optname) {
    case 10: /* SO_OOBINLINE: no OOB data delivery; report off like a fresh Linux socket. */
      *val = 0;
      koptlen = sizeof(int);
      break;
    case SO_TYPE:
      *val = sock->type;
      koptlen = sizeof(int);
      break;
    case SO_DOMAIN:
      *val = sock->domain;
      koptlen = sizeof(int);
      break;
    case SO_PROTOCOL:
      *val = sock->protocol;
      koptlen = sizeof(int);
      break;
    case SO_ERROR:
      // Let the family report connection errors (e.g. failed non-blocking
      // connect); fall back to the generic socket error.
      if (sock->ops && sock->ops->getsockopt) {
        int ret = sock->ops->getsockopt(sock, (int)level, (int)optname,
                                        (void *)optval_ptr, (int *)optlen_ptr);
        if (ret == 0) {
          if (koptval != stack_opt) kfree(koptval);
          return 0;
        }
      }
      *val = socket_get_error(sock);
      koptlen = sizeof(int);
      break;
    case SO_RCVBUF:
      *val = sock->rcvbuf;
      koptlen = sizeof(int);
      break;
    case SO_SNDBUF:
      *val = sock->sndbuf;
      koptlen = sizeof(int);
      break;
    case SO_REUSEADDR:
      *val = sock->reuseaddr;
      koptlen = sizeof(int);
      break;
    default:
      handled = 0;
      break;
    }
    if (handled) {
      int copy_len = koptlen < orig_optlen ? koptlen : orig_optlen;
      int err = 0;
      if ((copy_len > 0 && copy_to_user((void *)optval_ptr, koptval, (size_t)copy_len) != 0) ||
          copy_to_user((void *)optlen_ptr, &koptlen, sizeof(int)) != 0)
        err = -14;
      if (koptval != stack_opt) kfree(koptval);
      return (uint64_t)(int64_t)err;
    }
  }

  // Try family-specific handler
  if (sock->ops && sock->ops->getsockopt) {
    if (koptval != stack_opt) kfree(koptval);
    int ret = sock->ops->getsockopt(sock, (int)level, (int)optname,
                                    (void *)optval_ptr, (int *)optlen_ptr);
    /* The family owns this option's result. In particular ERANGE must keep
     * the required SO_PEERGROUPS length, and ENOTCONN/EINVAL must not be
     * converted to ENOPROTOOPT by the generic fallback below. */
    return (uint64_t)ret;
  }

  // Generic SOL_SOCKET integer-option fallback. Linux accepts these on every
  // socket type with sensible defaults; failing them with ENOPROTOOPT breaks
  // pipewire/SPA, dbus and other runtimes that probe buffer sizes, timestamps
  // or credentials unconditionally. This mirrors what sys_setsockopt() already
  // does (stub success) so set-then-get stays coherent.
  if ((int)level == SOL_SOCKET && koptlen >= (int)sizeof(int)) {
    int handled = 1;
    switch ((int)optname) {
    case SO_DONTROUTE:
    case SO_BROADCAST:
    case SO_OOBINLINE:
    case SO_PRIORITY:
    case SO_BSDCOMPAT:
    case SO_REUSEPORT:
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:
    case 29: // SO_TIMESTAMP
    case 35: // SO_TIMESTAMPNS
    case 37: // SO_TIMESTAMPING
    case 41: // SO_SELECT_ERR_QUEUE
    case 39: // SO_DOMAIN (non-generic sockets)
    case 38: // SO_PROTOCOL (non-generic sockets)
      *(int *)koptval = 0;
      koptlen = sizeof(int);
      break;
    case SO_LINGER: {
      // struct linger { int l_onoff, l_linger; } — report disabled.
      if (koptlen >= 8) {
        ((int *)koptval)[0] = 0;
        ((int *)koptval)[1] = 0;
        koptlen = 8;
      } else {
        handled = 0;
      }
      break;
    }
    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
      // struct timeval { long tv_sec, tv_usec; } — report zero timeout.
      // Some callers pass an int-sized buffer; honor that too.
      if (koptlen >= 16) {
        ((long *)koptval)[0] = 0;
        ((long *)koptval)[1] = 0;
        koptlen = 16;
      } else {
        *(int *)koptval = 0;
        koptlen = sizeof(int);
      }
      break;
    }
    default:
      handled = 0;
      break;
    }
    if (handled) {
      int copy_len = koptlen < orig_optlen ? koptlen : orig_optlen;
      int err = 0;
      if ((copy_len > 0 && copy_to_user((void *)optval_ptr, koptval, (size_t)copy_len) != 0) ||
          copy_to_user((void *)optlen_ptr, &koptlen, sizeof(int)) != 0)
        err = -14;
      if (koptval != stack_opt) kfree(koptval);
      return (uint64_t)(int64_t)err;
    }
  }

  if (koptval != stack_opt) kfree(koptval);
  char opt_buf[48];
  snprintf(opt_buf, sizeof(opt_buf), "getsockopt lvl=%d opt=%d", (int)level, (int)optname);
  sysdiag_record_unimplemented_feature("socket", opt_buf, sched_get_current(), -92);
  return (uint64_t)-92; // ENOPROTOOPT
}

// Syscall: setsockopt(int sockfd, int level, int optname, ...)
static uint64_t sys_setsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  // Validate optlen — must be at least 1 byte (timeval, int, etc.)
  if (optlen < 1 || optlen > 65536) {
    return (uint64_t)-22; // EINVAL
  }

  // Validate pointer
  if (!is_user_range((const void *)optval_ptr, (size_t)optlen)) {
    return (uint64_t)-14; // EFAULT
  }

  uint8_t stack_opt[64];
  void *koptval = optlen <= sizeof(stack_opt) ? (void *)stack_opt
                                              : kmalloc((size_t)optlen);
  if (!koptval) {
    return (uint64_t)-12;
  }
  if (copy_from_user(koptval, (const void *)optval_ptr, (size_t)optlen) != 0) {
    if (koptval != stack_opt) kfree(koptval);
    return (uint64_t)-14;
  }

  // Handle socket-level options. These are remembered on the generic socket
  // and then forwarded to the family handler so TCP/UDP can apply them.
  bool sol_handled = false;
  if ((int)level == SOL_SOCKET && optlen >= sizeof(int)) {
    const int *val = (const int *)koptval;
    switch ((int)optname) {
    case SO_RCVBUF:
    case SO_RCVBUFFORCE: {
      int v = *val;
      if (v < 256)
        v = 256;
      if (v > 1024 * 1024)
        v = 1024 * 1024; // 1MB max
      sock->rcvbuf = v;
      sol_handled = true;
      break;
    }
    case SO_SNDBUF:
    case SO_SNDBUFFORCE: {
      int v = *val;
      if (v < 256)
        v = 256;
      if (v > 1024 * 1024)
        v = 1024 * 1024; // 1MB max
      sock->sndbuf = v;
      sol_handled = true;
      break;
    }
    case SO_REUSEADDR:
      sock->reuseaddr = *val;
      sol_handled = true;
      break;
    case SO_KEEPALIVE:
    case SO_BROADCAST:
    case SO_RCVTIMEO:
    case SO_SNDTIMEO:
    case SO_LINGER:
      sol_handled = true;
      break;
    default:
      break;
    }
  }

  // Try family-specific handler
  if (koptval != stack_opt) kfree(koptval);
  if (sock->ops && sock->ops->setsockopt) {
    int ret = sock->ops->setsockopt(sock, (int)level, (int)optname,
                                    (const void *)optval_ptr, (int)optlen);
    /* A generic option can fall back when the family does not implement it,
     * but recognized invalid lengths or copy faults must reach the caller. */
    if (sol_handled && ret == -92)
      return 0;
    return (uint64_t)ret;
  }

  if (sol_handled)
    return 0;

  // Default stub for common SOL_SOCKET options if not handled by family
  if ((int)level == SOL_SOCKET) {
    switch ((int)optname) {
    case SO_PASSCRED:
    case 26: // SO_ATTACH_FILTER
      return 0;
    }
  }

  // Unknown level/option — stub success to avoid breaking applications
  return 0;
}

// Syscall: getsockname(int sockfd, struct sockaddr *addr, ...)
static uint64_t sys_getsockname(uint64_t sockfd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t _arg3,
                                uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (sock->closing) {
    return (uint64_t)-9;
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_range((void *)addrlen_ptr, sizeof(int))) {
    return (uint64_t)-14;
  }

  int user_addrlen = 0;
  if (copy_from_user(&user_addrlen, (void *)addrlen_ptr, sizeof(int)) != 0) {
    return (uint64_t)-14;
  }
  if (user_addrlen < 0) {
    return (uint64_t)-22;
  }

  uint8_t kaddr_buf[128];
  /* Families fill a kernel buffer independently of the user's capacity.
   * copy_sockaddr_out truncates the copy and returns the complete length. */
  int kaddrlen = sizeof(kaddr_buf);
  int ret = socket_getsockname(sock, (struct sockaddr *)kaddr_buf, &kaddrlen);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  ret = copy_sockaddr_out(addr_ptr, addrlen_ptr, (struct sockaddr *)kaddr_buf,
                          kaddrlen);
  return (uint64_t)ret;
}

// Syscall: getpeername(int sockfd, struct sockaddr *addr, ...)
static uint64_t sys_getpeername(uint64_t sockfd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t _arg3,
                                uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock __attribute__((cleanup(socket_release_fd))) =
      socket_from_fd(fd);
  if (!sock) {
    // GLib deliberately probes stderr with getpeername() and relies on
    // ENOTSOCK for a valid descriptor that is not a socket.
    return socket_eno_notsock(fd);
  }
  if (sock->closing) {
    return (uint64_t)-9;
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_range((void *)addrlen_ptr, sizeof(int))) {
    return (uint64_t)-14;
  }

  int user_addrlen = 0;
  if (copy_from_user(&user_addrlen, (void *)addrlen_ptr, sizeof(int)) != 0) {
    return (uint64_t)-14;
  }
  if (user_addrlen < 0) {
    return (uint64_t)-22;
  }

  uint8_t kaddr_buf[128];
  /* Families fill a kernel buffer independently of the user's capacity.
   * copy_sockaddr_out truncates the copy and returns the complete length. */
  int kaddrlen = sizeof(kaddr_buf);
  int ret = socket_getpeername(sock, (struct sockaddr *)kaddr_buf, &kaddrlen);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  ret = copy_sockaddr_out(addr_ptr, addrlen_ptr, (struct sockaddr *)kaddr_buf,
                          kaddrlen);
  return (uint64_t)ret;
}

// Socket Syscall Registration
void syscall_register_socket(void) {
  syscall_register(SYS_SOCKET, sys_socket);
  syscall_register(SYS_SOCKETPAIR, sys_socketpair);
  syscall_register(SYS_BIND, sys_bind);
  syscall_register(SYS_CONNECT, sys_connect);
  syscall_register(SYS_LISTEN, sys_listen);
  syscall_register(SYS_ACCEPT, sys_accept);
  syscall_register(SYS_ACCEPT4, sys_accept4);
  syscall_register(SYS_SENDTO, sys_sendto);
  syscall_register(SYS_RECVFROM, sys_recvfrom);
  syscall_register(SYS_RECVMSG, sys_recvmsg);
  syscall_register(SYS_SHUTDOWN, sys_shutdown);
  syscall_register(SYS_SETSOCKOPT, sys_setsockopt);
  syscall_register(SYS_GETSOCKOPT, sys_getsockopt);
  syscall_register(SYS_SENDMSG, sys_sendmsg);
  syscall_register(SYS_SENDMMSG, sys_sendmmsg);
  syscall_register(SYS_RECVMMSG, sys_recvmmsg);
  syscall_register(SYS_GETSOCKNAME, sys_getsockname);
  syscall_register(SYS_GETPEERNAME, sys_getpeername);

  klog_puts("[OK] Socket syscalls registered\n");
}
