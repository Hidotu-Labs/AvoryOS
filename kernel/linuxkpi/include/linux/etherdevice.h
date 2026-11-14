#ifndef LINUXKPI_ETHERDEVICE_OVERLAY_H
#define LINUXKPI_ETHERDEVICE_OVERLAY_H

#ifdef LINUXKPI_NET_FRONTEND
#define _LINUX_ETHERDEVICE_H
#include <linux/netdevice.h>
#include <linux/skbuff.h>

struct net_device *alloc_etherdev_mqs(int, unsigned int, unsigned int);
struct device;
struct net_device *devm_alloc_etherdev_mqs(struct device *, int,
                                           unsigned int, unsigned int);
#define alloc_etherdev(size) alloc_etherdev_mqs((size), 1, 1)
#define alloc_etherdev_mq(size, count) alloc_etherdev_mqs((size), (count), (count))
#define devm_alloc_etherdev(dev, size) \
  devm_alloc_etherdev_mqs((dev), (size), 1, 1)
__be16 eth_type_trans(struct sk_buff *, struct net_device *);
bool is_valid_ether_addr(const u8 *addr);
void eth_random_addr(u8 *addr);
void eth_hw_addr_set(struct net_device *dev, const u8 *addr);
int eth_mac_addr(struct net_device *dev, void *p);
int eth_validate_addr(struct net_device *dev);
int eth_platform_get_mac_address(struct device *dev, u8 *addr);

#else
#include_next <linux/etherdevice.h>
#endif

#endif
