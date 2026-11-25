#ifndef NET_LINUXKPI_BRIDGE_H
#define NET_LINUXKPI_BRIDGE_H

#include "core.h"
#include "lock/spinlock.h"

#define LKPI_NET_BRIDGE_TX_LIMIT 128
#define LKPI_NET_BRIDGE_NAPI_MAX 8
#define LKPI_NAPI_DEFAULT_WEIGHT 64

struct lkpi_skb {
  uint8_t *head;
  uint8_t *data;
  uint8_t *tail;
  uint8_t *end;
  uint8_t *mac_header;
  size_t len;
  uint16_t protocol;
  struct lkpi_skb *next;
};

struct lkpi_skb *lkpi_alloc_skb(size_t payload, size_t headroom);
void lkpi_kfree_skb(struct lkpi_skb *);
void *lkpi_skb_put(struct lkpi_skb *, size_t);
void *lkpi_skb_push(struct lkpi_skb *, size_t);
void *lkpi_skb_pull(struct lkpi_skb *, size_t);
void lkpi_skb_reserve(struct lkpi_skb *, size_t);
uint16_t lkpi_eth_type_trans(struct lkpi_skb *);

struct lkpi_napi;
typedef int (*lkpi_napi_poll_t)(struct lkpi_napi *, int budget);
struct lkpi_napi {
  struct net_deferred_work work;
  spinlock_t lock;
  lkpi_napi_poll_t poll;
  void *context;
  unsigned weight;
  bool enabled;
  bool scheduled;
  bool running;
  bool missed;
  bool completed;
  bool deferred;
};

void lkpi_napi_init(struct lkpi_napi *, lkpi_napi_poll_t, void *, unsigned weight);
void lkpi_napi_enable(struct lkpi_napi *);
bool lkpi_napi_schedule(struct lkpi_napi *);
bool lkpi_napi_complete_done(struct lkpi_napi *, int work_done);
bool lkpi_napi_defer(struct lkpi_napi *, int work_done);
void lkpi_napi_disable(struct lkpi_napi *);

struct lkpi_net_bridge;
enum lkpi_tx_result {
  LKPI_TX_ACCEPTED = 0, /* callback owns skb and must eventually free it */
  LKPI_TX_BUSY = 1,     /* callback did not take skb */
  LKPI_TX_FAILED = 2,   /* callback did not take skb */
};
struct lkpi_net_bridge_ops {
  bool (*open)(void *context);
  void (*stop)(void *context);
  bool (*carrier)(void *context);
  enum lkpi_tx_result (*transmit)(void *context, struct lkpi_skb *skb);
};
struct lkpi_net_bridge {
  struct net_device native;
  const struct lkpi_net_bridge_ops *ops;
  void *context;
  struct net_deferred_work tx_work;
  spinlock_t tx_lock;
  struct lkpi_skb *tx_head;
  struct lkpi_skb *tx_tail;
  unsigned tx_queued;
  bool opened;
  bool tx_blocked;
  bool tx_inflight;
  uint64_t tx_wake_generation;
  struct lkpi_napi *napi[LKPI_NET_BRIDGE_NAPI_MAX];
  unsigned napi_count;
};

/* The caller owns the bridge storage and must keep it alive through
 * unregister. TX acceptance transfers skb ownership to the driver callback;
 * BUSY retains the skb in a bounded retry queue until tx_wake is called. */
int lkpi_net_bridge_register(struct lkpi_net_bridge *, const char *name,
                             const uint8_t mac[6], uint16_t mtu,
                             const struct lkpi_net_bridge_ops *, void *context);
int lkpi_net_bridge_unregister(struct lkpi_net_bridge *);
void lkpi_net_bridge_tx_wake(struct lkpi_net_bridge *);
bool lkpi_net_bridge_add_napi(struct lkpi_net_bridge *, struct lkpi_napi *);

#endif
