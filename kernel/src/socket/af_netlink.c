#include "../sched/net_namespace.h"
#include "../sched/capability.h"
#include "af_netlink.h"
#include "../console/klog.h"
#include "../fs/sysfs.h"
#include "../lib/string.h"
#include "../net/core.h"
#include "../net/ipv4.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include "../include/arch/uaccess.h"

#include "../net/ipv6.h"

static struct list_head netlink_sockets;
static spinlock_t netlink_lock;
static uint64_t uevent_seqnum;
uint64_t netlink_uevent_next_seqnum(void) {
  return __atomic_add_fetch(&uevent_seqnum, 1, __ATOMIC_RELAXED);
}

struct nlmsghdr_min {
  uint32_t nlmsg_len;
  uint16_t nlmsg_type;
  uint16_t nlmsg_flags;
  uint32_t nlmsg_seq;
  uint32_t nlmsg_pid;
};

#define NLMSG_DONE 3
#define NLM_F_MULTI 2

#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define IFLA_ADDRESS 1
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_OPERSTATE 16
#define IFLA_STATS64 23
#define IFLA_CARRIER 33
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4
#define IFA_FLAGS 8
#define RTA_DST 1
#define RTA_SRC 2
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PRIORITY 6
#define RTA_PREFSRC 7
#define RTA_TABLE 15
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define IFF_LOWER_UP 0x10000
#define ARPHRD_ETHER 1
#define ARPHRD_LOOPBACK 772
#define NL_ALIGN(n) (((n) + 3U) & ~3U)

struct ifinfomsg_min {
  uint8_t family, pad; uint16_t type; int32_t index;
  uint32_t flags, change;
};
struct ifaddrmsg_min {
  uint8_t family, prefixlen, flags, scope; uint32_t index;
};
struct rtmsg_min {
  uint8_t rtm_family, rtm_dst_len, rtm_src_len, rtm_tos;
  uint8_t rtm_table, rtm_protocol, rtm_scope, rtm_type;
  uint32_t rtm_flags;
};
struct rtattr_min { uint16_t len, type; };

/* Linux rtnl_link_stats64 order, including fields not tracked by our driver. */
struct rtnl_link_stats64_min {
  uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
  uint64_t rx_errors, tx_errors, rx_dropped, tx_dropped;
  uint64_t multicast, collisions;
  uint64_t rx_length_errors, rx_over_errors, rx_crc_errors, rx_frame_errors;
  uint64_t rx_fifo_errors, rx_missed_errors;
  uint64_t tx_aborted_errors, tx_carrier_errors, tx_fifo_errors;
  uint64_t tx_heartbeat_errors, tx_window_errors;
  uint64_t rx_compressed, tx_compressed, rx_nohandler, rx_otherhost_dropped;
};

static size_t route_attr(uint8_t *msg, size_t pos, size_t cap, uint16_t type,
                         const void *data, size_t len) {
  size_t total = sizeof(struct rtattr_min) + len, aligned = NL_ALIGN(total);
  if (pos + aligned > cap) return pos;
  struct rtattr_min *a = (struct rtattr_min *)(msg + pos);
  a->len = (uint16_t)total; a->type = type;
  memcpy(a + 1, data, len);
  if (aligned > total) memset(msg + pos + total, 0, aligned - total);
  return pos + aligned;
}

static int route_queue(netlink_sock_t *nsk, const void *data, size_t len) {
  sk_buff_t *skb = alloc_skb(len);
  if (!skb) return -12;
  memcpy(skb->data, data, len); skb->len = len;
  skb_queue_tail(&nsk->recv_queue, skb);
  return 0;
}

static uint8_t ipv4_prefix(uint32_t mask) {
  uint8_t bits = 0;
  while (mask & 0x80000000U) { bits++; mask <<= 1; }
  return bits;
}

static void ipv4_bytes(uint8_t out[4], uint32_t ip) {
  out[0] = ip >> 24; out[1] = ip >> 16; out[2] = ip >> 8; out[3] = ip;
}

