#include "net/linuxkpi_bridge.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include <linuxkpi/native_net.h>

#define LKPI_SKB_HEADROOM 64

struct lkpi_skb *lkpi_alloc_skb(size_t payload, size_t headroom) {
  if (payload > SIZE_MAX - headroom)
    return NULL;
  struct lkpi_skb *skb = kmalloc(sizeof(*skb));
  if (!skb)
    return NULL;
  uint8_t *head = kmalloc(payload + headroom ? payload + headroom : 1);
  if (!head) {
    kfree(skb);
    return NULL;
  }
  memset(skb, 0, sizeof(*skb));
  skb->head = skb->data = skb->tail = head;
  skb->end = head + payload + headroom;
  skb->data += headroom;
  skb->tail += headroom;
  skb->mac_header = skb->data;
  return skb;
}

void lkpi_kfree_skb(struct lkpi_skb *skb) {
  if (!skb)
    return;
  kfree(skb->head);
  kfree(skb);
}

void *lkpi_skb_put(struct lkpi_skb *skb, size_t length) {
  if (!skb || length > (size_t)(skb->end - skb->tail))
    return NULL;
  void *result = skb->tail;
  skb->tail += length;
  skb->len += length;
  return result;
}

void *lkpi_skb_push(struct lkpi_skb *skb, size_t length) {
  if (!skb || length > (size_t)(skb->data - skb->head))
    return NULL;
  skb->data -= length;
  skb->len += length;
  return skb->data;
}

void *lkpi_skb_pull(struct lkpi_skb *skb, size_t length) {
  if (!skb || length > skb->len)
    return NULL;
  skb->data += length;
  skb->len -= length;
  return skb->data;
}

void lkpi_skb_reserve(struct lkpi_skb *skb, size_t length) {
  if (skb && skb->len == 0 && length <= (size_t)(skb->end - skb->tail)) {
    skb->data += length;
    skb->tail += length;
    skb->mac_header = skb->data;
  }
}

uint16_t lkpi_eth_type_trans(struct lkpi_skb *skb) {
  if (!skb || skb->len < 14)
    return 0;
  skb->mac_header = skb->data;
  uint16_t network_protocol =
      (uint16_t)((uint16_t)skb->data[12] << 8 | skb->data[13]);
  skb->protocol = __builtin_bswap16(network_protocol);
  lkpi_skb_pull(skb, 14);
  return skb->protocol;
}

static bool napi_deferred_run(struct net_deferred_work *work) {
  struct lkpi_napi *napi = (struct lkpi_napi *)((char *)work -
      offsetof(struct lkpi_napi, work));
  spinlock_acquire(&napi->lock);
  if (!napi->enabled || !napi->poll) {
    napi->scheduled = false;
    napi->running = false;
    napi->missed = false;
    spinlock_release(&napi->lock);
    return false;
  }
  napi->scheduled = false;
  napi->running = true;
  napi->completed = false;
  napi->missed = false;
  lkpi_napi_poll_t poll = napi->poll;
  int budget = (int)napi->weight;
  spinlock_release(&napi->lock);

  int work_done = poll(napi, budget);

  spinlock_acquire(&napi->lock);
  napi->running = false;
  bool again = napi->enabled &&
      (napi->missed || work_done >= budget ||
       (work_done < budget && !napi->completed && work_done > 0));
  if (!napi->enabled) {
    napi->scheduled = false;
    napi->missed = false;
  } else if (again) {
    napi->scheduled = true;
  } else {
    napi->scheduled = false;
  }
  napi->completed = false;
  napi->missed = false;
  spinlock_release(&napi->lock);
  return again;
}

void lkpi_napi_init(struct lkpi_napi *napi, lkpi_napi_poll_t poll,
                    void *context, unsigned weight) {
  if (!napi)
    return;
  memset(napi, 0, sizeof(*napi));
  spinlock_init(&napi->lock);
  napi->poll = poll;
  napi->context = context;
  napi->weight = weight ? weight : LKPI_NAPI_DEFAULT_WEIGHT;
  net_deferred_work_init(&napi->work, napi_deferred_run);
}

