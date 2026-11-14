#ifndef LINUXKPI_NETDEVICE_OVERLAY_H
#define LINUXKPI_NETDEVICE_OVERLAY_H

/* The frontend is compiled against this explicit supported slice. Imported
 * drivers continue to see the pinned upstream header until their broader
 * R3 dependency closure is complete. */
#ifdef LINUXKPI_NET_FRONTEND
#define _LINUX_NETDEVICE_H
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/time64.h>
#include <linux/device.h>
#include <uapi/linux/if.h>
#include <uapi/linux/if_arp.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_link.h>
#include <linux/mutex.h>
#include <linux/netdev_features.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#define NET_NAME_ENUM 1
#define NAPI_POLL_WEIGHT 64
#define NETDEV_ALIGN 32
int net_ratelimit(void);
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 0x10
#define __LINK_STATE_NOCARRIER 0
#define __QUEUE_STATE_DRV_XOFF 0
#define NAPI_STATE_SCHED 0
#define NAPI_STATE_MISSED 1
#define NAPI_STATE_DISABLE 2
#define IFF_LIVE_ADDR_CHANGE (1UL << 15)
#define IFF_ALLMULTI (1U << 9)
#define PACKET_MULTICAST 2
#define NET_ADDR_RANDOM 1
#define NETDEV_PCPU_STAT_TSTATS 1
#define __LINK_STATE_START 1
#define __LINK_STATE_PRESENT 2
#define __LINK_STATE_TESTING 5

enum gro_result { GRO_MERGED, GRO_MERGED_FREE, GRO_HELD, GRO_NORMAL, GRO_CONSUMED };
typedef enum gro_result gro_result_t;

struct sk_buff;
struct net_device;
struct napi_struct;
struct rtnl_link_stats64;
struct ethtool_ops;
struct ethtool_netdev_state;
struct phy_device;
struct phy_link_topology;
struct sfp_bus;
struct hwtstamp_provider;
struct ifreq;
enum skb_drop_reason;
struct gro_node { unsigned long reserved; };
typedef int netdev_tx_t;

struct netdev_hw_addr { struct list_head list; unsigned char addr[ETH_ALEN]; };

struct net_device_ops {
  int (*ndo_init)(struct net_device *);
  void (*ndo_uninit)(struct net_device *);
  int (*ndo_open)(struct net_device *);
  int (*ndo_stop)(struct net_device *);
  netdev_tx_t (*ndo_start_xmit)(struct sk_buff *, struct net_device *);
  void (*ndo_get_stats64)(struct net_device *, struct rtnl_link_stats64 *);
  netdev_features_t (*ndo_features_check)(struct sk_buff *, struct net_device *,
                                          netdev_features_t);
  netdev_features_t (*ndo_fix_features)(struct net_device *, netdev_features_t);
  int (*ndo_set_features)(struct net_device *, netdev_features_t);
  int (*ndo_validate_addr)(struct net_device *);
  int (*ndo_change_mtu)(struct net_device *, int);
  int (*ndo_set_mac_address)(struct net_device *, void *);
  int (*ndo_eth_ioctl)(struct net_device *, struct ifreq *, int);
  void (*ndo_set_rx_mode)(struct net_device *);
  void (*ndo_tx_timeout)(struct net_device *, unsigned int);
};

struct netdev_queue {
  struct net_device *dev;
  spinlock_t _xmit_lock;
  int xmit_lock_owner;
  unsigned long state;
};

struct net_device_stats {
  unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes;
  unsigned long rx_errors, tx_errors, rx_dropped, tx_dropped;
  unsigned long multicast, collisions;
  unsigned long rx_length_errors, rx_over_errors, rx_crc_errors;
  unsigned long rx_frame_errors, rx_fifo_errors, rx_missed_errors;
  unsigned long tx_aborted_errors, tx_carrier_errors, tx_fifo_errors;
  unsigned long tx_heartbeat_errors, tx_window_errors;
  unsigned long rx_compressed, tx_compressed;
};

/* The Avory frontend has one CPU-local netdev instance per registered device.
 * Atomic counters preserve updates from IRQ and NAPI contexts without Linux's
 * per-CPU allocator or u64_stats_sync machinery. */
struct pcpu_sw_netstats { u64 rx_packets, rx_bytes, tx_packets, tx_bytes; };

