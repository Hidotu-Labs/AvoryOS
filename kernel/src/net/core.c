#include "net/core.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"

#define NET_PACKET_HOT_CAPACITY 64
#define NET_RX_BATCH_MAX 64
/* RX buffers come directly from PMM: unlike kmalloc's OOM fallback, this
 * never invokes filesystem/cache reclaim from a NIC interrupt. One page owns
 * both the packet and its intrusive queue link. */
struct rx_buffer {
  struct rx_buffer *next;
  struct net_packet packet;
};
_Static_assert(sizeof(struct rx_buffer) <= PAGE_SIZE, "RX buffer fits a page");

/* Growth may use 1/8 of free + RX-owned pages. This is a memory-pressure
 * policy, refreshed by the timer worker, rather than a packet-count ceiling. */
#define NET_RX_MEMORY_SHARE 8

static struct rx_buffer *hot_pool[NET_PACKET_HOT_CAPACITY];
static uint32_t hot_count = 0;
static uint32_t active_packets = 0;
static uint64_t rx_page_limit;
static struct rx_buffer *queue_head, *queue_tail;
static uint32_t queue_depth;
static spinlock_t lock = SPINLOCK_INIT;
static wait_queue_t worker_wait;
static struct net_device *default_device;
static struct net_device *devices[NET_DEVICE_MAX];
static net_rx_handler_t rx_handler;
static bool ready;

/* Refresh the pressure budget in thread context and outside the RX lock.
 * IRQ admission only reads the resulting cached limit. */
static void refresh_rx_budget(void) {
  uint64_t available = pmm_get_free_pages();
  spinlock_acquire(&lock);
  rx_page_limit = (available + active_packets) / NET_RX_MEMORY_SHARE;
  spinlock_release(&lock);
}

static struct rx_buffer *alloc_locked(void) {
  struct rx_buffer *p;
  if (hot_count > 0) {
    p = hot_pool[--hot_count];
  } else {
    if ((uint64_t)active_packets + 1 > rx_page_limit)
      return NULL;
    void *phys = pmm_alloc_page();
    if (!phys)
      return NULL;
    p = (void *)((uintptr_t)phys + pmm_get_hhdm_offset());
    active_packets++;
  }
  p->next = NULL;
  p->packet.device = NULL;
  p->packet.length = 0;
  return p;
}

static void free_locked(struct rx_buffer *p) {
  if (!p)
    return;
  if (hot_count < NET_PACKET_HOT_CAPACITY) {
    hot_pool[hot_count++] = p;
  } else {
    active_packets--;
    pmm_free_page((void *)((uintptr_t)p - pmm_get_hhdm_offset()));
  }
}

static void push_locked(struct rx_buffer *p) {
  p->next = NULL;
  if (queue_tail)
    queue_tail->next = p;
  else
    queue_head = p;
  queue_tail = p;
  queue_depth++;
}

static struct rx_buffer *pop_locked(void) {
  struct rx_buffer *p = queue_head;
  if (!p)
    return NULL;
  queue_head = p->next;
  if (!queue_head)
    queue_tail = NULL;
  p->next = NULL;
  queue_depth--;
  return p;
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
  struct rx_buffer *batch[128];
  for (;;) {
    size_t count = 0;
    for (;;) {
      spinlock_acquire(&lock);
      if (count > 0) {
        for (size_t i = 0; i < count; i++)
          free_locked(batch[i]);
      }
      count = 0;
      while (count < 128 && queue_head)
        batch[count++] = pop_locked();
      spinlock_release(&lock);

      if (count == 0)
        break;

      for (size_t i = 0; i < count; i++) {
        if (rx_handler && packet_current(&batch[i]->packet))
          rx_handler(&batch[i]->packet);
      }
    }

    /* Publish the blocked state and queue entry with local ticks masked. The
     * queue recheck closes the enqueue-before-park race, so idle workers do
     * not need periodic timeout wakeups. */
    hal_irq_state_t flags = hal_irq_save();
    self->wakeup_ticks = 0;
    self->state = THREAD_BLOCKED;
    wait_queue_add(&worker_wait, entry);
    spinlock_acquire(&lock);
    bool empty = (queue_head == NULL);
    if (!empty) {
      self->state = THREAD_RUNNING;
    }
    spinlock_release(&lock);
    hal_irq_restore(flags);
    if (empty)
      sched_yield();

    flags = hal_irq_save();
    self->state = THREAD_RUNNING;
    self->wakeup_ticks = 0;
    wait_queue_remove(&worker_wait, entry);
    hal_irq_restore(flags);
  }
}

