#ifndef AVORY_LINUXKPI_IP6_CHECKSUM_H
#define AVORY_LINUXKPI_IP6_CHECKSUM_H

#ifdef LINUXKPI_NET_FRONTEND
#include <linux/in.h>
#include <linux/tcp.h>
#include <uapi/linux/ipv6.h>
#include <asm/byteorder.h>

/* Seed the TCP checksum with the unfolded IPv6 pseudoheader sum. Hardware
 * adds each segment's TCP length and payload. Load bytes explicitly because
 * Ethernet payload headers are not guaranteed to be aligned. */
static inline void tcp_v6_gso_csum_prep(struct sk_buff *skb) {
  struct ipv6hdr *ip = (struct ipv6hdr *)skb_network_header(skb);
  struct tcphdr *tcp = tcp_hdr(skb);
  const unsigned char *source = ip->saddr.s6_addr;
  const unsigned char *dest = ip->daddr.s6_addr;
  unsigned int sum = IPPROTO_TCP;

  for (unsigned int i = 0; i < 16; i += 2) {
    sum += ((unsigned int)source[i] << 8) | source[i + 1];
    sum += ((unsigned int)dest[i] << 8) | dest[i + 1];
  }
  while (sum >> 16)
    sum = (sum & 0xffffU) + (sum >> 16);
  ip->payload_len = 0;
  tcp->check = (__force __sum16)cpu_to_be16((u16)sum);
}
#else
#include_next <net/ip6_checksum.h>
#endif

#endif
