#include "af_packet.h"
#include "../console/klog.h"
#include "../include/arch/uaccess.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../net/core.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"

#define ETH_P_ALL_BE 0x0300U /* htons(0x0003) on little-endian x86_64 */
#define ETH_HDR_LEN 14U
#define PACKET_QUEUE_CAP 64

#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3

struct packet_frame {
  uint16_t length;
  uint16_t eth_type_be;
  uint8_t pkttype;
  uint8_t src_mac[6];
  uint8_t data[NET_FRAME_MAX];
};

struct packet_sock {
  socket_t *sock;
  spinlock_t lock;
  uint16_t protocol_be; /* 0 or ETH_P_ALL_BE matches all */
  int32_t ifindex;      /* 0 = any, 2 = default NIC */
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  struct packet_frame queue[PACKET_QUEUE_CAP];
  struct packet_sock *next;
};

static spinlock_t packet_list_lock = SPINLOCK_INIT;
static struct packet_sock *packet_list_head = NULL;

static bool packet_proto_matches(uint16_t sock_proto_be, uint16_t eth_type_be) {
  if (sock_proto_be == 0 || sock_proto_be == ETH_P_ALL_BE)
    return true;
  return sock_proto_be == eth_type_be;
}

static uint8_t packet_classify_dst(const uint8_t dst[6], const uint8_t dev_mac[6]) {
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  if (memcmp(dst, bcast, 6) == 0)
    return PACKET_BROADCAST;
  if (dst[0] & 0x01)
    return PACKET_MULTICAST;
  if (memcmp(dst, dev_mac, 6) == 0)
    return PACKET_HOST;
  return PACKET_OTHERHOST;
}

static void packet_rx_tap(const struct net_packet *pkt, void *context) {
  (void)context;
  if (!pkt || !pkt->device || pkt->length < ETH_HDR_LEN ||
      pkt->length > NET_FRAME_MAX)
    return;

  uint16_t eth_type_be;
  memcpy(&eth_type_be, pkt->data + 12, sizeof(eth_type_be));
  uint8_t pkttype = packet_classify_dst(pkt->data, pkt->device->mac);

  spinlock_acquire(&packet_list_lock);
  for (struct packet_sock *ps = packet_list_head; ps; ps = ps->next) {
    if (ps->ifindex != 0 && ps->ifindex != 2)
      continue;
    if (!packet_proto_matches(ps->protocol_be, eth_type_be))
      continue;

    spinlock_acquire(&ps->lock);
    if (ps->count < PACKET_QUEUE_CAP) {
      struct packet_frame *slot = &ps->queue[ps->tail];
      slot->length = pkt->length;
      slot->eth_type_be = eth_type_be;
      slot->pkttype = pkttype;
      memcpy(slot->src_mac, pkt->data + 6, 6);
      memcpy(slot->data, pkt->data, pkt->length);
      ps->tail = (ps->tail + 1) % PACKET_QUEUE_CAP;
      ps->count++;
    }
    spinlock_release(&ps->lock);

    if (ps->sock) {
      socket_wake(ps->sock);
      if (ps->sock->node)
        epoll_notify_event(ps->sock->node, EPOLLIN);
    }
  }
  spinlock_release(&packet_list_lock);
}

static int packet_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !sock->sk || !addr)
    return -22;
  if (addrlen < (int)sizeof(struct sockaddr_ll))
    return -22;

  struct sockaddr_ll sll;
  if (is_user_ptr((uint64_t)addr)) {
    if (!vmm_is_user_addr_range_valid((uint64_t)addr, sizeof(sll)))
      return -14;
    if (copy_from_user(&sll, addr, sizeof(sll)) != 0)
      return -14;
  } else {
    memcpy(&sll, addr, sizeof(sll));
  }

  if (sll.sll_family != AF_PACKET)
    return -22;
  if (sll.sll_ifindex != 0 && sll.sll_ifindex != 2)
    return -19; /* ENODEV */

  struct packet_sock *ps = (struct packet_sock *)sock->sk;
  spinlock_acquire(&ps->lock);
  if (sll.sll_protocol != 0)
    ps->protocol_be = sll.sll_protocol;
  ps->ifindex = sll.sll_ifindex;
  spinlock_release(&ps->lock);
  return 0;
}

