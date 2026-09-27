#include "socket/af_inet6.h"
#include "apic/lapic_timer.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "net/ipv6.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"
#include "socket/socket.h"
#include "socket/socket_internal.h"

#define UDP6_MAX 32
#define UDP6_QUEUE 16
#define UDP6_PAYLOAD 2048

struct udp6_packet {
  uint8_t source[16];
  uint16_t port, length;
  uint8_t data[UDP6_PAYLOAD];
};

struct inet6_sock {
  socket_t *parent;
  bool used, bound, connected;
  uint8_t local[16], remote[16];
  uint16_t local_port, remote_port;
  uint32_t scope_id;
  struct udp6_packet queue[UDP6_QUEUE];
  uint32_t head, tail;
  wait_queue_t wait;
  struct tcp_tcb *tcp;
  bool heap_allocated;
  /* TCP options */
  bool tcp_nodelay;
  bool tcp_keepalive;
  int keepidle_s, keepintvl_s, keepcnt;
  int linger_seconds;
  bool shut_wr;
};

static struct inet6_sock udp6[UDP6_MAX];
static spinlock_t udp6_lock = SPINLOCK_INIT;
static uint16_t next_port = 49152;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static bool zero_addr(const uint8_t a[16]) {
  static const uint8_t zero[16];
  return memcmp(a, zero, 16) == 0;
}
static bool same_addr(const uint8_t a[16], const uint8_t b[16]) {
  return memcmp(a, b, 16) == 0;
}
static bool link_local_addr(const uint8_t a[16]) {
  return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}
