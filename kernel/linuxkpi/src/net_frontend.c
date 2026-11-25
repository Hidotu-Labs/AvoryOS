/* Linux-shaped frontend for the native Ethernet bridge. This file deliberately
 * sees only upstream Linux headers and opaque native handles. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hrtimer.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <linuxkpi/native_net.h>
#include <linuxkpi/native_sched.h>
#include <linuxkpi/frontend_net.h>

extern void linuxkpi_guid_bytes(void *buffer, size_t len);

#define FRONTEND_NETDEVS 8
#define FRONTEND_NAPI_SLOTS 64

struct frontend_netdev {
  struct net_device *dev;
  struct linuxkpi_native_net *native;
  bool used;
  bool active;
  unsigned int rx_tap_users;
  unsigned int rx_inflight;
};
struct frontend_napi {
  struct napi_struct *napi;
  struct net_device *dev;
  struct linuxkpi_native_napi *native;
  bool used;
  bool removing;
  unsigned references;
  void *poll_thread;
  struct frontend_netdev *rx_batch_netdev;
  struct linuxkpi_native_net *rx_batch_native;
  struct sk_buff *rx_batch_skbs[LINUXKPI_NATIVE_RX_BATCH_MAX];
  struct linuxkpi_native_rx_frame rx_batch_frames[LINUXKPI_NATIVE_RX_BATCH_MAX];
  unsigned int rx_batch_count;
  struct hrtimer defer_timer;
  unsigned int defer_hard_irqs_count;
};

static struct frontend_netdev netdevs[FRONTEND_NETDEVS];
static struct frontend_napi napis[FRONTEND_NAPI_SLOTS];
/* Driver IRQ handlers can schedule NAPI or wake TX on this CPU. */
static DEFINE_SPINLOCK(frontend_lock);
static DEFINE_MUTEX(frontend_rtnl_lock);
static unsigned int frontend_tx_active;
static const struct ethtool_phy_ops *frontend_phy_ethtool_ops;

void rtnl_lock(void) { mutex_lock(&frontend_rtnl_lock); }
void rtnl_unlock(void) { mutex_unlock(&frontend_rtnl_lock); }
int rtnl_trylock(void) { return mutex_trylock(&frontend_rtnl_lock); }
int rtnl_is_locked(void) { return mutex_is_locked(&frontend_rtnl_lock); }
void ethtool_set_ethtool_phy_ops(const struct ethtool_phy_ops *ops) {
  __atomic_store_n(&frontend_phy_ethtool_ops, ops, __ATOMIC_RELEASE);
}
const struct ethtool_phy_ops *linuxkpi_ethtool_phy_ops_get(void) {
  return __atomic_load_n(&frontend_phy_ethtool_ops, __ATOMIC_ACQUIRE);
}
int ethtool_op_get_ts_info(struct net_device *dev,
                           struct kernel_ethtool_ts_info *info) {
  (void)dev;
  memset(info, 0, sizeof(*info));
  info->cmd = ETHTOOL_GET_TS_INFO;
  info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
                          SOF_TIMESTAMPING_RX_SOFTWARE |
                          SOF_TIMESTAMPING_SOFTWARE;
  info->phc_index = -1;
  return 0;
}

static struct frontend_netdev *find_netdev_locked(struct net_device *dev) {
  for (unsigned i = 0; i < FRONTEND_NETDEVS; i++)
    if (netdevs[i].used && netdevs[i].active && netdevs[i].dev == dev)
      return &netdevs[i];
  return NULL;
}

static struct frontend_napi *find_napi_locked(struct napi_struct *napi) {
  for (unsigned i = 0; i < FRONTEND_NAPI_SLOTS; i++)
    if (napis[i].used && !napis[i].removing && napis[i].napi == napi)
      return &napis[i];
  return NULL;
}

static struct frontend_napi *find_polling_napi_locked(struct net_device *dev,
                                                      void *thread) {
  if (!dev || !thread)
    return NULL;
  for (unsigned i = 0; i < FRONTEND_NAPI_SLOTS; i++)
    if (napis[i].used && !napis[i].removing && napis[i].dev == dev &&
        __atomic_load_n(&napis[i].poll_thread, __ATOMIC_ACQUIRE) == thread)
      return &napis[i];
  return NULL;
}

static struct frontend_napi *frontend_napi_get(struct napi_struct *napi) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  if (entry)
    __atomic_add_fetch(&entry->references, 1, __ATOMIC_RELAXED);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return entry;
}

