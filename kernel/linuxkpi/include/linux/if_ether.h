#ifndef LINUXKPI_IF_ETHER_OVERLAY_H
#define LINUXKPI_IF_ETHER_OVERLAY_H

/* Keep the supported Ethernet frontend out of the full upstream skb header
 * closure. Other imported objects continue through to the pinned header. */
#ifdef LINUXKPI_NET_FRONTEND
#define _LINUX_IF_ETHER_H
#include <linux/skbuff.h>
#include <uapi/linux/if_ether.h>

#define MAC_ADDR_STR_LEN (3 * ETH_ALEN - 1)
static inline struct ethhdr *eth_hdr(const struct sk_buff *skb) {
  return (struct ethhdr *)skb_mac_header(skb);
}
static inline struct ethhdr *skb_eth_hdr(const struct sk_buff *skb) {
  return (struct ethhdr *)skb->data;
}
static inline struct ethhdr *inner_eth_hdr(const struct sk_buff *skb) {
  return (struct ethhdr *)skb_mac_header(skb);
}
#else
#include_next <linux/if_ether.h>
#endif

#endif
