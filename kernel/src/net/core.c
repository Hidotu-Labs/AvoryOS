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

#define NET_PACKET_HOT_CAPACITY 512
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
static struct net_deferred_work *work_head, *work_tail;
static uint32_t work_depth, work_running;
static spinlock_t lock = SPINLOCK_INIT;
static wait_queue_t worker_wait;
static struct net_device *default_device;
static struct net_device *devices[NET_DEVICE_MAX];
static net_rx_handler_t rx_handler;
static net_rx_tap_t rx_tap;
static void *rx_tap_context;
static uint32_t rx_tap_active;
static bool ready;
static bool devices_started;

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

/* Drop the RX-held lifetime reference after the handler can no longer see the
 * packet. Caller holds the queue lock so unregister's admission barrier and
 * queue state remain serialized. */
static void release_rx_packet_locked(struct rx_buffer *p) {
  struct net_device *dev = p ? p->packet.device : NULL;
  free_locked(p);
  if (!dev)
    return;
  __atomic_sub_fetch(&dev->references, 1, __ATOMIC_RELEASE);
}

static void wait_for_device_references(struct net_device *dev) {
  struct thread *self = sched_get_current();
  while (__atomic_load_n(&dev->references, __ATOMIC_ACQUIRE) != 0) {
    if (self)
      sched_yield();
    else
      __asm__ volatile("pause" ::: "memory");
  }
}

