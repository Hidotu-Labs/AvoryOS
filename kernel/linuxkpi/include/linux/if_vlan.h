#ifndef AVORY_LINUXKPI_IF_VLAN_H
#define AVORY_LINUXKPI_IF_VLAN_H

#ifdef LINUXKPI_NET_FRONTEND
#include <asm/byteorder.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <uapi/linux/if_vlan.h>

#define VLAN_HLEN 4
#define VLAN_ETH_HLEN (ETH_HLEN + VLAN_HLEN)
#define VLAN_MAX_DEPTH 8

struct vlan_hdr {
  __be16 h_vlan_TCI;
  __be16 h_vlan_encapsulated_proto;
};

static inline bool eth_type_vlan(__be16 type) {
  return type == cpu_to_be16(ETH_P_8021Q) ||
         type == cpu_to_be16(ETH_P_8021AD);
}

static inline bool skb_vlan_tag_present(const struct sk_buff *skb) {
  return skb->vlan_present;
}

static inline u16 skb_vlan_tag_get(const struct sk_buff *skb) {
  return skb->vlan_tci;
}

static inline void __vlan_hwaccel_put_tag(struct sk_buff *skb, __be16 proto,
                                        u16 tci) {
  skb->vlan_proto = proto;
  skb->vlan_tci = tci;
  skb->vlan_present = true;
}

static inline __be16 vlan_get_protocol(const struct sk_buff *skb) {
  __be16 type = skb->protocol;
  if (!eth_type_vlan(type))
    return type;
  int offset = ETH_HLEN;
  if (skb_mac_header_was_set(skb))
    offset += (int)(skb_mac_header(skb) - skb->data);
  for (unsigned int depth = 0; depth < VLAN_MAX_DEPTH; depth++) {
    struct vlan_hdr buffer;
    const struct vlan_hdr *header =
        skb_header_pointer(skb, offset, sizeof(buffer), &buffer);
    if (!header)
      return 0;
    type = header->h_vlan_encapsulated_proto;
    if (!eth_type_vlan(type))
      return type;
    offset += VLAN_HLEN;
  }
  return 0;
}

static inline netdev_features_t vlan_features_check(const struct sk_buff *skb,
                                                     netdev_features_t features) {
  /* Inline tags travel as frame bytes. This frontend has no tag offloads. */
  (void)skb;
  return features & NETIF_F_HIGHDMA;
}
#else
#include_next <linux/if_vlan.h>
#endif

#endif
