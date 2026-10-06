#include "af_qrtr.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include "../include/arch/uaccess.h"

// Qualcomm IPC Router (QRTR) Socket Implementation
// AF_QIPCRTR (domain 42) is used by ModemManager and qrtr tooling.

typedef struct qrtr_sock {
  socket_t *parent;
  uint32_t node;
  uint32_t port;
  sk_buff_head_t recv_queue;
} qrtr_sock_t;

static uint32_t next_qrtr_port = 1;
static spinlock_t qrtr_port_lock = SPINLOCK_INIT;

static int qrtr_bind(struct socket *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !sock->sk)
    return -22; // EINVAL

  if (!addr || addrlen < (int)sizeof(struct sockaddr_qrtr))
    return -22;

  struct sockaddr_qrtr sq;
  if (copy_from_user(&sq, addr, sizeof(sq)) != 0)
    return -14; // EFAULT

  if (sq.sq_family != AF_QIPCRTR)
    return -22;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;
  if (sq.sq_node != 0)
    qsk->node = sq.sq_node;
  if (sq.sq_port != 0)
    qsk->port = sq.sq_port;

  sock->state = SS_CONNECTED;
  return 0;
}

static int qrtr_getsockname(struct socket *sock, struct sockaddr *addr,
                            int *addrlen) {
  if (!sock || !sock->sk || !addr || !addrlen)
    return -22;

  int user_len = 0;
  if (copy_from_user(&user_len, addrlen, sizeof(int)) != 0)
    return -14;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;
  struct sockaddr_qrtr sq;
  memset(&sq, 0, sizeof(sq));
  sq.sq_family = AF_QIPCRTR;
  sq.sq_node = qsk->node;
  sq.sq_port = qsk->port;

  size_t to_copy = sizeof(sq);
  if (user_len > 0 && (size_t)user_len < to_copy)
    to_copy = (size_t)user_len;

  if (copy_to_user(addr, &sq, to_copy) != 0)
    return -14;

  int full_len = (int)sizeof(sq);
  if (copy_to_user(addrlen, &full_len, sizeof(full_len)) != 0)
    return -14;

  return 0;
}

static int qrtr_getpeername(struct socket *sock, struct sockaddr *addr,
                            int *addrlen) {
  (void)sock;
  (void)addr;
  (void)addrlen;
  return -107; // ENOTCONN
}

static ssize_t qrtr_sendto(struct socket *sock, const void *buf, size_t len,
                           int flags, struct sockaddr *dest_addr,
                           int addrlen) {
  (void)flags;
  (void)dest_addr;
  (void)addrlen;
  if (!sock)
    return -22;
  if (len == 0)
    return 0;
  if (!buf)
    return -14;
  if (!vmm_is_user_addr_range_valid((uint64_t)buf, len))
    return -14;

  // Packets sent into the QRTR router bus are accepted.
  // In the absence of attached hardware modems, broadcast/discovery packets
  // are successfully accepted as routed.
  return (ssize_t)len;
}

static ssize_t qrtr_send(struct socket *sock, const void *buf, size_t len,
                         int flags) {
  return qrtr_sendto(sock, buf, len, flags, NULL, 0);
}

static ssize_t qrtr_sendmsg(struct socket *sock, struct msghdr *msg,
                            int flags) {
  (void)flags;
  if (!sock || !msg)
    return -22;

  struct msghdr kmsg;
  if (copy_from_user(&kmsg, msg, sizeof(kmsg)) != 0)
    return -14;

  size_t total_len = 0;
  for (size_t i = 0; i < (size_t)kmsg.msg_iovlen; i++) {
    struct iovec iov;
    if (copy_from_user(&iov, &kmsg.msg_iov[i], sizeof(iov)) != 0)
      return -14;
    total_len += iov.iov_len;
  }

  return (ssize_t)total_len;
}

static ssize_t qrtr_recvfrom(struct socket *sock, void *buf, size_t len,
                             int flags, struct sockaddr *src_addr,
                             int *addrlen) {
  (void)src_addr;
  if (!sock || !sock->sk)
    return -22;

  if (len == 0) {
    if (addrlen) {
      int zero = 0;
      copy_to_user(addrlen, &zero, sizeof(zero));
    }
    return 0;
  }

  if (!buf || !vmm_is_user_addr_range_valid((uint64_t)buf, len))
    return -14;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;
  struct thread *cur = sched_get_current();
  wait_queue_entry_t wqe;
  memset(&wqe, 0, sizeof(wqe));
  wqe.thread = cur;

  while (1) {
    spinlock_acquire(&qsk->recv_queue.lock);
    sk_buff_t *skb = skb_dequeue(&qsk->recv_queue);
    if (skb) {
      spinlock_release(&qsk->recv_queue.lock);
      size_t to_copy = skb->len < len ? skb->len : len;
      if (copy_to_user(buf, skb->data, to_copy) != 0) {
        free_skb(skb);
        return -14;
      }
      free_skb(skb);
      return (ssize_t)to_copy;
    }

    if ((flags & MSG_DONTWAIT) || (sock->flags & SOCK_NONBLOCK)) {
      spinlock_release(&qsk->recv_queue.lock);
      return -11; // EAGAIN
    }

    if (cur)
      cur->state = THREAD_BLOCKED;
    if (sock->wait_queue)
      wait_queue_add((wait_queue_t *)sock->wait_queue, &wqe);

    spinlock_release(&qsk->recv_queue.lock);

    sched_yield();

    if (sock->wait_queue)
      wait_queue_remove((wait_queue_t *)sock->wait_queue, &wqe);
    if (cur)
      cur->state = THREAD_RUNNING;

    if (cur && thread_has_pending_signal(cur))
      return -4; // EINTR
  }
}

