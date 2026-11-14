#ifndef LINUXKPI_ETHTOOL_OVERLAY_H
#define LINUXKPI_ETHTOOL_OVERLAY_H

/* Narrow ethtool declaration surface used by the selected Ethernet frontend
 * and R8169 candidate. Management/netlink ioctl implementations remain out of
 * scope; callback structures retain the pinned Linux 6.18 ABI layouts. */
#ifdef LINUXKPI_NET_FRONTEND
#define _LINUX_ETHTOOL_H
#include <linux/bitops.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <uapi/linux/ethtool.h>
#include <uapi/linux/net_tstamp.h>

#define __ETHTOOL_DECLARE_LINK_MODE_MASK(name) DECLARE_BITMAP(name, __ETHTOOL_LINK_MODE_MASK_NBITS)
#define ETHTOOL_COALESCE_USECS (BIT(0) | BIT(4))
#define ETHTOOL_COALESCE_MAX_FRAMES (BIT(1) | BIT(5))

struct netlink_ext_ack;
struct link_mode_info { int speed; u8 lanes, duplex; };
extern const struct link_mode_info link_mode_params[];
struct ethtool_netdev_state {
  struct xarray rss_ctx;
  struct mutex rss_lock;
  unsigned int wol_enabled : 1;
  unsigned int module_fw_flash_in_progress : 1;
};
struct kernel_ethtool_ringparam {
  u32 rx_buf_len;
  u8 tcp_data_split, tx_push, rx_push;
  u32 cqe_size, tx_push_buf_len, tx_push_buf_max_len;
  u32 hds_thresh, hds_thresh_max;
};
struct kernel_ethtool_coalesce {
  u8 use_cqe_mode_tx, use_cqe_mode_rx;
  u32 tx_aggr_max_bytes, tx_aggr_max_frames, tx_aggr_time_usecs;
};
struct ethtool_keee {
  __ETHTOOL_DECLARE_LINK_MODE_MASK(supported);
  __ETHTOOL_DECLARE_LINK_MODE_MASK(advertised);
  __ETHTOOL_DECLARE_LINK_MODE_MASK(lp_advertised);
  u32 tx_lpi_timer;
  bool tx_lpi_enabled, eee_active, eee_enabled;
};
struct ethtool_link_ksettings {
  struct ethtool_link_settings base;
  struct {
    __ETHTOOL_DECLARE_LINK_MODE_MASK(supported);
    __ETHTOOL_DECLARE_LINK_MODE_MASK(advertising);
    __ETHTOOL_DECLARE_LINK_MODE_MASK(lp_advertising);
  } link_modes;
  u32 lanes;
};
struct ethtool_eth_mac_stats {
  enum ethtool_mac_stats_src src;
  u64 FramesTransmittedOK, SingleCollisionFrames, MultipleCollisionFrames;
  u64 FramesReceivedOK, FrameCheckSequenceErrors, AlignmentErrors;
  u64 OctetsTransmittedOK, FramesWithDeferredXmissions, LateCollisions;
  u64 FramesAbortedDueToXSColls, FramesLostDueToIntMACXmitError;
  u64 CarrierSenseErrors, OctetsReceivedOK, FramesLostDueToIntMACRcvError;
  u64 MulticastFramesXmittedOK, BroadcastFramesXmittedOK;
  u64 FramesWithExcessiveDeferral, MulticastFramesReceivedOK;
  u64 BroadcastFramesReceivedOK, InRangeLengthErrors, OutOfRangeLengthField;
  u64 FrameTooLongErrors;
};
struct ethtool_eth_ctrl_stats {
  enum ethtool_mac_stats_src src;
  u64 MACControlFramesTransmitted, MACControlFramesReceived;
  u64 UnsupportedOpcodesReceived;
};
struct ethtool_pause_stats {
  enum ethtool_mac_stats_src src;
  u64 tx_pause_frames, rx_pause_frames;
};
struct ethtool_eth_phy_stats {
  enum ethtool_mac_stats_src src;
  u64 SymbolErrorDuringCarrier;
};
struct ethtool_phy_stats {
  u64 rx_packets, rx_bytes, rx_errors, tx_packets, tx_bytes, tx_errors;
};
struct ethtool_link_ext_stats { u64 link_down_events; };
struct phy_device;
struct phy_tdr_config;
struct phy_plca_cfg;
struct phy_plca_status;
struct ethtool_phy_ops {
  int (*get_sset_count)(struct phy_device *);
  int (*get_strings)(struct phy_device *, u8 *);
  int (*get_stats)(struct phy_device *, struct ethtool_stats *, u64 *);
  int (*get_plca_cfg)(struct phy_device *, struct phy_plca_cfg *);
  int (*set_plca_cfg)(struct phy_device *, const struct phy_plca_cfg *,
                      struct netlink_ext_ack *);
  int (*get_plca_status)(struct phy_device *, struct phy_plca_status *);
  int (*start_cable_test)(struct phy_device *, struct netlink_ext_ack *);
  int (*start_cable_test_tdr)(struct phy_device *, struct netlink_ext_ack *,
                              const struct phy_tdr_config *);
};
void ethtool_set_ethtool_phy_ops(const struct ethtool_phy_ops *ops);
const struct ethtool_phy_ops *linuxkpi_ethtool_phy_ops_get(void);
struct kernel_ethtool_ts_info {
  u32 cmd, so_timestamping;
  int phc_index;
  enum hwtstamp_provider_qualifier phc_qualifier;
  int phc_source;
  int phc_phyindex;
  u32 tx_types, rx_filters;
};