struct net_device {
  struct device dev;
  char name[IFNAMSIZ];
  const struct net_device_ops *netdev_ops;
  unsigned int mtu;
  unsigned int max_mtu;
  netdev_features_t features, hw_features, wanted_features, vlan_features;
  unsigned long priv_flags;
  unsigned char addr_assign_type;
  unsigned int pcpu_stat_type;
  struct net_device_stats stats;
  struct pcpu_sw_netstats tstats_storage;
  struct pcpu_sw_netstats *tstats;
  const struct ethtool_ops *ethtool_ops;
  struct ethtool_netdev_state *ethtool;
  struct phy_device *phydev;
  struct phy_link_topology *link_topo;
  struct sfp_bus *sfp_bus;
  struct hwtstamp_provider *hwprov;
  unsigned int gso_max_size, gso_max_segs;
  unsigned int gro_flush_timeout;
  unsigned int napi_defer_hard_irqs;
  unsigned int tx_queue_len;
  unsigned char *dev_addr;
  unsigned char perm_addr[ETH_ALEN];
  unsigned char broadcast[ETH_ALEN];
  unsigned char addr_len;
  unsigned short type;
  unsigned int flags;
  unsigned long state;
  void *priv;
  struct mutex lock;
  struct netdev_queue *_tx;
  struct list_head mc_list;
  unsigned int mc_count;
  unsigned int num_tx_queues, real_num_tx_queues;
  unsigned int num_rx_queues, real_num_rx_queues;
};

#define SET_NETDEV_DEV(netdev, parent_device) \
  ((netdev)->dev.parent = (parent_device))
#define netdev_err(netdev, fmt, ...) dev_err(&(netdev)->dev, fmt, ##__VA_ARGS__)
#define netdev_warn(netdev, fmt, ...) dev_warn(&(netdev)->dev, fmt, ##__VA_ARGS__)
#define netdev_info(netdev, fmt, ...) dev_info(&(netdev)->dev, fmt, ##__VA_ARGS__)
#define netdev_warn_once(netdev, fmt, ...) \
  dev_warn_once(&(netdev)->dev, fmt, ##__VA_ARGS__)
#define netdev_notice_once(netdev, fmt, ...) \
  dev_notice_once(&(netdev)->dev, fmt, ##__VA_ARGS__)
static inline bool netdev_mc_empty(const struct net_device *dev) {
  return list_empty(&dev->mc_list);
}
#define netdev_for_each_mc_addr(ha, dev) \
  list_for_each_entry((ha), &(dev)->mc_list, list)

struct napi_struct {
  struct list_head poll_list;
  unsigned long state;
  int weight;
  int (*poll)(struct napi_struct *, int);
  struct net_device *dev;
  struct list_head dev_list;
  struct gro_node gro;
};

struct net_device *alloc_netdev_mqs(int, const char *, unsigned char,
                                    void (*)(struct net_device *),
                                    unsigned int, unsigned int);
void free_netdev(struct net_device *);
int register_netdev(struct net_device *);
void unregister_netdev(struct net_device *);
void netif_device_detach(struct net_device *);
void netif_device_attach(struct net_device *);
void synchronize_net(void);
void rtnl_lock(void);
void rtnl_unlock(void);
int netdev_update_features(struct net_device *);
void netdev_reset_queue(struct net_device *);
void dev_consume_skb_any(struct sk_buff *);
bool __netdev_sent_queue(struct net_device *, unsigned int, bool);
void netdev_sw_irq_coalesce_default_on(struct net_device *);
static inline bool netdev_xmit_more(void) { return false; }
static inline void skb_tx_timestamp(struct sk_buff *skb) { (void)skb; }
static inline void napi_consume_skb(struct sk_buff *skb, int budget) {
  (void)budget;
  kfree_skb(skb);
}
static inline bool netif_running(const struct net_device *dev) {
  return test_bit(__LINK_STATE_START, &dev->state);
}
int netif_set_real_num_tx_queues(struct net_device *, unsigned int);
int netif_set_real_num_rx_queues(struct net_device *, unsigned int);
void ether_setup(struct net_device *);
void netif_carrier_on(struct net_device *);
void netif_carrier_off(struct net_device *);
void netif_tx_wake_queue(struct netdev_queue *);
void netif_tx_stop_all_queues(struct net_device *);
void netif_napi_add_weight_locked(struct net_device *, struct napi_struct *,
                                  int (*)(struct napi_struct *, int), int);
void __netif_napi_del_locked(struct napi_struct *);
bool napi_schedule_prep(struct napi_struct *);
void __napi_schedule(struct napi_struct *);
void __napi_schedule_irqoff(struct napi_struct *);
bool napi_complete_done(struct napi_struct *, int);
void napi_enable(struct napi_struct *);
void napi_disable(struct napi_struct *);
int netif_receive_skb(struct sk_buff *);
int dev_queue_xmit(struct sk_buff *);
int netif_rx(struct sk_buff *);
int __netif_rx(struct sk_buff *);
gro_result_t gro_receive_skb(struct gro_node *, struct sk_buff *);
void dev_kfree_skb_any_reason(struct sk_buff *, enum skb_drop_reason);
static inline void dev_kfree_skb_any(struct sk_buff *skb) {
  dev_kfree_skb_any_reason(skb, SKB_DROP_REASON_NOT_SPECIFIED);
}

static inline void *netdev_priv(const struct net_device *dev) { return dev->priv; }
static inline struct netdev_queue *netdev_get_tx_queue(
    const struct net_device *dev, unsigned int index) { return &dev->_tx[index]; }
static inline bool netif_carrier_ok(const struct net_device *dev) {
  return !(dev->state & (1UL << __LINK_STATE_NOCARRIER));
}
static inline void netif_testing_on(struct net_device *dev) {
  set_bit(__LINK_STATE_TESTING, &dev->state);
}
static inline void netif_testing_off(struct net_device *dev) {
  clear_bit(__LINK_STATE_TESTING, &dev->state);
}
static inline bool netif_queue_stopped(const struct net_device *dev) {
  return test_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[0].state);
}
static inline bool netif_tx_queue_stopped(const struct netdev_queue *queue) {
  return test_bit(__QUEUE_STATE_DRV_XOFF, &queue->state);
}
static inline void netif_tx_start_queue(struct netdev_queue *queue) {
  clear_bit(__QUEUE_STATE_DRV_XOFF, &queue->state);
}
static inline void netif_tx_stop_queue(struct netdev_queue *queue) {
  set_bit(__QUEUE_STATE_DRV_XOFF, &queue->state);
}
/* Byte queue limits are disabled in this profile. Descriptor-ring flow
 * control still uses the upstream queue stop/wake logic and its barriers. */
static inline void netdev_tx_completed_queue(struct netdev_queue *queue,
                                             unsigned int packets,
                                             unsigned int bytes) {
  (void)queue;
  (void)packets;
  (void)bytes;
}
static inline void netdev_tx_sent_queue(struct netdev_queue *queue,
                                        unsigned int bytes) {
  (void)queue;
  (void)bytes;
}
static inline void netif_start_queue(struct net_device *dev) {
  clear_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[0].state);
}
static inline void netif_stop_queue(struct net_device *dev) {
  set_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[0].state);
}
static inline void netif_wake_queue(struct net_device *dev) {
  netif_tx_wake_queue(netdev_get_tx_queue(dev, 0));
}
static inline void netif_set_tso_max_size(struct net_device *dev, unsigned int size) {
  dev->gso_max_size = size;
}
static inline void netif_set_tso_max_segs(struct net_device *dev, unsigned int segs) {
  dev->gso_max_segs = segs;
}
static inline void dev_sw_netstats_rx_add(struct net_device *dev, unsigned int len) {
  __atomic_fetch_add(&dev->tstats->rx_packets, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&dev->tstats->rx_bytes, len, __ATOMIC_RELAXED);
}
static inline void dev_sw_netstats_tx_add(struct net_device *dev,
                                          unsigned int packets,
                                          unsigned int len) {
  __atomic_fetch_add(&dev->tstats->tx_packets, packets, __ATOMIC_RELAXED);
  __atomic_fetch_add(&dev->tstats->tx_bytes, len, __ATOMIC_RELAXED);
}
/* Output uses pinned v6.18 uapi/linux/if_link.h's sequential u64 stats ABI.
 * Keep this opaque here to avoid pulling full upstream netlink into frontend. */