static size_t route_build_link(uint8_t msg[512], uint32_t seq, uint32_t pid,
                               uint16_t flags, const struct net_device *dev) {
  memset(msg, 0, 512);
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifinfomsg_min *i = (struct ifinfomsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWLINK; h->nlmsg_flags = flags; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->type = ARPHRD_ETHER;
  uint8_t carrier = dev->ops && dev->ops->link_up && dev->ops->link_up((struct net_device *)dev);
  i->index = 2; i->flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
  if (carrier)
    i->flags |= IFF_RUNNING | IFF_LOWER_UP;
  i->change = 0xffffffffU;
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, 512, IFLA_IFNAME, dev->name, strlen(dev->name) + 1);
  uint32_t mtu = dev->mtu; /* IFLA_MTU is u32 even though the NIC stores u16. */
  pos = route_attr(msg, pos, 512, IFLA_MTU, &mtu, sizeof(mtu));
  pos = route_attr(msg, pos, 512, IFLA_ADDRESS, dev->mac, 6);
  pos = route_attr(msg, pos, 512, IFLA_CARRIER, &carrier, sizeof(carrier));
  uint8_t operstate = carrier ? 6 /* IF_OPER_UP */ : 2 /* IF_OPER_DOWN */;
  pos = route_attr(msg, pos, 512, IFLA_OPERSTATE, &operstate, sizeof(operstate));
  struct rtnl_link_stats64_min stats = {0};
  /* Take fresh counters for every request. NetworkManager polls these to
   * publish Device.Statistics; sysfs's boot-time snapshot cannot serve it. */
#define COPY_STAT(field) stats.field = __atomic_load_n(&dev->stats.field, __ATOMIC_RELAXED)
  COPY_STAT(rx_packets); COPY_STAT(tx_packets);
  COPY_STAT(rx_bytes); COPY_STAT(tx_bytes);
  COPY_STAT(rx_errors); COPY_STAT(tx_errors);
  COPY_STAT(rx_dropped); COPY_STAT(tx_dropped);
#undef COPY_STAT
  pos = route_attr(msg, pos, 512, IFLA_STATS64, &stats, sizeof(stats));
  h->nlmsg_len = pos; return pos;
}

static int route_link(netlink_sock_t *nsk, uint32_t seq, uint32_t pid,
                      const struct net_device *dev) {
  uint8_t msg[512];
  size_t len = route_build_link(msg, seq, pid, NLM_F_MULTI, dev);
  return route_queue(nsk, msg, len);
}

void netlink_notify_link(struct net_device *dev) {
  /* Routing/netlink currently exports only the one configured default NIC. */
  if (!dev || dev != net_device_default() || !dev->registered)
    return;
  uint8_t msg[512];
  size_t len = route_build_link(msg, 0, 0, 0, dev);
  netlink_broadcast(NETLINK_ROUTE, 1 /* RTMGRP_LINK */, msg, len);
}

