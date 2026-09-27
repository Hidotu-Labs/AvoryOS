#include "net/ipv4.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "net/core.h"
#include "net/ipv6.h"
#include "sched/sched.h"

#include <stddef.h>
#include <stdint.h>

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IPV6 0x86dd
#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8
#define ICMP_PORT_UNREACHABLE_TYPE 3
#define ICMP_PORT_UNREACHABLE_CODE 3
#define ARP_CACHE_SIZE 64
#define ARP_REACHABLE_MS 60000
#define ARP_RESOLVE_WAIT_MS 50

typedef void (*udp_handler_t)(uint32_t, uint16_t, uint16_t,
                               const uint8_t *, uint16_t);
static udp_handler_t udp_handler;
static void (*tcp_handler)(uint32_t, uint32_t, const uint8_t *, size_t);
static void (*icmp_handler)(uint32_t, uint32_t, const uint8_t *, size_t);

struct eth_header {
  uint8_t destination[6];
  uint8_t source[6];
  uint16_t type;
} __attribute__((packed));

struct arp_packet {
  uint16_t hardware_type;
  uint16_t protocol_type;
  uint8_t hardware_length;
  uint8_t protocol_length;
  uint16_t operation;
  uint8_t sender_mac[6];
  uint8_t sender_ip[4];
  uint8_t target_mac[6];
  uint8_t target_ip[4];
} __attribute__((packed));

struct ipv4_header {
  uint8_t version_ihl;
  uint8_t dscp_ecn;
  uint16_t total_length;
  uint16_t identification;
  uint16_t flags_fragment;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint8_t source[4];
  uint8_t destination[4];
} __attribute__((packed));

struct icmp_echo {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t identifier;
  uint16_t sequence;
} __attribute__((packed));

enum arp_state { ARP_FREE, ARP_INCOMPLETE, ARP_REACHABLE };
struct arp_entry {
  uint32_t ip;
  uint8_t mac[6];
  enum arp_state state;
  uint8_t retries;
  uint64_t updated;
};

static struct ipv4_config config;
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static spinlock_t arp_lock = SPINLOCK_INIT;
static volatile bool address_conflict;
static uint16_t next_ip_id = 1;

static uint16_t be16(uint16_t value) {
  return (uint16_t)((value << 8) | (value >> 8));
}
static void put_ip(uint8_t out[4], uint32_t ip) {
  out[0] = (uint8_t)(ip >> 24);
  out[1] = (uint8_t)(ip >> 16);
  out[2] = (uint8_t)(ip >> 8);
  out[3] = (uint8_t)ip;
}
static uint32_t get_ip(const uint8_t in[4]) {
  return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
         ((uint32_t)in[2] << 8) | in[3];
}
static bool mac_equal(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}
static bool mac_broadcast(const uint8_t mac[6]) {
  static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  return mac_equal(mac, broadcast);
}

static uint16_t checksum(const void *data, size_t length) {
  const uint8_t *bytes = data;
  uint32_t sum = 0;
  while (length > 1) {
    sum += ((uint16_t)bytes[0] << 8) | bytes[1];
    bytes += 2;
    length -= 2;
  }
  if (length)
    sum += (uint16_t)bytes[0] << 8;
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static int ethernet_send(const uint8_t destination[6], uint16_t type,
                         const void *payload, size_t length) {
  struct net_device *dev = net_device_default();
  if (!dev || length + sizeof(struct eth_header) > NET_FRAME_MAX - 4)
    return -1;
  uint8_t frame[NET_FRAME_MAX - 4];
  struct eth_header *header = (struct eth_header *)frame;
  memcpy(header->destination, destination, 6);
  memcpy(header->source, dev->mac, 6);
  header->type = be16(type);
  memcpy(frame + sizeof(*header), payload, length);
  return dev->ops->transmit(dev, frame, sizeof(*header) + length);
}

static struct arp_entry *arp_find_locked(uint32_t ip) {
  for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++)
    if (arp_cache[i].state != ARP_FREE && arp_cache[i].ip == ip)
      return &arp_cache[i];
  return NULL;
}

