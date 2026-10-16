/* Deferred node reclamation for the lockless dentry/path-cache readers.
 * The design and its correctness argument live in vfs_rcu.h; this file is the
 * engine: a free queue drained by one kthread whose grace periods are IPI
 * barriers, not time-based epochs. */

#include "vfs_rcu.h"
#include "vfs.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../lib/tsc.h"
#include "../lib/radix_tree.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "hal/hal.h"

/* Hold a fresh batch this long before reclaiming it, so a burst of frees pays
 * one IPI round instead of one per node.  A batch at least this large is
 * reclaimed immediately: under heavy churn the window is already full. */
#define VFS_RCU_ACCUMULATE_MS 2
#define VFS_RCU_ACCUMULATE_LIMIT 256

/* The page cache queues a departing radix node by its address, so the node's
 * first two words (struct radix_tree_defer_slot) must be layout-identical to
 * the queue head the drainer will read back. */
_Static_assert(sizeof(struct vfs_rcu_head) == sizeof(struct radix_tree_defer_slot),
               "vfs_rcu_head must match the radix deferred-free slot");
_Static_assert(__builtin_offsetof(struct vfs_rcu_head, next) == 0 &&
                   __builtin_offsetof(struct radix_tree_defer_slot, next) == 0 &&
                   __builtin_offsetof(struct vfs_rcu_head, reclaim) ==
                       __builtin_offsetof(struct radix_tree_defer_slot, reclaim),
               "vfs_rcu_head field offsets must match the radix slot");

/* Objects released by the last reference, oldest first.  The queue links
 * through each item's struct vfs_rcu_head and runs the item's reclaim
 * callback on the drainer thread after a grace period. */
static struct vfs_rcu_head *vfs_rcu_head;
static struct vfs_rcu_head *vfs_rcu_tail;
static uint32_t vfs_rcu_queued;
static spinlock_t vfs_rcu_lock = SPINLOCK_INIT;

/* Barrier state: vfs_rcu_ack[cpu] is 1 while a request is armed for that CPU
 * and 0 once it acknowledged.  Written by the single barrier initiator (the
 * drainer, serialized by vfs_rcu_barrier_lock) and cleared by the target's
 * IPI handler. */
static volatile uint8_t vfs_rcu_ack[MAX_CPUS];
static spinlock_t vfs_rcu_barrier_lock = SPINLOCK_INIT;
static uint8_t vfs_rcu_vector;

static volatile uint64_t vfs_rcu_freed;
static volatile int vfs_rcu_up;

static wait_queue_t vfs_rcu_wait;

bool vfs_rcu_ready(void) {
  return __atomic_load_n(&vfs_rcu_up, __ATOMIC_ACQUIRE) != 0;
}

uint64_t vfs_rcu_freed_count(void) {
  return __atomic_load_n(&vfs_rcu_freed, __ATOMIC_RELAXED);
}

uint64_t vfs_rcu_queued_count(void) {
  return __atomic_load_n(&vfs_rcu_queued, __ATOMIC_RELAXED);
}

/* Runs on the target CPU with interrupts disabled (interrupt gate).  Clearing
 * the flag is the whole acknowledgement; no work is attributed to it, so a
 * delayed IPI from an earlier round may satisfy a newer request - it still ran
 * after that request was armed, which is exactly what the barrier needs. */
static void vfs_rcu_isr(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || cpu->cpu_id >= MAX_CPUS)
    return;
  if (!__atomic_load_n(&vfs_rcu_ack[cpu->cpu_id], __ATOMIC_ACQUIRE))
    return;
  __atomic_store_n(&vfs_rcu_ack[cpu->cpu_id], 0, __ATOMIC_RELEASE);
}

/* Detach the whole queue.  Returns NULL when empty. */
static struct vfs_rcu_head *vfs_rcu_detach(struct vfs_rcu_head **tail_out,
                                           uint32_t *count_out) {
  spinlock_acquire(&vfs_rcu_lock);
  struct vfs_rcu_head *head = vfs_rcu_head;
  struct vfs_rcu_head *tail = vfs_rcu_tail;
  uint32_t count = vfs_rcu_queued;
  vfs_rcu_head = NULL;
  vfs_rcu_tail = NULL;
  vfs_rcu_queued = 0;
  spinlock_release(&vfs_rcu_lock);

  if (tail_out)
    *tail_out = tail;
  if (count_out)
    *count_out = count;
  return head;
}