static ssize_t packet_sendto(socket_t *sock, const void *buf, size_t len,
                             int flags, struct sockaddr *dest_addr,
                             int addrlen) {
  (void)flags;
  if (!sock || !sock->sk || !buf)
    return -22;

  struct packet_sock *ps = (struct packet_sock *)sock->sk;
  struct sockaddr_ll sll = {0};
  bool have_dest = false;
  if (dest_addr && addrlen >= (int)sizeof(struct sockaddr_ll)) {
    if (is_user_ptr((uint64_t)dest_addr)) {
      if (!vmm_is_user_addr_range_valid((uint64_t)dest_addr, sizeof(sll)))
        return -14;
      if (copy_from_user(&sll, dest_addr, sizeof(sll)) != 0)
        return -14;
    } else {
      memcpy(&sll, dest_addr, sizeof(sll));
    }
    if (sll.sll_family != AF_PACKET)
      return -22;
    have_dest = true;
  }

  int32_t ifindex = have_dest && sll.sll_ifindex ? sll.sll_ifindex : ps->ifindex;
  if (ifindex != 0 && ifindex != 2)
    return -19; /* ENODEV */

  struct net_device *dev = net_device_default_acquire();
  if (!dev || !dev->ops || !dev->ops->transmit) {
    if (dev)
      net_device_put(dev);
    return -100; /* ENETDOWN */
  }

  uint8_t frame[NET_FRAME_MAX];
  size_t tx_len = 0;

  if (sock->type == SOCK_RAW) {
    if (len < ETH_HDR_LEN || len > NET_FRAME_MAX) {
      net_device_put(dev);
      return -90; /* EMSGSIZE */
    }
    if (is_user_ptr((uint64_t)buf)) {
      if (!vmm_is_user_addr_range_valid((uint64_t)buf, len)) {
        net_device_put(dev);
        return -14;
      }
      if (copy_from_user(frame, buf, len) != 0) {
        net_device_put(dev);
        return -14;
      }
    } else {
      memcpy(frame, buf, len);
    }
    tx_len = len;
  } else { /* SOCK_DGRAM */
    if (!have_dest || len > NET_FRAME_MAX - ETH_HDR_LEN) {
      net_device_put(dev);
      return -22;
    }
    uint16_t proto = sll.sll_protocol ? sll.sll_protocol : ps->protocol_be;
    memcpy(frame, sll.sll_addr, 6);
    memcpy(frame + 6, dev->mac, 6);
    memcpy(frame + 12, &proto, sizeof(proto));
    if (len > 0) {
      if (is_user_ptr((uint64_t)buf)) {
        if (!vmm_is_user_addr_range_valid((uint64_t)buf, len)) {
          net_device_put(dev);
          return -14;
        }
        if (copy_from_user(frame + ETH_HDR_LEN, buf, len) != 0) {
          net_device_put(dev);
          return -14;
        }
      } else {
        memcpy(frame + ETH_HDR_LEN, buf, len);
      }
    }
    tx_len = ETH_HDR_LEN + len;
  }

  int rc = dev->ops->transmit(dev, frame, tx_len);
  net_device_put(dev);
  return rc < 0 ? rc : (ssize_t)len;
}

static ssize_t packet_send(socket_t *sock, const void *buf, size_t len,
                           int flags) {
  return packet_sendto(sock, buf, len, flags, NULL, 0);
}

static ssize_t packet_sendmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || !msg->msg_iov || msg->msg_iovlen == 0)
    return -22;
  if (msg->msg_iovlen == 1) {
    struct iovec iov;
    if (is_user_ptr((uint64_t)msg->msg_iov)) {
      if (copy_from_user(&iov, msg->msg_iov, sizeof(iov)) != 0)
        return -14;
    } else {
      iov = msg->msg_iov[0];
    }
    return packet_sendto(sock, iov.iov_base, iov.iov_len, flags,
                         (struct sockaddr *)msg->msg_name,
                         (int)msg->msg_namelen);
  }

  uint8_t linear[NET_FRAME_MAX];
  size_t total = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec iov;
    if (is_user_ptr((uint64_t)&msg->msg_iov[i])) {
      if (copy_from_user(&iov, &msg->msg_iov[i], sizeof(iov)) != 0)
        return -14;
    } else {
      iov = msg->msg_iov[i];
    }
    if (total + iov.iov_len > sizeof(linear))
      return -90; /* EMSGSIZE */
    if (iov.iov_len > 0) {
      if (is_user_ptr((uint64_t)iov.iov_base)) {
        if (!vmm_is_user_addr_range_valid((uint64_t)iov.iov_base, iov.iov_len))
          return -14;
        if (copy_from_user(linear + total, iov.iov_base, iov.iov_len) != 0)
          return -14;
      } else {
        memcpy(linear + total, iov.iov_base, iov.iov_len);
      }
      total += iov.iov_len;
    }
  }
  return packet_sendto(sock, linear, total, flags,
                       (struct sockaddr *)msg->msg_name,
                       (int)msg->msg_namelen);
}

