#include "vblank.h"
#include "../apic/lapic_timer.h"
#include "../lock/spinlock.h"
#include "../sched/sched.h"
#include "../sched/wait.h"

#define VBLANK_DEFAULT_HZ 60
#define VBLANK_MIN_HZ 24
#define VBLANK_MAX_HZ 240

static spinlock_t vblank_lock = SPINLOCK_INIT;
static wait_queue_t vblank_wq;
static bool vblank_ready = false;
static uint64_t vblank_period = 1000000000ULL / VBLANK_DEFAULT_HZ;
static uint64_t vblank_epoch_ns = 0;

static void vblank_ensure_locked(void) {
  if (vblank_ready)
    return;
  wait_queue_init(&vblank_wq);
  vblank_epoch_ns = lapic_timer_get_ns();
  vblank_ready = true;
}

void vblank_set_refresh_hz(uint32_t hz) {
  if (hz < VBLANK_MIN_HZ || hz > VBLANK_MAX_HZ)
    return;
  spinlock_acquire(&vblank_lock);
  vblank_ensure_locked();
  uint64_t now = lapic_timer_get_ns();
  uint64_t new_period = 1000000000ULL / hz;
  /* Rebase the epoch so the sequence number stays continuous across the
   * rate change instead of jumping. */
  uint32_t cur =
      (uint32_t)((now - vblank_epoch_ns) / vblank_period);
  vblank_period = new_period;
  vblank_epoch_ns = now - (uint64_t)cur * new_period;
  spinlock_release(&vblank_lock);
}

uint64_t vblank_period_ns(void) {
  spinlock_acquire(&vblank_lock);
  vblank_ensure_locked();
  uint64_t period = vblank_period;
  spinlock_release(&vblank_lock);
  return period;
}

uint32_t vblank_sequence(void) {
  spinlock_acquire(&vblank_lock);
  vblank_ensure_locked();
  uint64_t period = vblank_period;
  uint64_t epoch = vblank_epoch_ns;
  spinlock_release(&vblank_lock);
  uint64_t now = lapic_timer_get_ns();
  if (now < epoch)
    return 0;
  return (uint32_t)((now - epoch) / period);
}

void vblank_timestamp(uint32_t seq, uint32_t *sec, uint32_t *usec) {
  spinlock_acquire(&vblank_lock);
  vblank_ensure_locked();
  uint64_t t = vblank_epoch_ns + (uint64_t)seq * vblank_period;
  spinlock_release(&vblank_lock);
  if (sec)
    *sec = (uint32_t)(t / 1000000000ULL);
  if (usec)
    *usec = (uint32_t)((t % 1000000000ULL) / 1000ULL);
}

int64_t vblank_wait_next(void) {
  spinlock_acquire(&vblank_lock);
  vblank_ensure_locked();
  uint64_t period = vblank_period;
  uint64_t epoch = vblank_epoch_ns;
  spinlock_release(&vblank_lock);

  uint64_t now = lapic_timer_get_ns();
  uint64_t elapsed = (now >= epoch) ? now - epoch : 0;
  uint64_t deadline = epoch + (elapsed / period + 1) * period;

  struct thread *t = sched_get_current();
  if (!t) {
    /* No thread context (early boot): spin-sleep; bounded by one period. */
    while (lapic_timer_get_ns() < deadline)
      lapic_timer_sleep(1);
    return 0;
  }

  /* Sleep on a private queue entry with the scheduler deadline set to the
   * boundary, exactly like the poll() timeout path: no timer IRQ exists to
   * wake us, wakeup_ticks does it. Max sleep is one period (~16ms). */
  /* Scheduler deadlines are milliseconds. Round up so a wake cannot precede
   * the boundary (and force repeated sleeps on an already expired deadline). */
  uint64_t deadline_ms = deadline / 1000000ULL +
                         (deadline % 1000000ULL != 0);
  wait_queue_entry_t entry;
  entry.thread = t;
  entry.wq = NULL;
  entry.next = NULL;
  entry.thread_next = NULL;
  entry.thread_prev = NULL;
  int64_t rc = 0;
  for (;;) {
    /* Publish BLOCKED, the deadline and the queue entry without a local
     * timer preempting halfway through. Otherwise the scheduler can remove
     * this thread before it has any deadline or registered wake source. */
    hal_irq_state_t irq_flags = hal_irq_save();
    if (thread_has_pending_signal(t)) {
      rc = -4; /* EINTR */
      hal_irq_restore(irq_flags);
      break;
    }
    if (lapic_timer_get_ns() >= deadline) {
      hal_irq_restore(irq_flags);
      break;
    }
    t->wakeup_ticks = deadline_ms;
    t->state = THREAD_BLOCKED;
    wait_queue_add(&vblank_wq, &entry);
    /* A remote signal may have arrived before the entry was registered.
     * Recheck it as well as the clock before allowing the thread to park. */
    if (!thread_has_pending_signal(t) && lapic_timer_get_ns() < deadline) {
      hal_irq_restore(irq_flags);
      sched_yield();
      irq_flags = hal_irq_save();
    }
    /* Finish cleanup before preemption can observe a blocked thread whose
     * entry has been removed. Re-register on every sleep: wakers detach it. */
    t->state = THREAD_RUNNING;
    t->wakeup_ticks = 0;
    wait_queue_remove(&vblank_wq, &entry);
    hal_irq_restore(irq_flags);
  }
  return rc;
}

int64_t vblank_wait_for(uint32_t target) {
  for (;;) {
    uint32_t cur = vblank_sequence();
    if ((int32_t)(target - cur) <= 0)
      return 0;
    int64_t rc = vblank_wait_next();
    if (rc != 0)
      return rc;
  }
}
