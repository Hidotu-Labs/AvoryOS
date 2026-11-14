#ifndef LINUXKPI_FRONTEND_NET_H
#define LINUXKPI_FRONTEND_NET_H

#include <linux/netdevice.h>

/* Test-only native counter snapshot used by the isolated R2 software fixture. */
void linuxkpi_netdev_test_stats(struct net_device *,
                                unsigned long long *rx_packets,
                                unsigned long long *tx_packets,
                                unsigned long long *rx_dropped,
                                unsigned long long *tx_dropped);
typedef void (*linuxkpi_test_rx_tap_fn)(void *, const void *,
                                        unsigned long length);
void linuxkpi_netdev_test_set_rx_tap(struct net_device *,
                                     linuxkpi_test_rx_tap_fn, void *context);
unsigned linuxkpi_netdev_test_references(struct net_device *);

#endif
