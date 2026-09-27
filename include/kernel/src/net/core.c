#include "net/core.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"

static struct net_packet pool[NET_PACKET_POOL_SIZE];
static bool used[NET_PACKET_POOL_SIZE];
static struct net_packet *queue[NET_PACKET_POOL_SIZE];
static uint32_t head, tail;
static spinlock_t lock = SPINLOCK_INIT;
static wait_queue_t worker_wait;
static struct net_device *default_device;
static struct net_device *devices[NET_DEVICE_MAX];
static net_rx_handler_t rx_handler;
static bool ready;

static uint32_t free_stack[NET_PACKET_POOL_SIZE];
static uint32_t free_top = 0;
static bool pool_inited = false;

static void init_pool_locked(void) {
  if (pool_inited) return;
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE; i++) {
    free_stack[i] = i;
  }
  free_top = NET_PACKET_POOL_SIZE;
  pool_inited = true;
}

static struct net_packet *alloc_locked(void) {
  if (__builtin_expect(!pool_inited, 0))
    init_pool_locked();
  if (free_top > 0) {
    uint32_t idx = free_stack[--free_top];
    used[idx] = true;
    pool[idx].device = NULL;
    pool[idx].length = 0;
    return &pool[idx];
  }
  return NULL;
}
static void free_locked(struct net_packet *p) {
  if (p >= pool && p < pool + NET_PACKET_POOL_SIZE) {
    if (free_top < NET_PACKET_POOL_SIZE) {
      uint32_t idx = (uint32_t)(p - pool);
      if (used[idx]) {
        used[idx] = false;
        free_stack[free_top++] = idx;
      }
    }
  }
}
static bool push_locked(struct net_packet *p) {
  uint32_t next = (head + 1) % NET_PACKET_POOL_SIZE;
  if (next == tail)
    return false;
  queue[head] = p;
  head = next;
  return true;
}

static void net_worker(void) {
  struct thread *self = sched_get_current();
  /* Heap, not stack: a waker on another CPU walks worker_wait until it takes
   * the queue lock, and a stack slot would be recycled by the next loop
   * iteration while that walk is still in flight.  Kernel workers never exit,
   * so the entry has the same lifetime as the thread. */
  wait_queue_entry_t *entry = kmalloc(sizeof(*entry));
  if (!entry) {
    klog_puts("[NET] worker wait-entry allocation failed\n");
    return;
  }
  memset(entry, 0, sizeof(*entry));
  entry->thread = self;
  struct net_packet *batch[128];
  for (;;) {
    size_t count = 0;
    for (;;) {
      spinlock_acquire(&lock);
      if (count > 0) {
        for (size_t i = 0; i < count; i++)
          free_locked(batch[i]);
      }
      count = 0;
      while (count < 128 && tail != head) {
        batch[count++] = queue[tail];
        tail = (tail + 1) % NET_PACKET_POOL_SIZE;
      }
      spinlock_release(&lock);

      if (count == 0)
        break;

      for (size_t i = 0; i < count; i++) {
        if (rx_handler)
          rx_handler(batch[i]);
      }
    }

    wait_queue_add(&worker_wait, entry);
    spinlock_acquire(&lock);
    bool empty = (head == tail);
    if (empty) {
      self->state = THREAD_BLOCKED;
      self->wakeup_ticks = lapic_timer_get_ticks() + 50;
    }
    spinlock_release(&lock);
    if (empty) {
      sched_yield();
      self->wakeup_ticks = 0;
    }
    wait_queue_remove(&worker_wait, entry);
  }
}

/* Dedicated TCP timer so RX processing never has to drive retransmission
 * timers on the packet hot path. */
static void net_timer_worker(void) {
  struct thread *self = sched_get_current();
  for (;;) {
    tcp_timer_tick(lapic_timer_get_ticks());
    self->state = THREAD_SLEEPING;
    self->wakeup_ticks = lapic_timer_get_ticks() + 10;
    sched_yield();
    self->wakeup_ticks = 0;
  }
}

bool net_rx_submit_irq(struct net_device *dev, const void *frame, size_t len) {
  if (!ready || !frame || !len || len > NET_FRAME_MAX) {
    if (dev && (uintptr_t)dev >= 0xFFFF800000000000ULL)
      dev->stats.rx_errors++;
    return false;
  }
  if (!dev || (uintptr_t)dev < 0xFFFF800000000000ULL)
    dev = default_device;
  if (!dev)
    return false;
  spinlock_acquire(&lock);
  struct net_packet *p = alloc_locked();
  if (!p) {
    dev->stats.rx_dropped++;
    dev->stats.queue_full++;
    spinlock_release(&lock);
    static uint64_t last_pool_log = 0;
    uint64_t now = lapic_timer_get_ticks();
    if (now - last_pool_log >= 1000) {
      last_pool_log = now;
      klog_puts("[NET] rx packet pool empty!\n");
    }
    return false;
  }
  p->device = dev;
  p->length = (uint16_t)len;
  memcpy(p->data, frame, len);
  bool was_empty = (head == tail);
  if (!push_locked(p)) {
    free_locked(p);
    dev->stats.rx_dropped++;
    dev->stats.queue_full++;
    spinlock_release(&lock);
    static uint64_t last_qfull_log = 0;
    uint64_t now = lapic_timer_get_ticks();
    if (now - last_qfull_log >= 1000) {
      last_qfull_log = now;
      klog_puts("[NET] rx queue full!\n");
    }
    return false;
  }
  dev->stats.rx_packets++;
  dev->stats.rx_bytes += len;
  spinlock_release(&lock);
  if (was_empty)
    wait_queue_wake_all(&worker_wait);
  else
    wait_queue_wake_one(&worker_wait);
  return true;
}

