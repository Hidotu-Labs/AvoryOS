#ifndef NET_CORE_H
#define NET_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MTU_ETHERNET 1500
#define NET_FRAME_MAX 1518
#define NET_PACKET_POOL_SIZE 4096
#define NET_DEVICE_MAX 8

struct net_device;
struct net_device_stats {
  uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
  uint64_t rx_dropped, tx_dropped, rx_errors, tx_errors;
  uint64_t interrupts, resets, queue_full;
  uint64_t link_changes, rx_overflows, tx_underruns;
};
struct net_device_ops {
  int (*transmit)(struct net_device *, const void *, size_t);
  bool (*link_up)(struct net_device *);
  void (*stop)(struct net_device *);
};
struct net_device {
  char name[16];
  uint8_t mac[6];
  uint16_t mtu;
  bool registered;
  const struct net_device_ops *ops;
  void *driver_private;
  struct net_device_stats stats;
};
struct net_packet {
  struct net_device *device;
  uint16_t length;
  uint8_t data[NET_FRAME_MAX];
};
typedef void (*net_rx_handler_t)(struct net_packet *);

void net_core_init(void);
void net_core_start_worker(void);
void net_core_start_timer(void);
int net_device_register(struct net_device *);
int net_device_unregister(struct net_device *);
struct net_device *net_device_default(void);
struct net_device *net_device_find(const char *name);
void net_set_rx_handler(net_rx_handler_t);
net_rx_handler_t net_get_rx_handler(void);
bool net_rx_submit_irq(struct net_device *, const void *, size_t);
void net_queue_snapshot(uint32_t *queued, uint32_t *in_use);
void net_print_stats(const struct net_device *);
#endif