/* Dedicated TCP timer so RX processing never has to drive retransmission
 * timers on the packet hot path. */
static void net_timer_worker(void) {
  struct thread *self = sched_get_current();
  uint64_t last_budget = lapic_timer_get_ticks();
  for (;;) {
    uint64_t now = lapic_timer_get_ticks();
    if (now - last_budget >= 1000) {
      refresh_rx_budget();
      last_budget = now;
    }
    tcp_timer_tick(now);
    /* Publish the timer deadline before a tick can observe SLEEPING. */
    hal_irq_state_t flags = hal_irq_save();
    self->wakeup_ticks = lapic_timer_get_ticks() + 10;
    self->state = THREAD_SLEEPING;
    hal_irq_restore(flags);
    sched_yield();
    self->wakeup_ticks = 0;
  }
}

size_t net_rx_submit_batch_irq(struct net_device *dev,
                               const struct net_rx_frame *frames,
                               size_t count) {
  if (!ready || !frames || !count || count > NET_RX_BATCH_MAX) {
    if (dev && (uintptr_t)dev >= 0xFFFF800000000000ULL)
      __atomic_add_fetch(&dev->stats.rx_errors, count, __ATOMIC_RELAXED);
    return 0;
  }
  if (!dev || (uintptr_t)dev < 0xFFFF800000000000ULL)
    dev = default_device;
  if (!dev)
    return 0;

  struct rx_buffer *accepted[NET_RX_BATCH_MAX];
  size_t accepted_indices[NET_RX_BATCH_MAX];
  size_t accepted_count = 0;
  uint64_t generation = __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE);
  uint64_t dropped = 0;
  bool report_pressure = false;
  spinlock_acquire(&lock);
  for (size_t i = 0; i < count; i++) {
    if (!frames[i].data || !frames[i].length ||
        frames[i].length > NET_FRAME_MAX) {
      dev->stats.rx_errors++;
      continue;
    }
    struct rx_buffer *p = alloc_locked();
    if (!p) {
      dev->stats.rx_dropped++;
      dev->stats.queue_full++;
      dropped++;
      continue;
    }
    accepted[accepted_count] = p;
    accepted_indices[accepted_count++] = i;
  }
  if (dropped) {
    /* Serialize rate limiting with queue state; IRQ producers run on SMP. */
    static uint64_t last_pressure_log;
    uint64_t now = lapic_timer_get_ticks();
    report_pressure = now - last_pressure_log >= 1000;
    if (report_pressure)
      last_pressure_log = now;
  }
  spinlock_release(&lock);

  /* Buffers are reserved but not linked yet, so frame copies do not hold the
   * shared queue lock. NIC callers keep their ring lock until this returns. */
  uint64_t accepted_bytes = 0;
  for (size_t i = 0; i < accepted_count; i++) {
    const struct net_rx_frame *frame = &frames[accepted_indices[i]];
    struct rx_buffer *p = accepted[i];
    p->packet.device = dev;
    p->packet.generation = generation;
    p->packet.length = (uint16_t)frame->length;
    memcpy(p->packet.data, frame->data, frame->length);
    accepted_bytes += frame->length;
  }

  spinlock_acquire(&lock);
  bool was_empty = (queue_head == NULL);
  for (size_t i = 0; i < accepted_count; i++)
    push_locked(accepted[i]);
  dev->stats.rx_packets += accepted_count;
  dev->stats.rx_bytes += accepted_bytes;
  spinlock_release(&lock);

  if (dropped && report_pressure)
    klog_puts("[NET] rx dropped: memory pressure\n");
  if (accepted_count && was_empty)
    wait_queue_wake_all(&worker_wait);
  else if (accepted_count)
    wait_queue_wake_one(&worker_wait);
  return accepted_count;
}

bool net_rx_submit_irq(struct net_device *dev, const void *frame, size_t len) {
  const struct net_rx_frame packet = {.data = frame, .length = len};
  return net_rx_submit_batch_irq(dev, &packet, 1) == 1;
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
  queue_head = queue_tail = NULL;
  queue_depth = 0;
  refresh_rx_budget();
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
    *queued = queue_depth;
  if (in_use)
    *in_use = active_packets;
  spinlock_release(&lock);
}