static void frontend_napi_put(struct frontend_napi *entry) {
  if (!entry)
    return;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  __atomic_sub_fetch(&entry->references, 1, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

/* Imported drivers can enable features during PCI probe. Reconcile their
 * hardware state before opening the native linear-frame endpoint. */
static int frontend_limit_features(struct net_device *dev) {
  netdev_features_t features = dev->features & NETIF_F_HIGHDMA;
  if (features != dev->features && dev->netdev_ops &&
      dev->netdev_ops->ndo_set_features) {
    int ret = dev->netdev_ops->ndo_set_features(dev, features);
    if (ret < 0)
      return ret;
  }
  dev->features = features;
  dev->hw_features = 0;
  dev->wanted_features = 0;
  dev->vlan_features = 0;
  return 0;
}

static int frontend_open(void *context) {
  struct frontend_netdev *entry = context;
  int ret = entry->dev->netdev_ops && entry->dev->netdev_ops->ndo_open
                ? entry->dev->netdev_ops->ndo_open(entry->dev) : 0;
  if (!ret)
    set_bit(__LINK_STATE_START, &entry->dev->state);
  return ret;
}

static void frontend_stop(void *context) {
  struct frontend_netdev *entry = context;
  struct net_device *dev = entry->dev;
  clear_bit(__LINK_STATE_START, &dev->state);
  if (dev->netdev_ops && dev->netdev_ops->ndo_stop)
    dev->netdev_ops->ndo_stop(dev);
  /* A driver may omit napi_disable in ndo_stop. Synchronize every registered
   * poll context before native unregister drains RX references. */
  for (unsigned i = 0; i < FRONTEND_NAPI_SLOTS; i++) {
    unsigned long irq_flags;
    spin_lock_irqsave(&frontend_lock, irq_flags);
    struct frontend_napi *n = napis[i].used && !napis[i].removing &&
                              napis[i].dev == dev ? &napis[i] : NULL;
    if (n)
      __atomic_add_fetch(&n->references, 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    if (n)
      linuxkpi_native_napi_disable(n->native);
    frontend_napi_put(n);
  }
}

static int frontend_carrier(void *context) {
  struct frontend_netdev *entry = context;
  return netif_carrier_ok(entry->dev);
}

static enum linuxkpi_native_tx_result frontend_transmit(
    void *context, const unsigned char *data, unsigned long length) {
  struct frontend_netdev *entry = context;
  struct net_device *dev = entry->dev;
  __atomic_add_fetch(&frontend_tx_active, 1, __ATOMIC_ACQUIRE);
  if (!test_bit(__LINK_STATE_PRESENT, &dev->state) ||
      !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) {
    __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
    return LINUXKPI_NATIVE_TX_FAILED;
  }
  if (netif_queue_stopped(dev)) {
    __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
    return LINUXKPI_NATIVE_TX_BUSY;
  }
  if (length > (unsigned long)~0U - (ETH_ZLEN - ETH_HLEN)) {
    __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
    return LINUXKPI_NATIVE_TX_FAILED;
  }
  struct sk_buff *skb = alloc_skb((unsigned int)length + ETH_ZLEN - ETH_HLEN,
                                  GFP_ATOMIC);
  if (!skb) {
    __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
    return LINUXKPI_NATIVE_TX_FAILED;
  }
  void *dst = skb_put(skb, (unsigned int)length);
  if (!dst) {
    kfree_skb(skb);
    __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
    return LINUXKPI_NATIVE_TX_FAILED;
  }
  memcpy(dst, data, length);
  skb->dev = dev;
  skb->queue_mapping = 0;
  unsigned int network_offset = ETH_HLEN;
  unsigned short protocol = length >= ETH_HLEN
      ? (unsigned short)(((unsigned short)data[12] << 8) | data[13])
      : ETH_P_802_3;
  if (length >= ETH_HLEN + 4 &&
      (protocol == ETH_P_8021Q || protocol == ETH_P_8021AD)) {
    protocol = (unsigned short)(((unsigned short)data[16] << 8) | data[17]);
    network_offset += 4;
  }
  skb->protocol = htons(protocol);
  if (length >= ETH_HLEN)
    skb->network_header = (u16)network_offset;
  netdev_tx_t result = dev->netdev_ops->ndo_start_xmit(skb, dev);
  __atomic_sub_fetch(&frontend_tx_active, 1, __ATOMIC_RELEASE);
  if (result == NETDEV_TX_OK)
    return LINUXKPI_NATIVE_TX_ACCEPTED;
  kfree_skb(skb);
  return result == NETDEV_TX_BUSY ? LINUXKPI_NATIVE_TX_BUSY
                                  : LINUXKPI_NATIVE_TX_FAILED;
}

static const struct linuxkpi_native_net_ops frontend_net_ops = {
  .open = frontend_open,
  .stop = frontend_stop,
  .carrier = frontend_carrier,
  .transmit = frontend_transmit,
};

bool linuxkpi_netdev_is_bound(struct net_device *dev) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  bool bound = entry != NULL;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return bound;
}

/* Test hook for the R3 failed-probe case: fail the next registration once,
 * after the driver has already acquired its probe-time resources. */
static int register_fault;

void linuxkpi_netdev_fail_next_register(int err) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  register_fault = err;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

unsigned linuxkpi_netdev_registered_count(void) {
  unsigned count = 0;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  for (unsigned i = 0; i < FRONTEND_NETDEVS; i++)
    count += netdevs[i].used;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return count;
}

int register_netdev(struct net_device *dev) {
  if (!dev || !dev->dev_addr || dev->addr_len != ETH_ALEN)
    return -EINVAL;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  int fault = register_fault;
  register_fault = 0;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  if (fault) {
    pr_info("[KERNEL] LinuxKPI: injected register_netdev failure %d\n", fault);
    return fault;
  }
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  if (entry) {
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    return -EEXIST;
  }
  for (unsigned i = 0; i < FRONTEND_NETDEVS; i++) {
    if (!netdevs[i].used) {
      entry = &netdevs[i];
      memset(entry, 0, sizeof(*entry));
      entry->used = true;
      entry->dev = dev;
      break;
    }
  }
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  if (!entry)
    return -ENOSPC;

  char requested_name[IFNAMSIZ];
  strscpy(requested_name, dev->name, sizeof(requested_name));
  if (strchr(requested_name, '%'))
    requested_name[0] = '\0';
  if (dev->netdev_ops && dev->netdev_ops->ndo_init) {
    int ret = dev->netdev_ops->ndo_init(dev);
    if (ret) {
      spin_lock_irqsave(&frontend_lock, irq_flags);
      memset(entry, 0, sizeof(*entry));
      spin_unlock_irqrestore(&frontend_lock, irq_flags);
      return ret;
    }
  }
  int ret = frontend_limit_features(dev);
  if (ret) {
    if (dev->netdev_ops && dev->netdev_ops->ndo_uninit)
      dev->netdev_ops->ndo_uninit(dev);
    spin_lock_irqsave(&frontend_lock, irq_flags);
    memset(entry, 0, sizeof(*entry));
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    return ret;
  }
  spin_lock_irqsave(&frontend_lock, irq_flags);
  entry->active = true;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  struct linuxkpi_native_net *native = linuxkpi_native_net_register(
      requested_name, dev->dev_addr, (unsigned short)dev->mtu,
      &frontend_net_ops, entry);
  if (!native) {
    if (dev->netdev_ops && dev->netdev_ops->ndo_uninit)
      dev->netdev_ops->ndo_uninit(dev);
    spin_lock_irqsave(&frontend_lock, irq_flags);
    memset(entry, 0, sizeof(*entry));
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    return -ENODEV;
  }
  spin_lock_irqsave(&frontend_lock, irq_flags);
  entry->native = native;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  linuxkpi_native_net_get_name(native, dev->name);
  dev->dev.init_name = dev->name;
  return 0;
}

void unregister_netdev(struct net_device *dev) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  struct linuxkpi_native_net *native = entry ? entry->native : NULL;
  if (entry)
    entry->active = false;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  if (!entry)
    return;
  while (__atomic_load_n(&entry->rx_tap_users, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&entry->rx_inflight, __ATOMIC_ACQUIRE))
    msleep(1);
  if (linuxkpi_native_net_unregister(native) != 0) {
    spin_lock_irqsave(&frontend_lock, irq_flags);
    entry->active = true;
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    return;
  }
  if (dev->netdev_ops && dev->netdev_ops->ndo_uninit)
    dev->netdev_ops->ndo_uninit(dev);
  spin_lock_irqsave(&frontend_lock, irq_flags);
  memset(entry, 0, sizeof(*entry));
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

void netif_carrier_on(struct net_device *dev) {
  if (dev)
    clear_bit(__LINK_STATE_NOCARRIER, &dev->state);
}

void netif_carrier_off(struct net_device *dev) {
  if (dev)
    set_bit(__LINK_STATE_NOCARRIER, &dev->state);
}

void netif_tx_wake_queue(struct netdev_queue *queue) {
  if (!queue)
    return;
  clear_bit(__QUEUE_STATE_DRV_XOFF, &queue->state);
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(queue->dev);
  struct linuxkpi_native_net *native = entry ? entry->native : NULL;
  if (native)
    linuxkpi_native_net_tx_wake(native);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

void linuxkpi_netdev_test_stats(struct net_device *dev,
                                unsigned long long *rx_packets,
                                unsigned long long *tx_packets,
                                unsigned long long *rx_dropped,
                                unsigned long long *tx_dropped) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  if (entry)
    linuxkpi_native_net_stats(entry->native, rx_packets, tx_packets,
                              rx_dropped, tx_dropped);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

void linuxkpi_netdev_test_set_rx_tap(struct net_device *dev,
                                     linuxkpi_test_rx_tap_fn tap,
                                     void *context) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  struct linuxkpi_native_net *native = entry ? entry->native : NULL;
  if (native)
    __atomic_add_fetch(&entry->rx_tap_users, 1, __ATOMIC_RELAXED);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  if (native) {
    /* Changing the tap can wait for an RX worker. Keep the device pinned
     * while allowing interrupts and the worker to make progress. */
    linuxkpi_native_net_set_rx_tap(native, tap, context);
    __atomic_sub_fetch(&entry->rx_tap_users, 1, __ATOMIC_RELEASE);
  }
}

unsigned linuxkpi_netdev_test_references(struct net_device *dev) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  unsigned references = entry ? linuxkpi_native_net_references(entry->native) : 0;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return references;
}

int dev_queue_xmit(struct sk_buff *skb) {
  if (!skb || !skb->dev) {
    kfree_skb(skb);
    return -EINVAL;
  }
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(skb->dev);
  int ret = entry ? linuxkpi_native_net_transmit(
                        entry->native, skb->data, skb->len) : -ENODEV;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  kfree_skb(skb);
  return ret;
}

void netif_tx_stop_all_queues(struct net_device *dev) {
  if (!dev || !dev->_tx)
    return;
  for (unsigned int i = 0; i < dev->num_tx_queues; i++)
    set_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[i].state);
}

