#include "net/ipv6.h"
#include "console/klog.h"
#include "lib/string.h"
#include "net/core.h"
#include "apic/lapic_timer.h"
#include "sched/sched.h"

#define ETH_TYPE_IPV6 0x86dd
#define IPV6_NEXT_ICMP 58
#define IPV6_NEXT_TCP 6
#define IPV6_NEXT_UDP 17
#define ICMP6_RS 133
#define ICMP6_RA 134
#define ICMP6_NS 135
#define ICMP6_NA 136
#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY 129
#define NEIGHBOR_COUNT 16

struct ipv6_header {
  uint32_t version_flow;
  uint16_t payload_length;
  uint8_t next_header;
  uint8_t hop_limit;
  uint8_t source[16];
  uint8_t destination[16];
} __attribute__((packed));

struct neighbor {
  uint8_t address[16];
  uint8_t mac[6];
  bool used;
};

static struct ipv6_config config;
static struct neighbor neighbors[NEIGHBOR_COUNT];
static uint32_t next_neighbor;
static void (*udp_handler)(const uint8_t[16], const uint8_t[16],
                           const uint8_t *, size_t);
static void (*tcp_handler)(const uint8_t[16], const uint8_t[16],
                           const uint8_t *, size_t);

static uint16_t be16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static bool addr_equal(const uint8_t a[16], const uint8_t b[16]) {
  return memcmp(a, b, 16) == 0;
}
static bool addr_zero(const uint8_t a[16]) {
  static const uint8_t zero[16];
  return addr_equal(a, zero);
}
static bool addr_multicast(const uint8_t a[16]) { return a[0] == 0xff; }

static uint32_t checksum_add(uint32_t sum, const uint8_t *p, size_t n) {
  while (n > 1) {
    sum += ((uint16_t)p[0] << 8) | p[1];
    p += 2;
    n -= 2;
  }
  if (n)
    sum += (uint16_t)p[0] << 8;
  return sum;
}

