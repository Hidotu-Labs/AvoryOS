#ifndef AVORY_LINUXKPI_UDP_H
#define AVORY_LINUXKPI_UDP_H

#ifdef LINUXKPI_NET_FRONTEND
#include <linux/skbuff.h>
#include <uapi/linux/udp.h>

static inline struct udphdr *udp_hdr(const struct sk_buff *skb) {
  return (struct udphdr *)skb_transport_header(skb);
}
#else
#include_next <linux/udp.h>
#endif

#endif