static struct arp_entry *arp_get_slot_locked(uint32_t ip) {
  struct arp_entry *entry = arp_find_locked(ip);
  if (entry)
    return entry;
  uint64_t oldest = UINT64_MAX;
  uint32_t slot = 0;
  for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
    if (arp_cache[i].state == ARP_FREE)
      return &arp_cache[i];
    if (arp_cache[i].updated < oldest) {
      oldest = arp_cache[i].updated;
      slot = i;
    }
  }
  return &arp_cache[slot];
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
  spinlock_acquire(&arp_lock);
  struct arp_entry *entry = arp_get_slot_locked(ip);
  entry->ip = ip;
  memcpy(entry->mac, mac, 6);
  entry->state = ARP_REACHABLE;
  entry->retries = 0;
  entry->updated = lapic_timer_get_ticks();
  spinlock_release(&arp_lock);
}

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
  bool found = false;
  spinlock_acquire(&arp_lock);
  struct arp_entry *entry = arp_find_locked(ip);
  /* Keep learned entries usable until an explicit cache/config flush.
   * Expiring one synchronously from the RX worker would make TCP ACK output
   * enter arp_resolve() and wait for a reply that only this worker can process. */
  if (entry && entry->state == ARP_REACHABLE) {
    memcpy(mac, entry->mac, 6);
    found = true;
  }
  spinlock_release(&arp_lock);
  return found;
}

static int arp_send(uint16_t operation, const uint8_t destination_mac[6],
                    uint32_t target_ip, const uint8_t target_mac[6]) {
  struct net_device *dev = net_device_default();
  struct arp_packet packet;
  memset(&packet, 0, sizeof(packet));
  packet.hardware_type = be16(ARP_HTYPE_ETHERNET);
  packet.protocol_type = be16(ETH_TYPE_IPV4);
  packet.hardware_length = 6;
  packet.protocol_length = 4;
  packet.operation = be16(operation);
  memcpy(packet.sender_mac, dev->mac, 6);
  put_ip(packet.sender_ip, config.address);
  if (target_mac)
    memcpy(packet.target_mac, target_mac, 6);
  put_ip(packet.target_ip, target_ip);
  return ethernet_send(destination_mac, ETH_TYPE_ARP, &packet, sizeof(packet));
}

static bool in_net_worker_thread(void) {
  struct thread *cur = sched_get_current();
  return cur && strcmp(cur->comm, "net-worker") == 0;
}

static bool arp_resolve(uint32_t ip, uint8_t mac[6]) {
  static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  static const uint8_t zero[6] = {0};
  if (arp_lookup(ip, mac))
    return true;

  /* A worker thread cannot wait for an ARP reply because it is also the
   * thread that would process it.  From other contexts a short wait keeps
   * the first packet of a connection from being dropped. */
  bool can_wait = !in_net_worker_thread();
  uint8_t attempts = can_wait ? 2 : 1;

  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    spinlock_acquire(&arp_lock);
    struct arp_entry *entry = arp_get_slot_locked(ip);
    entry->ip = ip;
    entry->state = ARP_INCOMPLETE;
    entry->retries = (uint8_t)(attempt + 1);
    entry->updated = lapic_timer_get_ticks();
    spinlock_release(&arp_lock);
    if (arp_send(ARP_OP_REQUEST, broadcast, ip, zero) != 0)
      return false;

    uint64_t deadline = lapic_timer_get_ticks() +
                        (can_wait ? ARP_RESOLVE_WAIT_MS : 0);
    while (lapic_timer_get_ticks() < deadline) {
      if (arp_lookup(ip, mac))
        return true;
      sched_yield();
    }
  }
  return false;
}

static void handle_arp(const uint8_t *payload, size_t length) {
  if (length < sizeof(struct arp_packet))
    return;
  const struct arp_packet *packet = (const struct arp_packet *)payload;
  if (be16(packet->hardware_type) != ARP_HTYPE_ETHERNET ||
      be16(packet->protocol_type) != ETH_TYPE_IPV4 ||
      packet->hardware_length != 6 || packet->protocol_length != 4)
    return;
  uint32_t sender_ip = get_ip(packet->sender_ip);
  uint32_t target_ip = get_ip(packet->target_ip);
  if (sender_ip == config.address &&
      !mac_equal(packet->sender_mac, net_device_default()->mac)) {
    address_conflict = true;
    return;
  }
  arp_learn(sender_ip, packet->sender_mac);
  if (be16(packet->operation) == ARP_OP_REQUEST &&
      target_ip == config.address) {
    arp_send(ARP_OP_REPLY, packet->sender_mac, sender_ip, packet->sender_mac);
  }
}

