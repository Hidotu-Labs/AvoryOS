#ifndef LINUXKPI_SKBUFF_OVERLAY_H
#define LINUXKPI_SKBUFF_OVERLAY_H

/* The LinuxKPI frontend provides a deliberately linear skb contract. Imported
 * drivers keep the full upstream definition until scatter-gather/page-pool
 * closure is completed in R3. */
#ifdef LINUXKPI_NET_FRONTEND
#include <linux/gfp.h>
#include <linux/errno.h>
#define _LINUX_SKBUFF_H
#include <linux/refcount.h>
#include <linux/types.h>

typedef unsigned int sk_buff_data_t;
#define SKB_DATA_ALIGN(size) (((size) + 15U) & ~15U)
#define MAX_SKB_FRAGS 17
#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define CHECKSUM_COMPLETE 2
#define CHECKSUM_PARTIAL 3
#define SKB_GSO_TCPV4 (1U << 0)
#define SKB_GSO_TCPV6 (1U << 1)
struct net_device;
struct sock;
struct napi_struct;
enum skb_drop_reason { SKB_DROP_REASON_NOT_SPECIFIED = 0 };
typedef struct skb_frag { void *address; unsigned int size; } skb_frag_t;
struct skb_shared_info {
  unsigned char nr_frags;
  unsigned short gso_size;
  unsigned short gso_segs;
  unsigned int gso_type;
  skb_frag_t frags[MAX_SKB_FRAGS];
  atomic_t dataref;
};
struct sk_buff {
  struct sk_buff *next, *prev;
  struct net_device *dev;
  __be16 protocol;
  __be16 vlan_proto;
  u16 vlan_tci;
  bool vlan_present;
  unsigned int len;
  unsigned int truesize;
  unsigned int queue_mapping;
  unsigned char ip_summed, pkt_type;
  unsigned short mac_header;
  unsigned short network_header;
  unsigned short transport_header;
  sk_buff_data_t tail, end;
  unsigned char *head, *data;
  refcount_t users;
};

struct sk_buff *__alloc_skb(unsigned int, gfp_t, int, int);
void __kfree_skb(struct sk_buff *);
void sk_skb_reason_drop(struct sock *, struct sk_buff *, enum skb_drop_reason);
static inline struct sk_buff *alloc_skb(unsigned int size, gfp_t flags) {
  return __alloc_skb(size, flags, 0, -1);
}
static inline void kfree_skb_reason(struct sk_buff *skb,
                                    enum skb_drop_reason reason) {
  sk_skb_reason_drop(NULL, skb, reason);
}
static inline void kfree_skb(struct sk_buff *skb) {
  kfree_skb_reason(skb, SKB_DROP_REASON_NOT_SPECIFIED);
}
static inline void *skb_put(struct sk_buff *skb, unsigned int length) {
  if (length > skb->end - skb->tail) return NULL;
  void *result = skb->head + skb->tail;
  skb->tail += length; skb->len += length; return result;
}
static inline void *skb_push(struct sk_buff *skb, unsigned int length) {
  if (length > (unsigned int)(skb->data - skb->head)) return NULL;
  skb->data -= length; skb->len += length; return skb->data;
}
static inline void *skb_pull(struct sk_buff *skb, unsigned int length) {
  if (length > skb->len) return NULL;
  skb->data += length; skb->len -= length; return skb->data;
}
static inline void skb_reserve(struct sk_buff *skb, unsigned int length) {
  if (!skb->len && length <= skb->end - skb->tail) {
    skb->data += length; skb->tail += length;
  }
}
static inline unsigned char *skb_tail_pointer(const struct sk_buff *skb) {
  return skb->head + skb->tail;
}
static inline unsigned int skb_tailroom(const struct sk_buff *skb) {
  return skb->end - skb->tail;
}
static inline unsigned int skb_headlen(const struct sk_buff *skb) {
  return skb->len;
}
static inline void skb_reset_mac_header(struct sk_buff *skb) {
  skb->mac_header = (unsigned short)(skb->data - skb->head);
}
static inline unsigned char *skb_network_header(const struct sk_buff *skb) {
  return skb->head + skb->network_header;
}
static inline unsigned char *skb_transport_header(const struct sk_buff *skb) {
  return skb->head + skb->transport_header;
}
static inline int skb_network_offset(const struct sk_buff *skb) {
  return (int)(skb_network_header(skb) - skb->data);
}
static inline int skb_transport_offset(const struct sk_buff *skb) {
  return (int)(skb_transport_header(skb) - skb->data);
}
static inline void *skb_header_pointer(const struct sk_buff *skb, int offset,
                                       int len, void *buffer) {
  (void)buffer;
  if (offset < 0 || len < 0 || (unsigned int)offset > skb->len ||
      (unsigned int)len > skb->len - (unsigned int)offset)
    return NULL;
  return skb->data + offset;
}
static inline bool skb_network_header_was_set(const struct sk_buff *skb) {
  return skb->network_header != (unsigned short)~0U;
}
static inline bool skb_transport_header_was_set(const struct sk_buff *skb) {
  return skb->transport_header != (unsigned short)~0U;
}
static inline bool skb_mac_header_was_set(const struct sk_buff *skb) {
  return skb->mac_header != (unsigned short)~0U;
}
static inline unsigned char *skb_mac_header(const struct sk_buff *skb) {
  return skb->head + skb->mac_header;
}
static inline struct skb_shared_info *skb_shinfo(const struct sk_buff *skb) {
  return (struct skb_shared_info *)(skb->head + skb->end);
}
static inline bool skb_is_gso(const struct sk_buff *skb) {
  return skb_shinfo(skb)->gso_size != 0;
}
static inline int skb_cow_head(struct sk_buff *skb, unsigned int headroom) {
  /* This frontend has no cloned buffers or head reallocation. */
  return headroom <= (unsigned int)(skb->data - skb->head) &&
                 atomic_read(&skb_shinfo(skb)->dataref) == 1
             ? 0 : -ENOMEM;
}
static inline void *skb_frag_address(const skb_frag_t *frag) {
  return frag->address;
}
static inline unsigned int skb_frag_size(const skb_frag_t *frag) {
  return frag->size;
}
static inline struct sk_buff *napi_alloc_skb(struct napi_struct *napi,
                                             unsigned int size) {
  (void)napi;
  return alloc_skb(size, GFP_ATOMIC);
}
static inline void skb_copy_to_linear_data(const struct sk_buff *skb,
                                           const void *from, unsigned int len) {
  __builtin_memcpy(skb->data, from, len);
}
static inline int __skb_put_padto(struct sk_buff *skb, unsigned int len,
                                  bool free_on_error) {
  if (skb->len < len) {
    unsigned int padding = len - skb->len;
    void *dst = skb_put(skb, padding);
    if (!dst) {
      if (free_on_error) kfree_skb(skb);
      return -ENOMEM;
    }
    __builtin_memset(dst, 0, padding);
  }
  return 0;
}
static inline void skb_checksum_none_assert(const struct sk_buff *skb) {
  /* DEBUG_NET_WARN_ON_ONCE is disabled in this LinuxKPI profile. */
  (void)skb;
}
#else
#include_next <linux/skbuff.h>
#endif

#endif