static bool vfs_rcu_queue_empty(void) {
  spinlock_acquire(&vfs_rcu_lock);
  bool empty = vfs_rcu_head == NULL;
  spinlock_release(&vfs_rcu_lock);
  return empty;
}

/* Wait until every other online CPU has acknowledged the armed request.
 * Called with vfs_rcu_barrier_lock held (interrupts masked): tlb_shootdown_poll
 * is mandatory here, otherwise a concurrent TLB shootdown targeting this CPU
 * would wait for an ack this loop cannot send, and the two subsystems would
 * deadlock against each other. */
static void vfs_rcu_wait_acks(uint64_t pending) {
  if (!pending)
    return;

  uint32_t count = cpu_get_count();
  if (count > MAX_CPUS)
    count = MAX_CPUS;

  uint64_t mhz = tsc_get_mhz();
  uint64_t deadline = mhz ? rdtsc() + mhz * 1000ULL * 5000 : 0;
  bool reported = false;

  while (pending) {
    for (uint32_t i = 0; i < count; i++) {
      struct cpu_info *c = cpu_get_info(i);
      if (!c || !(pending & (1ULL << c->cpu_id)))
        continue;
      if (!__atomic_load_n(&vfs_rcu_ack[c->cpu_id], __ATOMIC_ACQUIRE))
        pending &= ~(1ULL << c->cpu_id);
    }
    if (!pending)
      break;
    if (!reported && deadline && rdtsc() > deadline) {
      /* Diagnostic only: the acks are still required before anything is
       * freed.  A CPU that never answers is either wedged or spinning with
       * interrupts off, and freeing early would reopen the use-after-free. */
      reported = true;
      for (uint32_t i = 0; i < count; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (c && (pending & (1ULL << c->cpu_id))) {
          klog_puts("[VFS-RCU] waiting>5s for deferred-free ack from cpu ");
          klog_hex32(c->cpu_id);
          klog_putchar('\n');
        }
      }
    }
    tlb_shootdown_poll();
    hal_cpu_relax();
  }
}

/* One grace period.  Any node enqueued before this call is unreachable from
 * the caches; the barriers prove that every reader which could still hold a
 * raw pointer to one of them has left its IRQ-off read section. */
static void vfs_rcu_grace_period(void) {
  uint32_t count = cpu_get_count();
  if (count > MAX_CPUS)
    count = MAX_CPUS;
  if (count <= 1 || !lapic_is_ready())
    return; /* a single CPU cannot have a suspended reader section */

  struct cpu_info *self = cpu_get_current();
  uint32_t self_id = self ? self->cpu_id : 0;

  spinlock_acquire(&vfs_rcu_barrier_lock);

  uint64_t pending = 0;
  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *c = cpu_get_info(i);
    if (!c || c->cpu_id == self_id)
      continue;
    if (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP)
      continue;
    __atomic_store_n(&vfs_rcu_ack[c->cpu_id], 1, __ATOMIC_RELEASE);
    pending |= 1ULL << c->cpu_id;
  }
  if (!pending) {
    spinlock_release(&vfs_rcu_barrier_lock);
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *c = cpu_get_info(i);
    if (!c || !(pending & (1ULL << c->cpu_id)))
      continue;
    lapic_send_ipi(c->apic_id, vfs_rcu_vector);
  }

  vfs_rcu_wait_acks(pending);
  spinlock_release(&vfs_rcu_barrier_lock);
}

static void vfs_rcu_reclaim(struct vfs_rcu_head *batch) {
  if (!batch)
    return;
  vfs_rcu_grace_period();

  uint64_t freed = 0;
  while (batch) {
    /* `next` is read before the reclaim: it may kfree the object that holds
     * this very link word. */
    struct vfs_rcu_head *next = batch->next;
    batch->reclaim(batch);
    batch = next;
    freed++;
  }
  __atomic_add_fetch(&vfs_rcu_freed, freed, __ATOMIC_RELAXED);
}

static void vfs_rcu_node_reclaim(struct vfs_rcu_head *head) {
  kfree((void *)((uint8_t *)head - __builtin_offsetof(struct vfs_node, rcu)));
}