void lkpi_napi_enable(struct lkpi_napi *napi) {
  if (!napi)
    return;
  spinlock_acquire(&napi->lock);
  napi->enabled = true;
  spinlock_release(&napi->lock);
}

bool lkpi_napi_schedule(struct lkpi_napi *napi) {
  if (!napi)
    return false;
  spinlock_acquire(&napi->lock);
  if (!napi->enabled) {
    spinlock_release(&napi->lock);
    return false;
  }
  if (napi->running) {
    napi->missed = true;
    spinlock_release(&napi->lock);
    return true;
  }
  if (napi->scheduled) {
    spinlock_release(&napi->lock);
    return true;
  }
  napi->scheduled = true;
  bool queued = net_deferred_work_schedule(&napi->work);
  if (!queued)
    napi->scheduled = false;
  spinlock_release(&napi->lock);
  return queued;
}

bool lkpi_napi_complete_done(struct lkpi_napi *napi, int work_done) {
  if (!napi)
    return false;
  spinlock_acquire(&napi->lock);
  bool complete = napi->running && napi->enabled && !napi->missed &&
                  work_done >= 0 &&
                  (unsigned)work_done < napi->weight;
  if (complete)
    napi->completed = true;
  spinlock_release(&napi->lock);
  return complete;
}

void lkpi_napi_disable(struct lkpi_napi *napi) {
  if (!napi)
    return;
  spinlock_acquire(&napi->lock);
  napi->enabled = false;
  napi->scheduled = false;
  napi->missed = false;
  spinlock_release(&napi->lock);
  net_deferred_work_cancel(&napi->work);
  /* cancel waits through a popped/running callback before caller may free the
   * NAPI object or its poll context. */
  spinlock_acquire(&napi->lock);
  napi->running = false;
  spinlock_release(&napi->lock);
}

static void tx_queue_push_locked(struct lkpi_net_bridge *bridge,
                                 struct lkpi_skb *skb) {
  skb->next = NULL;
  if (bridge->tx_tail)
    bridge->tx_tail->next = skb;
  else
    bridge->tx_head = skb;
  bridge->tx_tail = skb;
  bridge->tx_queued++;
}

static struct lkpi_skb *tx_queue_pop_locked(struct lkpi_net_bridge *bridge) {
  struct lkpi_skb *skb = bridge->tx_head;
  if (!skb)
    return NULL;
  bridge->tx_head = skb->next;
  if (!bridge->tx_head)
    bridge->tx_tail = NULL;
  skb->next = NULL;
  bridge->tx_queued--;
  return skb;
}

static bool bridge_tx_work(struct net_deferred_work *work) {
  struct lkpi_net_bridge *bridge = (struct lkpi_net_bridge *)((char *)work -
      offsetof(struct lkpi_net_bridge, tx_work));
  spinlock_acquire(&bridge->tx_lock);
  if (!__atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE) ||
      bridge->tx_blocked || bridge->tx_inflight ||
      !bridge->tx_head) {
    spinlock_release(&bridge->tx_lock);
    return false;
  }
  struct lkpi_skb *skb = tx_queue_pop_locked(bridge);
  size_t length = skb->len;
  bridge->tx_inflight = true;
  uint64_t wake_generation = bridge->tx_wake_generation;
  spinlock_release(&bridge->tx_lock);

  enum lkpi_tx_result result = bridge->ops->transmit(bridge->context, skb);

  spinlock_acquire(&bridge->tx_lock);
  bridge->tx_inflight = false;
  bool opened = __atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE);
  if (result == LKPI_TX_BUSY && opened) {
    skb->next = bridge->tx_head;
    bridge->tx_head = skb;
    if (!bridge->tx_tail)
      bridge->tx_tail = skb;
    bridge->tx_queued++;
    bool woken = bridge->tx_wake_generation != wake_generation;
    bridge->tx_blocked = !woken;
    spinlock_release(&bridge->tx_lock);
    return woken;
  }
  bool more = opened && bridge->tx_head != NULL;
  spinlock_release(&bridge->tx_lock);
  if (result == LKPI_TX_ACCEPTED) {
    __atomic_add_fetch(&bridge->native.stats.tx_packets, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&bridge->native.stats.tx_bytes, length, __ATOMIC_RELAXED);
  } else {
    __atomic_add_fetch(&bridge->native.stats.tx_errors, 1, __ATOMIC_RELAXED);
    lkpi_kfree_skb(skb);
  }
  return more;
}