struct ethtool_ops {
  u32 supported_coalesce_params;
  void (*get_drvinfo)(struct net_device *, struct ethtool_drvinfo *);
  int (*get_regs_len)(struct net_device *);
  u32 (*get_link)(struct net_device *);
  int (*get_coalesce)(struct net_device *, struct ethtool_coalesce *,
                      struct kernel_ethtool_coalesce *, struct netlink_ext_ack *);
  int (*set_coalesce)(struct net_device *, struct ethtool_coalesce *,
                      struct kernel_ethtool_coalesce *, struct netlink_ext_ack *);
  void (*get_regs)(struct net_device *, struct ethtool_regs *, void *);
  void (*get_wol)(struct net_device *, struct ethtool_wolinfo *);
  int (*set_wol)(struct net_device *, struct ethtool_wolinfo *);
  void (*get_strings)(struct net_device *, u32, u8 *);
  int (*get_sset_count)(struct net_device *, int);
  void (*get_ethtool_stats)(struct net_device *, struct ethtool_stats *, u64 *);
  int (*get_ts_info)(struct net_device *, struct kernel_ethtool_ts_info *);
  int (*nway_reset)(struct net_device *);
  int (*get_eee)(struct net_device *, struct ethtool_keee *);
  int (*set_eee)(struct net_device *, struct ethtool_keee *);
  int (*get_link_ksettings)(struct net_device *, struct ethtool_link_ksettings *);
  int (*set_link_ksettings)(struct net_device *, const struct ethtool_link_ksettings *);
  void (*get_ringparam)(struct net_device *, struct ethtool_ringparam *,
                        struct kernel_ethtool_ringparam *, struct netlink_ext_ack *);
  void (*get_pause_stats)(struct net_device *, struct ethtool_pause_stats *);
  void (*get_pauseparam)(struct net_device *, struct ethtool_pauseparam *);
  int (*set_pauseparam)(struct net_device *, struct ethtool_pauseparam *);
  void (*get_eth_mac_stats)(struct net_device *, struct ethtool_eth_mac_stats *);
  void (*get_eth_ctrl_stats)(struct net_device *, struct ethtool_eth_ctrl_stats *);
};

static inline u32 ethtool_op_get_link(struct net_device *dev) {
  return netif_carrier_ok(dev);
}
int ethtool_op_get_ts_info(struct net_device *, struct kernel_ethtool_ts_info *);
#else
#include_next <linux/ethtool.h>
#endif

#endif