void vfs_rcu_defer(struct vfs_rcu_head *head,
                   void (*reclaim)(struct vfs_rcu_head *head)) {
  if (!head || !reclaim)
    return;

  if (!vfs_rcu_ready()) {
    /* No engine: the lockless lookups are gated on vfs_rcu_ready() too, so
     * every reader holds a real reference and this object is already
     * unreachable.  Immediate reclaim is then the pre-RCU behavior, and boot
     * does not pile up objects for a drainer that may never come. */
    reclaim(head);
    return;
  }

  head->next = NULL;
  head->reclaim = reclaim;
  spinlock_acquire(&vfs_rcu_lock);
  if (vfs_rcu_tail)
    vfs_rcu_tail->next = head;
  else
    vfs_rcu_head = head;
  vfs_rcu_tail = head;
  vfs_rcu_queued++;
  spinlock_release(&vfs_rcu_lock);

  wait_queue_wake_one(&vfs_rcu_wait);
}

void vfs_rcu_defer_free(struct vfs_node *node) {
  if (!node)
    return;
  vfs_rcu_defer(&node->rcu, vfs_rcu_node_reclaim);
}

void vfs_rcu_synchronize(void) {
  for (;;) {
    struct vfs_rcu_head *batch = vfs_rcu_detach(NULL, NULL);
    if (!batch)
      return;
    vfs_rcu_reclaim(batch);
  }
}

static void vfs_rcu_worker(void) {
  struct thread *self = sched_get_current();
  for (;;) {
    struct vfs_rcu_head *tail = NULL;
    uint32_t count = 0;
    struct vfs_rcu_head *batch = vfs_rcu_detach(&tail, &count);

    if (!batch) {
      /* Block until a deferral wakes us; the same block-then-link order as
       * the page-cache prefetch worker, so a wake landing between the two
       * cannot be overwritten by a later THREAD_BLOCKED store. */
      if (!self)
        return;
      wait_queue_entry_t entry = { .thread = self, .next = NULL };
      self->state = THREAD_BLOCKED;
      wait_queue_add(&vfs_rcu_wait, &entry);
      if (!vfs_rcu_queue_empty()) {
        self->state = THREAD_RUNNING;
        wait_queue_remove(&vfs_rcu_wait, &entry);
        continue;
      }
      /* Timed fallback: keeps the queue moving even if a wake was lost. */
      self->wakeup_ticks = lapic_timer_get_ticks() + 100;
      sched_yield();
      self->wakeup_ticks = 0;
      self->state = THREAD_RUNNING;
      wait_queue_remove(&vfs_rcu_wait, &entry);
      continue;
    }

    if (count < VFS_RCU_ACCUMULATE_LIMIT) {
      self->state = THREAD_SLEEPING;
      self->wakeup_ticks = lapic_timer_get_ticks() + VFS_RCU_ACCUMULATE_MS;
      sched_yield();
      self->wakeup_ticks = 0;
      self->state = THREAD_RUNNING;

      struct vfs_rcu_head *more_tail = NULL;
      uint32_t more = 0;
      struct vfs_rcu_head *extra = vfs_rcu_detach(&more_tail, &more);
      (void)more;
      if (extra) {
        tail->next = extra;
        tail = more_tail;
      }
    }

    vfs_rcu_reclaim(batch);
  }
}

void vfs_rcu_init(void) {
  if (vfs_rcu_ready() || vfs_rcu_vector)
    return;

  int vector = interrupt_vector_alloc(vfs_rcu_isr);
  if (vector < 0) {
    klog_puts("[VFS] deferred node reclamation off: no free IPI vector\n");
    return;
  }
  vfs_rcu_vector = (uint8_t)vector;
  wait_queue_init(&vfs_rcu_wait);

  if (!sched_create_kernel_thread(vfs_rcu_worker, NULL, true)) {
    interrupt_vector_free(vfs_rcu_vector);
    vfs_rcu_vector = 0;
    klog_puts("[VFS] deferred node reclamation off: no drainer thread\n");
    return;
  }

  __atomic_store_n(&vfs_rcu_up, 1, __ATOMIC_RELEASE);
  klog_puts("[VFS] deferred node reclamation active, IPI ");
  klog_hex32(vfs_rcu_vector);
  klog_putchar('\n');
}