static void frontend_napi_flush_rx(struct frontend_napi *entry) {
  if (!entry || !entry->rx_batch_count)
    return;
  unsigned int count = entry->rx_batch_count;
  struct frontend_netdev *netdev = entry->rx_batch_netdev;
  struct linuxkpi_native_net *native = entry->rx_batch_native;
  entry->rx_batch_count = 0;
  entry->rx_batch_netdev = NULL;
  entry->rx_batch_native = NULL;
  if (native)
    (void)linuxkpi_native_net_rx_batch(native, entry->rx_batch_frames, count);
  for (unsigned int i = 0; i < count; i++) {
    kfree_skb(entry->rx_batch_skbs[i]);
    entry->rx_batch_skbs[i] = NULL;
  }
  if (netdev)
    __atomic_sub_fetch(&netdev->rx_inflight, 1, __ATOMIC_RELEASE);
}

static enum hrtimer_restart frontend_napi_defer_cb(struct hrtimer *timer) {
  struct frontend_napi *entry =
      container_of(timer, struct frontend_napi, defer_timer);
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  if (entry->used && !entry->removing && entry->native && entry->napi &&
      !test_bit(NAPI_STATE_DISABLE, &entry->napi->state)) {
    if (linuxkpi_native_napi_schedule(entry->native) != 0)
      clear_bit(NAPI_STATE_SCHED, &entry->napi->state);
  }
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return HRTIMER_NORESTART;
}