static bool loopback_addr(const uint8_t a[16]) {
  static const uint8_t loopback[16] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  return same_addr(a, loopback);
}
static uint32_t sum_bytes(uint32_t sum, const uint8_t *p, size_t n) {
  while (n > 1) { sum += get16(p); p += 2; n -= 2; }
  if (n) sum += (uint16_t)p[0] << 8;
  return sum;
}
static uint16_t udp6_checksum(const uint8_t src[16], const uint8_t dst[16],
                              const uint8_t *segment, size_t length) {
  uint32_t sum = sum_bytes(0, src, 16);
  sum = sum_bytes(sum, dst, 16);
  sum += (uint32_t)(length >> 16) + (uint16_t)length + IPPROTO_UDP;
  sum = sum_bytes(sum, segment, length);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static bool port_used(uint16_t port, const struct inet6_sock *skip) {
  for (int i = 0; i < UDP6_MAX; i++)
    if (&udp6[i] != skip && udp6[i].used && udp6[i].bound &&
        udp6[i].local_port == port)
      return true;
  return false;
}

static int bind_port(struct inet6_sock *s, uint16_t port) {
  spinlock_acquire(&udp6_lock);
  if (port && port_used(port, s)) {
    spinlock_release(&udp6_lock);
    return -98;
  }
  if (!port) {
    for (uint32_t n = 0; n < 16384; n++) {
      uint16_t candidate = next_port++;
      if (next_port < 49152) next_port = 49152;
      if (!port_used(candidate, s)) { port = candidate; break; }
    }
  }
  if (!port) { spinlock_release(&udp6_lock); return -98; }
  s->local_port = port; s->bound = true;
  spinlock_release(&udp6_lock);
  return 0;
}

static void fill_addr(struct sockaddr_in6 *out, const uint8_t addr[16],
                      uint16_t port, uint32_t scope) {
  memset(out, 0, sizeof(*out));
  out->sin6_family = AF_INET6;
  out->sin6_port = (uint16_t)(port << 8 | port >> 8);
  memcpy(out->sin6_addr.s6_addr, addr, 16);
  out->sin6_scope_id = scope;
}

static int inet6_bind(socket_t *sock, struct sockaddr *addr, int len) {
  if (len < (int)sizeof(struct sockaddr_in6) || addr->sa_family != AF_INET6)
    return -22;
  struct inet6_sock *s = sock->sk;
  struct sockaddr_in6 *a = (struct sockaddr_in6 *)addr;
  if (s->tcp)
    return tcp_bind6(s->tcp, a->sin6_addr.s6_addr,
                     get16((uint8_t *)&a->sin6_port));
  memcpy(s->local, a->sin6_addr.s6_addr, 16);
  s->scope_id = a->sin6_scope_id;
  return bind_port(s, get16((uint8_t *)&a->sin6_port));
}

static int inet6_connect(socket_t *sock, struct sockaddr *addr, int len) {
  struct inet6_sock *s = sock->sk;
  if (!s) return -22;
  if (addr && addr->sa_family == 0 /* AF_UNSPEC */ && sock->type == SOCK_DGRAM) {
    spinlock_acquire(&udp6_lock);
    s->connected = false;
    memset(s->remote, 0, sizeof(s->remote));
    s->remote_port = 0;
    spinlock_release(&udp6_lock);
    sock->state = SS_UNCONNECTED;
    return 0;
  }
  if (len < (int)sizeof(struct sockaddr_in6) || addr->sa_family != AF_INET6)
    return -22;
  struct sockaddr_in6 *a = (struct sockaddr_in6 *)addr;
  const struct ipv6_config *cfg = ipv6_get_config();
  const uint8_t *destination = a->sin6_addr.s6_addr;
  bool link = link_local_addr(destination);
  bool loop = loopback_addr(destination);
  bool mcast = destination[0] == 0xff;
  bool dest_global = (destination[0] & 0xe0) == 0x20;
  bool cfg_global = cfg->global_valid && ((cfg->global[0] & 0xe0) == 0x20);

  if (!link && !loop && !mcast) {
    if (!cfg->global_valid || (dest_global && !cfg_global))
      return -101; // ENETUNREACH
  }

  if (s->tcp) {
    if (sock->state == SS_CONNECTED || s->tcp->state == TCP_ESTABLISHED)
      return -106;
    if (sock->state == SS_CONNECTING || s->tcp->state == TCP_SYN_SENT || s->tcp->state == TCP_SYN_RECEIVED)
      return -114;
    s->tcp->vfs_node = sock->node;
    s->tcp->wait_queue = sock->wait_queue;
    tcp_set_nodelay(s->tcp, s->tcp_nodelay);
    tcp_set_reuseaddr(s->tcp, sock->reuseaddr != 0);
    tcp_set_keepalive(s->tcp, s->tcp_keepalive, s->keepidle_s,
                      s->keepintvl_s, s->keepcnt);
    if (s->linger_seconds >= 0)
      tcp_set_linger(s->tcp, s->linger_seconds);
    int r = tcp_active_open6(s->tcp, a->sin6_addr.s6_addr,
                             get16((uint8_t *)&a->sin6_port));
    if (r < 0) return r;
    memcpy(s->remote, a->sin6_addr.s6_addr, 16);
    s->remote_port = get16((uint8_t *)&a->sin6_port);
    s->scope_id = a->sin6_scope_id;
    if (socket_is_nonblocking(sock)) { sock->state = SS_CONNECTING; return -115; }
    uint64_t deadline = lapic_timer_get_ticks() + 10000;
    struct thread *self = sched_get_current();
    wait_queue_entry_t wqe = { .thread = self, .next = NULL };
    while (s->tcp->state == TCP_SYN_SENT && !s->tcp->error &&
           lapic_timer_get_ticks() < deadline) {
      wait_queue_add(sock->wait_queue, &wqe);
      if (self) {
        self->wakeup_ticks = lapic_timer_get_ticks() + 50;
        self->state = THREAD_BLOCKED;
      }
      sched_yield();
      if (self) self->wakeup_ticks = 0;
      wait_queue_remove(sock->wait_queue, &wqe);
      if (self) self->state = THREAD_RUNNING;
    }
    if (s->tcp->state != TCP_ESTABLISHED)
      return s->tcp->error ? -s->tcp->error : -110;
    s->connected = true; sock->state = SS_CONNECTED; return 0;
  }

  if (!s->bound) { int r = bind_port(s, 0); if (r < 0) return r; }
  memcpy(s->remote, a->sin6_addr.s6_addr, 16);
  s->remote_port = get16((uint8_t *)&a->sin6_port);
  s->scope_id = a->sin6_scope_id;
  s->connected = true; sock->state = SS_CONNECTED;
  return 0;
}

static ssize_t inet6_sendto(socket_t *sock, const void *buf, size_t len,
                            int flags, struct sockaddr *dest, int addrlen) {
  struct inet6_sock *s = sock->sk;
  if (s->tcp)
    return tcp_send(s->tcp, buf, len,
                    socket_is_nonblocking(sock) || (flags & MSG_DONTWAIT));
  const uint8_t *address; uint16_t port;
  if (dest && addrlen >= (int)sizeof(struct sockaddr_in6)) {
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)dest;
    if (a->sin6_family != AF_INET6) return -97;
    address = a->sin6_addr.s6_addr;
    port = get16((uint8_t *)&a->sin6_port);
  } else if (s->connected) { address = s->remote; port = s->remote_port; }
  else return -107;
  if (len > UDP6_PAYLOAD) return -90;
  if (!s->bound) { int r = bind_port(s, 0); if (r < 0) return r; }
  const struct ipv6_config *cfg = ipv6_get_config();
  const uint8_t *source = !zero_addr(s->local) ? s->local :
      (address[0] == 0xfe ? cfg->link_local : cfg->global);
  uint8_t segment[8 + UDP6_PAYLOAD];
  put16(segment, s->local_port); put16(segment + 2, port);
  put16(segment + 4, (uint16_t)(len + 8)); segment[6] = segment[7] = 0;
  memcpy(segment + 8, buf, len);
  uint16_t csum = udp6_checksum(source, address, segment, len + 8);
  if (!csum) csum = 0xffff;
  put16(segment + 6, csum);
  int r = ipv6_send_raw(address, IPPROTO_UDP, segment, len + 8);
  return r < 0 ? r : (ssize_t)len;
}

