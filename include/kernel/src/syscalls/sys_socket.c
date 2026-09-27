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
#include <stdint.h>

#define SOCKET_FD_FLAGS_CLOEXEC_BIT (1u << 24)

/* Linux distinguishes a closed/invalid descriptor (EBADF) from a valid
 * descriptor whose underlying object is not a socket (ENOTSOCK).  Libraries
 * depend on the difference: PulseAudio's pa_write() tries send() first and
 * only falls back to write() when errno is ENOTSOCK, so answering EBADF for
 * its wakeup pipe makes every libpulse mainloop wakeup fail.  GLib probes
 * stderr with getpeername() for the same reason. */
static uint64_t socket_eno_notsock(int fd) {
  struct thread *current = sched_get_current();
  bool fd_exists = current && fd >= 0 && fd < MAX_FDS && current->fds[fd];
  return fd_exists ? (uint64_t)-88 : (uint64_t)-9;
}

static int copy_sockaddr_out(uint64_t addr_ptr, uint64_t addrlen_ptr,
                             const struct sockaddr *kaddr, int kaddrlen) {
  if (!addr_ptr || !addrlen_ptr)
    return 0;

  if (!is_user_ptr(addr_ptr) || !is_user_ptr(addrlen_ptr))
    return -14; // EFAULT

  if (!vmm_is_user_addr_range_writable(addrlen_ptr, sizeof(int)))
    return -14;

  int user_len = 0;
  if (copy_from_user(&user_len, (void *)addrlen_ptr, sizeof(int)) != 0)
    return -14;
  if (user_len < 0)
    return -22; // EINVAL

  int copy = kaddrlen < user_len ? kaddrlen : user_len;
  if (copy > 0) {
    if (!vmm_is_user_addr_range_writable(addr_ptr, (size_t)copy))
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

  // Create socket
  socket_t *sock = socket_create(dom, typ, proto);
  if (!sock) {
    // Determine error - extract base type for comparison
    int base = typ & ~SOCK_NONBLOCK & ~SOCK_CLOEXEC;
    if (dom != AF_UNIX && dom != AF_INET && dom != AF_INET6 && dom != AF_NETLINK)
      return (uint64_t)-EAFNOSUPPORT;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_RAW &&
        base != SOCK_SEQPACKET)
      return (uint64_t)-EPROTONOSUPPORT;
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

  // Validate sv pointer
  if (!is_user_ptr(sv_ptr) || (sv_ptr & 3)) {
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
  int *sv = (int *)sv_ptr;
  sv[0] = fd0;
  sv[1] = fd1;

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

  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  uint8_t kaddr_buf[128];
  if (addrlen > sizeof(kaddr_buf))
    return (uint64_t)-22; // EINVAL

  if (!vmm_is_user_addr_range_valid(addr_ptr, addrlen))
    return (uint64_t)-14; // EFAULT

  memset(kaddr_buf, 0, sizeof(kaddr_buf));
  memcpy(kaddr_buf, (void *)addr_ptr, addrlen);
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
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  // Copy sockaddr safely into kernel stack buffer before reading fields
  uint8_t kaddr_buf[128];
  if (addrlen > sizeof(kaddr_buf))
    return (uint64_t)-22; // EINVAL

  memset(kaddr_buf, 0, sizeof(kaddr_buf));
  memcpy(kaddr_buf, (void *)addr_ptr, addrlen);
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
  socket_t *sock = socket_from_fd(fd);
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
  socket_t *sock = socket_from_fd(fd);
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

  if (t && newfd < MAX_FDS) {
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

// Syscall: sendto(int sockfd, const void *buf, size_t len, int flags, ...)
static uint64_t sys_sendto(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                           uint64_t flags, uint64_t dest_addr_ptr,
                           uint64_t addrlen) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (!is_user_ptr(buf_ptr) || (len > 0 && !is_user_ptr(buf_ptr + len - 1))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  const void *buf = (const void *)buf_ptr;
  struct sockaddr *dest_addr = NULL;
  if (dest_addr_ptr) {
    if (!is_user_ptr(dest_addr_ptr)) {
      socket_put(sock);
      return (uint64_t)-14;
    }
    dest_addr = (struct sockaddr *)dest_addr_ptr;
  }

  ssize_t ret =
      socket_sendto(sock, buf, len, (int)flags, dest_addr, (int)addrlen);
  socket_put(sock);
  return (uint64_t)ret;
}

// Syscall: recvfrom(int sockfd, void *buf, size_t len, int flags, ...)
static uint64_t sys_recvfrom(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t src_addr_ptr,
                             uint64_t addrlen_ptr) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (!is_user_ptr(buf_ptr) || (len > 0 && !is_user_ptr(buf_ptr + len - 1))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  void *buf = (void *)buf_ptr;
  struct sockaddr *src_addr = NULL;
  int *addrlen = NULL;

  if (src_addr_ptr) {
    if (!is_user_ptr(src_addr_ptr)) {
      socket_put(sock);
      return (uint64_t)-14;
    }
    src_addr = (struct sockaddr *)src_addr_ptr;
  }

  if (addrlen_ptr) {
    if (!is_user_ptr(addrlen_ptr)) {
      socket_put(sock);
      return (uint64_t)-14;
    }
    addrlen = (int *)addrlen_ptr;
  }

  ssize_t ret = socket_recvfrom(sock, buf, len, (int)flags, src_addr, addrlen);
  socket_put(sock);
  return (uint64_t)ret;
}

// Structures for recvmsg/sendmsg

// Syscall: sendmsg(int sockfd, struct msghdr *msg, int flags)
static uint64_t sys_sendmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Validate msghdr pointer
  if (!is_user_ptr(msg_ptr) ||
      !vmm_is_user_addr_range_valid(msg_ptr, sizeof(struct msghdr))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  struct msghdr *msg = (struct msghdr *)msg_ptr;

  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  // Validate iovec array
  if (msg->msg_iovlen > 0 &&
      (!is_user_ptr((uint64_t)msg->msg_iov) ||
       !vmm_is_user_addr_range_valid((uint64_t)msg->msg_iov,
                                     msg->msg_iovlen * sizeof(struct iovec)))) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];
    if (iov->iov_len > 0 &&
        (!is_user_ptr((uint64_t)iov->iov_base) ||
         !vmm_is_user_addr_range_valid((uint64_t)iov->iov_base, iov->iov_len))) {
      socket_put(sock);
      return (uint64_t)-14; // EFAULT
    }
  }

  // Use family-specific sendmsg if available
  if (sock->ops && sock->ops->sendmsg) {
    ssize_t r = sock->ops->sendmsg(sock, msg, (int)flags);
    socket_put(sock);
    return (uint64_t)r;
  }

  // Fallback to simple send from iovec array
  ssize_t total_sent = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];

    if (!is_user_ptr((uint64_t)iov->iov_base) ||
        !vmm_is_user_addr_range_valid((uint64_t)iov->iov_base, iov->iov_len)) {
      socket_put(sock);
      return (uint64_t)-14; // EFAULT
    }

    if (iov->iov_len == 0) {
      continue;
    }

    ssize_t ret = socket_send(sock, iov->iov_base, iov->iov_len, (int)flags);
    if (ret < 0) {
      if (total_sent > 0) {
        socket_put(sock);
        return (uint64_t)total_sent;
      }
      socket_put(sock);
      return (uint64_t)ret;
    }

    total_sent += ret;

    // If we sent less than requested, we're done (buffer full)
    if ((size_t)ret < iov->iov_len) {
      break;
    }
  }

  socket_put(sock);
  return (uint64_t)total_sent;
}

