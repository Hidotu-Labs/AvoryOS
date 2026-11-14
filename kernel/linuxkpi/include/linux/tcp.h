#ifndef AVORY_LINUXKPI_TCP_H
#define AVORY_LINUXKPI_TCP_H

#ifdef LINUXKPI_NET_FRONTEND
/* Packet header access for Ethernet drivers. TCP socket state lives in the
 * native network stack and is not part of this driver interface. */
#include <linux/skbuff.h>
#include <uapi/linux/tcp.h>

static inline struct tcphdr *tcp_hdr(const struct sk_buff *skb) {
  return (struct tcphdr *)skb_transport_header(skb);
}

static inline unsigned int tcp_hdrlen(const struct sk_buff *skb) {
  return tcp_hdr(skb)->doff * 4U;
}
#else
#include_next <linux/tcp.h>
#endif

#endif