static ssize_t packet_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, int *addrlen) {
  if (!sock || !sock->sk || (!buf && len > 0))
    return -22;

  struct packet_sock *ps = (struct packet_sock *)sock->sk;
  bool nonblock = (sock->flags & SOCK_NONBLOCK) || (flags & MSG_DONTWAIT);
  struct packet_frame frm;

  for (;;) {
    spinlock_acquire(&ps->lock);
    if (ps->count > 0) {
      frm = ps->queue[ps->head];
      if (!(flags & MSG_PEEK)) {
        ps->head = (ps->head + 1) % PACKET_QUEUE_CAP;
        ps->count--;
      }
      spinlock_release(&ps->lock);
      break;
    }
    spinlock_release(&ps->lock);

    if (sock->closing)
      return 0;
    if (nonblock)
      return -EAGAIN;
    socket_wait(sock);
  }

  size_t payload_off = (sock->type == SOCK_DGRAM) ? ETH_HDR_LEN : 0;
  size_t avail = frm.length > payload_off ? frm.length - payload_off : 0;
  size_t to_copy = avail < len ? avail : len;

  if (to_copy > 0) {
    if (is_user_ptr((uint64_t)buf)) {
      if (!vmm_is_user_addr_range_valid((uint64_t)buf, to_copy))
        return -14;
      if (copy_to_user(buf, frm.data + payload_off, to_copy) != 0)
        return -14;
    } else {
      memcpy(buf, frm.data + payload_off, to_copy);
    }
  }

  if (src_addr && addrlen) {
    int ulen = 0;
    if (is_user_ptr((uint64_t)addrlen)) {
      if (copy_from_user(&ulen, addrlen, sizeof(ulen)) != 0)
        return -14;
    } else {
      ulen = *addrlen;
    }
    struct sockaddr_ll sll = {0};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = frm.eth_type_be;
    sll.sll_ifindex = 2;
    sll.sll_hatype = 1; /* ARPHRD_ETHER */
    sll.sll_pkttype = frm.pkttype;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, frm.src_mac, 6);

    int copy_sz = ulen < (int)sizeof(sll) ? ulen : (int)sizeof(sll);
    if (copy_sz > 0) {
      if (is_user_ptr((uint64_t)src_addr)) {
        if (copy_to_user(src_addr, &sll, (size_t)copy_sz) != 0)
          return -14;
      } else {
        memcpy(src_addr, &sll, (size_t)copy_sz);
      }
    }
    int actual_len = (int)sizeof(sll);
    if (is_user_ptr((uint64_t)addrlen)) {
      if (copy_to_user(addrlen, &actual_len, sizeof(actual_len)) != 0)
        return -14;
    } else {
      *addrlen = actual_len;
    }
  }

  return (flags & MSG_TRUNC) ? (ssize_t)avail : (ssize_t)to_copy;
}

static ssize_t packet_recv(socket_t *sock, void *buf, size_t len, int flags) {
  return packet_recvfrom(sock, buf, len, flags, NULL, NULL);
}

static ssize_t packet_recvmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || !msg->msg_iov || msg->msg_iovlen == 0)
    return -22;
  struct iovec iov;
  if (is_user_ptr((uint64_t)msg->msg_iov)) {
    if (copy_from_user(&iov, msg->msg_iov, sizeof(iov)) != 0)
      return -14;
  } else {
    iov = msg->msg_iov[0];
  }

  int namelen = (int)msg->msg_namelen;
  ssize_t r = packet_recvfrom(sock, iov.iov_base, iov.iov_len, flags,
                              (struct sockaddr *)msg->msg_name,
                              msg->msg_name ? &namelen : NULL);
  if (r >= 0) {
    msg->msg_namelen = (uint32_t)namelen;
    msg->msg_controllen = 0;
    msg->msg_flags = 0;
  }
  return r;
}

static int packet_poll(socket_t *sock, int events) {
  if (!sock || !sock->sk)
    return POLLNVAL;
  struct packet_sock *ps = (struct packet_sock *)sock->sk;
  int revents = 0;

  if (events & POLLIN) {
    spinlock_acquire(&ps->lock);
    if (ps->count > 0)
      revents |= POLLIN;
    spinlock_release(&ps->lock);
  }
  if (events & POLLOUT) {
    struct net_device *dev = net_device_default_acquire();
    if (dev) {
      revents |= POLLOUT;
      net_device_put(dev);
    }
  }
  return revents;
}