static int route_address(netlink_sock_t *nsk, uint32_t seq, uint32_t pid, const char *name,
                         uint32_t address, uint32_t mask) {
  uint8_t msg[128], ip[4], broadcast[4]; memset(msg, 0, sizeof(msg));
  ipv4_bytes(ip, address); ipv4_bytes(broadcast, address | ~mask);
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifaddrmsg_min *i = (struct ifaddrmsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWADDR; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->family = 2; i->prefixlen = ipv4_prefix(mask); i->index = 2;
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, sizeof(msg), IFA_ADDRESS, ip, 4);
  pos = route_attr(msg, pos, sizeof(msg), IFA_LOCAL, ip, 4);
  pos = route_attr(msg, pos, sizeof(msg), IFA_LABEL, name, strlen(name) + 1);
  pos = route_attr(msg, pos, sizeof(msg), IFA_BROADCAST, broadcast, 4);
  uint32_t address_flags = i->flags;
  pos = route_attr(msg, pos, sizeof(msg), IFA_FLAGS, &address_flags, sizeof(address_flags));
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

static int route_loopback_link(netlink_sock_t *nsk, uint32_t seq, uint32_t pid) {
  uint8_t msg[128]; memset(msg, 0, sizeof(msg));
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifinfomsg_min *i = (struct ifinfomsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWLINK; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->type = ARPHRD_LOOPBACK;
  i->index = 1; i->flags = nsk->parent->net_ns ? __atomic_load_n(&nsk->parent->net_ns->loopback_flags, __ATOMIC_ACQUIRE) : IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
  i->change = 0xffffffffU;
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, sizeof(msg), IFLA_IFNAME, "lo", 3);
  uint32_t mtu = 65536;
  pos = route_attr(msg, pos, sizeof(msg), IFLA_MTU, &mtu, sizeof(mtu));
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

static int route_loopback_addr(netlink_sock_t *nsk, uint32_t seq, uint32_t pid) {
  uint8_t msg[128], ip[4] = {127, 0, 0, 1}; memset(msg, 0, sizeof(msg));
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifaddrmsg_min *i = (struct ifaddrmsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWADDR; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->family = 2; i->prefixlen = 8; i->index = 1; i->scope = 254; // RT_SCOPE_HOST
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, sizeof(msg), IFA_ADDRESS, ip, 4);
  pos = route_attr(msg, pos, sizeof(msg), IFA_LOCAL, ip, 4);
  pos = route_attr(msg, pos, sizeof(msg), IFA_LABEL, "lo", 3);
  uint32_t address_flags = i->flags;
  pos = route_attr(msg, pos, sizeof(msg), IFA_FLAGS, &address_flags, sizeof(address_flags));
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

static int route_address6(netlink_sock_t *nsk, uint32_t seq, uint32_t pid, const uint8_t ip[16]) {
  uint8_t msg[128]; memset(msg, 0, sizeof(msg));
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifaddrmsg_min *i = (struct ifaddrmsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWADDR; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->family = 10; // AF_INET6
  i->prefixlen = 64; i->index = 2; i->scope = 253; // RT_SCOPE_LINK
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, sizeof(msg), IFA_ADDRESS, ip, 16);
  pos = route_attr(msg, pos, sizeof(msg), IFA_LOCAL, ip, 16);
  uint32_t address_flags = i->flags;
  pos = route_attr(msg, pos, sizeof(msg), IFA_FLAGS, &address_flags, sizeof(address_flags));
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

static int route_default(netlink_sock_t *nsk, uint32_t seq, uint32_t pid, uint32_t gateway, uint32_t prefsrc) {
  uint8_t msg[128], gw[4], src[4]; memset(msg, 0, sizeof(msg));
  ipv4_bytes(gw, gateway); ipv4_bytes(src, prefsrc);
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct rtmsg_min *r = (struct rtmsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWROUTE; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  r->rtm_family = 2; // AF_INET
  r->rtm_dst_len = 0; // Default route 0.0.0.0/0
  r->rtm_table = 254; // RT_TABLE_MAIN
  r->rtm_protocol = 3; // RTPROT_BOOT
  r->rtm_scope = 0; // RT_SCOPE_UNIVERSE
  r->rtm_type = 1; // RTN_UNICAST
  uint32_t oif = 2;
  uint32_t table = 254;
  size_t pos = sizeof(*h) + sizeof(*r);
  pos = route_attr(msg, pos, sizeof(msg), RTA_GATEWAY, gw, 4);
  pos = route_attr(msg, pos, sizeof(msg), RTA_OIF, &oif, sizeof(oif));
  pos = route_attr(msg, pos, sizeof(msg), RTA_PREFSRC, src, 4);
  pos = route_attr(msg, pos, sizeof(msg), RTA_TABLE, &table, sizeof(table));
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

// Append a null-terminated field to uevent buffer, return new position
static size_t ue_append(char *buf, size_t pos, const char *str) {
  size_t len = strlen(str);
  memcpy(buf + pos, str, len + 1); // copy including null
  return pos + len + 1;
}


void netlink_broadcast(int protocol, uint32_t group, const void *data,
                       size_t len) {
  spinlock_acquire(&netlink_lock);
  netlink_sock_t *nsk;
  list_for_each_entry(nsk, &netlink_sockets, list) {
    if (!nsk->parent->net_ns && nsk->protocol == protocol && (nsk->groups & group)) {
      sk_buff_t *skb = alloc_skb(len);
      if (skb) {
        memcpy(skb->data, data, len);
        skb->len = len;
        skb->nl_groups = group;
        skb_queue_tail(&nsk->recv_queue, skb);
        socket_wake(nsk->parent);
        if (nsk->parent->node)
          epoll_notify_event(nsk->parent->node, EPOLLIN);
      }
    }
  }
  spinlock_release(&netlink_lock);
}

void netlink_broadcast_drm_hotplug(const char *devpath, uint32_t connector) {
  if (!devpath || strlen(devpath) > 160) return;
  char event[512], field[192];
  size_t pos = 0;
  snprintf(field, sizeof(field), "change@%s", devpath);
  pos = ue_append(event, pos, field);
  pos = ue_append(event, pos, "ACTION=change");
  snprintf(field, sizeof(field), "DEVPATH=%s", devpath);
  pos = ue_append(event, pos, field);
  pos = ue_append(event, pos, "SUBSYSTEM=drm");
  pos = ue_append(event, pos, "DEVTYPE=drm_connector");
  pos = ue_append(event, pos, "HOTPLUG=1");
  snprintf(field, sizeof(field), "CONNECTOR=HDMI-A-%u", connector);
  pos = ue_append(event, pos, field);
  snprintf(field, sizeof(field), "SEQNUM=%llu",
           (unsigned long long)netlink_uevent_next_seqnum());
  pos = ue_append(event, pos, field);
  netlink_broadcast(NETLINK_KOBJECT_UEVENT, 1, event, pos);
}

// AF_NETLINK Operations

static int netlink_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (addrlen < (int)sizeof(struct sockaddr_nl))
    return -22; // EINVAL
  struct sockaddr_nl *nl = (struct sockaddr_nl *)addr;
  if (nl->nl_family != AF_NETLINK)
    return -97; // EAFNOSUPPORT

  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
  static uint32_t next_auto_portid = UINT32_MAX;
  struct thread *t = sched_get_current();
  uint32_t candidate = nl->nl_pid ? nl->nl_pid :
      nsk->portid ? (uint32_t)nsk->portid : t ? (t->tgid ? t->tgid : t->tid) : 1;
  spinlock_acquire(&netlink_lock);
  for (;;) {
    bool collision = false;
    netlink_sock_t *other;
    list_for_each_entry(other, &netlink_sockets, list) {
      if (other != nsk && other->parent->net_ns == sock->net_ns && other->protocol == nsk->protocol &&
          other->portid == candidate) { collision = true; break; }
    }
    if (!collision) break;
    if (nl->nl_pid) { spinlock_release(&netlink_lock); return -98; }
    candidate = next_auto_portid--;
    if (!candidate) { spinlock_release(&netlink_lock); return -98; }
  }
  nsk->portid = candidate;
  nsk->groups = nl->nl_groups;
  spinlock_release(&netlink_lock);

  klogf("[NETLINK] bind port=%llu groups=%u\n",
        (unsigned long long)nsk->portid, nsk->groups);


  return 0;
}

static int netlink_getsockname(socket_t *sock, struct sockaddr *addr,
                               int *addrlen) {
  if (*addrlen < (int)sizeof(struct sockaddr_nl))
    return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

  struct sockaddr_nl nl;
  memset(&nl, 0, sizeof(nl));
  nl.nl_family = AF_NETLINK;
  nl.nl_pid = (uint32_t)nsk->portid;
  nl.nl_groups = nsk->groups;

  memcpy(addr, &nl, sizeof(nl));
  *addrlen = sizeof(nl);
  return 0;
}

static int netlink_setsockopt(socket_t *sock, int level, int optname,
                              const void *optval, int optlen) {
  if (!sock || !sock->sk)
    return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;


  if (level == SOL_NETLINK) {
    switch (optname) {
    case NETLINK_PKTINFO: {
      int enabled;
      if (optlen < (int)sizeof(enabled)) return -22;
      if (copy_from_user(&enabled, optval, sizeof(enabled))) return -14;
      nsk->pktinfo = enabled != 0;
      return 0;
    }
    case NETLINK_EXT_ACK: {
      int enabled;
      if (optlen < (int)sizeof(enabled)) return -22;
      if (copy_from_user(&enabled, optval, sizeof(enabled))) return -14;
      nsk->ext_ack = enabled != 0;
      return 0;
    }
    case NETLINK_NO_ENOBUFS: {
      int enabled;
      if (optlen < (int)sizeof(enabled)) return -22;
      if (copy_from_user(&enabled, optval, sizeof(enabled))) return -14;
      nsk->no_enobufs = enabled != 0;
      return 0;
    }
    case NETLINK_ADD_MEMBERSHIP: {
      if (optlen < (int)sizeof(int))
        return -22;
      int group = *(const int *)optval;

      if (group < 1 || group > 32) return -22;
      uint32_t group_mask = (1u << (group - 1));
      nsk->groups |= group_mask;

      return 0;
    }
    case NETLINK_DROP_MEMBERSHIP: {
      if (optlen < (int)sizeof(int))
        return -22;
      int group = *(const int *)optval;
      if (group < 1 || group > 32) return -22;
      uint32_t group_mask = (1u << (group - 1));
      nsk->groups &= ~group_mask;
      return 0;
    }
    }
  } else if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_KEEPALIVE: {
      int enabled;
      if (optlen < (int)sizeof(enabled)) return -22;
      if (copy_from_user(&enabled, optval, sizeof(enabled))) return -14;
      nsk->keepalive = enabled != 0;
      return 0;
    }
    case SO_PASSCRED:

      if (optlen >= (int)sizeof(int))
        nsk->passcred = (*(const int *)optval != 0);
      else
        nsk->passcred = true;
      return 0;
    case 26: // SO_ATTACH_FILTER

      return 0; // Stub success
    case 2:     // SO_REUSEADDR
      return 0;
    case 7:     // SO_SNDBUF
    case 8:     // SO_RCVBUF
    case 32:    // SO_SNDBUFFORCE
    case 33:    // SO_RCVBUFFORCE
      return 0;
    }
  }

  return -92; // ENOPROTOOPT
}

static int netlink_getsockopt(socket_t *sock, int level, int optname,
                              void *optval, int *optlen) {
  if (!sock || !sock->sk) return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
  int value;
  if (level == SOL_NETLINK && optname == NETLINK_LIST_MEMBERSHIPS) {
    int length;
    if (copy_from_user(&length, optlen, sizeof(length))) return -14;
    if (length < 0) return -22;
    if (length >= (int)sizeof(nsk->groups) &&
        copy_to_user(optval, &nsk->groups, sizeof(nsk->groups))) return -14;
    length = sizeof(nsk->groups);
    return copy_to_user(optlen, &length, sizeof(length)) ? -14 : 0;
  }
  if (level == SOL_NETLINK && optname == NETLINK_EXT_ACK) {
    value = nsk->ext_ack;
  } else if (level == SOL_NETLINK && optname == NETLINK_PKTINFO) {
    value = nsk->pktinfo;
  } else if (level == SOL_NETLINK && optname == NETLINK_NO_ENOBUFS) {
    value = nsk->no_enobufs;
  } else if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_KEEPALIVE: value = nsk->keepalive; break;
    case SO_PASSCRED: value = nsk->passcred; break;
    default: return -92;
    }
  } else {
    return -92;
  }
  int len;
  if (copy_from_user(&len, optlen, sizeof(len))) return -14;
  if (len < 0) return -22;
  if (len > (int)sizeof(value)) len = sizeof(value);
  if (len && copy_to_user(optval, &value, len)) return -14;
  if (copy_to_user(optlen, &len, sizeof(len))) return -14;
  return 0;
}

struct netlink_sender { uint32_t pid, uid, gid, groups; };
static ssize_t netlink_recvfrom_full(socket_t *sock, void *buf, size_t len,
                                int flags, struct sockaddr *src_addr,
                                int *addrlen, struct netlink_sender *sender) {
  if (!sock || !sock->sk)
    return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

  spinlock_acquire(&nsk->recv_queue.lock);
  while (nsk->recv_queue.head == NULL) {
    if (sock->flags & SOCK_NONBLOCK || (flags & MSG_DONTWAIT)) {
      spinlock_release(&nsk->recv_queue.lock);
      return -11; // EAGAIN
    }
    /* Race-free wait: BLOCKED+add while holding recv_queue.lock, recheck
     * before parking. socket_wait() alone would lose a wake between the
     * empty check and the add. */
    struct thread *cur = sched_get_current();
    wait_queue_entry_t nle = {.thread = cur, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
    if (cur)
      cur->state = THREAD_BLOCKED;
    if (sock->wait_queue)
      wait_queue_add((wait_queue_t *)sock->wait_queue, &nle);
    if (nsk->recv_queue.head != NULL) {
      if (cur)
        cur->state = THREAD_RUNNING;
      if (sock->wait_queue)
        wait_queue_remove((wait_queue_t *)sock->wait_queue, &nle);
      if (cur)
        cur->state = THREAD_RUNNING;
      continue;
    }
    spinlock_release(&nsk->recv_queue.lock);
    sched_yield();
    if (sock->wait_queue)
      wait_queue_remove((wait_queue_t *)sock->wait_queue, &nle);
    if (cur)
      cur->state = THREAD_RUNNING;
    if (cur && thread_has_pending_signal(cur))
      return -4; // EINTR
    spinlock_acquire(&nsk->recv_queue.lock);
  }

  // Manual dequeue while holding lock
  sk_buff_t *skb = nsk->recv_queue.head;
  if (skb && !(flags & MSG_PEEK)) {
    nsk->recv_queue.head = skb->next;
    if (!nsk->recv_queue.head)
      nsk->recv_queue.tail = NULL;
    skb->next = NULL;
    nsk->recv_queue.len--;
  }
  spinlock_release(&nsk->recv_queue.lock);

  if (!skb)
    return -11; // EAGAIN

  size_t to_copy = (len < skb->len) ? len : skb->len;
  if (to_copy)
    memcpy(buf, skb->data, to_copy);

  if (src_addr && addrlen) {
    struct sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = AF_NETLINK;
    nl.nl_pid = skb->nl_portid;
    nl.nl_groups = skb->nl_groups;

    int copy_len = (*addrlen < (int)sizeof(nl)) ? *addrlen : (int)sizeof(nl);
    memcpy(src_addr, &nl, copy_len);
    *addrlen = sizeof(nl);
  }

  if (sender) {
    sender->pid = skb->nl_pid; sender->uid = skb->nl_uid; sender->gid = skb->nl_gid;
    sender->groups = skb->nl_groups;
  }
  ssize_t ret = (flags & MSG_TRUNC) ? (ssize_t)skb->len : (ssize_t)to_copy;
  if (!(flags & MSG_PEEK)) {
    free_skb(skb);
  }
  return ret;
}

static ssize_t netlink_recvfrom(socket_t *sock, void *buf, size_t len,
                                int flags, struct sockaddr *src_addr, int *addrlen) {
  return netlink_recvfrom_full(sock, buf, len, flags, src_addr, addrlen, NULL);
}

static ssize_t netlink_recv(socket_t *sock, void *buf, size_t len, int flags) {
  return netlink_recvfrom(sock, buf, len, flags, NULL, NULL);
}

static ssize_t netlink_sendto(socket_t *sock, const void *buf, size_t len,
                              int flags, struct sockaddr *dest_addr,
                              int addrlen) {
  (void)buf;
  (void)flags;
  (void)dest_addr;
  (void)addrlen;
  klogf("[NETLINK] sendto len=%llu\n", (unsigned long long)len);

  netlink_sock_t *nsk = sock ? (netlink_sock_t *)sock->sk : NULL;
  if (!nsk)
    return -22; // EINVAL

  if (nsk->protocol == NETLINK_KOBJECT_UEVENT) {
    if (!dest_addr || addrlen < (int)sizeof(struct sockaddr_nl)) return -89;
    struct sockaddr_nl dest;
    memcpy(&dest, dest_addr, sizeof(dest));
    if (dest.nl_family != AF_NETLINK) return -22;
    struct thread *current = sched_get_current();
    // Group 1 is exclusively kernel events. Only root may publish processed
    // udev events to group 2; listeners receive the actual sender credentials.
    if ((dest.nl_groups & 1) || (dest.nl_groups && (!current || !capability_has(current, CAP_NET_ADMIN))))
      return -1;
    if (!len || len > 1024 * 1024) return -90;
    void *payload = kmalloc(len);
    if (!payload) return -12;
    if (is_user_ptr((uint64_t)buf)) {
      if (copy_from_user(payload, buf, len)) { kfree(payload); return -14; }
    } else memcpy(payload, buf, len);
    spinlock_acquire(&netlink_lock);
    netlink_sock_t *receiver;
    list_for_each_entry(receiver, &netlink_sockets, list) {
      if (receiver->parent->net_ns != sock->net_ns || receiver->protocol != nsk->protocol || receiver == nsk) continue;
      if (dest.nl_groups ? !(receiver->groups & dest.nl_groups) :
                           receiver->portid != dest.nl_pid) continue;
      sk_buff_t *skb = alloc_skb(len);
      if (!skb) continue;
      memcpy(skb->data, payload, len); skb->len = len;
      skb->nl_portid = nsk->portid; skb->nl_groups = dest.nl_groups;
      skb->nl_pid = current ? current->tgid : 0;
      skb->nl_uid = current ? current->euid : 0;
      skb->nl_gid = current ? current->egid : 0;
      skb_queue_tail(&receiver->recv_queue, skb);
      socket_wake(receiver->parent);
      if (receiver->parent->node) epoll_notify_event(receiver->parent->node, EPOLLIN);
    }
    spinlock_release(&netlink_lock);
    kfree(payload);
    return len;
  }

  /* Complete route dump requests with an empty multipart result.  This is a
   * valid netlink transaction and lets getifaddrs(3) return an empty list
   * until RTM_NEWLINK/RTM_NEWADDR records are implemented. */
  if (nsk->protocol == NETLINK_ROUTE) {
    if (!buf || len < sizeof(struct nlmsghdr_min))
      return -22; // EINVAL

    struct nlmsghdr_min request;
    if (is_user_ptr((uint64_t)buf)) {
      if (copy_from_user(&request, buf, sizeof(request)) != 0)
        return -14; // EFAULT
    } else {
      memcpy(&request, buf, sizeof(request));
    }

    uint32_t reply_pid = nsk->portid ? (uint32_t)nsk->portid : request.nlmsg_pid;
    struct net_device *dev = sock->net_ns ? NULL : net_device_default();
    const struct ipv4_config *cfg = sock->net_ns ? NULL : ipv4_get_config();
    const struct ipv6_config *cfg6 = sock->net_ns ? NULL : ipv6_get_config();
    if (sock->net_ns && (request.nlmsg_type == RTM_NEWLINK || request.nlmsg_type == RTM_NEWADDR)) {
      struct { struct nlmsghdr_min hdr; int32_t error; struct nlmsghdr_min request; } ack = {0};
      ack.hdr.nlmsg_len = sizeof(ack); ack.hdr.nlmsg_type = 2;
      ack.hdr.nlmsg_seq = request.nlmsg_seq; ack.hdr.nlmsg_pid = reply_pid; ack.request = request;
      ack.error = -22;
      uint8_t payload[256];
      if (!capability_has(sched_get_current(), CAP_NET_ADMIN)) ack.error = -1;
      else if (request.nlmsg_len <= len && request.nlmsg_len <= sizeof(payload) && request.nlmsg_len >= sizeof(request)) {
        int fault = 0;
        if (is_user_ptr((uint64_t)buf)) fault = copy_from_user(payload, buf, request.nlmsg_len);
        else memcpy(payload, buf, request.nlmsg_len);
        if (fault) ack.error = -14;
        else if (request.nlmsg_type == RTM_NEWLINK && request.nlmsg_len >= sizeof(request) + sizeof(struct ifinfomsg_min)) {
          struct ifinfomsg_min info; memcpy(&info, payload + sizeof(request), sizeof(info));
          if (info.index != 1) ack.error = -19;
          else if (info.change & ~IFF_UP) ack.error = -95;
          else {
            uint32_t flags = __atomic_load_n(&sock->net_ns->loopback_flags, __ATOMIC_ACQUIRE);
            flags = (flags & ~(info.change & IFF_UP)) | (info.flags & info.change & IFF_UP) | IFF_LOOPBACK;
            if (flags & IFF_UP) flags |= IFF_RUNNING;
            else flags &= ~IFF_RUNNING;
            __atomic_store_n(&sock->net_ns->loopback_flags, flags, __ATOMIC_RELEASE); ack.error = 0;
          }
        } else if (request.nlmsg_type == RTM_NEWADDR && request.nlmsg_len >= sizeof(request) + sizeof(struct ifaddrmsg_min)) {
          struct ifaddrmsg_min info; memcpy(&info, payload + sizeof(request), sizeof(info));
          if (info.index != 1) ack.error = -19;
          else if (info.family == 2 && info.prefixlen == 8) {
            uint32_t address = 0; bool invalid = false;
            for (size_t off = sizeof(request) + sizeof(info); off + 4 <= request.nlmsg_len;) {
              struct rtattr_min attr; memcpy(&attr, payload + off, sizeof(attr));
              if (attr.len < 4 || off + attr.len > request.nlmsg_len) { invalid = true; break; }
              if (attr.type == IFA_LOCAL || attr.type == IFA_ADDRESS) {
                if (attr.len != 8 || memcmp(payload + off + 4, "\177\0\0\1", 4)) invalid = true;
                else address = 0x7f000001;
              }
              off += NL_ALIGN(attr.len);
            }
            if (!invalid && address) {
              if ((request.nlmsg_flags & 0x200) && sock->net_ns->loopback_address) ack.error = -17;
              else { __atomic_store_n(&sock->net_ns->loopback_address, address, __ATOMIC_RELEASE); ack.error = 0; }
            }
          }
        }
      }
      if (route_queue(nsk, &ack, sizeof(ack))) return -12;
      socket_wake(sock); if (sock->node) epoll_notify_event(sock->node, EPOLLIN);
      return len;
    }
    int route_ret = 0;
    if (request.nlmsg_type == RTM_GETLINK) {
      route_ret = route_loopback_link(nsk, request.nlmsg_seq, reply_pid);
      if (route_ret == 0 && dev)
        route_ret = route_link(nsk, request.nlmsg_seq, reply_pid, dev);
    } else if (request.nlmsg_type == RTM_GETADDR) {
      if (!sock->net_ns || sock->net_ns->loopback_address)
        route_ret = route_loopback_addr(nsk, request.nlmsg_seq, reply_pid);
      if (route_ret == 0 && dev && cfg && cfg->address)
        route_ret = route_address(nsk, request.nlmsg_seq, reply_pid, dev->name,
                                  cfg->address, cfg->netmask);
      if (route_ret == 0 && cfg6 && cfg6->link_local[0] != 0)
        route_ret = route_address6(nsk, request.nlmsg_seq, reply_pid, cfg6->link_local);
    } else if (request.nlmsg_type == RTM_GETROUTE) {
      if (cfg && cfg->gateway)
        route_ret = route_default(nsk, request.nlmsg_seq, reply_pid, cfg->gateway, cfg->address);
    }
    if (route_ret != 0)
      return route_ret;

    struct nlmsghdr_min done;
    memset(&done, 0, sizeof(done));
    done.nlmsg_len = sizeof(done);
    done.nlmsg_type = NLMSG_DONE;
    done.nlmsg_flags = NLM_F_MULTI;
    done.nlmsg_seq = request.nlmsg_seq;
    done.nlmsg_pid = reply_pid;

    if (route_queue(nsk, &done, sizeof(done)) != 0)
      return -12; // ENOMEM
    socket_wake(sock);
    if (sock->node)
      epoll_notify_event(sock->node, EPOLLIN);
  }

  return (ssize_t)len;
}

static ssize_t netlink_send(socket_t *sock, const void *buf, size_t len,
                            int flags) {
  return netlink_sendto(sock, buf, len, flags, NULL, 0);
}

struct ucred_nl {
  int pid;
  int uid;
  int gid;
};

static ssize_t netlink_recvmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || msg->msg_iovlen == 0)
    return -22;

  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

  // For now, only support single iovec
  struct iovec *iov = &msg->msg_iov[0];
  struct netlink_sender sender = {0};
  ssize_t ret = netlink_recvfrom_full(sock, iov->iov_base, iov->iov_len, flags,
                                 (struct sockaddr *)msg->msg_name,
                                 (int *)&msg->msg_namelen, &sender);
  size_t capacity = msg->msg_controllen, used = 0;
  msg->msg_flags = 0;

  // Populate SCM_CREDENTIALS ancillary data when SO_PASSCRED is set.
  // Kernel events have pid 0. Processed udev events carry the sending
  // worker's credentials; libudev authenticates these before consuming them.
  if (ret > 0 && nsk && nsk->passcred && msg->msg_control &&
      msg->msg_controllen >= CMSG_SPACE(sizeof(struct ucred_nl))) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred_nl));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;

    struct ucred_nl *cred = (struct ucred_nl *)CMSG_DATA(cmsg);
    cred->pid = sender.pid;
    cred->uid = sender.uid;
    cred->gid = sender.gid;

    used = CMSG_SPACE(sizeof(struct ucred_nl));
  }
  if (ret > 0 && nsk && nsk->pktinfo) {
    if (msg->msg_control && capacity - used >= CMSG_SPACE(sizeof(uint32_t))) {
      struct cmsghdr *cmsg = (void *)((uint8_t *)msg->msg_control + used);
      cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
      cmsg->cmsg_level = SOL_NETLINK;
      cmsg->cmsg_type = NETLINK_PKTINFO;
      uint32_t group = sender.groups ? (uint32_t)__builtin_ctz(sender.groups) + 1 : 0;
      memcpy(CMSG_DATA(cmsg), &group, sizeof(group));
      used += CMSG_SPACE(sizeof(uint32_t));
    } else {
      msg->msg_flags |= MSG_CTRUNC;
    }
  }
  msg->msg_controllen = used;
  return ret;
}