static int frontend_poll(void *context, int budget) {
  struct frontend_napi *entry = context;
  __atomic_store_n(&entry->poll_thread, linuxkpi_current_thread(),
                   __ATOMIC_RELEASE);
  int work = entry->napi->poll(entry->napi, budget);
  frontend_napi_flush_rx(entry);
  __atomic_store_n(&entry->poll_thread, NULL, __ATOMIC_RELEASE);
  return work;
}

void netif_napi_add_weight_locked(struct net_device *dev,
                                  struct napi_struct *napi,
                                  int (*poll)(struct napi_struct *, int),
                                  int weight) {
  if (!dev || !napi || !poll || weight <= 0)
    return;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  if (!entry) {
    for (unsigned i = 0; i < FRONTEND_NAPI_SLOTS; i++)
      if (!napis[i].used) {
        entry = &napis[i];
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        break;
      }
  }
  if (!entry) {
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    return;
  }
  entry->napi = napi;
  entry->dev = dev;
  entry->defer_hard_irqs_count = 0;
  hrtimer_init(&entry->defer_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  entry->defer_timer.function = frontend_napi_defer_cb;
  napi->dev = dev;
  napi->poll = poll;
  napi->weight = weight;
  napi->state = BIT(NAPI_STATE_DISABLE);
  INIT_LIST_HEAD(&napi->poll_list);
  INIT_LIST_HEAD(&napi->dev_list);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  struct linuxkpi_native_napi *native = linuxkpi_native_napi_create(
      frontend_poll, entry, (unsigned)weight);
  spin_lock_irqsave(&frontend_lock, irq_flags);
  bool attached = entry->used && entry->napi == napi && !entry->removing;
  if (attached)
    entry->native = native;
  if (!native)
    memset(entry, 0, sizeof(*entry));
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  if (native && !attached)
    linuxkpi_native_napi_destroy(native);
}

void __netif_napi_del_locked(struct napi_struct *napi) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  if (entry)
    entry->removing = true;
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  while (entry && __atomic_load_n(&entry->references, __ATOMIC_ACQUIRE) != 0)
    __asm__ volatile("pause" ::: "memory");
  if (entry) {
    hrtimer_cancel(&entry->defer_timer);
    entry->defer_hard_irqs_count = 0;
  }
  struct linuxkpi_native_napi *native = entry ? entry->native : NULL;
  if (native)
    linuxkpi_native_napi_destroy(native);
  if (entry) {
    spin_lock_irqsave(&frontend_lock, irq_flags);
    memset(entry, 0, sizeof(*entry));
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
  }
  if (napi) {
    clear_bit(NAPI_STATE_SCHED, &napi->state);
    clear_bit(NAPI_STATE_MISSED, &napi->state);
    napi->dev = NULL;
  }
}