static int ipv4_send_tos(uint32_t destination, uint8_t protocol, uint8_t tos,
                         const void *payload, size_t payload_length) {
  if (payload_length + sizeof(struct ipv4_header) > NET_MTU_ETHERNET)
    return -1;

  struct net_device *dev = net_device_default();
  if (!dev)
    return -1;

  uint32_t next_hop =
      ((destination & config.netmask) == (config.address & config.netmask))
          ? destination
          : config.gateway;
  uint8_t destination_mac[6];
  if (!arp_resolve(next_hop, destination_mac))
    return -1;

  /* Build the whole Ethernet frame in one buffer: no intermediate packet
   * copy beyond the payload itself. */
  uint8_t frame[NET_FRAME_MAX - 4];
  struct eth_header *eth = (struct eth_header *)frame;
  memcpy(eth->destination, destination_mac, 6);
  memcpy(eth->source, dev->mac, 6);
  eth->type = be16(ETH_TYPE_IPV4);

  struct ipv4_header *header = (struct ipv4_header *)(frame + sizeof(*eth));
  memset(header, 0, sizeof(*header));
  header->version_ihl = 0x45;
  header->dscp_ecn = tos;
  header->total_length =
      be16((uint16_t)(sizeof(*header) + payload_length));
  header->identification = be16(next_ip_id++);
  header->flags_fragment = be16(0x4000);
  header->ttl = 64;
  header->protocol = protocol;
  put_ip(header->source, config.address);
  put_ip(header->destination, destination);
  header->checksum = be16(checksum(header, sizeof(*header)));

  memcpy(frame + sizeof(*eth) + sizeof(*header), payload, payload_length);
  return dev->ops->transmit(dev, frame,
                            sizeof(*eth) + sizeof(*header) + payload_length);
}

static int ipv4_send(uint32_t destination, uint8_t protocol,
                     const void *payload, size_t payload_length) {
  return ipv4_send_tos(destination, protocol, 0, payload, payload_length);
}

static void handle_icmp(uint32_t source, const uint8_t *payload, size_t length) {
  if (length < sizeof(struct icmp_echo) || checksum(payload, length) != 0)
    return;
  const struct icmp_echo *echo = (const struct icmp_echo *)payload;
  if (echo->type == ICMP_ECHO_REPLY)
    return;
  if (echo->type != ICMP_ECHO_REQUEST || echo->code != 0)
    return;
  uint8_t reply[NET_MTU_ETHERNET - sizeof(struct ipv4_header)];
  memcpy(reply, payload, length);
  struct icmp_echo *reply_echo = (struct icmp_echo *)reply;
  reply_echo->type = ICMP_ECHO_REPLY;
  reply_echo->checksum = 0;
  reply_echo->checksum = be16(checksum(reply, length));
  ipv4_send(source, IP_PROTO_ICMP, reply, length);
}

static void send_icmp_port_unreachable(uint32_t src_ip,
                                       const uint8_t *orig_ip_hdr,
                                       size_t orig_ip_len) {
  size_t copy_len = orig_ip_len > 28 ? 28 : orig_ip_len;
  uint8_t icmp_buf[4 + 28];
  icmp_buf[0] = ICMP_PORT_UNREACHABLE_TYPE;
  icmp_buf[1] = ICMP_PORT_UNREACHABLE_CODE;
  icmp_buf[2] = 0; icmp_buf[3] = 0;
  icmp_buf[4] = 0; icmp_buf[5] = 0; icmp_buf[6] = 0; icmp_buf[7] = 0;
  memcpy(icmp_buf + 4, orig_ip_hdr, copy_len);
  size_t total = 4 + copy_len;
  uint16_t cs = checksum(icmp_buf, total);
  icmp_buf[2] = (uint8_t)(cs >> 8);
  icmp_buf[3] = (uint8_t)cs;
  ipv4_send(src_ip, IP_PROTO_ICMP, icmp_buf, total);
}

static void handle_udp(uint32_t src_ip, const uint8_t *ip_hdr,
                        const uint8_t *seg, size_t seg_len) {
  if (seg_len < 8) return;
  uint16_t src_port = (uint16_t)(((uint16_t)seg[0] << 8) | seg[1]);
  uint16_t dst_port = (uint16_t)(((uint16_t)seg[2] << 8) | seg[3]);
  uint16_t udp_len  = (uint16_t)(((uint16_t)seg[4] << 8) | seg[5]);
  if (udp_len < 8 || udp_len > seg_len) return;
  if (udp_handler) {
    udp_handler(src_ip, src_port, dst_port,
                seg + 8, (uint16_t)(udp_len - 8));
  } else if (ip_hdr) {
    send_icmp_port_unreachable(src_ip, ip_hdr,
                               sizeof(struct ipv4_header) + udp_len);
  }
}

/* ---- IPv4 fragment reassembly ------------------------------------------- */

