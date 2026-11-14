#ifndef NET_CORE_H
#define NET_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MTU_ETHERNET 1500
#define NET_FRAME_MAX 1518
#define NET_DEVICE_MAX 8

struct net_device;
struct pci_device;
struct net_device_stats {
  uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
  uint64_t rx_dropped, tx_dropped, rx_errors, tx_errors;
  uint64_t interrupts, resets, queue_full;
  uint64_t link_changes, rx_overflows, tx_underruns;
  uint64_t tx_timeouts, recoveries, recovery_failures;
  uint64_t dma_bytes;
};
struct net_device_ops {
  /* Optional boot activation. Omit for devices already active at registration. */
  bool (*start)(struct net_device *);
  int (*transmit)(struct net_device *, const void *, size_t);
  bool (*link_up)(struct net_device *);
  void (*stop)(struct net_device *);
};
struct net_device {
  char name[16];
  uint8_t mac[6];
  uint16_t mtu;
  bool registered;
  bool started;
  bool unregistering; /* RX admission closed until queued references drain. */
  uint32_t references; /* RX packets and short-lived device users */
  bool persistent; /* netdev address remains valid until reboot */
  uint64_t generation; /* rejects packets queued before stop/reset/rebind */
  const struct net_device_ops *ops;
  struct pci_device *pci;
  void *driver_private;
  struct net_device_stats stats;
};
struct net_packet {
  struct net_device *device;
  uint64_t generation;
  uint16_t length;
  uint8_t data[NET_FRAME_MAX];
};
typedef void (*net_rx_handler_t)(struct net_packet *);
typedef void (*net_rx_tap_t)(const struct net_packet *, void *context);

struct net_rx_frame {
  const void *data;
  size_t length;
};

/* Bounded deferred work shared by LinuxKPI NAPI and bridge TX. The work item
 * storage is caller-owned and must outlive execution and cancellation. */
struct net_deferred_work {
  struct net_deferred_work *next;
  bool (*run)(struct net_deferred_work *);
  bool queued;
  bool running;
  bool reschedule;
  bool cancelled;
};

void net_core_init(void);
void net_core_start_worker(void);
void net_core_start_timer(void);
int net_device_register(struct net_device *);
/* Activate registered NICs and select a working default; boot-thread only. */
bool net_devices_start(void);
/* Synchronous device-reference grace period: process context only, not from
 * the RX handler or while holding a net_device reference. On return no
 * queued/worker-held RX packet or short-lived TX user references the device. */
int net_device_unregister(struct net_device *);
/* Raw registry lookup for legacy reporting paths. The returned pointer is
 * borrowed without a lifetime reference; use default_acquire for concurrent
 * packet/device access. */
struct net_device *net_device_default(void);
/* Acquire a reference to the current default. Release with net_device_put(). */
struct net_device *net_device_default_acquire(void);
void net_device_put(struct net_device *);
bool net_device_is_default(const struct net_device *);
/* Select an activated NIC before protocol startup. */
bool net_device_set_default(const char *name);
/* Borrowed registry result; current callers use only for a presence check. */
struct net_device *net_device_find(const char *name);
void net_set_rx_handler(net_rx_handler_t);
net_rx_handler_t net_get_rx_handler(void);
/* Diagnostic receive tap. Clearing/replacing synchronizes with callbacks and
 * must not be done from the tap itself or RX worker context. */
void net_set_rx_tap(net_rx_tap_t, void *context);
/* RX producers must stop/synchronize before unregister and keep @dev valid
 * through return. Accepted frames hold an internal reference until the
 * handler finishes or the stale packet is discarded. */
bool net_rx_submit_irq(struct net_device *, const void *, size_t);
/* Submit a burst with one queue-lock acquisition and one worker wake. */
size_t net_rx_submit_batch_irq(struct net_device *,
                               const struct net_rx_frame *, size_t);
void net_queue_snapshot(uint32_t *queued, uint32_t *in_use);
void net_deferred_work_init(struct net_deferred_work *,
                            bool (*)(struct net_deferred_work *));
bool net_deferred_work_schedule(struct net_deferred_work *);
void net_deferred_work_cancel(struct net_deferred_work *);
void net_print_stats(const struct net_device *);
#endif