static int packet_getsockname(socket_t *sock, struct sockaddr *addr,
                              int *addrlen) {
  if (!sock || !sock->sk || !addr || !addrlen)
    return -22;
  struct packet_sock *ps = (struct packet_sock *)sock->sk;
  struct sockaddr_ll sll = {0};
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = ps->protocol_be;
  sll.sll_ifindex = ps->ifindex ? ps->ifindex : 2;
  sll.sll_hatype = 1;
  sll.sll_halen = 6;
  struct net_device *dev = net_device_default_acquire();
  if (dev) {
    memcpy(sll.sll_addr, dev->mac, 6);
    net_device_put(dev);
  }

  int ulen = *addrlen;
  int copy_sz = ulen < (int)sizeof(sll) ? ulen : (int)sizeof(sll);
  if (copy_sz > 0)
    memcpy(addr, &sll, (size_t)copy_sz);
  *addrlen = (int)sizeof(sll);
  return 0;
}

static int packet_setsockopt(socket_t *sock, int level, int optname,
                             const void *optval, int optlen) {
  (void)sock;
  (void)level;
  (void)optname;
  (void)optval;
  (void)optlen;
  return 0;
}

static int packet_getsockopt(socket_t *sock, int level, int optname,
                             void *optval, int *optlen) {
  if (!sock || !optval || !optlen || *optlen < (int)sizeof(int))
    return -22;
  int val = 0;
  if (level == SOL_SOCKET) {
    if (optname == SO_TYPE)
      val = sock->type;
    else if (optname == SO_DOMAIN)
      val = AF_PACKET;
    else if (optname == SO_PROTOCOL)
      val = sock->protocol;
    else if (optname == SO_ERROR)
      val = 0;
  }
  memcpy(optval, &val, sizeof(val));
  *optlen = sizeof(val);
  return 0;
}

static void packet_destroy(socket_t *sock) {
  if (!sock || !sock->sk)
    return;
  struct packet_sock *ps = (struct packet_sock *)sock->sk;

  bool empty = false;
  spinlock_acquire(&packet_list_lock);
  struct packet_sock **pp = &packet_list_head;
  while (*pp) {
    if (*pp == ps) {
      *pp = ps->next;
      break;
    }
    pp = &(*pp)->next;
  }
  empty = (packet_list_head == NULL);
  spinlock_release(&packet_list_lock);

  if (empty)
    net_set_rx_tap(NULL, NULL);

  kfree(ps);
  sock->sk = NULL;
}

static sock_ops_t packet_ops = {
    .bind = packet_bind,
    .send = packet_send,
    .recv = packet_recv,
    .sendto = packet_sendto,
    .recvfrom = packet_recvfrom,
    .sendmsg = packet_sendmsg,
    .recvmsg = packet_recvmsg,
    .poll = packet_poll,
    .getsockname = packet_getsockname,
    .setsockopt = packet_setsockopt,
    .getsockopt = packet_getsockopt,
    .destroy = packet_destroy,
};

static int packet_create(socket_t *sock, int protocol) {
  if (!sock)
    return -22;
  if (sock->type != SOCK_RAW && sock->type != SOCK_DGRAM)
    return -EPROTONOSUPPORT;

  struct packet_sock *ps = kmalloc(sizeof(struct packet_sock));
  if (!ps)
    return -12; /* ENOMEM */
  memset(ps, 0, sizeof(*ps));
  spinlock_init(&ps->lock);
  ps->sock = sock;
  ps->protocol_be = (uint16_t)protocol;
  ps->ifindex = 0;

  sock->ops = &packet_ops;
  sock->sk = ps;

  bool was_empty = false;
  spinlock_acquire(&packet_list_lock);
  was_empty = (packet_list_head == NULL);
  ps->next = packet_list_head;
  packet_list_head = ps;
  spinlock_release(&packet_list_lock);

  if (was_empty)
    net_set_rx_tap(packet_rx_tap, NULL);

  return 0;
}

static net_family_t packet_family = {
    .family = AF_PACKET,
    .create = packet_create,
};

void af_packet_init(void) {
  sock_register_family(&packet_family);
  klog_puts("[OK] AF_PACKET socket family registered\n");
}