#define IP_REASM_SLOTS 4
#define IP_REASM_MAX_BYTES 65535
#define IP_REASM_TIMEOUT_MS 10000

struct ip_reasm {
  bool used;
  uint32_t src, dst;
  uint16_t id;
  uint8_t proto;
  uint16_t total_len;   /* known once the final fragment arrives */
  size_t received;
  uint8_t *data;
  uint8_t *bitmap;      /* one bit per received byte */
  uint64_t last;
};

static struct ip_reasm reasm[IP_REASM_SLOTS];
static spinlock_t reasm_lock = SPINLOCK_INIT;

static void reasm_release_locked(struct ip_reasm *r) {
  kfree(r->data);
  kfree(r->bitmap);
  memset(r, 0, sizeof(*r));
}

static struct ip_reasm *reasm_get_locked(uint32_t src, uint32_t dst,
                                         uint16_t id, uint8_t proto,
                                         uint64_t now) {
  for (uint32_t i = 0; i < IP_REASM_SLOTS; i++) {
    struct ip_reasm *r = &reasm[i];
    if (r->used && now - r->last > IP_REASM_TIMEOUT_MS)
      reasm_release_locked(r);
  }
  for (uint32_t i = 0; i < IP_REASM_SLOTS; i++) {
    struct ip_reasm *r = &reasm[i];
    if (r->used && r->src == src && r->dst == dst && r->id == id &&
        r->proto == proto)
      return r;
  }
  for (uint32_t i = 0; i < IP_REASM_SLOTS; i++) {
    struct ip_reasm *r = &reasm[i];
    if (r->used)
      continue;
    r->data = kmalloc(IP_REASM_MAX_BYTES);
    r->bitmap = kmalloc(IP_REASM_MAX_BYTES / 8);
    if (!r->data || !r->bitmap) {
      kfree(r->data);
      kfree(r->bitmap);
      memset(r, 0, sizeof(*r));
      return NULL;
    }
    memset(r->bitmap, 0, IP_REASM_MAX_BYTES / 8);
    r->used = true;
    r->src = src;
    r->dst = dst;
    r->id = id;
    r->proto = proto;
    r->last = now;
    return r;
  }
  return NULL;
}

static void handle_ipv4_fragment(const uint8_t *packet,
                                 const struct ipv4_header *hdr,
                                 size_t header_len, uint16_t frag) {
  uint16_t offset = (uint16_t)((frag & 0x1fff) * 8);
  bool more = (frag & 0x2000) != 0;
  uint16_t total = be16(hdr->total_length);
  if (total < header_len)
    return;
  size_t plen = total - header_len;
  if ((size_t)offset + plen > IP_REASM_MAX_BYTES)
    return;

  uint32_t src = get_ip(hdr->source);
  uint32_t dst = get_ip(hdr->destination);
  uint16_t id = be16(hdr->identification);
  uint64_t now = lapic_timer_get_ticks();

  spinlock_acquire(&reasm_lock);
  struct ip_reasm *r = reasm_get_locked(src, dst, id, hdr->protocol, now);
  if (!r) {
    spinlock_release(&reasm_lock);
    return;
  }

  const uint8_t *payload = packet + header_len;
  memcpy(r->data + offset, payload, plen);
  for (size_t i = 0; i < plen; i++) {
    size_t b = (size_t)offset + i;
    uint8_t mask = (uint8_t)(1u << (b & 7));
    if (!(r->bitmap[b >> 3] & mask)) {
      r->bitmap[b >> 3] |= mask;
      r->received++;
    }
  }
  r->last = now;
  if (!more)
    r->total_len = (uint16_t)(offset + plen);

  if (r->total_len && r->received >= r->total_len) {
    uint8_t *data = r->data;
    uint8_t proto = r->proto;
    size_t len = r->total_len;
    r->data = NULL;
    r->bitmap = NULL;
    reasm_release_locked(r);
    spinlock_release(&reasm_lock);

    if (proto == IP_PROTO_UDP)
      handle_udp(src, NULL, data, len);
    else if (proto == IP_PROTO_TCP && tcp_handler)
      tcp_handler(src, dst, data, len);
    else if (proto == IP_PROTO_ICMP)
      handle_icmp(src, data, len);
    kfree(data);
    return;
  }
  spinlock_release(&reasm_lock);
}