static bool bridge_start(struct net_device *native) {
  struct lkpi_net_bridge *bridge = native->driver_private;
  for (unsigned i = 0; i < bridge->napi_count; i++)
    lkpi_napi_enable(bridge->napi[i]);
  __atomic_store_n(&bridge->opened, true, __ATOMIC_RELEASE);
  if (bridge->ops->open && !bridge->ops->open(bridge->context)) {
    __atomic_store_n(&bridge->opened, false, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < bridge->napi_count; i++)
      lkpi_napi_disable(bridge->napi[i]);
    return false;
  }
  spinlock_acquire(&bridge->tx_lock);
  bridge->tx_blocked = false;
  spinlock_release(&bridge->tx_lock);
  return true;
}

static bool bridge_link_up(struct net_device *native) {
  struct lkpi_net_bridge *bridge = native->driver_private;
  spinlock_acquire(&bridge->tx_lock);
  bool up = __atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE) &&
            bridge->ops->carrier && bridge->ops->carrier(bridge->context);
  spinlock_release(&bridge->tx_lock);
  return up;
}

static int bridge_transmit(struct net_device *native, const void *data,
                           size_t length) {
  struct lkpi_net_bridge *bridge = native->driver_private;
  if (!data || !length || length > NET_FRAME_MAX)
    return -1;
  struct lkpi_skb *skb = lkpi_alloc_skb(length, LKPI_SKB_HEADROOM);
  if (!skb)
    return -12;
  void *dst = lkpi_skb_put(skb, length);
  if (!dst) {
    lkpi_kfree_skb(skb);
    return -12;
  }
  memcpy(dst, data, length);
  spinlock_acquire(&bridge->tx_lock);
  if (!__atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE) ||
      bridge->tx_queued + (bridge->tx_inflight ? 1U : 0U) >=
          LKPI_NET_BRIDGE_TX_LIMIT) {
    __atomic_add_fetch(&native->stats.tx_dropped, 1, __ATOMIC_RELAXED);
    spinlock_release(&bridge->tx_lock);
    lkpi_kfree_skb(skb);
    return -11;
  }
  struct lkpi_skb *old_tail = bridge->tx_tail;
  tx_queue_push_locked(bridge, skb);
  bool queued = true;
  if (!bridge->tx_blocked)
    queued = net_deferred_work_schedule(&bridge->tx_work);
  if (!queued) {
    if (old_tail) {
      old_tail->next = NULL;
      bridge->tx_tail = old_tail;
    } else {
      bridge->tx_head = bridge->tx_tail = NULL;
    }
    bridge->tx_queued--;
  }
  spinlock_release(&bridge->tx_lock);
  if (!queued) {
    lkpi_kfree_skb(skb);
    __atomic_add_fetch(&native->stats.tx_dropped, 1, __ATOMIC_RELAXED);
    return -11;
  }
  return 0;
}