static ssize_t inet6_send(socket_t *s, const void *b, size_t n, int f) {
  return inet6_sendto(s, b, n, f, NULL, 0);
}

static ssize_t inet6_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                              struct sockaddr *src, int *addrlen) {
  struct inet6_sock *s = sock->sk;
  if (s->tcp)
    return tcp_recv(s->tcp, buf, len,
                    socket_is_nonblocking(sock) || (flags & MSG_DONTWAIT));
  for (;;) {
    spinlock_acquire(&udp6_lock);
    if (s->tail != s->head) {
      struct udp6_packet *p = &s->queue[s->tail++ % UDP6_QUEUE];
      size_t copy = p->length < len ? p->length : len;
      memcpy(buf, p->data, copy);
      if (src && addrlen && *addrlen >= (int)sizeof(struct sockaddr_in6)) {
        fill_addr((struct sockaddr_in6 *)src, p->source, p->port, s->scope_id);
        *addrlen = sizeof(struct sockaddr_in6);
      }
      spinlock_release(&udp6_lock);
      return copy;
    }
    spinlock_release(&udp6_lock);
    if (socket_is_nonblocking(sock) || (flags & MSG_DONTWAIT)) return -11;
    struct thread *t = sched_get_current();
    wait_queue_entry_t e = {.thread = t};
    wait_queue_t *wq = (wait_queue_t *)(sock->wait_queue ? sock->wait_queue : &s->wait);
    wait_queue_add(wq, &e); t->state = THREAD_BLOCKED;
    sched_yield(); wait_queue_remove(wq, &e);
  }
}

static ssize_t inet6_recv(socket_t *s, void *b, size_t n, int f) {
  return inet6_recvfrom(s, b, n, f, NULL, NULL);
}