void netdev_stats_to_stats64(void *, const struct net_device_stats *);
void dev_fetch_sw_netstats(void *, const struct pcpu_sw_netstats *);
static inline void netif_napi_add_weight(struct net_device *dev,
    struct napi_struct *napi, int (*poll)(struct napi_struct *, int), int weight) {
  netif_napi_add_weight_locked(dev, napi, poll, weight);
}
static inline void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
    int (*poll)(struct napi_struct *, int)) {
  netif_napi_add_weight_locked(dev, napi, poll, NAPI_POLL_WEIGHT);
}
static inline void netif_napi_del(struct napi_struct *napi) {
  __netif_napi_del_locked(napi);
}
static inline unsigned int eth_hw_addr_crc(const struct netdev_hw_addr *ha) {
  unsigned int crc = ~0U;
  for (unsigned int i = 0; i < ETH_ALEN; i++) {
    unsigned char byte = ha->addr[i];
    for (unsigned int bit = 0; bit < 8; bit++) {
      unsigned int carry = (crc ^ byte) & 1U;
      crc >>= 1;
      if (carry) crc ^= 0xedb88320U;
      byte >>= 1;
    }
  }
  /* Linux ether_crc() returns the bit-reversed CRC32-LE value. */
  unsigned int reversed = 0;
  for (unsigned int bit = 0; bit < 32; bit++)
    reversed = (reversed << 1) | ((crc >> bit) & 1U);
  return reversed;
}
static inline bool napi_schedule(struct napi_struct *napi) {
  if (!napi_schedule_prep(napi)) return false;
  __napi_schedule(napi);
  return true;
}
static inline bool napi_schedule_irqoff(struct napi_struct *napi) {
  if (!napi_schedule_prep(napi)) return false;
  __napi_schedule_irqoff(napi);
  return true;
}
static inline bool napi_complete(struct napi_struct *napi) {
  return napi_complete_done(napi, 0);
}
static inline gro_result_t napi_gro_receive(struct napi_struct *napi,
                                            struct sk_buff *skb) {
  return gro_receive_skb(&napi->gro, skb);
}

#else
#include_next <linux/netdevice.h>

bool linuxkpi_netdev_is_bound(struct net_device *dev);
#endif

#endif
