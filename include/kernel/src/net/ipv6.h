#ifndef NET_IPV6_H
#define NET_IPV6_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ipv6_config {
  uint8_t link_local[16];
  uint8_t global[16];
  uint8_t router[16];
  uint8_t prefix_length;
  bool global_valid;
};

bool net_phase11_init(void);
const struct ipv6_config *ipv6_get_config(void);
void ipv6_receive(const uint8_t source_mac[6], const uint8_t *packet,
                  size_t length);
int ipv6_send_raw(const uint8_t destination[16], uint8_t next_header,
                  const void *payload, size_t length);
void ipv6_set_udp_handler(void (*handler)(const uint8_t source[16],
                                          const uint8_t destination[16],
                                          const uint8_t *segment,
                                          size_t length));
void ipv6_set_tcp_handler(void (*handler)(const uint8_t source[16],
                                          const uint8_t destination[16],
                                          const uint8_t *segment,
                                          size_t length));

#endif