int net_device_register(struct net_device *dev) {
  if (!ready || !dev || !dev->ops || !dev->ops->link_up || !dev->ops->stop ||
      dev->registered || !dev->name[0])
    return -1;
  int free_slot = -1;
  for (int i = 0; i < NET_DEVICE_MAX; i++) {
    if (devices[i] && strcmp(devices[i]->name, dev->name) == 0)
      return -1;
    if (!devices[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0)
    return -1;
  if (!dev->mtu)
    dev->mtu = NET_MTU_ETHERNET;
  dev->registered = true;
  devices[free_slot] = dev;
  if (!default_device)
    default_device = dev;
  return 0;
}

int net_device_unregister(struct net_device *dev) {
  if (!dev || !dev->registered)
    return -1;
  for (int i = 0; i < NET_DEVICE_MAX; i++)
    if (devices[i] == dev)
      devices[i] = NULL;
  if (default_device == dev) {
    default_device = NULL;
    for (int i = 0; i < NET_DEVICE_MAX; i++)
      if (devices[i]) {
        default_device = devices[i];
        break;
      }
  }
  dev->registered = false;
  return 0;
}

struct net_device *net_device_default(void) { return default_device; }

struct net_device *net_device_find(const char *name) {
  if (!name)
    return NULL;
  for (int i = 0; i < NET_DEVICE_MAX; i++)
    if (devices[i] && strcmp(devices[i]->name, name) == 0)
      return devices[i];
  return NULL;
}
void net_set_rx_handler(net_rx_handler_t handler) { rx_handler = handler; }
net_rx_handler_t net_get_rx_handler(void) { return rx_handler; }

void net_core_init(void) {
  spinlock_init(&lock);
  wait_queue_init(&worker_wait);
  memset(used, 0, sizeof(used));
  memset(devices, 0, sizeof(devices));
  default_device = NULL;
  head = tail = 0;
  ready = true;
  klog_puts("[NET] core prepared\n");
}

void net_core_start_worker(void) {
  uint32_t cpus = cpu_get_count();
  uint32_t workers = (cpus > 1) ? 2 : 1;
  if (workers > 4)
    workers = 4;

  for (uint32_t i = 0; i < workers; i++) {
    struct thread *worker =
        sched_create_kernel_thread(net_worker, cpu_get_current(), true);
    if (!worker) {
      ready = false;
      klog_puts("[NET] worker creation failed\n");
      return;
    }
    strcpy(worker->comm, "net-worker");
    sched_set_priority(worker, 0, -20);
  }
  klog_puts("[NET] rx workers started\n");
}

void net_core_start_timer(void) {
  struct thread *timer =
      sched_create_kernel_thread(net_timer_worker, cpu_get_current(), true);
  if (!timer) {
    klog_puts("[NET] tcp timer creation failed\n");
    return;
  }
  strcpy(timer->comm, "tcp-timer");
  sched_set_priority(timer, 0, -10);
  klog_puts("[NET] tcp timer started\n");
}

void net_print_stats(const struct net_device *dev) {
  if (!dev)
    return;
  klog_puts("[NET] rx=");
  klog_uint64(dev->stats.rx_packets);
  klog_puts(" tx=");
  klog_uint64(dev->stats.tx_packets);
  klog_puts(" drop=");
  klog_uint64(dev->stats.rx_dropped + dev->stats.tx_dropped);
  klog_puts(" err=");
  klog_uint64(dev->stats.rx_errors + dev->stats.tx_errors);
  klog_puts(" irq=");
  klog_uint64(dev->stats.interrupts);
  klog_puts(" reset=");
  klog_uint64(dev->stats.resets);
  klog_puts(" qfull=");
  klog_uint64(dev->stats.queue_full);
  klog_puts(" linkchg=");
  klog_uint64(dev->stats.link_changes);
  klog_puts(" rxovf=");
  klog_uint64(dev->stats.rx_overflows);
  klog_puts(" txunderrun=");
  klog_uint64(dev->stats.tx_underruns);
  klog_puts("\n");
}

void net_queue_snapshot(uint32_t *queued, uint32_t *in_use) {
  uint32_t allocated = 0;
  spinlock_acquire(&lock);
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE; i++)
    allocated += used[i] ? 1u : 0u;
  if (queued)
    *queued = (head + NET_PACKET_POOL_SIZE - tail) % NET_PACKET_POOL_SIZE;
  if (in_use)
    *in_use = allocated;
  spinlock_release(&lock);
}