static void bridge_stop(struct net_device *native) {
  struct lkpi_net_bridge *bridge = native->driver_private;
  spinlock_acquire(&bridge->tx_lock);
  bool was_open = __atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE);
  __atomic_store_n(&bridge->opened, false, __ATOMIC_RELEASE);
  bridge->tx_blocked = true;
  spinlock_release(&bridge->tx_lock);
  net_deferred_work_cancel(&bridge->tx_work);
  if (was_open && bridge->ops->stop)
    bridge->ops->stop(bridge->context);
  for (unsigned i = 0; i < bridge->napi_count; i++)
    lkpi_napi_disable(bridge->napi[i]);
  spinlock_acquire(&bridge->tx_lock);
  struct lkpi_skb *skb;
  struct lkpi_skb *discard = NULL;
  while ((skb = tx_queue_pop_locked(bridge)) != NULL) {
    skb->next = discard;
    discard = skb;
  }
  spinlock_release(&bridge->tx_lock);
  while (discard) {
    skb = discard;
    discard = discard->next;
    lkpi_kfree_skb(skb);
  }
}

static const struct net_device_ops bridge_native_ops = {
  .start = bridge_start,
  .transmit = bridge_transmit,
  .link_up = bridge_link_up,
  .stop = bridge_stop,
};

int lkpi_net_bridge_register(struct lkpi_net_bridge *bridge,
                             const char *name, const uint8_t mac[6],
                             uint16_t mtu,
                             const struct lkpi_net_bridge_ops *ops,
                             void *context) {
  if (!bridge || !mac || !ops || !ops->transmit || !ops->carrier)
    return -1;
  memset(bridge, 0, sizeof(*bridge));
  spinlock_init(&bridge->tx_lock);
  net_deferred_work_init(&bridge->tx_work, bridge_tx_work);
  bridge->ops = ops;
  bridge->context = context;
  bridge->native.ops = &bridge_native_ops;
  bridge->native.driver_private = bridge;
  bridge->native.mtu = mtu;
  memcpy(bridge->native.mac, mac, sizeof(bridge->native.mac));
  if (name) {
    size_t n = strlen(name);
    if (n >= sizeof(bridge->native.name))
      n = sizeof(bridge->native.name) - 1;
    memcpy(bridge->native.name, name, n);
  }
  return net_device_register(&bridge->native);
}

int lkpi_net_bridge_unregister(struct lkpi_net_bridge *bridge) {
  if (!bridge || !bridge->native.registered)
    return -1;
  bridge_stop(&bridge->native);
  return net_device_unregister(&bridge->native);
}

void lkpi_net_bridge_tx_wake(struct lkpi_net_bridge *bridge) {
  if (!bridge)
    return;
  spinlock_acquire(&bridge->tx_lock);
  bridge->tx_blocked = false;
  bridge->tx_wake_generation++;
  bool pending = __atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE) &&
                 bridge->tx_head != NULL;
  spinlock_release(&bridge->tx_lock);
  if (pending)
    net_deferred_work_schedule(&bridge->tx_work);
}

bool lkpi_net_bridge_add_napi(struct lkpi_net_bridge *bridge,
                              struct lkpi_napi *napi) {
  if (!bridge || !napi ||
      __atomic_load_n(&bridge->native.registered, __ATOMIC_ACQUIRE) ||
      bridge->napi_count >= LKPI_NET_BRIDGE_NAPI_MAX)
    return false;
  bridge->napi[bridge->napi_count++] = napi;
  return true;
}

struct linuxkpi_native_net {
  struct lkpi_net_bridge bridge;
  const struct linuxkpi_native_net_ops *ops;
  void *context;
  linuxkpi_native_rx_tap_fn rx_tap;
  void *rx_tap_context;
};

struct linuxkpi_native_napi {
  struct lkpi_napi napi;
  linuxkpi_native_poll_fn poll;
  void *context;
};

static bool native_net_open(void *opaque) {
  struct linuxkpi_native_net *net = opaque;
  return !net->ops->open || net->ops->open(net->context) == 0;
}

static void native_net_stop(void *opaque) {
  struct linuxkpi_native_net *net = opaque;
  if (net->ops->stop)
    net->ops->stop(net->context);
}

