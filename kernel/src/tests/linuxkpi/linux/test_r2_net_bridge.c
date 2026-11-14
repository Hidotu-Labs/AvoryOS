#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/frontend_net.h>
#include <linuxkpi/log.h>

struct r2_net_fixture {
  struct net_device *dev;
  struct napi_struct napi;
  volatile unsigned rx_pending;
  volatile unsigned poll_calls;
  volatile unsigned tx_calls;
  volatile unsigned tx_accepted;
  volatile unsigned busy_once;
  volatile unsigned busy_always;
  volatile unsigned opened;
  volatile unsigned stopped;
  volatile unsigned missed_complete_false;
  volatile unsigned schedule_during_poll_once;
  volatile unsigned rx_tap_claimed;
  volatile unsigned rx_tap_done;
  volatile unsigned tx_bytes_match;
  volatile unsigned block_rx_tap;
  struct completion rx_tap_entered;
  struct completion rx_tap_release;
  unsigned char expected_rx[64];
  unsigned char seen_rx[64];
  unsigned char expected_tx[64];
};

static struct r2_net_fixture *active_fixture;
static int r2_failures;

static void r2_check(bool condition, const char *name) {
  if (condition)
    klogf("[ OK ] R2 net bridge: %s\n", name);
  else {
    r2_failures++;
    klogf("[FAIL] R2 net bridge: %s\n", name);
  }
}

static int r2_open(struct net_device *dev) {
  struct r2_net_fixture *fixture = active_fixture;
  fixture->dev = dev;
  __atomic_add_fetch(&fixture->opened, 1, __ATOMIC_RELAXED);
  netif_carrier_on(dev);
  netif_start_queue(dev);
  napi_enable(&fixture->napi);
  return 0;
}

static int r2_stop(struct net_device *dev) {
  struct r2_net_fixture *fixture = active_fixture;
  __atomic_add_fetch(&fixture->stopped, 1, __ATOMIC_RELAXED);
  netif_stop_queue(dev);
  napi_disable(&fixture->napi);
  netif_carrier_off(dev);
  return 0;
}

static netdev_tx_t r2_xmit(struct sk_buff *skb, struct net_device *dev) {
  struct r2_net_fixture *fixture = active_fixture;
  __atomic_add_fetch(&fixture->tx_calls, 1, __ATOMIC_RELAXED);
  if (skb->len != sizeof(fixture->expected_tx) ||
      memcmp(skb->data, fixture->expected_tx,
             sizeof(fixture->expected_tx)) != 0)
    __atomic_store_n(&fixture->tx_bytes_match, 0, __ATOMIC_RELEASE);
  if (__atomic_load_n(&fixture->busy_always, __ATOMIC_RELAXED) ||
      __atomic_exchange_n(&fixture->busy_once, 0, __ATOMIC_RELAXED)) {
    netif_stop_queue(dev);
    return NETDEV_TX_BUSY;
  }
  __atomic_add_fetch(&fixture->tx_accepted, 1, __ATOMIC_RELAXED);
  dev_kfree_skb_any_reason(skb, SKB_DROP_REASON_NOT_SPECIFIED);
  return NETDEV_TX_OK;
}

static const struct net_device_ops r2_netdev_ops = {
  .ndo_open = r2_open,
  .ndo_stop = r2_stop,
  .ndo_start_xmit = r2_xmit,
};

static void r2_rx_tap(void *context, const void *frame, unsigned long length) {
  struct r2_net_fixture *fixture = context;
  if (length != sizeof(fixture->seen_rx))
    return;
  if (__atomic_exchange_n(&fixture->rx_tap_claimed, 1, __ATOMIC_ACQ_REL))
    return;
  memcpy(fixture->seen_rx, frame, sizeof(fixture->seen_rx));
  __atomic_store_n(&fixture->rx_tap_done, 1, __ATOMIC_RELEASE);
  if (__atomic_load_n(&fixture->block_rx_tap, __ATOMIC_ACQUIRE)) {
    complete(&fixture->rx_tap_entered);
    wait_for_completion(&fixture->rx_tap_release);
  }
}

