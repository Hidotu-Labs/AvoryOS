#include "af_netlink.h"
#include "../console/klog.h"
#include "../fs/sysfs.h"
#include "../lib/string.h"
#include "../net/core.h"
#include "../net/ipv4.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include "../include/arch/uaccess.h"

#include "../net/ipv6.h"

static struct list_head netlink_sockets;
static spinlock_t netlink_lock;
static uint64_t uevent_seqnum = 1;

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
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4
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

static int route_link(netlink_sock_t *nsk, uint32_t seq, uint32_t pid,
                      const struct net_device *dev) {
  uint8_t msg[128]; memset(msg, 0, sizeof(msg));
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifinfomsg_min *i = (struct ifinfomsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWLINK; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->index = 2; i->flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
  i->change = 0xffffffffU;
  size_t pos = sizeof(*h) + sizeof(*i);
  pos = route_attr(msg, pos, sizeof(msg), IFLA_IFNAME, dev->name, strlen(dev->name) + 1);
  pos = route_attr(msg, pos, sizeof(msg), IFLA_MTU, &dev->mtu, sizeof(dev->mtu));
  pos = route_attr(msg, pos, sizeof(msg), IFLA_ADDRESS, dev->mac, 6);
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
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
  h->nlmsg_len = pos; return route_queue(nsk, msg, pos);
}

static int route_loopback_link(netlink_sock_t *nsk, uint32_t seq, uint32_t pid) {
  uint8_t msg[128]; memset(msg, 0, sizeof(msg));
  struct nlmsghdr_min *h = (struct nlmsghdr_min *)msg;
  struct ifinfomsg_min *i = (struct ifinfomsg_min *)(h + 1);
  h->nlmsg_type = RTM_NEWLINK; h->nlmsg_flags = NLM_F_MULTI; h->nlmsg_seq = seq; h->nlmsg_pid = pid;
  i->index = 1; i->flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
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

static void netlink_push_fake_uevent(socket_t *sock) {
  klog_puts("[NETLINK] push_fake_uevent start\n");
  if (!sock || !sock->sk)
    return;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

  // Devices to notify: DRM card, DRM connector, Keyboard, Mouse
  const char *devpaths[] = {sysfs_gpu_devpath, sysfs_gpu_connector_devpath,
                            "/devices/virtual/input/input0/event0",
                            "/devices/virtual/input/input1/event1"};
  const char *subsystems[] = {"drm", "drm", "input", "input"};
  const char *devnames[] = {"/dev/dri/card0", "", "/dev/input/event0",
                            "/dev/input/event1"};
  const char *majors[] = {"226", "", "13", "13"};
  const char *minors[] = {"0", "", "64", "65"};

  for (int i = 0; i < 4; i++) {
    char uevent_buf[512];
    memset(uevent_buf, 0, sizeof(uevent_buf));
    size_t pos = 0;

    char tmp[256];
    strcpy(tmp, "add@");
    strcat(tmp, devpaths[i]);
    pos = ue_append(uevent_buf, pos, tmp);

    pos = ue_append(uevent_buf, pos, "ACTION=add");

    strcpy(tmp, "DEVPATH=");
    strcat(tmp, devpaths[i]);
    pos = ue_append(uevent_buf, pos, tmp);

    strcpy(tmp, "SUBSYSTEM=");
    strcat(tmp, subsystems[i]);
    pos = ue_append(uevent_buf, pos, tmp);

    if (devnames[i][0]) {
      strcpy(tmp, "DEVNAME=");
      strcat(tmp, devnames[i]);
      pos = ue_append(uevent_buf, pos, tmp);
    }

    if (majors[i][0]) {
      strcpy(tmp, "MAJOR=");
      strcat(tmp, majors[i]);
      pos = ue_append(uevent_buf, pos, tmp);
    }

    if (minors[i][0]) {
      strcpy(tmp, "MINOR=");
      strcat(tmp, minors[i]);
      pos = ue_append(uevent_buf, pos, tmp);
    }

    if (i == 1) {
      pos = ue_append(uevent_buf, pos, "DEVTYPE=drm_connector");
      pos = ue_append(uevent_buf, pos, "HOTPLUG=1");
      pos = ue_append(uevent_buf, pos, "CONNECTOR=HDMI-A-1");
    }

    char seq_buf[32];
    strcpy(seq_buf, "SEQNUM=");
    char tmp_dec[20];
    uint64_t current_seq;
    spinlock_acquire(&netlink_lock);
    current_seq = uevent_seqnum++;
    spinlock_release(&netlink_lock);

    // Simple u64 to dec (shared with sysfs but local for now or use a shared
    // helper)
    char *p = tmp_dec + 19;
    *p = '\0';
    uint64_t val = current_seq;
    if (val == 0)
      *--p = '0';
    else {
      while (val > 0) {
        *--p = '0' + (val % 10);
        val /= 10;
      }
    }
    strcat(seq_buf, p);
    pos = ue_append(uevent_buf, pos, seq_buf);

    klog_puts("[NETLINK] Pushing fake uevent for ");
    klog_puts(devpaths[i]);
    klog_puts("\n");

    sk_buff_t *skb = alloc_skb(pos);
    if (skb) {
      memcpy(skb->data, uevent_buf, pos);
      skb->len = pos;
      skb_queue_tail(&nsk->recv_queue, skb);
    }
  }

  socket_wake(sock);
  if (sock->node)
    epoll_notify_event(sock->node, EPOLLIN);
}

void netlink_broadcast(int protocol, uint32_t group, const void *data,
                       size_t len) {
  spinlock_acquire(&netlink_lock);
  netlink_sock_t *nsk;
  list_for_each_entry(nsk, &netlink_sockets, list) {
    if (nsk->protocol == protocol && (nsk->groups & group)) {
      sk_buff_t *skb = alloc_skb(len);
      if (skb) {
        memcpy(skb->data, data, len);
        skb->len = len;
        skb_queue_tail(&nsk->recv_queue, skb);
        socket_wake(nsk->parent);
        if (nsk->parent->node)
          epoll_notify_event(nsk->parent->node, EPOLLIN);
      }
    }
  }
  spinlock_release(&netlink_lock);
}

void netlink_broadcast_drm_hotplug(const char *devpath,uint32_t connector){
 char buf[512],tmp[192],num[12];size_t pos=0;memset(buf,0,sizeof(buf));strcpy(tmp,"change@");strcat(tmp,devpath);pos=ue_append(buf,pos,tmp);pos=ue_append(buf,pos,"ACTION=change");strcpy(tmp,"DEVPATH=");strcat(tmp,devpath);pos=ue_append(buf,pos,tmp);pos=ue_append(buf,pos,"SUBSYSTEM=drm");pos=ue_append(buf,pos,"DEVTYPE=drm_connector");pos=ue_append(buf,pos,"HOTPLUG=1");char *p=num+sizeof(num);*--p=0;uint32_t v=connector;if(!v)*--p=48;while(v){*--p=(char)(48+v%10);v/=10;}strcpy(tmp,"CONNECTOR=HDMI-A-");strcat(tmp,p);pos=ue_append(buf,pos,tmp);netlink_broadcast(15,1,buf,pos);
}

// AF_NETLINK Operations

static int netlink_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (addrlen < (int)sizeof(struct sockaddr_nl))
    return -22; // EINVAL
  struct sockaddr_nl *nl = (struct sockaddr_nl *)addr;
  if (nl->nl_family != AF_NETLINK)
    return -97; // EAFNOSUPPORT

  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
  nsk->groups = nl->nl_groups;
  if (nl->nl_pid == 0) {
    struct thread *t = sched_get_current();
    nsk->portid = (t) ? t->tid : 1;
  } else {
    nsk->portid = nl->nl_pid;
  }

  klog_puts("[NETLINK] bind called: port=");
  klog_uint64(nsk->portid);
  klog_puts(" groups=");
  klog_uint64(nsk->groups);
  klog_puts("\n");

  // If udev is binding (portid != 0 or just protocol is uevent), push a fake
  // uevent to notify it about the DRM card and input devices.
  if (nsk->protocol == 15) { // NETLINK_KOBJECT_UEVENT
    netlink_push_fake_uevent(sock);
  }

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

  klog_puts("[NETLINK] setsockopt: level=");
  klog_uint64((uint64_t)level);
  klog_puts(" optname=");
  klog_uint64((uint64_t)optname);
  klog_puts("\n");

  if (level == SOL_NETLINK) {
    switch (optname) {
    case NETLINK_ADD_MEMBERSHIP: {
      if (optlen < (int)sizeof(int))
        return -22;
      int group = *(const int *)optval;
      klog_puts("[NETLINK] setsockopt: ADD_MEMBERSHIP group=");
      klog_uint64((uint64_t)group);
      klog_puts("\n");

      uint32_t group_mask = (1 << (group - 1));
      nsk->groups |= group_mask;

      if (group == 1) { // 1 is often the uevent group
        netlink_push_fake_uevent(sock);
      }
      return 0;
    }
    case NETLINK_DROP_MEMBERSHIP: {
      if (optlen < (int)sizeof(int))
        return -22;
      int group = *(const int *)optval;
      uint32_t group_mask = (1 << (group - 1));
      nsk->groups &= ~group_mask;
      return 0;
    }
    }
  } else if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_PASSCRED:
      klog_puts("[NETLINK] setsockopt: SO_PASSCRED\n");
      if (optlen >= (int)sizeof(int))
        nsk->passcred = (*(const int *)optval != 0);
      else
        nsk->passcred = true;
      return 0;
    case 26: // SO_ATTACH_FILTER
      klog_puts("[NETLINK] setsockopt: SO_ATTACH_FILTER\n");
      return 0; // Stub success
    case 2:     // SO_REUSEADDR
      return 0;
    }
  }

  return -92; // ENOPROTOOPT
}