static int inet6_name(socket_t *sock, struct sockaddr *addr, int *len,
                      bool peer) {
  if (!addr || !len || *len < (int)sizeof(struct sockaddr_in6)) return -22;
  struct inet6_sock *s = sock->sk;
  if (s->tcp) {
    if (peer && !s->tcp->remote_port) return -107;
    const uint8_t *a = peer ? s->tcp->remote_ip6 : s->tcp->local_ip6;
    fill_addr((struct sockaddr_in6 *)addr, a,
              peer ? s->tcp->remote_port : s->tcp->local_port, s->scope_id);
    *len = sizeof(struct sockaddr_in6); return 0;
  }
  if (peer && !s->connected) return -107;
  const uint8_t *a = peer ? s->remote : s->local;
  fill_addr((struct sockaddr_in6 *)addr, a,
            peer ? s->remote_port : s->local_port, s->scope_id);
  *len = sizeof(struct sockaddr_in6); return 0;
}
static int inet6_getsockname(socket_t *s, struct sockaddr *a, int *l) {
  return inet6_name(s, a, l, false);
}
static int inet6_getpeername(socket_t *s, struct sockaddr *a, int *l) {
  return inet6_name(s, a, l, true);
}
static int inet6_poll(socket_t *sock, int events) {
  struct inet6_sock *s = sock->sk; int r = 0;
  if (s->tcp) {
    s->tcp->vfs_node = sock->node;
    if (s->tcp->state == TCP_LISTEN) {
      if ((events & POLLIN) && tcp_accept_pending(s->tcp))
        r |= POLLIN;
      return r;
    }
    if (s->tcp->state == TCP_ESTABLISHED || s->tcp->state == TCP_CLOSE_WAIT) {
      if ((events & POLLIN) && tcp_readable(s->tcp)) r |= POLLIN;
      if ((events & POLLOUT) && tcp_writable(s->tcp)) r |= POLLOUT;
    } else if (s->tcp->state == TCP_RESET) r |= POLLERR | POLLHUP;
    else if (s->tcp->state != TCP_SYN_SENT) r |= POLLHUP;
    if (s->tcp->error) r |= POLLERR;
    return r;
  }
  if ((events & POLLIN) && s->head != s->tail) r |= POLLIN;
  if (events & POLLOUT) r |= POLLOUT;
  return r;
}
static void inet6_destroy(socket_t *sock) {
  struct inet6_sock *s = sock->sk;
  if (!s) return;
  if (s->tcp) { tcp_close(s->tcp); tcp_put(s->tcp); s->tcp = NULL; }
  if (s->heap_allocated) kfree(s);
  else { spinlock_acquire(&udp6_lock); memset(s, 0, sizeof(*s));
         spinlock_release(&udp6_lock); }
  sock->sk = NULL;
}

static int inet6_listen(socket_t *sock, int backlog) {
  struct inet6_sock *s = sock->sk;
  if (!s->tcp) return -95;
  int r = tcp_listen(s->tcp, backlog);
  if (!r) sock->state = SS_LISTENING;
  return r;
}

static int inet6_accept(socket_t *sock, socket_t **out) {
  struct inet6_sock *s = sock->sk;
  if (!s->tcp || !out) return -22;
  struct tcp_tcb *child = tcp_accept(s->tcp, socket_is_nonblocking(sock));
  if (!child) return socket_is_nonblocking(sock) ? -11 : -4;
  socket_t *ns = socket_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (!ns) { tcp_abort(child); return -12; }
  struct inet6_sock *n = ns->sk;
  tcp_free(n->tcp);
  n->tcp = child;
  tcp_attach_socket(child, &n->wait, ns->node);
  memcpy(n->remote, child->remote_ip6, 16); n->remote_port = child->remote_port;
  n->connected = true; ns->state = SS_CONNECTED; *out = ns; return 0;
}

static int inet6_shutdown(socket_t *sock, int how) {
  struct inet6_sock *s = sock->sk;
  if (s && s->tcp && (how == SHUT_WR || how == SHUT_RDWR) && !s->shut_wr) {
    s->shut_wr = true;
    tcp_close(s->tcp);
  }
  if (sock->wait_queue) wait_queue_wake_all((wait_queue_t *)sock->wait_queue);
  return 0;
}

static ssize_t inet6_sendmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || !msg->msg_iov || !msg->msg_iovlen) return -22;
  struct inet6_sock *s = sock->sk;
  if (s->tcp) {
    ssize_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
      ssize_t r = inet6_send(sock, msg->msg_iov[i].iov_base,
                             msg->msg_iov[i].iov_len, flags);
      if (r < 0) return total ? total : r;
      total += r;
    }
    return total;
  }
  uint8_t data[UDP6_PAYLOAD]; size_t total = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    if (total + msg->msg_iov[i].iov_len > sizeof(data)) return -90;
    memcpy(data + total, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
    total += msg->msg_iov[i].iov_len;
  }
  return inet6_sendto(sock, data, total, flags,
                      (struct sockaddr *)msg->msg_name, msg->msg_namelen);
}

static ssize_t inet6_recvmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!msg || !msg->msg_iov || !msg->msg_iovlen) return -22;
  int addrlen = (int)msg->msg_namelen;
  ssize_t r = inet6_recvfrom(sock, msg->msg_iov[0].iov_base,
                             msg->msg_iov[0].iov_len, flags,
                             (struct sockaddr *)msg->msg_name,
                             msg->msg_name ? &addrlen : NULL);
  if (r >= 0 && msg->msg_name) msg->msg_namelen = (uint32_t)addrlen;
  return r;
}