static uint16_t icmp6_checksum(const uint8_t source[16],
                               const uint8_t destination[16],
                               const uint8_t *payload, size_t length) {
  uint32_t sum = checksum_add(0, source, 16);
  sum = checksum_add(sum, destination, 16);
  sum += (uint32_t)(length >> 16) + (uint16_t)length + IPV6_NEXT_ICMP;
  sum = checksum_add(sum, payload, length);
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static int ethernet_send(const uint8_t mac[6], const void *payload,
                         size_t length) {
  struct net_device *dev = net_device_default();
  if (!dev || length + 14 > NET_FRAME_MAX - 4)
    return -1;
  uint8_t frame[NET_FRAME_MAX - 4];
  memcpy(frame, mac, 6);
  memcpy(frame + 6, dev->mac, 6);
  frame[12] = (uint8_t)(ETH_TYPE_IPV6 >> 8);
  frame[13] = (uint8_t)ETH_TYPE_IPV6;
  memcpy(frame + 14, payload, length);
  return dev->ops->transmit(dev, frame, length + 14);
}

static void multicast_mac(const uint8_t address[16], uint8_t mac[6]) {
  mac[0] = 0x33; mac[1] = 0x33;
  memcpy(mac + 2, address + 12, 4);
}

static void neighbor_learn(const uint8_t address[16], const uint8_t mac[6]) {
  if (addr_zero(address) || addr_multicast(address))
    return;
  for (uint32_t i = 0; i < NEIGHBOR_COUNT; i++) {
    if (neighbors[i].used && addr_equal(neighbors[i].address, address)) {
      memcpy(neighbors[i].mac, mac, 6);
      return;
    }
  }
  struct neighbor *n = &neighbors[next_neighbor++ % NEIGHBOR_COUNT];
  memcpy(n->address, address, 16);
  memcpy(n->mac, mac, 6);
  n->used = true;
}

static bool neighbor_lookup(const uint8_t address[16], uint8_t mac[6]) {
  for (uint32_t i = 0; i < NEIGHBOR_COUNT; i++) {
    if (neighbors[i].used && addr_equal(neighbors[i].address, address)) {
      memcpy(mac, neighbors[i].mac, 6);
      return true;
    }
  }
  return false;
}

static int ipv6_send_to(const uint8_t source[16], const uint8_t destination[16],
                        const uint8_t mac[6], uint8_t next_header,
                        uint8_t *payload, size_t length) {
  if (length + sizeof(struct ipv6_header) > NET_MTU_ETHERNET)
    return -1;
  uint8_t packet[NET_MTU_ETHERNET];
  struct ipv6_header *h = (struct ipv6_header *)packet;
  memset(h, 0, sizeof(*h));
  packet[0] = 0x60;
  h->payload_length = be16((uint16_t)length);
  h->next_header = next_header;
  h->hop_limit = next_header == IPV6_NEXT_ICMP ? 255 : 64;
  memcpy(h->source, source, 16);
  memcpy(h->destination, destination, 16);
  if (next_header == IPV6_NEXT_ICMP && length >= 4) {
    payload[2] = payload[3] = 0;
    uint16_t csum = icmp6_checksum(source, destination, payload, length);
    payload[2] = (uint8_t)(csum >> 8);
    payload[3] = (uint8_t)csum;
  }
  memcpy(packet + sizeof(*h), payload, length);
  return ethernet_send(mac, packet, sizeof(*h) + length);
}

static bool local_address(const uint8_t address[16]) {
  return addr_equal(address, config.link_local) ||
         (config.global_valid && addr_equal(address, config.global));
}

static void send_na(const uint8_t destination[16], const uint8_t mac[6],
                    const uint8_t target[16]) {
  uint8_t na[32];
  memset(na, 0, sizeof(na));
  na[0] = ICMP6_NA;
  na[4] = 0x60; /* solicited + override */
  memcpy(na + 8, target, 16);
  na[24] = 2; na[25] = 1;
  memcpy(na + 26, net_device_default()->mac, 6);
  ipv6_send_to(target, destination, mac, IPV6_NEXT_ICMP, na, sizeof(na));
}

static bool resolve_neighbor(const uint8_t target[16], uint8_t mac[6]) {
  if (addr_multicast(target)) {
    multicast_mac(target, mac);
    return true;
  }
  if (neighbor_lookup(target, mac))
    return true;

  uint8_t solicited[16] = {0xff, 0x02};
  solicited[11] = 0x01;
  solicited[12] = 0xff;
  memcpy(solicited + 13, target + 13, 3);
  uint8_t multicast[6], ns[32];
  multicast_mac(solicited, multicast);
  memset(ns, 0, sizeof(ns));
  ns[0] = ICMP6_NS;
  memcpy(ns + 8, target, 16);
  ns[24] = 1; ns[25] = 1;
  memcpy(ns + 26, net_device_default()->mac, 6);
  for (int attempt = 0; attempt < 3; attempt++) {
    ipv6_send_to(config.link_local, solicited, multicast, IPV6_NEXT_ICMP,
                 ns, sizeof(ns));
    uint64_t deadline = lapic_timer_get_ticks() + 500;
    while (lapic_timer_get_ticks() < deadline) {
      if (neighbor_lookup(target, mac))
        return true;
      sched_yield();
    }
  }
  return false;
}

int ipv6_send_raw(const uint8_t destination[16], uint8_t next_header,
                  const void *payload, size_t length) {
  if (!destination || !payload)
    return -22;
  const uint8_t *source = config.link_local;
  const uint8_t *next_hop = destination;
  bool link_local = destination[0] == 0xfe &&
                    (destination[1] & 0xc0) == 0x80;
  if (!link_local && !addr_multicast(destination)) {
    bool dest_global = (destination[0] & 0xe0) == 0x20;
    bool cfg_global = config.global_valid && ((config.global[0] & 0xe0) == 0x20);
    if (dest_global && !cfg_global)
      return -101; // ENETUNREACH
    if (!config.global_valid)
      return -101;
    source = config.global;
    if (memcmp(destination, config.global, 8) != 0 && !addr_zero(config.router))
      next_hop = config.router;
  }
  uint8_t mac[6];
  if (!resolve_neighbor(next_hop, mac))
    return -113;
  uint8_t copy[NET_MTU_ETHERNET - sizeof(struct ipv6_header)];
  if (length > sizeof(copy))
    return -90;
  memcpy(copy, payload, length);
  return ipv6_send_to(source, destination, mac, next_header, copy, length);
}

static void handle_ra(const uint8_t source[16], const uint8_t source_mac[6],
                      const uint8_t *icmp, size_t length) {
  if (length < 16)
    return;
  neighbor_learn(source, source_mac);
  memcpy(config.router, source, 16);
  size_t offset = 16;
  while (offset + 2 <= length) {
    size_t option_length = (size_t)icmp[offset + 1] * 8;
    if (!option_length || offset + option_length > length)
      return;
    if (icmp[offset] == 3 && option_length == 32 && icmp[offset + 2] == 64 &&
        (icmp[offset + 3] & 0x40)) {
      const uint8_t *prefix = icmp + offset + 16;
      // Only 2000::/3 addresses are global unicast routable to the internet (RFC 4291)
      if ((prefix[0] & 0xe0) == 0x20) {
        memcpy(config.global, prefix, 8);
        memcpy(config.global + 8, config.link_local + 8, 8);
        config.prefix_length = 64;
        config.global_valid = true;
      }
    }
    offset += option_length;
  }
}

void ipv6_receive(const uint8_t source_mac[6], const uint8_t *packet,
                  size_t length) {
  if (!packet || length < sizeof(struct ipv6_header) || packet[0] >> 4 != 6)
    return;
  const struct ipv6_header *h = (const struct ipv6_header *)packet;
  size_t payload_length = be16(h->payload_length);
  if (payload_length > length - sizeof(*h))
    return;
  if (!local_address(h->destination) && !addr_multicast(h->destination))
    return;
  const uint8_t *icmp = packet + sizeof(*h);
  neighbor_learn(h->source, source_mac);
  if (h->next_header == IPV6_NEXT_UDP) {
    if (udp_handler)
      udp_handler(h->source, h->destination, icmp, payload_length);
    return;
  }
  if (h->next_header == IPV6_NEXT_TCP) {
    if (tcp_handler)
      tcp_handler(h->source, h->destination, icmp, payload_length);
    return;
  }
  if (h->next_header != IPV6_NEXT_ICMP)
    return;
  if (payload_length < 4 ||
      icmp6_checksum(h->source, h->destination, icmp, payload_length) != 0)
    return;

  if (icmp[0] == ICMP6_ECHO_REQUEST && local_address(h->destination)) {
    uint8_t reply[NET_MTU_ETHERNET - sizeof(*h)];
    memcpy(reply, icmp, payload_length);
    reply[0] = ICMP6_ECHO_REPLY;
    ipv6_send_to(h->destination, h->source, source_mac, IPV6_NEXT_ICMP,
                 reply, payload_length);
  } else if (icmp[0] == ICMP6_NS && payload_length >= 24 &&
             local_address(icmp + 8)) {
    send_na(h->source, source_mac, icmp + 8);
  } else if (icmp[0] == ICMP6_NA && payload_length >= 24) {
    neighbor_learn(icmp + 8, source_mac);
  } else if (icmp[0] == ICMP6_RA && h->hop_limit == 255) {
    handle_ra(h->source, source_mac, icmp, payload_length);
  }
}

static void send_router_solicitation(void) {
  static const uint8_t all_routers[16] =
      {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
  uint8_t mac[6], rs[16];
  multicast_mac(all_routers, mac);
  memset(rs, 0, sizeof(rs));
  rs[0] = ICMP6_RS;
  rs[8] = 1; rs[9] = 1;
  memcpy(rs + 10, net_device_default()->mac, 6);
  ipv6_send_to(config.link_local, all_routers, mac, IPV6_NEXT_ICMP,
               rs, sizeof(rs));
}

bool net_phase11_init(void) {
  struct net_device *dev = net_device_default();
  if (!dev)
    return false;
  memset(&config, 0, sizeof(config));
  memset(neighbors, 0, sizeof(neighbors));
  config.link_local[0] = 0xfe;
  config.link_local[1] = 0x80;
  config.link_local[8] = dev->mac[0] ^ 0x02;
  config.link_local[9] = dev->mac[1];
  config.link_local[10] = dev->mac[2];
  config.link_local[11] = 0xff;
  config.link_local[12] = 0xfe;
  config.link_local[13] = dev->mac[3];
  config.link_local[14] = dev->mac[4];
  config.link_local[15] = dev->mac[5];

  send_router_solicitation();
  klog_puts("[NET] IPv6 link-local, NDP and SLAAC initialized\n");
  return true;
}

const struct ipv6_config *ipv6_get_config(void) { return &config; }

void ipv6_set_udp_handler(void (*handler)(const uint8_t[16], const uint8_t[16],
                                          const uint8_t *, size_t)) {
  udp_handler = handler;
}

void ipv6_set_tcp_handler(void (*handler)(const uint8_t[16], const uint8_t[16],
                                          const uint8_t *, size_t)) {
  tcp_handler = handler;
}