bool napi_schedule_prep(struct napi_struct *napi) {
  if (!napi)
    return false;
  unsigned long irq_flags;
  bool start = false;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  if (entry && entry->native && !test_bit(NAPI_STATE_DISABLE, &napi->state)) {
    start = !test_and_set_bit(NAPI_STATE_SCHED, &napi->state);
    if (!start) {
      /* The first producer may still be masking its hardware IRQ before
       * calling __napi_schedule(). Only an active poll needs a repoll. */
      linuxkpi_native_napi_mark_missed(entry->native);
    }
  }
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return start;
}

static void frontend_napi_schedule(struct napi_struct *napi) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  struct linuxkpi_native_napi *native = entry ? entry->native : NULL;
  if (!native || test_bit(NAPI_STATE_DISABLE, &napi->state) ||
      linuxkpi_native_napi_schedule(native) != 0)
    clear_bit(NAPI_STATE_SCHED, &napi->state);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

void __napi_schedule(struct napi_struct *napi) { frontend_napi_schedule(napi); }
void __napi_schedule_irqoff(struct napi_struct *napi) { frontend_napi_schedule(napi); }

bool napi_complete_done(struct napi_struct *napi, int work_done) {
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  unsigned int max_defer = (entry && entry->dev && entry->dev->gro_flush_timeout)
                               ? entry->dev->napi_defer_hard_irqs : 0;
  if (entry && entry->native && !test_bit(NAPI_STATE_DISABLE, &napi->state) &&
      max_defer > 0) {
    if (work_done > 0)
      entry->defer_hard_irqs_count = max_defer;
    if (entry->defer_hard_irqs_count > 0 &&
        linuxkpi_native_napi_defer(entry->native, work_done)) {
      entry->defer_hard_irqs_count--;
      __atomic_add_fetch(&entry->references, 1, __ATOMIC_RELAXED);
      spin_unlock_irqrestore(&frontend_lock, irq_flags);
      hrtimer_start(&entry->defer_timer, ns_to_ktime(1000000ULL),
                    HRTIMER_MODE_REL);
      if (test_bit(NAPI_STATE_DISABLE, &napi->state))
        hrtimer_cancel(&entry->defer_timer);
      frontend_napi_put(entry);
      return false;
    }
  }
  bool complete = entry && entry->native &&
      !test_bit(NAPI_STATE_DISABLE, &napi->state) &&
      linuxkpi_native_napi_complete(entry->native, work_done);
  if (complete)
    clear_bit(NAPI_STATE_SCHED, &napi->state);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  return complete;
}

void napi_enable(struct napi_struct *napi) {
  if (!napi)
    return;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_napi *entry = find_napi_locked(napi);
  struct linuxkpi_native_napi *native = entry ? entry->native : NULL;
  if (native) {
    linuxkpi_native_napi_enable(native);
    clear_bit(NAPI_STATE_DISABLE, &napi->state);
  }
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
}

void napi_disable(struct napi_struct *napi) {
  if (!napi)
    return;
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  set_bit(NAPI_STATE_DISABLE, &napi->state);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);
  struct frontend_napi *entry = frontend_napi_get(napi);
  if (entry) {
    hrtimer_cancel(&entry->defer_timer);
    entry->defer_hard_irqs_count = 0;
  }
  struct linuxkpi_native_napi *native = entry ? entry->native : NULL;
  if (native)
    linuxkpi_native_napi_disable(native);
  if (entry)
    hrtimer_cancel(&entry->defer_timer);
  clear_bit(NAPI_STATE_SCHED, &napi->state);
  clear_bit(NAPI_STATE_MISSED, &napi->state);
  frontend_napi_put(entry);
}

gro_result_t gro_receive_skb(struct gro_node *gro, struct sk_buff *skb) {
  (void)gro;
  if (!skb)
    return GRO_NORMAL;
  netif_receive_skb(skb);
  return GRO_NORMAL;
}