static struct net_device *net_device_find_locked(const char *name) {
  if (!name)
    return NULL;
  for (int i = 0; i < NET_DEVICE_MAX; i++)
    if (devices[i] && strcmp(devices[i]->name, name) == 0)
      return devices[i];
  return NULL;
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

static struct net_deferred_work *pop_work_locked(void) {
  struct net_deferred_work *work = work_head;
  if (!work)
    return NULL;
  work_head = work->next;
  if (!work_head)
    work_tail = NULL;
  work->next = NULL;
  __atomic_store_n(&work->running, true, __ATOMIC_RELEASE);
  work->queued = false;
  work_depth--;
  work_running++;
  return work;
}

static bool packet_current(const struct net_packet *packet) {
  struct net_device *dev = packet->device;
  return !dev || (__atomic_load_n(&dev->registered, __ATOMIC_ACQUIRE) &&
         packet->generation == __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE));
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
    struct net_deferred_work *work = NULL;
    unsigned int rounds = 0;
    for (;;) {
      spinlock_acquire(&lock);
      if (count > 0) {
        for (size_t i = 0; i < count; i++)
          release_rx_packet_locked(batch[i]);
      }
      count = 0;
      while (count < 128 && queue_head)
        batch[count++] = pop_locked();
      /* Do not claim deferred work before invoking receive callbacks. A
       * callback may block while another worker could run the queued poll. */
      work = count ? NULL : pop_work_locked();
      spinlock_release(&lock);

      if (count == 0 && !work)
        break;

      for (size_t i = 0; i < count; i++) {
        if (!packet_current(&batch[i]->packet))
          continue;
        spinlock_acquire(&lock);
        net_rx_tap_t tap = batch[i]->packet.device ? rx_tap : NULL;
        void *tap_context = rx_tap_context;
        if (tap)
          __atomic_add_fetch(&rx_tap_active, 1, __ATOMIC_RELAXED);
        spinlock_release(&lock);
        if (tap) {
          tap(&batch[i]->packet, tap_context);
          __atomic_sub_fetch(&rx_tap_active, 1, __ATOMIC_RELEASE);
        }
        if (rx_handler)
          rx_handler(&batch[i]->packet);
      }
      if (count) {
        spinlock_acquire(&lock);
        work = pop_work_locked();
        spinlock_release(&lock);
      }
      if (work) {
        bool again = work->run(work);
        spinlock_acquire(&lock);
        work_running--;
        bool requeued = !work->cancelled && (again || work->reschedule) &&
                        !work->queued;
        if (requeued) {
          work->reschedule = false;
          work->queued = true;
          work->next = NULL;
          /* Reuse the just-completed slot for this repoll, so the global
           * outstanding-work bound cannot discard a budget continuation. */
          work_depth++;
          if (work_tail) work_tail->next = work;
          else work_head = work;
          work_tail = work;
        }
        work->reschedule = false;
        /* Last access to @work before releasing the queue lock. Cancel waits
         * on this flag and takes the same lock before permitting reclamation. */
        __atomic_store_n(&work->running, false, __ATOMIC_RELEASE);
        spinlock_release(&lock);
        if (requeued)
          wait_queue_wake_one(&worker_wait);
      }
      rounds++;
      if (rounds >= 4 ||
          __atomic_load_n(&self->need_resched, __ATOMIC_ACQUIRE)) {
        if (count > 0) {
          spinlock_acquire(&lock);
          for (size_t i = 0; i < count; i++)
            release_rx_packet_locked(batch[i]);
          spinlock_release(&lock);
          count = 0;
        }
        rounds = 0;
        sched_yield();
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
    bool empty = (queue_head == NULL && work_head == NULL);
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
  struct rx_buffer *accepted[NET_RX_BATCH_MAX];
  size_t accepted_indices[NET_RX_BATCH_MAX];
  size_t accepted_count = 0;
  uint64_t generation;
  uint64_t dropped = 0;
  bool report_pressure = false;

  /* Device removal closes RX admission while holding this same lock. Take
   * one reference per accepted packet before dropping it, so unregister can
   * safely wait through both queued and worker-local batches. */
  spinlock_acquire(&lock);
  if (!dev || (uintptr_t)dev < 0xFFFF800000000000ULL)
    dev = default_device;
  if (!dev || !__atomic_load_n(&dev->registered, __ATOMIC_RELAXED) ||
      dev->unregistering) {
    spinlock_release(&lock);
    return 0;
  }
  generation = __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE);
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
    __atomic_add_fetch(&dev->references, 1, __ATOMIC_RELAXED);
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

  if (!accepted_count) {
    if (dropped && report_pressure)
      klog_puts("[NET] rx dropped: memory pressure\n");
    return 0;
  }

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
  if (!__atomic_load_n(&dev->registered, __ATOMIC_RELAXED) ||
      dev->unregistering ||
      generation != __atomic_load_n(&dev->generation, __ATOMIC_ACQUIRE)) {
    for (size_t i = 0; i < accepted_count; i++) {
      free_locked(accepted[i]);
      __atomic_sub_fetch(&dev->references, 1, __ATOMIC_RELEASE);
    }
    spinlock_release(&lock);
    return 0;
  }
  bool was_empty = (queue_head == NULL);
  for (size_t i = 0; i < accepted_count; i++)
    push_locked(accepted[i]);
  dev->stats.rx_packets += accepted_count;
  dev->stats.rx_bytes += accepted_bytes;
  spinlock_release(&lock);

  if (dropped && report_pressure)
    klog_puts("[NET] rx dropped: memory pressure\n");
  if (accepted_count && was_empty && accepted_count > 32)
    wait_queue_wake_all(&worker_wait);
  else if (accepted_count)
    wait_queue_wake_one(&worker_wait);
  return accepted_count;
}

bool net_rx_submit_irq(struct net_device *dev, const void *frame, size_t len) {
  const struct net_rx_frame packet = {.data = frame, .length = len};
  return net_rx_submit_batch_irq(dev, &packet, 1) == 1;
}

bool net_rx_submit_loopback(const void *data, size_t len) {
  if (!ready || !data || !len || len > NET_FRAME_MAX)
    return false;
  spinlock_acquire(&lock);
  struct rx_buffer *p = alloc_locked();
  spinlock_release(&lock);
  if (!p)
    return false;
  p->packet.device = NULL;
  p->packet.length = (uint16_t)len;
  memcpy(p->packet.data, data, len);
  spinlock_acquire(&lock);
  push_locked(p);
  spinlock_release(&lock);
  wait_queue_wake_one(&worker_wait);
  return true;
}

int net_device_register(struct net_device *dev) {
  if (!dev)
    return -1;
  spinlock_acquire(&lock);
  if (!ready || !dev->ops || !dev->ops->link_up || !dev->ops->stop ||
      __atomic_load_n(&dev->registered, __ATOMIC_RELAXED) ||
      dev->unregistering || dev->references != 0) {
    spinlock_release(&lock);
    return -1;
  }
  /* Empty names request the first free ethN. Explicit user-driver names keep
   * their existing duplicate-name checks. */
  if (!dev->name[0]) {
    for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
      char name[sizeof(dev->name)];
      snprintf(name, sizeof(name), "eth%u", i);
      if (!net_device_find_locked(name)) {
        strcpy(dev->name, name);
        break;
      }
    }
    if (!dev->name[0]) {
      spinlock_release(&lock);
      return -1;
    }
  }
  int free_slot = -1;
  for (int i = 0; i < NET_DEVICE_MAX; i++) {
    if (devices[i] && strcmp(devices[i]->name, dev->name) == 0)
      goto register_fail;
    if (!devices[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0)
    goto register_fail;
  dev->references = 0;
  dev->unregistering = false;
  if (!dev->mtu)
    dev->mtu = NET_MTU_ETHERNET;
  __atomic_store_n(&dev->registered, true, __ATOMIC_RELEASE);
  devices[free_slot] = dev;
  bool start_now = devices_started;
  if (!default_device && !start_now)
    default_device = dev;
  spinlock_release(&lock);
  if (start_now) {
    if (dev->ops->start && !dev->ops->start(dev)) {
      klogf("[NET] %s hotplug activation failed\n", dev->name);
      dev->ops->stop(dev);
      net_device_unregister(dev);
      return -1;
    }
    spinlock_acquire(&lock);
    if (__atomic_load_n(&dev->registered, __ATOMIC_RELAXED) &&
        !dev->unregistering) {
      dev->started = true;
      if (!default_device)
        default_device = dev;
    }
    spinlock_release(&lock);
  }
  return 0;

register_fail:
  spinlock_release(&lock);
  return -1;
}

bool net_devices_start(void) {
  struct net_device *snapshot[NET_DEVICE_MAX];
  spinlock_acquire(&lock);
  default_device = NULL;
  devices_started = true;
  memcpy(snapshot, devices, sizeof(snapshot));
  spinlock_release(&lock);
  for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
    struct net_device *dev = snapshot[i];
    if (!dev)
      continue;
    if (!dev->started && dev->ops->start && !dev->ops->start(dev)) {
      klogf("[NET] %s activation failed\n", dev->name);
      dev->ops->stop(dev);
      net_device_unregister(dev);
      continue;
    }
    spinlock_acquire(&lock);
    dev->started = true;
    if (!default_device)
      default_device = dev;
    spinlock_release(&lock);
  }
  struct net_device *dev = net_device_default_acquire();
  if (!dev)
    return false;
  net_device_put(dev);
  return true;
}

int net_device_unregister(struct net_device *dev) {
  if (!dev)
    return -1;
  spinlock_acquire(&lock);
  if (!__atomic_load_n(&dev->registered, __ATOMIC_RELAXED) ||
      dev->unregistering) {
    spinlock_release(&lock);
    return -1;
  }
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
  dev->unregistering = true;
  __atomic_store_n(&dev->registered, false, __ATOMIC_RELEASE);
  __atomic_add_fetch(&dev->generation, 1, __ATOMIC_RELEASE);
  dev->started = false;
  spinlock_release(&lock);

  /* The producer must be stopped/synchronized before unregister. This grace
   * period covers packets already admitted to the shared queue or worker's
   * private batch; each packet holds the device alive through its handler. */
  wait_for_device_references(dev);

  spinlock_acquire(&lock);
  dev->unregistering = false;
  spinlock_release(&lock);
  return 0;
}

struct net_device *net_device_default(void) {
  spinlock_acquire(&lock);
  struct net_device *dev = default_device;
  spinlock_release(&lock);
  return dev;
}

struct net_device *net_device_default_acquire(void) {
  spinlock_acquire(&lock);
  struct net_device *dev = default_device;
  if (dev && !dev->unregistering &&
      __atomic_load_n(&dev->registered, __ATOMIC_RELAXED))
    __atomic_add_fetch(&dev->references, 1, __ATOMIC_RELAXED);
  else
    dev = NULL;
  spinlock_release(&lock);
  return dev;
}

void net_device_put(struct net_device *dev) {
  if (dev)
    __atomic_sub_fetch(&dev->references, 1, __ATOMIC_RELEASE);
}

bool net_device_is_default(const struct net_device *dev) {
  spinlock_acquire(&lock);
  bool is_default = dev && default_device == dev;
  spinlock_release(&lock);
  return is_default;
}

bool net_device_set_default(const char *name) {
  spinlock_acquire(&lock);
  struct net_device *dev = net_device_find_locked(name);
  if (!dev || !dev->started) {
    spinlock_release(&lock);
    return false;
  }
  default_device = dev;
  spinlock_release(&lock);
  return true;
}

struct net_device *net_device_find(const char *name) {
  spinlock_acquire(&lock);
  struct net_device *dev = net_device_find_locked(name);
  spinlock_release(&lock);
  return dev;
}
void net_set_rx_handler(net_rx_handler_t handler) { rx_handler = handler; }
net_rx_handler_t net_get_rx_handler(void) { return rx_handler; }

void net_set_rx_tap(net_rx_tap_t tap, void *context) {
  spinlock_acquire(&lock);
  rx_tap = NULL;
  rx_tap_context = NULL;
  spinlock_release(&lock);
  while (__atomic_load_n(&rx_tap_active, __ATOMIC_ACQUIRE) != 0)
    sched_yield();
  if (tap) {
    spinlock_acquire(&lock);
    rx_tap_context = context;
    rx_tap = tap;
    spinlock_release(&lock);
  }
}

void net_core_init(void) {
  spinlock_init(&lock);
  wait_queue_init(&worker_wait);
  hot_count = 0;
  active_packets = 0;
  memset(devices, 0, sizeof(devices));
  devices_started = false;
  default_device = NULL;
  queue_head = queue_tail = NULL;
  queue_depth = 0;
  work_head = work_tail = NULL;
  work_depth = work_running = 0;
  refresh_rx_budget();
  ready = true;
  rx_tap = NULL;
  rx_tap_context = NULL;
  rx_tap_active = 0;
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
    sched_set_priority(worker, 0, -2);
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

void net_deferred_work_init(struct net_deferred_work *work,
                            bool (*run)(struct net_deferred_work *)) {
  if (!work)
    return;
  work->next = NULL;
  work->run = run;
  work->queued = false;
  __atomic_store_n(&work->running, false, __ATOMIC_RELAXED);
  work->reschedule = false;
  work->cancelled = false;
}

bool net_deferred_work_schedule(struct net_deferred_work *work) {
  if (!work || !work->run || !ready)
    return false;
  spinlock_acquire(&lock);
  if (work->queued) {
    spinlock_release(&lock);
    return true;
  }
  if (__atomic_load_n(&work->running, __ATOMIC_ACQUIRE)) {
    work->cancelled = false;
    work->reschedule = true;
    spinlock_release(&lock);
    return true;
  }
  if (work_depth + work_running >= 128) {
    spinlock_release(&lock);
    return false;
  }
  work->queued = true;
  work->cancelled = false;
  work->next = NULL;
  if (work_tail)
    work_tail->next = work;
  else
    work_head = work;
  work_tail = work;
  work_depth++;
  spinlock_release(&lock);
  wait_queue_wake_one(&worker_wait);
  return true;
}

void net_deferred_work_cancel(struct net_deferred_work *work) {
  if (!work)
    return;
  spinlock_acquire(&lock);
  if (work->queued) {
    struct net_deferred_work **link = &work_head;
    while (*link && *link != work)
      link = &(*link)->next;
    if (*link == work) {
      *link = work->next;
      if (work_tail == work) {
        work_tail = NULL;
        for (struct net_deferred_work *it = work_head; it; it = it->next)
          work_tail = it;
      }
      work->next = NULL;
      work->queued = false;
      work->reschedule = false;
      work_depth--;
    }
  }
  work->cancelled = true;
  spinlock_release(&lock);
  while (__atomic_load_n(&work->running, __ATOMIC_ACQUIRE))
    sched_yield();
  spinlock_acquire(&lock);
  /* A callback cannot republish after cancellation; remove any item that was
   * queued just before the suppress flag became visible. */
  if (work->queued) {
    struct net_deferred_work **link = &work_head;
    while (*link && *link != work)
      link = &(*link)->next;
    if (*link == work) {
      *link = work->next;
      if (work_tail == work) {
        work_tail = NULL;
        for (struct net_deferred_work *it = work_head; it; it = it->next)
          work_tail = it;
      }
      work->next = NULL;
      work->queued = false;
      work_depth--;
    }
  }
  work->reschedule = false;
  spinlock_release(&lock);
}