static int r2_poll(struct napi_struct *napi, int budget) {
  struct r2_net_fixture *fixture = active_fixture;
  __atomic_add_fetch(&fixture->poll_calls, 1, __ATOMIC_RELAXED);
  if (__atomic_load_n(&fixture->rx_pending, __ATOMIC_RELAXED) <=
          (unsigned)budget &&
      __atomic_exchange_n(&fixture->schedule_during_poll_once, 0,
                          __ATOMIC_RELAXED))
    napi_schedule(napi); /* exercise schedule-during-poll missed handling */

  int done = 0;
  while (done < budget && __atomic_load_n(&fixture->rx_pending, __ATOMIC_RELAXED)) {
    struct sk_buff *skb = alloc_skb(64, GFP_ATOMIC);
    if (!skb)
      break;
    unsigned char *frame = skb_put(skb, 64);
    if (!frame) {
      kfree_skb(skb);
      break;
    }
    memcpy(frame, fixture->expected_rx, 64);
    eth_type_trans(skb, fixture->dev);
    napi_gro_receive(napi, skb);
    __atomic_sub_fetch(&fixture->rx_pending, 1, __ATOMIC_RELAXED);
    done++;
  }
  if (done < budget) {
    if (!napi_complete_done(napi, done))
      __atomic_add_fetch(&fixture->missed_complete_false, 1, __ATOMIC_RELAXED);
  }
  return done;
}

static bool wait_at_least(volatile unsigned *value, unsigned target) {
  for (unsigned i = 0; i < 5000; i++) {
    if (__atomic_load_n(value, __ATOMIC_ACQUIRE) >= target)
      return true;
    msleep(1);
  }
  return false;
}

static unsigned long long wait_rx(struct net_device *dev, unsigned target) {
  unsigned long long rx = 0;
  for (unsigned i = 0; i < 5000; i++) {
    linuxkpi_netdev_test_stats(dev, &rx, NULL, NULL, NULL);
    if (rx >= target)
      return rx;
    msleep(1);
  }
  return rx;
}

static unsigned wait_rx_refs(struct net_device *dev, unsigned target) {
  for (unsigned i = 0; i < 5000; i++) {
    unsigned refs = linuxkpi_netdev_test_references(dev);
    if (refs >= target)
      return refs;
    msleep(1);
  }
  return 0;
}

static int queue_one(struct net_device *dev) {
  struct sk_buff *skb = alloc_skb(64, GFP_KERNEL);
  if (!skb)
    return -ENOMEM;
  unsigned char *frame = skb_put(skb, 64);
  if (!frame) {
    kfree_skb(skb);
    return -ENOMEM;
  }
  memcpy(frame, active_fixture->expected_tx, 64);
  skb->dev = dev;
  return dev_queue_xmit(skb);
}

struct r2_unregister_job {
  struct net_device *dev;
  struct completion started;
  struct completion done;
};

static int r2_unregister_thread(void *opaque) {
  struct r2_unregister_job *job = opaque;
  complete(&job->started);
  unregister_netdev(job->dev);
  complete(&job->done);
  return 0;
}