static void handle_ipv4(const uint8_t *packet, size_t length) {
  if (length < sizeof(struct ipv4_header))
    return;
  const struct ipv4_header *header = (const struct ipv4_header *)packet;
  uint8_t version = header->version_ihl >> 4;
  size_t header_length = (header->version_ihl & 0x0f) * 4u;
  uint16_t total_length = be16(header->total_length);
  if (version != 4 || header_length < sizeof(*header) ||
      header_length > length || total_length < header_length ||
      total_length > length || checksum(packet, header_length) != 0)
    return;
  uint16_t frag = be16(header->flags_fragment);
  if (frag & 0x3fff) { /* MF set or non-zero offset (DF is 0x4000) */
    handle_ipv4_fragment(packet, header, header_length, frag);
    return;
  }
  uint32_t destination = get_ip(header->destination);
  if (destination != config.address && destination != 0xffffffff)
    return;
  uint32_t source = get_ip(header->source);
  if (header->protocol == IP_PROTO_ICMP) {
    if (icmp_handler)
      icmp_handler(source, destination, packet, total_length);
    handle_icmp(source, packet + header_length,
                total_length - header_length);
  }
  else if (header->protocol == IP_PROTO_UDP)
    handle_udp(source, packet, packet + header_length,
               total_length - header_length);
  else if (header->protocol == IP_PROTO_TCP && tcp_handler)
    tcp_handler(source, destination, packet + header_length,
                total_length - header_length);
}

static void ethernet_receive(struct net_packet *packet) {
  if (!packet || packet->length < sizeof(struct eth_header))
    return;
  const struct eth_header *header = (const struct eth_header *)packet->data;
  bool ipv6_multicast = header->destination[0] == 0x33 &&
                        header->destination[1] == 0x33;
  struct net_device *dev = packet->device;
  if (!dev || (uintptr_t)dev < 0xFFFF800000000000ULL)
    dev = net_device_default();
  if (!dev)
    return;
  if (!mac_equal(header->destination, dev->mac) &&
      !mac_broadcast(header->destination) && !ipv6_multicast)
    return;
  const uint8_t *payload = packet->data + sizeof(*header);
  size_t length = packet->length - sizeof(*header);
  uint16_t type = be16(header->type);
  if (type == ETH_TYPE_ARP)
    handle_arp(payload, length);
  else if (type == ETH_TYPE_IPV4)
    handle_ipv4(payload, length);
  else if (type == ETH_TYPE_IPV6)
    ipv6_receive(header->source, payload, length);
}

bool net_phase4_init(void) {
  if (!net_device_default())
    return false;
  spinlock_init(&arp_lock);
  memset(arp_cache, 0, sizeof(arp_cache));
  spinlock_init(&reasm_lock);
  memset(reasm, 0, sizeof(reasm));
  config.address = IPV4_ADDR(10, 0, 2, 15);
  config.netmask = IPV4_ADDR(255, 255, 255, 0);
  config.gateway = IPV4_ADDR(10, 0, 2, 2);
  address_conflict = false;
  net_set_rx_handler(ethernet_receive);
  klog_puts("[NET] Ethernet, ARP and IPv4 initialized\n");
  return true;
}

const struct ipv4_config *ipv4_get_config(void) { return &config; }

void ipv4_arp_flush(void) {
  spinlock_acquire(&arp_lock);
  memset(arp_cache, 0, sizeof(arp_cache));
  spinlock_release(&arp_lock);
}

void ipv4_apply_config(const struct ipv4_config *cfg) {
  if (!cfg) return;
  spinlock_acquire(&arp_lock);
  memset(arp_cache, 0, sizeof(arp_cache));
  config.address = cfg->address;
  config.netmask = cfg->netmask ? cfg->netmask : 0xffffff00u;
  config.gateway = cfg->gateway;
  address_conflict = false;
  spinlock_release(&arp_lock);
}

int ipv4_send_raw(uint32_t dst_ip, uint8_t proto,
                  const void *payload, size_t payload_len) {
  return ipv4_send(dst_ip, proto, payload, payload_len);
}

int ipv4_send_raw_tos(uint32_t dst_ip, uint8_t proto, uint8_t tos,
                      const void *payload, size_t payload_len) {
  return ipv4_send_tos(dst_ip, proto, tos, payload, payload_len);
}

void ipv4_set_udp_handler(void (*handler)(uint32_t, uint16_t, uint16_t,
                                          const uint8_t *, uint16_t)) {
  udp_handler = handler;
}

void ipv4_set_tcp_handler(void (*handler)(uint32_t, uint32_t,
                                          const uint8_t *, size_t)) {
  tcp_handler = handler;
}

void ipv4_set_icmp_handler(void (*handler)(uint32_t, uint32_t,
                                           const uint8_t *, size_t)) {
  icmp_handler = handler;
}
