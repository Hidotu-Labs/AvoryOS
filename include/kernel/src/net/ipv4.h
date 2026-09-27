#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPV4_ADDR(a, b, c, d)                                               \
  (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) |   \
   (uint32_t)(d))

struct ipv4_config {
  uint32_t address;
  uint32_t netmask;
  uint32_t gateway;
};

bool net_phase4_init(void);
const struct ipv4_config *ipv4_get_config(void);


void ipv4_apply_config(const struct ipv4_config *cfg);

void ipv4_arp_flush(void);

int ipv4_send_raw(uint32_t dst_ip, uint8_t proto,
                  const void *payload, size_t payload_len);

/* Same, with an explicit TOS/DSCP byte (UDP sockets may request one). */
int ipv4_send_raw_tos(uint32_t dst_ip, uint8_t proto, uint8_t tos,
                  const void *payload, size_t payload_len);

void ipv4_set_udp_handler(void (*handler)(uint32_t src_ip, uint16_t src_port,
                                          uint16_t dst_port,const uint8_t *payload,uint16_t length));

void ipv4_set_tcp_handler(void (*handler)(uint32_t src_ip, uint32_t dst_ip,
                                          const uint8_t *segment,size_t length));
void ipv4_set_icmp_handler(void (*handler)(uint32_t src_ip, uint32_t dst_ip,
                                         const uint8_t *packet,size_t length));

#endif