int netif_receive_skb(struct sk_buff *skb) {
  if (!skb)
    return -EINVAL;
  struct net_device *dev = skb->dev;
  unsigned char *mac = skb_mac_header_was_set(skb) ? skb_mac_header(skb)
                                                   : skb->data;
  unsigned char *tail = skb_tail_pointer(skb);
  if (tail < mac) {
    kfree_skb(skb);
    return -EINVAL;
  }
  unsigned long frame_len = (unsigned long)(tail - mac);

  void *self = linuxkpi_current_thread();
  unsigned long irq_flags;
  spin_lock_irqsave(&frontend_lock, irq_flags);
  struct frontend_netdev *entry = find_netdev_locked(dev);
  struct linuxkpi_native_net *native = entry ? entry->native : NULL;
  if (!entry || !native) {
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    kfree_skb(skb);
    return -ENODEV;
  }
  struct frontend_napi *napi_entry = find_polling_napi_locked(dev, self);
  if (napi_entry) {
    if (napi_entry->rx_batch_count == 0) {
      napi_entry->rx_batch_netdev = entry;
      napi_entry->rx_batch_native = native;
      __atomic_add_fetch(&entry->rx_inflight, 1, __ATOMIC_ACQUIRE);
    } else if (napi_entry->rx_batch_netdev != entry) {
      spin_unlock_irqrestore(&frontend_lock, irq_flags);
      frontend_napi_flush_rx(napi_entry);
      spin_lock_irqsave(&frontend_lock, irq_flags);
      entry = find_netdev_locked(dev);
      native = entry ? entry->native : NULL;
      if (!entry || !native) {
        spin_unlock_irqrestore(&frontend_lock, irq_flags);
        kfree_skb(skb);
        return -ENODEV;
      }
      napi_entry->rx_batch_netdev = entry;
      napi_entry->rx_batch_native = native;
      __atomic_add_fetch(&entry->rx_inflight, 1, __ATOMIC_ACQUIRE);
    }
    unsigned int idx = napi_entry->rx_batch_count++;
    napi_entry->rx_batch_skbs[idx] = skb;
    napi_entry->rx_batch_frames[idx].data = mac;
    napi_entry->rx_batch_frames[idx].length = frame_len;
    bool full = napi_entry->rx_batch_count == LINUXKPI_NATIVE_RX_BATCH_MAX;
    spin_unlock_irqrestore(&frontend_lock, irq_flags);
    if (full)
      frontend_napi_flush_rx(napi_entry);
    return 0;
  }
  __atomic_add_fetch(&entry->rx_inflight, 1, __ATOMIC_ACQUIRE);
  spin_unlock_irqrestore(&frontend_lock, irq_flags);

  int ret = linuxkpi_native_net_rx(native, mac, frame_len);
  __atomic_sub_fetch(&entry->rx_inflight, 1, __ATOMIC_RELEASE);
  kfree_skb(skb);
  return ret;
}

int __netif_rx(struct sk_buff *skb) { return netif_receive_skb(skb); }
int netif_rx(struct sk_buff *skb) { return netif_receive_skb(skb); }

__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev) {
  if (!skb || skb->len < ETH_HLEN)
    return 0;
  skb->dev = dev;
  skb_reset_mac_header(skb);
  __be16 protocol = *(__be16 *)(skb->data + 12);
  skb_pull(skb, ETH_HLEN);
  skb->network_header = (u16)(skb->data - skb->head);
  return protocol;
}

void netdev_stats_to_stats64(void *out,
                             const struct net_device_stats *in) {
  u64 *stats = out;
#define COPY_NET_STAT(index, name) stats[index] = in->name
  COPY_NET_STAT(0, rx_packets); COPY_NET_STAT(1, tx_packets);
  COPY_NET_STAT(2, rx_bytes); COPY_NET_STAT(3, tx_bytes);
  COPY_NET_STAT(4, rx_errors); COPY_NET_STAT(5, tx_errors);
  COPY_NET_STAT(6, rx_dropped); COPY_NET_STAT(7, tx_dropped);
  COPY_NET_STAT(8, multicast); COPY_NET_STAT(9, collisions);
  COPY_NET_STAT(10, rx_length_errors); COPY_NET_STAT(11, rx_over_errors);
  COPY_NET_STAT(12, rx_crc_errors); COPY_NET_STAT(13, rx_frame_errors);
  COPY_NET_STAT(14, rx_fifo_errors); COPY_NET_STAT(15, rx_missed_errors);
  COPY_NET_STAT(16, tx_aborted_errors); COPY_NET_STAT(17, tx_carrier_errors);
  COPY_NET_STAT(18, tx_fifo_errors); COPY_NET_STAT(19, tx_heartbeat_errors);
  COPY_NET_STAT(20, tx_window_errors); COPY_NET_STAT(21, rx_compressed);
  COPY_NET_STAT(22, tx_compressed);
#undef COPY_NET_STAT
  /* These two fields follow the legacy `net_device_stats` members in the
   * pinned v6.18 `rtnl_link_stats64` UAPI layout and have no legacy source. */
  stats[23] = 0;
  stats[24] = 0;
}

void dev_fetch_sw_netstats(void *out,
                           const struct pcpu_sw_netstats *software) {
  u64 *stats = out;
  stats[0] = __atomic_load_n(&software->rx_packets, __ATOMIC_RELAXED);
  stats[2] = __atomic_load_n(&software->rx_bytes, __ATOMIC_RELAXED);
  stats[1] = __atomic_load_n(&software->tx_packets, __ATOMIC_RELAXED);
  stats[3] = __atomic_load_n(&software->tx_bytes, __ATOMIC_RELAXED);
}