static bool fixture_cycle(unsigned cycle, bool full) {
  struct r2_net_fixture fixture = {0};
  active_fixture = &fixture;
  fixture.dev = alloc_etherdev(0);
  if (!fixture.dev)
    return false;
  static const unsigned char mac[6] = {0x02, 0x52, 0x32, 0, 0, 1};
  fixture.tx_bytes_match = 1;
  memset(fixture.expected_rx, 0x31, sizeof(fixture.expected_rx));
  memcpy(fixture.expected_rx, mac, sizeof(mac));
  fixture.expected_rx[6] = 0x02;
  fixture.expected_rx[11] = 0x02;
  fixture.expected_rx[12] = 0x08;
  fixture.expected_rx[13] = 0x00;
  memset(fixture.expected_tx, 0x5a, sizeof(fixture.expected_tx));
  memcpy(fixture.expected_tx, mac, sizeof(mac));
  fixture.expected_tx[12] = 0x08;
  fixture.expected_tx[13] = 0x00;
  memcpy(fixture.dev->perm_addr, mac, sizeof(mac));
  fixture.dev->dev_addr = fixture.dev->perm_addr;
  fixture.dev->netdev_ops = &r2_netdev_ops;
  netif_napi_add_weight(fixture.dev, &fixture.napi, r2_poll, 8);
  int reg = register_netdev(fixture.dev);
  r2_check(reg == 0, cycle ? "repeat register" : "register + automatic open");
  if (reg != 0) {
    netif_napi_del(&fixture.napi);
    free_netdev(fixture.dev);
    return false;
  }
  r2_check(__atomic_load_n(&fixture.opened, __ATOMIC_RELAXED) == 1,
           "open callback ran after registration");
  r2_check(netif_carrier_ok(fixture.dev), "carrier state exposed");

  if (full) {
    init_completion(&fixture.rx_tap_entered);
    init_completion(&fixture.rx_tap_release);
    linuxkpi_netdev_test_set_rx_tap(fixture.dev, r2_rx_tap, &fixture);
    fixture.rx_pending = 70;
    fixture.schedule_during_poll_once = 1;
    bool scheduled = napi_schedule(&fixture.napi);
    r2_check(scheduled, "NAPI schedules through deferred worker");
    bool polled = wait_at_least(&fixture.poll_calls, 3);
    unsigned long long rx = wait_rx(fixture.dev, 70);
    r2_check(polled && rx == 70, "budgeted NAPI delivers 70 linear frames");
    r2_check(wait_at_least(&fixture.rx_tap_done, 1) &&
                 memcmp(fixture.seen_rx, fixture.expected_rx, 64) == 0,
             "RX Ethernet bytes preserved through native queue");
    r2_check(__atomic_load_n(&fixture.missed_complete_false, __ATOMIC_RELAXED) >= 1,
             "missed schedule keeps completion masked");

    fixture.busy_once = 1;
    r2_check(queue_one(fixture.dev) == 0, "dev_queue_xmit accepts native TX");
    bool busy_seen = wait_at_least(&fixture.tx_calls, 1);
    netif_wake_queue(fixture.dev);
    bool accepted = wait_at_least(&fixture.tx_accepted, 1);
    r2_check(busy_seen && accepted, "BUSY retains packet until queue wake");

    fixture.busy_always = 1;
    r2_check(queue_one(fixture.dev) == 0, "TX retry enters bounded queue");
    bool blocked = wait_at_least(&fixture.tx_calls, 2);
    msleep(2);
    unsigned enqueued = 0;
    for (; enqueued < 127; enqueued++)
      if (queue_one(fixture.dev) != 0)
        break;
    int overflow = queue_one(fixture.dev);
    r2_check(blocked && enqueued == 127 && overflow == -11,
             "128-frame TX bound applies backpressure");
    fixture.busy_always = 0;
    netif_wake_queue(fixture.dev);
    bool drained = wait_at_least(&fixture.tx_accepted, 129);
    r2_check(drained, "queue wake drains retained TX frames");
    r2_check(__atomic_load_n(&fixture.tx_bytes_match, __ATOMIC_ACQUIRE),
             "TX bytes preserved across BUSY retry and queueing");

    unsigned polls_before = __atomic_load_n(&fixture.poll_calls, __ATOMIC_RELAXED);
    __atomic_store_n(&fixture.rx_tap_claimed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&fixture.rx_tap_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&fixture.block_rx_tap, 1, __ATOMIC_RELEASE);
    fixture.rx_pending = 64;
    bool scheduled_again = napi_schedule(&fixture.napi);
    r2_check(scheduled_again, "second RX burst scheduled");
    bool second_burst = wait_at_least(&fixture.poll_calls, polls_before + 8);
    rx = wait_rx(fixture.dev, 134);
    r2_check(second_burst && rx == 134, "second RX batch accepted before teardown");
    bool tap_held = wait_for_completion_timeout(&fixture.rx_tap_entered, 5000) != 0;
    unsigned refs_before = wait_rx_refs(fixture.dev, 1);
    r2_check(tap_held, "second RX callback holds worker packet");
    r2_check(refs_before >= 1, "RX device reference visible before unregister");
    struct r2_unregister_job job = {.dev = fixture.dev};
    init_completion(&job.started);
    init_completion(&job.done);
    struct task_struct *unregister_task =
        kthread_run(r2_unregister_thread, &job, "r2-unreg");
    bool thread_started = !IS_ERR(unregister_task) &&
                          wait_for_completion_timeout(&job.started, 5000) != 0;
    msleep(20);
    bool drained_early = completion_done(&job.done);
    r2_check(thread_started, "concurrent unregister thread started");
    r2_check(!drained_early, "unregister waits for worker-held RX reference");
    complete(&fixture.rx_tap_release);
    bool unregister_done = thread_started &&
        wait_for_completion_timeout(&job.done, 5000) != 0;
    if (!IS_ERR(unregister_task))
      kthread_stop(unregister_task);
    r2_check(unregister_done, "unregister completes after RX handler drains");
    if (unregister_done)
      goto registered_device_drained;
  }

  unregister_netdev(fixture.dev); /* closes producers and drains queued RX refs */
registered_device_drained:
  r2_check(__atomic_load_n(&fixture.stopped, __ATOMIC_RELAXED) == 1,
           "stop callback and accepted RX reference drain complete");
  netif_napi_del(&fixture.napi);
  free_netdev(fixture.dev);
  active_fixture = NULL;
  return true;
}

void linuxkpi_test_r2_net_bridge(void) {
  r2_failures = 0;
  klog_puts("[R2 NET] starting isolated LinuxKPI bridge fixture\n");
  bool first = fixture_cycle(0, true);
  bool second = fixture_cycle(1, false);
  if (!first || !second)
    r2_failures++;
  if (r2_failures)
    klogf("[R2 NET] FAIL failures=%d\n", r2_failures);
  else
    klog_puts("[R2 NET] PASS register/open, NAPI, RX, TX, backpressure, teardown, repeat\n");
}