static bool native_net_carrier(void *opaque) {
  struct linuxkpi_native_net *net = opaque;
  return net->ops->carrier && net->ops->carrier(net->context) != 0;
}

static enum lkpi_tx_result native_net_transmit(void *opaque,
                                               struct lkpi_skb *skb) {
  struct linuxkpi_native_net *net = opaque;
  enum linuxkpi_native_tx_result result = net->ops->transmit(
      net->context, skb->data, (unsigned long)skb->len);
  if (result == LINUXKPI_NATIVE_TX_ACCEPTED)
    lkpi_kfree_skb(skb);
  if (result == LINUXKPI_NATIVE_TX_BUSY)
    return LKPI_TX_BUSY;
  if (result == LINUXKPI_NATIVE_TX_ACCEPTED)
    return LKPI_TX_ACCEPTED;
  return LKPI_TX_FAILED;
}

static const struct lkpi_net_bridge_ops native_net_bridge_ops = {
  .open = native_net_open,
  .stop = native_net_stop,
  .carrier = native_net_carrier,
  .transmit = native_net_transmit,
};

struct linuxkpi_native_net *linuxkpi_native_net_register(
    const char *name, const unsigned char mac[6], unsigned short mtu,
    const struct linuxkpi_native_net_ops *ops, void *context) {
  if (!ops || !ops->transmit)
    return NULL;
  struct linuxkpi_native_net *net = kmalloc(sizeof(*net));
  if (!net)
    return NULL;
  memset(net, 0, sizeof(*net));
  net->ops = ops;
  net->context = context;
  if (lkpi_net_bridge_register(&net->bridge, name, mac, mtu,
                               &native_net_bridge_ops, net) != 0) {
    kfree(net);
    return NULL;
  }
  return net;
}

int linuxkpi_native_net_unregister(struct linuxkpi_native_net *net) {
  if (!net)
    return -1;
  /* Stop admission and drain packets first. An active RX observer is covered
   * by the packet's device reference, so it must not preempt the unregister
   * grace period. The bridge remains allocated until the tap is quiescent. */
  if (lkpi_net_bridge_unregister(&net->bridge) != 0)
    return -1;
  linuxkpi_native_net_set_rx_tap(net, NULL, NULL);
  kfree(net);
  return 0;
}

int linuxkpi_native_net_get_name(struct linuxkpi_native_net *net,
                                 char out[16]) {
  if (!net || !out)
    return -1;
  struct net_device *dev = net_device_default_acquire();
  if (dev == &net->bridge.native) {
    memcpy(out, dev->name, sizeof(net->bridge.native.name));
    net_device_put(dev);
    return 0;
  }
  net_device_put(dev);
  /* Non-default interfaces still need their allocated name; registration
   * storage remains caller-owned and protected until unregister. */
  if (!net->bridge.native.registered)
    return -1;
  memcpy(out, net->bridge.native.name, sizeof(net->bridge.native.name));
  return 0;
}

int linuxkpi_native_net_rx(struct linuxkpi_native_net *net, const void *data,
                           unsigned long length) {
  if (!net || !data || !length || length > NET_FRAME_MAX)
    return -1;
  struct lkpi_net_bridge *bridge = &net->bridge;
  /* The receive queue copies the frame before returning. The caller still
   * owns its skb here, so no intermediate allocation or copy is needed. */
  if (__atomic_load_n(&bridge->opened, __ATOMIC_ACQUIRE) &&
      net_rx_submit_irq(&bridge->native, data, length))
    return 0;
  __atomic_add_fetch(&bridge->native.stats.rx_dropped, 1, __ATOMIC_RELAXED);
  return -1;
}

int linuxkpi_native_net_transmit(struct linuxkpi_native_net *net,
                                 const void *data, unsigned long length) {
  if (!net || !data || !length)
    return -1;
  return net->bridge.native.ops->transmit(&net->bridge.native, data,
                                           (size_t)length);
}

