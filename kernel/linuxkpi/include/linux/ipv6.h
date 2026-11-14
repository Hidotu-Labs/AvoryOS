#ifndef AVORY_LINUXKPI_IPV6_H
#define AVORY_LINUXKPI_IPV6_H

#ifdef LINUXKPI_NET_FRONTEND
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <uapi/linux/ipv6.h>

static inline struct ipv6hdr *ipv6_hdr(const struct sk_buff *skb) {
  return (struct ipv6hdr *)skb_network_header(skb);
}
#else
#include_next <linux/ipv6.h>
#endif

#endif