bool is_valid_ether_addr(const u8 *addr) {
  if (!addr || (addr[0] & 1U))
    return false;
  u8 nonzero = 0;
  for (unsigned int i = 0; i < ETH_ALEN; i++)
    nonzero |= addr[i];
  return nonzero != 0;
}

void eth_hw_addr_set(struct net_device *dev, const u8 *addr) {
  if (dev && dev->dev_addr && addr)
    memcpy(dev->dev_addr, addr, ETH_ALEN);
}

int eth_mac_addr(struct net_device *dev, void *p) {
  struct frontend_sockaddr { unsigned short family; unsigned char data[14]; };
  if (!dev || !p)
    return -EINVAL;
  const struct frontend_sockaddr *sa = p;
  if (!is_valid_ether_addr(sa->data))
    return -EADDRNOTAVAIL;
  eth_hw_addr_set(dev, sa->data);
  return 0;
}

int eth_validate_addr(struct net_device *dev) {
  return dev && is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}

int eth_platform_get_mac_address(struct device *dev, u8 *addr) {
  (void)dev;
  (void)addr;
  /* This platform has no firmware-node MAC property service. */
  return -ENOENT;
}

void eth_random_addr(u8 *addr) {
  if (!addr)
    return;
  /* MAC address randomization is not secret material. Use the existing
   * hardware-RNG-first identifier byte source, then enforce local-unicast. */
  linuxkpi_guid_bytes(addr, ETH_ALEN);
  addr[0] = (addr[0] & 0xfeU) | 0x02U;
}

struct sk_buff *__alloc_skb(unsigned int size, gfp_t priority, int flags,
                            int node) {
  (void)priority;
  (void)flags;
  (void)node;
  size_t requested = size ? (size_t)size : 1;
  if (requested > (size_t)~0U - SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) - 15U)
    return NULL;
  size_t aligned = SKB_DATA_ALIGN(requested);
  if (aligned > (size_t)~0U)
    return NULL;
  size_t total = aligned + SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
  struct sk_buff *skb = kzalloc(sizeof(*skb), GFP_ATOMIC);
  if (!skb)
    return NULL;
  skb->head = kmalloc(total, GFP_ATOMIC);
  if (!skb->head) {
    kfree(skb);
    return NULL;
  }
  memset(skb->head + aligned, 0, sizeof(struct skb_shared_info));
  skb->data = skb->head;
  skb->tail = 0;
  skb->end = (sk_buff_data_t)aligned;
  skb->truesize = (unsigned int)(sizeof(*skb) + total);
  refcount_set(&skb->users, 1);
  skb_shinfo(skb)->nr_frags = 0;
  atomic_set(&skb_shinfo(skb)->dataref, 1);
  skb->mac_header = (u16)~0U;
  skb->network_header = (u16)~0U;
  skb->transport_header = (u16)~0U;
  return skb;
}

void __kfree_skb(struct sk_buff *skb) {
  if (!skb)
    return;
  kfree(skb->head);
  kfree(skb);
}

void sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb,
                        enum skb_drop_reason reason) {
  (void)sk;
  (void)reason;
  __kfree_skb(skb);
}

void dev_kfree_skb_any_reason(struct sk_buff *skb,
                              enum skb_drop_reason reason) {
  sk_skb_reason_drop(NULL, skb, reason);
}

void ether_setup(struct net_device *dev) {
  if (!dev)
    return;
  dev->type = ARPHRD_ETHER;
  dev->addr_len = ETH_ALEN;
  dev->mtu = ETH_DATA_LEN;
  dev->dev_addr = dev->perm_addr;
  dev->flags = IFF_BROADCAST | IFF_MULTICAST;
  INIT_LIST_HEAD(&dev->mc_list);
  set_bit(__LINK_STATE_PRESENT, &dev->state);
  memset(dev->broadcast, 0xff, ETH_ALEN);
  set_bit(__LINK_STATE_NOCARRIER, &dev->state);
}

struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name,
                                    unsigned char name_assign_type,
                                    void (*setup)(struct net_device *),
                                    unsigned int txqs, unsigned int rxqs) {
  (void)name_assign_type;
  if (sizeof_priv < 0 || txqs == 0 || rxqs == 0 || txqs > 16 || rxqs > 16)
    return NULL;
  size_t size = sizeof(struct net_device) + (size_t)sizeof_priv;
  struct net_device *dev = kzalloc(size, GFP_KERNEL);
  if (!dev)
    return NULL;
  device_initialize(&dev->dev);
  dev->tstats = &dev->tstats_storage;
  dev->ethtool = kzalloc(sizeof(*dev->ethtool), GFP_KERNEL);
  if (!dev->ethtool) {
    devres_release_all(&dev->dev);
    kfree(dev);
    return NULL;
  }
  xa_init(&dev->ethtool->rss_ctx);
  mutex_init(&dev->ethtool->rss_lock);
  dev->priv = (char *)dev + sizeof(*dev);
  dev->dev_addr = dev->perm_addr;
  set_bit(__LINK_STATE_PRESENT, &dev->state);
  dev->num_tx_queues = txqs;
  dev->real_num_tx_queues = txqs;
  dev->num_rx_queues = rxqs;
  dev->real_num_rx_queues = rxqs;
  dev->tx_queue_len = 1000;
  dev->_tx = kcalloc(txqs, sizeof(*dev->_tx), GFP_KERNEL);
  if (!dev->_tx) {
    xa_destroy(&dev->ethtool->rss_ctx);
    kfree(dev->ethtool);
    devres_release_all(&dev->dev);
    kfree(dev);
    return NULL;
  }
  mutex_init(&dev->lock);
  for (unsigned int i = 0; i < txqs; i++) {
    dev->_tx[i].dev = dev;
    dev->_tx[i].xmit_lock_owner = -1;
    spin_lock_init(&dev->_tx[i]._xmit_lock);
  }
  if (name)
    strscpy(dev->name, name, sizeof(dev->name));
  if (setup)
    setup(dev);
  return dev;
}

struct net_device *alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs,
                                      unsigned int rxqs) {
  return alloc_netdev_mqs(sizeof_priv, "eth%d", NET_NAME_ENUM, ether_setup,
                          txqs, rxqs);
}

static void frontend_devm_free_netdev(void *data) { free_netdev(data); }

struct net_device *devm_alloc_etherdev_mqs(struct device *parent,
                                           int sizeof_priv,
                                           unsigned int txqs,
                                           unsigned int rxqs) {
  if (!parent)
    return NULL;
  struct net_device *dev = alloc_etherdev_mqs(sizeof_priv, txqs, rxqs);
  if (!dev)
    return NULL;
  dev->dev.parent = parent;
  if (devm_add_action_or_reset(parent, frontend_devm_free_netdev, dev))
    return NULL;
  return dev;
}

void free_netdev(struct net_device *dev) {
  if (!dev)
    return;
  if (linuxkpi_netdev_is_bound(dev))
    unregister_netdev(dev);
  devres_release_all(&dev->dev);
  if (dev->ethtool) {
    xa_destroy(&dev->ethtool->rss_ctx);
    kfree(dev->ethtool);
  }
  kfree(dev->_tx);
  kfree(dev);
}

void netif_device_detach(struct net_device *dev) {
  if (dev)
    clear_bit(__LINK_STATE_PRESENT, &dev->state);
}

void netif_device_attach(struct net_device *dev) {
  if (dev)
    set_bit(__LINK_STATE_PRESENT, &dev->state);
}

void synchronize_net(void) {
  while (__atomic_load_n(&frontend_tx_active, __ATOMIC_ACQUIRE) != 0)
    msleep(1);
}

int netdev_update_features(struct net_device *dev) {
  if (!dev)
    return -EINVAL;
  netdev_features_t features = dev->features;
  if (dev->netdev_ops && dev->netdev_ops->ndo_fix_features)
    features = dev->netdev_ops->ndo_fix_features(dev, features);
  features &= NETIF_F_HIGHDMA;
  if (features == dev->features)
    return 0;
  if (dev->netdev_ops && dev->netdev_ops->ndo_set_features) {
    int ret = dev->netdev_ops->ndo_set_features(dev, features);
    if (ret)
      return ret;
  }
  dev->features = features;
  return 1;
}

void netdev_reset_queue(struct net_device *dev) {
  if (dev && dev->_tx)
    clear_bit(__QUEUE_STATE_DRV_XOFF, &dev->_tx[0].state);
}

void dev_consume_skb_any(struct sk_buff *skb) { kfree_skb(skb); }

bool __netdev_sent_queue(struct net_device *dev, unsigned int bytes, bool more) {
  (void)dev;
  (void)bytes;
  (void)more;
  /* BQL accounting is not enabled; always ring the device doorbell. */
  return true;
}

void netdev_sw_irq_coalesce_default_on(struct net_device *dev) {
  if (!dev)
    return;
#ifndef CONFIG_PREEMPT_RT
  dev->gro_flush_timeout = 20000;
  dev->napi_defer_hard_irqs = 1;
#endif
}

int netif_set_real_num_tx_queues(struct net_device *dev, unsigned int count) {
  if (!dev || count == 0 || count > dev->num_tx_queues)
    return -EINVAL;
  dev->real_num_tx_queues = count;
  return 0;
}

int netif_set_real_num_rx_queues(struct net_device *dev, unsigned int count) {
  if (!dev || count == 0 || count > dev->num_rx_queues)
    return -EINVAL;
  dev->real_num_rx_queues = count;
  return 0;
}