// Syscall: recvmsg(int sockfd, struct msghdr *msg, int flags)
static uint64_t sys_recvmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Validate msghdr pointer
  if (!is_user_ptr(msg_ptr) ||
      !vmm_is_user_addr_range_valid(msg_ptr, sizeof(struct msghdr))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  struct msghdr *msg = (struct msghdr *)msg_ptr;

  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  // Validate iovec array (only if iovlen > 0)
  if (msg->msg_iovlen > 0 &&
      (!is_user_ptr((uint64_t)msg->msg_iov) ||
       !vmm_is_user_addr_range_valid((uint64_t)msg->msg_iov,
                                     msg->msg_iovlen * sizeof(struct iovec)))) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];
    if (iov->iov_len > 0 &&
        (!is_user_ptr((uint64_t)iov->iov_base) ||
         !vmm_is_user_addr_range_valid((uint64_t)iov->iov_base, iov->iov_len))) {
      socket_put(sock);
      return (uint64_t)-14; // EFAULT
    }
  }

  // Use family-specific recvmsg if available
  if (sock->ops && sock->ops->recvmsg) {
    ssize_t r = sock->ops->recvmsg(sock, msg, (int)flags);
    socket_put(sock);
    return (uint64_t)r;
  }

  // Fallback to simple recv into iovec array
  ssize_t total_received = 0;

  /* Fill msg_name with the source address when requested.
   * Pull the first iovec as the data buffer and use recvfrom. */
  struct sockaddr *src_addr = NULL;
  int src_addrlen = 0;
  if (msg->msg_name && msg->msg_namelen > 0 &&
      is_user_ptr((uint64_t)msg->msg_name)) {
    src_addr = (struct sockaddr *)msg->msg_name;
    src_addrlen = (int)msg->msg_namelen;
  }

  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];

    if (!is_user_ptr((uint64_t)iov->iov_base) ||
        !vmm_is_user_addr_range_valid((uint64_t)iov->iov_base, iov->iov_len)) {
      socket_put(sock);
      return (uint64_t)-14; // EFAULT
    }

    if (iov->iov_len == 0) {
      continue;
    }

    ssize_t ret;
    if (i == 0 && src_addr) {
      /* First buffer: use recvfrom to capture source address */
      ret = socket_recvfrom(
          sock, iov->iov_base, iov->iov_len,
          (int)flags | (sock->flags & SOCK_NONBLOCK ? MSG_DONTWAIT : 0),
          src_addr, &src_addrlen);
      if (ret >= 0)
        msg->msg_namelen = (uint32_t)src_addrlen;
    } else {
      ret = socket_recv(sock, iov->iov_base, iov->iov_len, (int)flags);
    }

    if (ret < 0) {
      if (total_received > 0) {
        socket_put(sock);
        return (uint64_t)total_received;
      }
      socket_put(sock);
      return (uint64_t)ret;
    }

    total_received += ret;

    // If we received less than requested, we're done
    if ((size_t)ret < iov->iov_len) {
      break;
    }
  }

  // Update msghdr fields for user space
  // msg_namelen already updated above if src_addr was filled
  msg->msg_controllen = 0;
  msg->msg_flags = 0;

  socket_put(sock);
  return (uint64_t)total_received;
}