static ssize_t netlink_sendmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || msg->msg_iovlen == 0)
    return -22;

  size_t total_len = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    total_len += msg->msg_iov[i].iov_len;
  }
  if (total_len == 0)
    return 0;

  if (msg->msg_iovlen == 1) {
    return netlink_sendto(sock, msg->msg_iov[0].iov_base, total_len, flags,
                          (struct sockaddr *)msg->msg_name, msg->msg_namelen);
  }

  void *kbuf = kmalloc(total_len);
  if (!kbuf)
    return -12;
  size_t off = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    if (msg->msg_iov[i].iov_base && msg->msg_iov[i].iov_len) {
      memcpy((uint8_t *)kbuf + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
      off += msg->msg_iov[i].iov_len;
    }
  }
  ssize_t ret = netlink_sendto(sock, kbuf, total_len, flags,
                              (struct sockaddr *)msg->msg_name, msg->msg_namelen);
  kfree(kbuf);
  return ret;
}

static int netlink_poll(socket_t *sock, int events) {
  if (!sock || !sock->sk)
    return 0;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
  int revents = 0;

  spinlock_acquire(&nsk->recv_queue.lock);
  if (!skb_queue_empty(&nsk->recv_queue)) {
    revents |= (EPOLLIN | EPOLLRDNORM);
  }
  spinlock_release(&nsk->recv_queue.lock);

  if (events & (EPOLLOUT | EPOLLWRNORM)) {
    revents |= (EPOLLOUT | EPOLLWRNORM);
  }

  return revents;
}

