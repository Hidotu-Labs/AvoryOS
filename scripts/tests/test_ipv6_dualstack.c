/* Host regression against the real socket, UDP and TCP implementations.
 * cc -std=gnu11 -O1 -g -fsanitize=address,undefined -ffunction-sections
 * -fdata-sections -I kernel/src -I kernel/src/include -I kernel/linuxkpi/include
 * -I kernel/limine-protocol/include scripts/tests/test_ipv6_dualstack.c
 * -Wl,--gc-sections -o /tmp/test_ipv6_dualstack */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
#undef strchr
#undef strrchr
#undef strstr
typedef struct { int locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spinlock_init(spinlock_t *s) { s->locked = 0; }
static void spinlock_acquire(spinlock_t *s) { assert(!s->locked); s->locked = 1; }
static void spinlock_release(spinlock_t *s) { assert(s->locked); s->locked = 0; }
#include "../../kernel/src/socket/af_inet6.c"
#define next_ephemeral udp_next_ephemeral
#define alloc_ephemeral_locked udp_alloc_ephemeral_locked
#define hash_insert_locked udp_hash_insert_locked
#define hash_remove_locked udp_hash_remove_locked
#include "../../kernel/src/net/udp.c"
#undef next_ephemeral
#undef alloc_ephemeral_locked
#undef hash_insert_locked
#undef hash_remove_locked
#include "../../kernel/src/net/tcp.c"
#include "../../kernel/src/lib/rbtree.c"

static struct ipv4_config cfg4 = {.address = 0x0a000002};
static struct ipv6_config cfg6;
static uint32_t sent4;
static unsigned wakes;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
uint64_t lapic_timer_get_ticks(void) { return 100; }
const struct ipv4_config *ipv4_get_config(void) { return &cfg4; }
const struct ipv6_config *ipv6_get_config(void) { return &cfg6; }
int ipv4_send_raw(uint32_t ip, uint8_t proto, const void *p, size_t n) {
  (void)proto; (void)p; (void)n; sent4 = ip; return 0;
}
int ipv4_send_raw_tos(uint32_t ip, uint8_t proto, uint8_t tos, const void *p, size_t n) {
  (void)tos; return ipv4_send_raw(ip, proto, p, n);
}
int ipv6_send_raw(const uint8_t ip[16], uint8_t proto, const void *p, size_t n) {
  (void)ip; (void)proto; (void)p; (void)n; return 0;
}
void wait_queue_init(wait_queue_t *q) { memset(q, 0, sizeof(*q)); }
void wait_queue_add(wait_queue_t *q, wait_queue_entry_t *e) { (void)q; (void)e; }
void wait_queue_remove(wait_queue_t *q, wait_queue_entry_t *e) { (void)q; (void)e; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; wakes++; }
struct thread *sched_get_current(void) { return NULL; }
void sched_yield(void) { assert(!"unexpected blocking operation"); }
void epoll_notify_event(vfs_node_t *n, uint32_t e) { (void)n; (void)e; }
void klog_puts(const char *s) { (void)s; }
bool vmm_is_user_addr_range_valid(uint64_t a, uint64_t n) { (void)a; (void)n; return true; }
socket_t *socket_create(int domain, int type, int protocol) {
  assert(domain == AF_INET6);
  socket_t *s = calloc(1, sizeof(*s)); s->domain = domain; s->type = type;
  s->protocol = protocol;
  assert(inet6_create(s, protocol) == 0);
  return s;
}
unsigned long copy_to_user(void *to, const void *from, unsigned long n) {
  memcpy(to, from, n); return 0;
}
socket_t *socket_create_in_namespace(int domain, int type, int protocol,
                                     struct net_namespace *net_ns) {
  assert(!net_ns); return socket_create(domain, type, protocol);
}
static void drop(socket_t *s) { inet6_destroy(s); free(s); }
static void option(socket_t *s, int value) {
  assert(inet6_setsockopt(s, SOL_IPV6, IPV6_V6ONLY, &value, sizeof(value)) == 0);
  int read = -1, size = sizeof(read);
  assert(inet6_getsockopt(s, SOL_IPV6, IPV6_V6ONLY, &read, &size) == 0);
  assert(read == (value != 0));
}
static struct sockaddr_in6 address(uint16_t port) {
  struct sockaddr_in6 a; uint8_t any[16] = {0}; fill_addr(&a, any, port, 0); return a;
}
static void bind6(socket_t *s, uint16_t port) {
  struct sockaddr_in6 a = address(port);
  assert(inet6_bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
}
int main(void) {
  udp_init();
  /* qBittorrent opens one UDP socket per local interface on the same port. */
  struct udp_socket *loop = udp_socket_alloc();
  struct udp_socket *ether = udp_socket_alloc();
  struct udp_socket *conflict = udp_socket_alloc();
  assert(udp_bind(loop, 0x7f000001, 46026) == 0);
  assert(udp_bind(ether, cfg4.address, 46026) == 0);
  assert(udp_bind(conflict, cfg4.address, 46026) == -98);
  assert(udp_bind(conflict, 0, 46026) == -98);
  udp_deliver(0x0a000003, 6881, 46026, (const uint8_t *)"peer", 4);
  char peer[8];
  assert(udp_recvfrom(ether, peer, sizeof(peer), NULL, NULL, true, 0) == 4);
  assert(!memcmp(peer, "peer", 4));
  assert(udp_recvfrom(loop, peer, sizeof(peer), NULL, NULL, true, 0) == -11);
  udp_socket_free(loop);
  assert(udp_bind(conflict, cfg4.address, 46026) == -98);
  udp_socket_free(ether);
  assert(udp_bind(conflict, 0, 46026) == 0);
  loop = udp_socket_alloc();
  assert(udp_bind(loop, cfg4.address, 46026) == -98);
  udp_socket_free(loop); udp_socket_free(conflict);
  struct tcp_tcb *lo_tcp = tcp_alloc(), *eth_tcp = tcp_alloc();
  struct tcp_tcb *conflict_tcp = tcp_alloc(), *v6_tcp = tcp_alloc();
  assert(tcp_bind(lo_tcp, 0x7f000001, 46026) == 0);
  assert(tcp_listen(lo_tcp, 4) == 0);
  assert(tcp_bind(eth_tcp, cfg4.address, 46026) == 0);
  assert(tcp_listen(eth_tcp, 4) == 0);
  assert(tcp_bind(conflict_tcp, cfg4.address, 46026) == -98);
  assert(tcp_bind(conflict_tcp, 0, 46026) == -98);
  uint32_t loop_ip = 0x7f000001;
  assert(find_listener_address_locked(46026, 4, &loop_ip) == lo_tcp);
  assert(find_listener_address_locked(46026, 4, &cfg4.address) == eth_tcp);
  uint8_t link_ip[16] = {0xfe, 0x80}; link_ip[15] = 1;
  assert(tcp_bind6(v6_tcp, link_ip, 46026) == 0);
  assert(tcp_listen(v6_tcp, 4) == 0);
  assert(find_listener_address_locked(46026, 6, link_ip) == v6_tcp);
  tcp_free(lo_tcp);
  assert(tcp_bind(conflict_tcp, cfg4.address, 46026) == -98);
  tcp_free(eth_tcp); tcp_free(v6_tcp);
  assert(tcp_bind(conflict_tcp, 0, 46026) == 0);
  lo_tcp = tcp_alloc();
  assert(tcp_bind(lo_tcp, cfg4.address, 46026) == -98);
  tcp_free(lo_tcp); tcp_free(conflict_tcp);
  socket_t *s = socket_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  option(s, 2); option(s, 0);
  int off = 0;
  assert(inet6_setsockopt(s, SOL_IPV6, IPV6_V6ONLY, &off, 1) == -22);
  bind6(s, 1716);
  assert(inet6_setsockopt(s, SOL_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == -22);
  struct udp_socket *other = udp_socket_alloc();
  assert(udp_bind(other, 0, 1716) == -98); udp_socket_free(other);
  udp_deliver(0x0a000003, 1717, 1716, (const uint8_t *)"hello", 5);
  assert(wakes && (inet6_poll(s, POLLIN) & POLLIN));
  char buf[16]; struct sockaddr_in6 source; int size = sizeof(source);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&source, &size) == 5);
  assert(!memcmp(buf, "hello", 5)); assert(mapped_addr(source.sin6_addr.s6_addr));
  assert(mapped_ip(source.sin6_addr.s6_addr) == 0x0a000003);
  assert(get16((uint8_t *)&source.sin6_port) == 1717);
  assert(inet6_sendto(s, "reply", 5, 0, (struct sockaddr *)&source, size) == 5);
  assert(sent4 == 0x0a000003);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == -11);
  /* IPv6 traffic continues to use that same queue. */
  uint8_t src6[16] = {0xfe, 0x80}, dst6[16] = {0}, segment[9] = {0};
  put16(segment, 1717); put16(segment + 2, 1716); put16(segment + 4, 9);
  segment[8] = '6'; put16(segment + 6, udp6_checksum(src6, dst6, segment, 9));
  udp6_input(src6, dst6, segment, sizeof(segment));
  size = sizeof(source);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&source, &size) == 1);
  assert(buf[0] == '6' && same_addr(src6, source.sin6_addr.s6_addr));
  drop(s);
  other = udp_socket_alloc(); assert(udp_bind(other, 0, 1716) == 0);
  s = socket_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP); option(s, 0);
  struct sockaddr_in6 a = address(1716);
  assert(inet6_bind(s, (struct sockaddr *)&a, sizeof(a)) == -98);
  assert(!((struct inet6_sock *)s->sk)->bound);
  udp_socket_free(other); bind6(s, 1716); drop(s);
  s = socket_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP); option(s, 1); bind6(s, 1716);
  udp_deliver(0x0a000003, 1717, 1716, (const uint8_t *)"hello", 5);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == -11);
  a = address(1717); map_ip(a.sin6_addr.s6_addr, 0x0a000003);
  assert(inet6_sendto(s, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)) == -101);
  drop(s);
  /* Connected mapped datagrams filter by peer and keep a shared local port. */
  s = socket_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP); option(s, 0);
  assert(inet6_connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
  struct inet6_sock *isk = s->sk;
  assert(isk->udp4 && isk->local_port == isk->udp4->local_port);
  udp_deliver(0x0a000004, 1717, isk->local_port, (const uint8_t *)"bad", 3);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == -11);
  udp_deliver(0x0a000003, 1717, isk->local_port, (const uint8_t *)"ok", 2);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == 2);
  assert(inet6_send(s, "x", 1, 0) == 1 && sent4 == 0x0a000003);
  struct sockaddr unspec = {0};
  assert(inet6_connect(s, &unspec, sizeof(unspec)) == 0);
  udp_deliver(0x0a000004, 1717, isk->local_port, (const uint8_t *)"ok", 2);
  assert(inet6_recvfrom(s, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == 2);
  drop(s);
  puts("PASS: dual-stack UDP delivery, replies, IPv6 coexistence, port ownership and V6ONLY");
  s = socket_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP); option(s, 0); bind6(s, 1716);
  assert(inet6_listen(s, 4) == 0);
  struct tcp_tcb *listener = ((struct inet6_sock *)s->sk)->tcp;
  assert(find_listener_locked(1716, 4) == listener);
  assert(find_listener_locked(1716, 6) == listener);
  /* Complete an IPv4 handshake through the actual input and accept paths. */
  uint8_t syn[20] = {0}; p16(syn, 1717); p16(syn + 2, 1716);
  p32(syn + 4, 1000); syn[12] = 5 << 4; syn[13] = SYN; p16(syn + 14, 4096);
  p16(syn + 16, csum(0x0a000003, cfg4.address, syn, sizeof(syn)));
  tcp_input_ipv4(0x0a000003, cfg4.address, syn, sizeof(syn));
  struct tcp_tcb *child = find_locked(0x0a000003, 1717, 1716);
  assert(child && child->state == TCP_SYN_RECEIVED);
  p32(syn + 4, 1001); p32(syn + 8, child->snd_nxt); syn[13] = ACK;
  p16(syn + 16, 0); p16(syn + 16, csum(0x0a000003, cfg4.address, syn, sizeof(syn)));
  tcp_input_ipv4(0x0a000003, cfg4.address, syn, sizeof(syn));
  assert(child->state == TCP_ESTABLISHED);
  socket_t *accepted = NULL; assert(inet6_accept(s, &accepted) == 0);
  size = sizeof(source); assert(inet6_getpeername(accepted, (struct sockaddr *)&source, &size) == 0);
  assert(mapped_ip(source.sin6_addr.s6_addr) == 0x0a000003);
  assert(mapped_addr(source.sin6_addr.s6_addr));
  drop(accepted); drop(s);
  s = socket_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP); option(s, 1); bind6(s, 1720);
  assert(inet6_listen(s, 4) == 0); assert(!find_listener_locked(1720, 4)); drop(s);
  s = socket_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP); option(s, 0);
  s->flags |= SOCK_NONBLOCK;
  assert(inet6_connect(s, (struct sockaddr *)&a, sizeof(a)) == -115);
  assert(((struct inet6_sock *)s->sk)->tcp->address_family == 4);
  assert(sent4 == 0x0a000003); drop(s);
  s = socket_create(AF_INET6, SOCK_STREAM, IPPROTO_TCP); option(s, 0);
  assert(inet6_listen(s, 4) == 0);
  listener = ((struct inet6_sock *)s->sk)->tcp;
  assert(listener->address_family == 6);
  assert(find_listener_locked(listener->local_port, 4) == listener); drop(s);
  puts("PASS: IPv4 handshake and mapped accept on IPv6 listener; V6ONLY isolation");
  return 0;
}