static int netlink_getsockopt(socket_t *sock, int level, int optname,
                              void *optval, int *optlen) {
  (void)sock;
  (void)optval;
  (void)optlen;
  (void)level;
  (void)optname;
  return -92; // ENOPROTOOPT
}

static ssize_t netlink_recvfrom(socket_t *sock, void *buf, size_t len,
                                int flags, struct sockaddr *src_addr,
                                int *addrlen) {
  if (!sock || !sock->sk)
    return -22;
  netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

  spinlock_acquire(&nsk->recv_queue.lock);
  while (nsk->recv_queue.head == NULL) {
    if (sock->flags & SOCK_NONBLOCK || (flags & MSG_DONTWAIT)) {
      spinlock_release(&nsk->recv_queue.lock);
      return -11; // EAGAIN
    }
    spinlock_release(&nsk->recv_queue.lock);
    socket_wait(sock);
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
    return 0;

  size_t to_copy = (len < skb->len) ? len : skb->len;
  memcpy(buf, skb->data, to_copy);

  if (src_addr && addrlen) {
    struct sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = AF_NETLINK;
    nl.nl_pid = 0; // From kernel
    nl.nl_groups = 0;

    int copy_len = (*addrlen < (int)sizeof(nl)) ? *addrlen : (int)sizeof(nl);
    memcpy(src_addr, &nl, copy_len);
    *addrlen = sizeof(nl);
  }

  ssize_t ret = (flags & MSG_TRUNC) ? (ssize_t)skb->len : (ssize_t)to_copy;
  if (!(flags & MSG_PEEK)) {
    free_skb(skb);
  }
  return ret;
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
  klog_puts("[NETLINK] sendto len=");
  klog_uint64(len);
  klog_puts("\n");

  netlink_sock_t *nsk = sock ? (netlink_sock_t *)sock->sk : NULL;
  if (!nsk)
    return -22; // EINVAL

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
    struct net_device *dev = net_device_default();
    const struct ipv4_config *cfg = ipv4_get_config();
    const struct ipv6_config *cfg6 = ipv6_get_config();
    int route_ret = 0;
    if (request.nlmsg_type == RTM_GETLINK) {
      route_ret = route_loopback_link(nsk, request.nlmsg_seq, reply_pid);
      if (route_ret == 0 && dev)
        route_ret = route_link(nsk, request.nlmsg_seq, reply_pid, dev);
    } else if (request.nlmsg_type == RTM_GETADDR) {
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
  ssize_t ret = netlink_recvfrom(sock, iov->iov_base, iov->iov_len, flags,
                                 (struct sockaddr *)msg->msg_name,
                                 (int *)&msg->msg_namelen);

  // Populate SCM_CREDENTIALS ancillary data when SO_PASSCRED is set.
  // libudev validates ucred.pid == 0 (kernel sender) and silently drops
  // messages that lack credentials, so this is required for Weston to
  // consume the uevent payloads pushed at bind time.
  if (ret > 0 && nsk && nsk->passcred && msg->msg_control &&
      msg->msg_controllen >= CMSG_SPACE(sizeof(struct ucred_nl))) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred_nl));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;

    struct ucred_nl *cred = (struct ucred_nl *)CMSG_DATA(cmsg);
    cred->pid = 0; // 0 = from kernel
    cred->uid = 0;
    cred->gid = 0;

    msg->msg_controllen = CMSG_SPACE(sizeof(struct ucred_nl));
  } else {
    msg->msg_controllen = 0;
  }

  msg->msg_flags = 0;
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

  klog_puts("[OK] AF_NETLINK socket created (protocol=");
  klog_uint64((uint64_t)protocol);
  klog_puts(")\n");

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