static void netlink_destroy(socket_t *sock) {
  if (sock->sk) {
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    spinlock_acquire(&netlink_lock);
    list_del(&nsk->list);
    spinlock_release(&netlink_lock);

    // Free pending skbs
    while (!skb_queue_empty(&nsk->recv_queue)) {
      sk_buff_t *skb = skb_dequeue(&nsk->recv_queue);
      if (skb)
        free_skb(skb);
    }

    kfree(sock->sk);
    sock->sk = NULL;
  }
}

static int netlink_ioctl(socket_t *sock, uint32_t request, uint64_t arg) {
  if (!sock || !sock->sk)
    return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
  switch (request) {
  case 0x541B: { // FIONREAD / TIOCINQ
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
      return -14;
    spinlock_acquire(&nsk->recv_queue.lock);
    int unread = nsk->recv_queue.head ? (int)nsk->recv_queue.head->len : 0;
    spinlock_release(&nsk->recv_queue.lock);
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

static sock_ops_t netlink_ops = {
    .bind = netlink_bind,
    .recv = netlink_recv,
    .send = netlink_send,
    .recvfrom = netlink_recvfrom,
    .sendto = netlink_sendto,
    .recvmsg = netlink_recvmsg,
    .sendmsg = netlink_sendmsg,
    .poll = netlink_poll,
    .setsockopt = netlink_setsockopt,
    .getsockopt = netlink_getsockopt,
    .getsockname = netlink_getsockname,
    .ioctl = netlink_ioctl,
    .destroy = netlink_destroy,
};

int netlink_create(socket_t *sock, int protocol) {
  netlink_sock_t *nsk = kmalloc(sizeof(netlink_sock_t));
  if (!nsk)
    return -12; // ENOMEM

  memset(nsk, 0, sizeof(netlink_sock_t));
  nsk->parent = sock; // back-pointer so netlink_broadcast can wake the socket
  nsk->protocol = protocol;
  spinlock_init(&nsk->recv_queue.lock);
  nsk->recv_queue.head = NULL;
  nsk->recv_queue.tail = NULL;
  nsk->recv_queue.len = 0;

  sock->sk = nsk;
  sock->ops = &netlink_ops;
  sock->state = SS_UNCONNECTED;

  spinlock_acquire(&netlink_lock);
  list_add_tail(&nsk->list, &netlink_sockets);
  spinlock_release(&netlink_lock);

  klogf("[OK] AF_NETLINK socket created (protocol=%d)\n", protocol);

  return 0;
}

static net_family_t netlink_family = {
    .family = AF_NETLINK, .create = netlink_create, .next = NULL};

void af_netlink_init(void) {
  INIT_LIST_HEAD(&netlink_sockets);
  spinlock_init(&netlink_lock);
  sock_register_family(&netlink_family);
  klog_puts("[OK] AF_NETLINK protocol initialized\n");
}
