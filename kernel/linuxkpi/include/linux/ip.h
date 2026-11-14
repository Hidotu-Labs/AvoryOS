#ifndef AVORY_LINUXKPI_IP_H
#define AVORY_LINUXKPI_IP_H

#ifdef LINUXKPI_NET_FRONTEND
#include <linux/skbuff.h>
#include <uapi/linux/ip.h>

static inline struct iphdr *ip_hdr(const struct sk_buff *skb) {
  return (struct iphdr *)skb_network_header(skb);
}
static inline unsigned int ip_hdrlen(const struct sk_buff *skb) {
  return ip_hdr(skb)->ihl * 4U;
}
#else
#include_next <linux/ip.h>
#endif

#endif