static int inet6_getsockopt(socket_t *sock, int level, int option,
                            void *value, int *length) {
  if (!value || !length || *length < (int)sizeof(int)) return -22;
  struct inet6_sock *s = sock->sk;
  int result;
  if (level == SOL_SOCKET && option == SO_ERROR) {
    result = s->tcp ? s->tcp->error : sock->error;
    if (s->tcp) s->tcp->error = 0; else sock->error = 0;
  } else if (level == SOL_SOCKET && option == SO_TYPE) result = sock->type;
  else if (level == SOL_SOCKET && option == SO_DOMAIN) result = AF_INET6;
  else if (level == SOL_SOCKET && option == SO_PROTOCOL) result = sock->protocol;
  else if (level == SOL_SOCKET && option == SO_KEEPALIVE) result = s->tcp_keepalive ? 1 : 0;
  else if (level == SOL_SOCKET && option == SO_ACCEPTCONN)
    result = (s->tcp && s->tcp->state == TCP_LISTEN) ? 1 : 0;
  else if (level == SOL_TCP && option == TCP_NODELAY) result = s->tcp_nodelay ? 1 : 0;
  else if (level == SOL_TCP && option == TCP_KEEPIDLE) result = s->keepidle_s;
  else if (level == SOL_TCP && option == TCP_KEEPINTVL) result = s->keepintvl_s;
  else if (level == SOL_TCP && option == TCP_KEEPCNT) result = s->keepcnt;
  else if (level == SOL_IPV6 && option == IPV6_V6ONLY) result = 1;
  else return -92;
  *(int *)value = result; *length = sizeof(int); return 0;
}

static int inet6_setsockopt(socket_t *sock, int level, int option,
                            const void *value, int length) {
  struct inet6_sock *s = sock->sk;
  if (!s || !value || length < (int)sizeof(int)) return -22;
  int v = *(const int *)value;

  if (level == SOL_IPV6 && option == IPV6_V6ONLY)
    return v == 1 ? 0 : -92;

  if (level == SOL_SOCKET) {
    if (option == SO_REUSEADDR) {
      sock->reuseaddr = v;
      if (s->tcp) tcp_set_reuseaddr(s->tcp, v != 0);
      return 0;
    }
    if (option == SO_KEEPALIVE) {
      s->tcp_keepalive = v != 0;
      if (s->tcp)
        tcp_set_keepalive(s->tcp, s->tcp_keepalive, s->keepidle_s,
                          s->keepintvl_s, s->keepcnt);
      return 0;
    }
    if (option == SO_RCVBUF || option == SO_RCVBUFFORCE) {
      if (s->tcp) tcp_set_rcvbuf(s->tcp, v < 0 ? 0 : (size_t)v);
      return 0;
    }
    if (option == SO_SNDBUF || option == SO_SNDBUFFORCE) {
      if (s->tcp) tcp_set_sndbuf(s->tcp, v < 0 ? 0 : (size_t)v);
      return 0;
    }
    if (option == SO_LINGER && length >= (int)(2 * sizeof(int))) {
      const int *lv = (const int *)value;
      if (!lv[0]) s->linger_seconds = -1;
      else if (lv[1] <= 0) s->linger_seconds = 0;
      else s->linger_seconds = lv[1] > 3600 ? 3600 : lv[1];
      if (s->tcp) tcp_set_linger(s->tcp, s->linger_seconds);
      return 0;
    }
    if (option == SO_RCVTIMEO || option == SO_SNDTIMEO || option == SO_BROADCAST)
      return 0;
  }

  if (level == SOL_TCP && s->tcp) {
    switch (option) {
    case TCP_NODELAY:
      s->tcp_nodelay = v != 0;
      tcp_set_nodelay(s->tcp, v != 0);
      return 0;
    case TCP_MAXSEG:
      tcp_set_mss(s->tcp, (uint16_t)v);
      return 0;
    case TCP_KEEPIDLE:
      s->keepidle_s = v;
      tcp_set_keepalive(s->tcp, s->tcp_keepalive, s->keepidle_s,
                        s->keepintvl_s, s->keepcnt);
      return 0;
    case TCP_KEEPINTVL:
      s->keepintvl_s = v;
      tcp_set_keepalive(s->tcp, s->tcp_keepalive, s->keepidle_s,
                        s->keepintvl_s, s->keepcnt);
      return 0;
    case TCP_KEEPCNT:
      s->keepcnt = v;
      tcp_set_keepalive(s->tcp, s->tcp_keepalive, s->keepidle_s,
                        s->keepintvl_s, s->keepcnt);
      return 0;
    default:
      return 0;
    }
  }
  return 0;
}