struct mmsghdr {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

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

  if (!vmm_is_user_addr_range_writable(msgvec_ptr, count * sizeof(struct mmsghdr)))
    return (uint64_t)-14; // EFAULT

  struct mmsghdr *vec = (struct mmsghdr *)msgvec_ptr;
  unsigned int sent_msgs = 0;

  for (unsigned int i = 0; i < count; i++) {
    uint64_t ret = sys_sendmsg(sockfd, (uint64_t)&vec[i].msg_hdr, flags, 0, 0, 0);
    if ((int64_t)ret < 0) {
      if (sent_msgs > 0)
        return (uint64_t)sent_msgs;
      return ret;
    }
    vec[i].msg_len = (unsigned int)ret;
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

  if (!vmm_is_user_addr_range_writable(msgvec_ptr, count * sizeof(struct mmsghdr)))
    return (uint64_t)-14; // EFAULT

  struct mmsghdr *vec = (struct mmsghdr *)msgvec_ptr;
  unsigned int recvd_msgs = 0;

  for (unsigned int i = 0; i < count; i++) {
    uint64_t ret = sys_recvmsg(sockfd, (uint64_t)&vec[i].msg_hdr, flags, 0, 0, 0);
    if ((int64_t)ret < 0) {
      if (recvd_msgs > 0)
        return (uint64_t)recvd_msgs;
      return ret;
    }
    vec[i].msg_len = (unsigned int)ret;
    recvd_msgs++;
    if (flags & MSG_DONTWAIT)
      break;
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
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  if (!sock->ops || !sock->ops->shutdown) {
    socket_put(sock);
    return (uint64_t)-95; // EOPNOTSUPP
  }

  int ret = sock->ops->shutdown(sock, (int)how);
  socket_put(sock);
  return (uint64_t)ret;
}

// Syscall: getsockopt(int sockfd, int level, int optname, ...)
static uint64_t sys_getsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen_ptr, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;
  socket_t *sock = socket_from_fd(fd);
  if (!sock)
    return socket_eno_notsock(fd);

  if (!is_user_ptr(optval_ptr) || !is_user_ptr(optlen_ptr))
    return (uint64_t)-14;

  int *optlen = (int *)optlen_ptr;

  // Validate optlen
  if (*optlen < (int)sizeof(int)) {
    return (uint64_t)-22; // EINVAL
  }

  void *optval = (void *)optval_ptr;

  // Handle socket-level options
  if ((int)level == SOL_SOCKET) {
    int *val = (int *)optval;
    switch ((int)optname) {
    case SO_TYPE:
      *val = sock->type;
      *optlen = sizeof(int);
      return 0;
    case SO_DOMAIN:
      *val = sock->domain;
      *optlen = sizeof(int);
      return 0;
    case SO_PROTOCOL:
      *val = sock->protocol;
      *optlen = sizeof(int);
      return 0;
    case SO_ERROR:
      // Let the family report connection errors (e.g. failed non-blocking
      // connect); fall back to the generic socket error.
      if (sock->ops && sock->ops->getsockopt) {
        int ret = sock->ops->getsockopt(sock, (int)level, (int)optname,
                                        optval, optlen);
        if (ret == 0)
          return 0;
      }
      *val = socket_get_error(sock);
      *optlen = sizeof(int);
      return 0;
    case SO_RCVBUF:
      *val = sock->rcvbuf;
      *optlen = sizeof(int);
      return 0;
    case SO_SNDBUF:
      *val = sock->sndbuf;
      *optlen = sizeof(int);
      return 0;
    case SO_REUSEADDR:
      *val = sock->reuseaddr;
      *optlen = sizeof(int);
      return 0;
    default:
      break;
    }
  }

  // Try family-specific handler
  if (sock->ops && sock->ops->getsockopt) {
    int ret =
        sock->ops->getsockopt(sock, (int)level, (int)optname, optval, optlen);
    return (uint64_t)ret;
  }

  return (uint64_t)-92; // ENOPROTOOPT
}

// Syscall: setsockopt(int sockfd, int level, int optname, ...)
static uint64_t sys_setsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }

  // Validate pointer
  if (!is_user_ptr(optval_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  const void *optval = (const void *)optval_ptr;

  // Validate optlen — must be at least 1 byte (timeval, int, etc.)
  if (optlen < 1) {
    return (uint64_t)-22; // EINVAL
  }

  // Handle socket-level options. These are remembered on the generic socket
  // and then forwarded to the family handler so TCP/UDP can apply them.
  bool sol_handled = false;
  if ((int)level == SOL_SOCKET) {
    const int *val = (const int *)optval;
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
  if (sock->ops && sock->ops->setsockopt) {
    int ret = sock->ops->setsockopt(sock, (int)level, (int)optname, optval,
                                    (int)optlen);
    if (sol_handled)
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
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return socket_eno_notsock(fd);
  }
  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  if (!vmm_is_user_addr_range_writable(addrlen_ptr, sizeof(int))) {
    socket_put(sock);
    return (uint64_t)-14;
  }

  int user_addrlen = 0;
  if (copy_from_user(&user_addrlen, (void *)addrlen_ptr, sizeof(int)) != 0) {
    socket_put(sock);
    return (uint64_t)-14;
  }
  if (user_addrlen < 0) {
    socket_put(sock);
    return (uint64_t)-22;
  }

  uint8_t kaddr_buf[128];
  if (user_addrlen > (int)sizeof(kaddr_buf))
    user_addrlen = (int)sizeof(kaddr_buf);

  int kaddrlen = user_addrlen;
  int ret = socket_getsockname(sock, (struct sockaddr *)kaddr_buf, &kaddrlen);
  if (ret < 0) {
    socket_put(sock);
    return (uint64_t)ret;
  }

  ret = copy_sockaddr_out(addr_ptr, addrlen_ptr, (struct sockaddr *)kaddr_buf,
                          kaddrlen);
  socket_put(sock);
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
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    // GLib deliberately probes stderr with getpeername() and relies on
    // ENOTSOCK for a valid descriptor that is not a socket.
    return socket_eno_notsock(fd);
  }
  if (!socket_try_get(sock))
    return (uint64_t)-9;
  if (sock->closing) {
    socket_put(sock);
    return (uint64_t)-9;
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    socket_put(sock);
    return (uint64_t)-14; // EFAULT
  }

  if (!vmm_is_user_addr_range_writable(addrlen_ptr, sizeof(int))) {
    socket_put(sock);
    return (uint64_t)-14;
  }

  int user_addrlen = 0;
  if (copy_from_user(&user_addrlen, (void *)addrlen_ptr, sizeof(int)) != 0) {
    socket_put(sock);
    return (uint64_t)-14;
  }
  if (user_addrlen < 0) {
    socket_put(sock);
    return (uint64_t)-22;
  }

  uint8_t kaddr_buf[128];
  if (user_addrlen > (int)sizeof(kaddr_buf))
    user_addrlen = (int)sizeof(kaddr_buf);

  int kaddrlen = user_addrlen;
  int ret = socket_getpeername(sock, (struct sockaddr *)kaddr_buf, &kaddrlen);
  if (ret < 0) {
    socket_put(sock);
    return (uint64_t)ret;
  }

  ret = copy_sockaddr_out(addr_ptr, addrlen_ptr, (struct sockaddr *)kaddr_buf,
                          kaddrlen);
  socket_put(sock);
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
