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

#define NET_PACKET_HOT_CAPACITY 64
#define NET_QUEUE_CAPACITY 1024
#define NET_PACKET_MAX 4096

static struct net_packet *hot_pool[NET_PACKET_HOT_CAPACITY];
static uint32_t hot_count = 0;
static uint32_t active_packets = 0;
static struct net_packet *queue[NET_QUEUE_CAPACITY];
static uint32_t head, tail;
static spinlock_t lock = SPINLOCK_INIT;
static wait_queue_t worker_wait;
static struct net_device *default_device;
static struct net_device *devices[NET_DEVICE_MAX];
static net_rx_handler_t rx_handler;
static bool ready;

static struct net_packet *alloc_locked(void) {
  if (hot_count > 0) {
    struct net_packet *p = hot_pool[--hot_count];
    p->device = NULL;
    p->length = 0;
    return p;
  }
  if (active_packets >= NET_PACKET_MAX)
    return NULL;
  struct net_packet *p = kmalloc(sizeof(struct net_packet));
  if (p) {
    active_packets++;
    p->device = NULL;
    p->length = 0;
  }
  return p;
}

static void free_locked(struct net_packet *p) {
  if (!p)
    return;
  if (hot_count < NET_PACKET_HOT_CAPACITY) {
    hot_pool[hot_count++] = p;
  } else {
    if (active_packets > 0)
      active_packets--;
    kfree(p);
  }
}

static bool push_locked(struct net_packet *p) {
  uint32_t next = (head + 1) % NET_QUEUE_CAPACITY;
  if (next == tail)
    return false;
  queue[head] = p;
  head = next;
  return true;
}

static bool packet_current(const struct net_packet *packet) {
  struct net_device *dev = packet->device;
  return dev && __atomic_load_n(&dev->registered, __ATOMIC_ACQUIRE) &&
         packet->generation == __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE);
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
        tail = (tail + 1) % NET_QUEUE_CAPACITY;
      }
      spinlock_release(&lock);

      if (count == 0)
        break;

      for (size_t i = 0; i < count; i++) {
        if (rx_handler && packet_current(batch[i]))
          rx_handler(batch[i]);
      }
    }

    self->state = THREAD_BLOCKED;
    wait_queue_add(&worker_wait, entry);
    spinlock_acquire(&lock);
    bool empty = (head == tail);
    if (!empty) {
      self->state = THREAD_RUNNING;
    } else {
      self->wakeup_ticks = lapic_timer_get_ticks() + 50;
    }
    spinlock_release(&lock);
    if (empty) {
      sched_yield();
      self->wakeup_ticks = 0;
    } else {
      self->wakeup_ticks = 0;
      wait_queue_remove(&worker_wait, entry);
      continue;
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
  p->generation = __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE);
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
      dev->registered)
    return -1;
  /* Empty names request the first free ethN. Explicit user-driver names keep
   * their existing duplicate-name checks. */
  if (!dev->name[0]) {
    for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
      char name[sizeof(dev->name)];
      snprintf(name, sizeof(name), "eth%u", i);
      if (!net_device_find(name)) {
        strcpy(dev->name, name);
        break;
      }
    }
    if (!dev->name[0])
      return -1;
  }
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

bool net_devices_start(void) {
  default_device = NULL;
  for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
    struct net_device *dev = devices[i];
    if (!dev)
      continue;
    if (!dev->started && dev->ops->start && !dev->ops->start(dev)) {
      klogf("[NET] %s activation failed\n", dev->name);
      dev->ops->stop(dev);
      net_device_unregister(dev);
      continue;
    }
    dev->started = true;
    if (!default_device)
      default_device = dev;
  }
  return default_device != NULL;
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
  __atomic_add_fetch(&dev->generation, 1, __ATOMIC_RELEASE);
  dev->started = false;
  return 0;
}

struct net_device *net_device_default(void) { return default_device; }

bool net_device_set_default(const char *name) {
  struct net_device *dev = net_device_find(name);
  if (!dev || !dev->started)
    return false;
  default_device = dev;
  return true;
}

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
  hot_count = 0;
  active_packets = 0;
  memset(devices, 0, sizeof(devices));
  default_device = NULL;
  head = tail = 0;
  ready = true;
  klog_puts("[NET] core prepared\n");
}

void net_core_start_worker(void) {
  struct cpu_info *online_cpus[4];
  uint32_t online_count = 0;
  uint32_t cpu_count = cpu_get_count();
  for (uint32_t i = 0; i < cpu_count && online_count < 4; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (cpu && cpu->status != CPU_STATUS_OFFLINE)
      online_cpus[online_count++] = cpu;
  }

  uint32_t workers = online_count > 1 ? 2 : 1;
  if (!online_count) {
    klog_puts("[NET] no online CPU for RX workers\n");
    ready = false;
    return;
  }

  for (uint32_t i = 0; i < workers; i++) {
    struct cpu_info *target = online_cpus[i];
    /* Prepare the worker before publishing it to a remote CPU's runqueue. */
    struct thread *worker =
        sched_create_kernel_thread(net_worker, target, false);
    if (!worker) {
      ready = false;
      klog_puts("[NET] worker creation failed\n");
      return;
    }
    strcpy(worker->comm, "net-worker");
    sched_set_priority(worker, 0, -20);
    sched_enqueue_thread(worker, target);
    klog_puts("[NET] rx worker assigned cpu=");
    klog_uint64(target->cpu_id);
    klog_puts("\n");
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
  klog_puts(" txtimeout=");
  klog_uint64(dev->stats.tx_timeouts);
  klog_puts(" recovered=");
  klog_uint64(dev->stats.recoveries);
  klog_puts(" recoveryfail=");
  klog_uint64(dev->stats.recovery_failures);
  klog_puts("\n");
}

void net_queue_snapshot(uint32_t *queued, uint32_t *in_use) {
  spinlock_acquire(&lock);
  if (queued)
    *queued = (head + NET_QUEUE_CAPACITY - tail) % NET_QUEUE_CAPACITY;
  if (in_use)
    *in_use = active_packets;
  spinlock_release(&lock);
}