void linuxkpi_native_net_tx_wake(struct linuxkpi_native_net *net) {
  if (net)
    lkpi_net_bridge_tx_wake(&net->bridge);
}

void linuxkpi_native_net_stats(struct linuxkpi_native_net *net,
                               unsigned long long *rx_packets,
                               unsigned long long *tx_packets,
                               unsigned long long *rx_dropped,
                               unsigned long long *tx_dropped) {
  if (!net)
    return;
  struct net_device_stats *stats = &net->bridge.native.stats;
  if (rx_packets)
    *rx_packets = __atomic_load_n(&stats->rx_packets, __ATOMIC_RELAXED);
  if (tx_packets)
    *tx_packets = __atomic_load_n(&stats->tx_packets, __ATOMIC_RELAXED);
  if (rx_dropped)
    *rx_dropped = __atomic_load_n(&stats->rx_dropped, __ATOMIC_RELAXED);
  if (tx_dropped)
    *tx_dropped = __atomic_load_n(&stats->tx_dropped, __ATOMIC_RELAXED);
}

static void native_net_rx_tap(const struct net_packet *packet, void *context) {
  struct linuxkpi_native_net *net = context;
  if (net && net->rx_tap && packet &&
      packet->device == &net->bridge.native)
    net->rx_tap(net->rx_tap_context, packet->data, packet->length);
}

void linuxkpi_native_net_set_rx_tap(struct linuxkpi_native_net *net,
                                    linuxkpi_native_rx_tap_fn tap,
                                    void *context) {
  if (!net)
    return;
  net_set_rx_tap(NULL, NULL);
  net->rx_tap = NULL;
  net->rx_tap_context = NULL;
  if (tap) {
    net->rx_tap_context = context;
    net->rx_tap = tap;
    net_set_rx_tap(native_net_rx_tap, net);
  }
}

unsigned linuxkpi_native_net_references(struct linuxkpi_native_net *net) {
  return net ? __atomic_load_n(&net->bridge.native.references, __ATOMIC_ACQUIRE)
             : 0;
}

static int native_napi_poll(struct lkpi_napi *napi, int budget) {
  struct linuxkpi_native_napi *native = napi->context;
  return native->poll(native->context, budget);
}

struct linuxkpi_native_napi *linuxkpi_native_napi_create(
    linuxkpi_native_poll_fn poll, void *context, unsigned weight) {
  if (!poll)
    return NULL;
  struct linuxkpi_native_napi *native = kmalloc(sizeof(*native));
  if (!native)
    return NULL;
  memset(native, 0, sizeof(*native));
  native->poll = poll;
  native->context = context;
  lkpi_napi_init(&native->napi, native_napi_poll, native, weight);
  return native;
}

int linuxkpi_native_napi_enable(struct linuxkpi_native_napi *native) {
  if (!native)
    return -1;
  lkpi_napi_enable(&native->napi);
  return 0;
}

int linuxkpi_native_napi_schedule(struct linuxkpi_native_napi *native) {
  return native && lkpi_napi_schedule(&native->napi) ? 0 : -1;
}

void linuxkpi_native_napi_mark_missed(struct linuxkpi_native_napi *native) {
  if (!native)
    return;
  struct lkpi_napi *napi = &native->napi;
  spinlock_acquire(&napi->lock);
  if (napi->running && napi->enabled)
    napi->missed = true;
  spinlock_release(&napi->lock);
}

int linuxkpi_native_napi_complete(struct linuxkpi_native_napi *native,
                                  int work_done) {
  return native && lkpi_napi_complete_done(&native->napi, work_done);
}

void linuxkpi_native_napi_disable(struct linuxkpi_native_napi *native) {
  if (native)
    lkpi_napi_disable(&native->napi);
}

void linuxkpi_native_napi_destroy(struct linuxkpi_native_napi *native) {
  if (!native)
    return;
  lkpi_napi_disable(&native->napi);
  kfree(native);
}