static sock_ops_t inet6_ops = {
  .bind=inet6_bind, .connect=inet6_connect, .listen=inet6_listen,
  .accept=inet6_accept,
  .send=inet6_send, .recv=inet6_recv,
  .sendto=inet6_sendto, .recvfrom=inet6_recvfrom,
  .sendmsg=inet6_sendmsg, .recvmsg=inet6_recvmsg,
  .poll=inet6_poll, .getsockname=inet6_getsockname,
  .getpeername=inet6_getpeername, .destroy=inet6_destroy,
  .shutdown=inet6_shutdown,
  .getsockopt=inet6_getsockopt, .setsockopt=inet6_setsockopt,
};

static int inet6_create(socket_t *sock, int protocol) {
  if (sock->type == SOCK_STREAM) {
    if (protocol && protocol != IPPROTO_TCP) return -93;
    struct inet6_sock *s = kmalloc(sizeof(*s));
    if (!s) return -12;
    memset(s, 0, sizeof(*s)); s->used = true; s->heap_allocated = true;
    s->parent = sock; wait_queue_init(&s->wait); s->tcp = tcp_alloc();
    if (!s->tcp) { kfree(s); return -105; }
    s->tcp->address_family = 6;
    s->keepidle_s = TCP_KEEPALIVE_IDLE_DEFAULT;
    s->keepintvl_s = TCP_KEEPALIVE_INTVL_DEFAULT;
    s->keepcnt = TCP_KEEPALIVE_CNT_DEFAULT;
    s->linger_seconds = -1;
    tcp_attach_socket(s->tcp, sock->wait_queue, sock->node);
    sock->sk = s; sock->ops = &inet6_ops; return 0;
  }
  if (sock->type != SOCK_DGRAM || (protocol && protocol != IPPROTO_UDP))
    return -93;
  spinlock_acquire(&udp6_lock);
  struct inet6_sock *s = NULL;
  for (int i = 0; i < UDP6_MAX; i++) if (!udp6[i].used) {
    s = &udp6[i]; memset(s, 0, sizeof(*s)); s->used = true; break;
  }
  spinlock_release(&udp6_lock);
  if (!s) return -105;
  s->parent = sock; wait_queue_init(&s->wait);
  sock->sk = s; sock->ops = &inet6_ops; return 0;
}

static void udp6_input(const uint8_t src[16], const uint8_t dst[16],
                       const uint8_t *segment, size_t length) {
  if (length < 8 || get16(segment + 4) < 8 || get16(segment + 4) > length ||
      get16(segment + 4) - 8 > UDP6_PAYLOAD ||
      !get16(segment + 6) || udp6_checksum(src, dst, segment,
                                           get16(segment + 4)) != 0)
      return;
  uint16_t sport = get16(segment), dport = get16(segment + 2);
  spinlock_acquire(&udp6_lock);
  for (int i = 0; i < UDP6_MAX; i++) {
    struct inet6_sock *s = &udp6[i];
    if (!s->used || !s->bound || s->local_port != dport ||
        (!zero_addr(s->local) && !same_addr(s->local, dst)) ||
        (s->connected && (!same_addr(s->remote, src) || s->remote_port != sport)))
      continue;
    if (s->head - s->tail >= UDP6_QUEUE) break;
    struct udp6_packet *p = &s->queue[s->head++ % UDP6_QUEUE];
    memcpy(p->source, src, 16); p->port = sport;
    p->length = (uint16_t)(get16(segment + 4) - 8);
    memcpy(p->data, segment + 8, p->length);
    wait_queue_wake_all(&s->wait);
    if (s->parent) {
      if (s->parent->wait_queue)
        wait_queue_wake_all((wait_queue_t *)s->parent->wait_queue);
      if (s->parent->node)
        epoll_notify_event(s->parent->node, EPOLLIN);
    }
    break;
  }
  spinlock_release(&udp6_lock);
}

static net_family_t family = {.family=AF_INET6, .create=inet6_create};
void af_inet6_init(void) {
  spinlock_init(&udp6_lock); memset(udp6, 0, sizeof(udp6));
  sock_register_family(&family); ipv6_set_udp_handler(udp6_input);
  ipv6_set_tcp_handler(tcp_input_ipv6);
}