static ssize_t qrtr_recv(struct socket *sock, void *buf, size_t len, int flags) {
  return qrtr_recvfrom(sock, buf, len, flags, NULL, NULL);
}

static ssize_t qrtr_recvmsg(struct socket *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22;

  struct msghdr kmsg;
  if (copy_from_user(&kmsg, msg, sizeof(kmsg)) != 0)
    return -14;

  if (kmsg.msg_iovlen == 0)
    return 0;

  struct iovec iov;
  if (copy_from_user(&iov, &kmsg.msg_iov[0], sizeof(iov)) != 0)
    return -14;

  ssize_t ret = qrtr_recvfrom(sock, iov.iov_base, iov.iov_len, flags,
                              kmsg.msg_name, (int *)&kmsg.msg_namelen);
  if (ret >= 0) {
    kmsg.msg_flags = 0;
    copy_to_user(msg, &kmsg, sizeof(kmsg));
  }
  return ret;
}

static int qrtr_poll(struct socket *sock, int events) {
  (void)events;
  if (!sock || !sock->sk)
    return POLLERR;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;
  int mask = POLLOUT | POLLWRNORM;

  spinlock_acquire(&qsk->recv_queue.lock);
  if (!skb_queue_empty(&qsk->recv_queue))
    mask |= (POLLIN | POLLRDNORM);
  spinlock_release(&qsk->recv_queue.lock);

  return mask;
}

static int qrtr_setsockopt(struct socket *sock, int level, int optname,
                           const void *optval, int optlen) {
  (void)sock;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  return 0; // Accept socket options gracefully
}

static int qrtr_getsockopt(struct socket *sock, int level, int optname,
                           void *optval, int *optlen) {
  (void)sock;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  return 0;
}

static int qrtr_ioctl(struct socket *sock, uint32_t request, uint64_t arg) {
  if (!sock || !sock->sk)
    return -22;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;

  switch (request) {
  case 0x541B: { // FIONREAD
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    spinlock_acquire(&qsk->recv_queue.lock);
    int unread = qsk->recv_queue.head ? (int)qsk->recv_queue.head->len : 0;
    spinlock_release(&qsk->recv_queue.lock);
    if (copy_to_user((void *)arg, &unread, sizeof(int)) != 0)
      return -14;
    return 0;
  }
  case 0x5421: { // FIONBIO
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    int nonblocking = 0;
    if (copy_from_user(&nonblocking, (const void *)arg, sizeof(int)) != 0)
      return -14;
    if (nonblocking)
      sock->flags |= SOCK_NONBLOCK;
    else
      sock->flags &= ~SOCK_NONBLOCK;
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

static void qrtr_destroy(struct socket *sock) {
  if (!sock || !sock->sk)
    return;

  qrtr_sock_t *qsk = (qrtr_sock_t *)sock->sk;
  sock->sk = NULL;

  spinlock_acquire(&qsk->recv_queue.lock);
  while (!skb_queue_empty(&qsk->recv_queue)) {
    sk_buff_t *skb = skb_dequeue(&qsk->recv_queue);
    if (skb)
      free_skb(skb);
  }
  spinlock_release(&qsk->recv_queue.lock);

  kfree(qsk);
}

static sock_ops_t qrtr_ops = {
    .bind = qrtr_bind,
    .connect = NULL,
    .listen = NULL,
    .accept = NULL,
    .send = qrtr_send,
    .recv = qrtr_recv,
    .sendto = qrtr_sendto,
    .recvfrom = qrtr_recvfrom,
    .sendmsg = qrtr_sendmsg,
    .recvmsg = qrtr_recvmsg,
    .shutdown = NULL,
    .poll = qrtr_poll,
    .setsockopt = qrtr_setsockopt,
    .getsockopt = qrtr_getsockopt,
    .getsockname = qrtr_getsockname,
    .getpeername = qrtr_getpeername,
    .ioctl = qrtr_ioctl,
    .destroy = qrtr_destroy,
};

static int qrtr_create(socket_t *sock, int protocol) {
  (void)protocol;
  if (!sock)
    return -22;

  if (sock->type != SOCK_DGRAM)
    return -41; // EPROTOTYPE

  qrtr_sock_t *qsk = kmalloc(sizeof(qrtr_sock_t));
  if (!qsk)
    return -12; // ENOMEM

  memset(qsk, 0, sizeof(*qsk));
  qsk->parent = sock;
  qsk->node = 1; // Default local node ID (1)
  spinlock_acquire(&qrtr_port_lock);
  qsk->port = next_qrtr_port++;
  spinlock_release(&qrtr_port_lock);

  spinlock_init(&qsk->recv_queue.lock);
  qsk->recv_queue.head = NULL;
  qsk->recv_queue.tail = NULL;
  qsk->recv_queue.len = 0;

  sock->sk = qsk;
  sock->ops = &qrtr_ops;
  sock->state = SS_UNCONNECTED;

  klog_puts("[OK] AF_QIPCRTR socket created (node=");
  klog_uint64((uint64_t)qsk->node);
  klog_puts(" port=");
  klog_uint64((uint64_t)qsk->port);
  klog_puts(")\n");

  return 0;
}

static net_family_t qrtr_family = {
    .family = AF_QIPCRTR,
    .create = qrtr_create,
    .next = NULL,
};

void af_qrtr_init(void) {
  sock_register_family(&qrtr_family);
  klog_puts("[OK] AF_QIPCRTR protocol initialized\n");
}
