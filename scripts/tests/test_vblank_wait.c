/* Exercise the actual software vblank engine with deterministic preemption. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
typedef bool hal_irq_state_t;
typedef struct { int unused; } spinlock_t;
#define SPINLOCK_INIT {0}
struct thread { int state; uint64_t wakeup_ticks; };
typedef struct wait_queue_entry {
  struct thread *thread;
  void *wq, *next, *thread_next, *thread_prev;
} wait_queue_entry_t;
typedef struct { wait_queue_entry_t *entry; } wait_queue_t;
static struct thread task;
static uint64_t now_ns;
static bool irq_enabled = true, signal_pending, no_thread;
static unsigned scenario, adds, removes, yields;

static void timer_checkpoint(void) {
  /* A timer here would dequeue this task with no way to wake it. */
  assert(!(irq_enabled && task.state == THREAD_BLOCKED &&
           !task.wakeup_ticks) && "vblank waiter stranded before deadline");
}
static hal_irq_state_t hal_irq_save(void) {
  bool previous = irq_enabled;
  irq_enabled = false;
  return previous;
}
static void hal_irq_restore(hal_irq_state_t previous) {
  irq_enabled = previous;
  timer_checkpoint();
}
static void spinlock_acquire(spinlock_t *lock) { (void)lock; }
static void spinlock_release(spinlock_t *lock) { (void)lock; }
static uint64_t lapic_timer_get_ns(void) { return now_ns; }
static void lapic_timer_sleep(unsigned ms) { now_ns += ms * 1000000ULL; }
static struct thread *sched_get_current(void) { return no_thread ? NULL : &task; }
static bool thread_has_pending_signal(struct thread *t) {
  assert(t == &task);
  return signal_pending;
}
static void wait_queue_init(wait_queue_t *q) { q->entry = NULL; }
static void wait_queue_add(wait_queue_t *q, wait_queue_entry_t *entry) {
  timer_checkpoint();
  assert(!irq_enabled && task.wakeup_ticks && task.state == THREAD_BLOCKED);
  assert(!q->entry && !entry->wq);
  q->entry = entry;
  entry->wq = q;
  adds++;
  if (scenario == 2) signal_pending = true; /* signal during publication */
}
static void wait_queue_remove(wait_queue_t *q, wait_queue_entry_t *entry) {
  assert(!irq_enabled && task.state == THREAD_RUNNING && !task.wakeup_ticks);
  assert(!q->entry || q->entry == entry);
  q->entry = NULL;
  entry->wq = NULL;
  removes++;
}
static void sched_yield(void);

/* The shell runner strips only platform includes from the real source. */
#include VBLANK_SOURCE

static void sched_yield(void) {
  timer_checkpoint();
  assert(irq_enabled && vblank_wq.entry && task.wakeup_ticks);
  assert(task.wakeup_ticks * 1000000ULL >=
         vblank_epoch_ns + ((now_ns - vblank_epoch_ns) / vblank_period + 1) *
                               vblank_period);
  yields++;
  if (scenario == 1 && yields == 1) {
    /* An early wake detaches the entry. A second sleep must register again. */
    vblank_wq.entry->wq = NULL;
    vblank_wq.entry = NULL;
    now_ns += 1000000;
  } else if (scenario == 3) {
    signal_pending = true;
  } else {
    now_ns = task.wakeup_ticks * 1000000ULL;
  }
  task.state = THREAD_RUNNING;
  task.wakeup_ticks = 0;
}

int main(void) {
  /* Keep the same harness compilable against the original unguarded wait. */
  (void)hal_irq_save;
  (void)hal_irq_restore;
  for (scenario = 0; scenario < 5; scenario++) {
    task = (struct thread){.state = THREAD_RUNNING};
    now_ns = 123456789;
    signal_pending = scenario == 4;
    adds = removes = yields = 0;
    vblank_ready = false;
    int64_t result = vblank_wait_next();
    assert(result == (scenario >= 2 ? -4 : 0));
    assert(irq_enabled && task.state == THREAD_RUNNING && !task.wakeup_ticks);
    assert(!vblank_wq.entry && adds == removes);
    assert(adds == (scenario == 1 ? 2U : scenario == 4 ? 0U : 1U));
    assert(yields == (scenario == 1 ? 2U : scenario == 2 || scenario == 4 ? 0U : 1U));
  }
  no_thread = true;
  signal_pending = false;
  assert(!vblank_wait_next());
  puts("PASS: vblank deadline publication, early wake re-registration, signal races, deadline rounding and boot wait");
}
