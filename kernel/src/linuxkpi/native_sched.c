/* Native scheduler/time bridge for the LinuxKPI sleeping layer.  Compiled with
 * native headers only; see linuxkpi/native_sched.h for the contract. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "lock/lockdiag.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"

struct linuxkpi_kthread_boot {
  void (*fn)(void *);
  void *arg;
};

/* LinuxKPI time layer (linuxkpi/src/time.c) when the Linux tree is imported;
 * absent in a kernel built without it. */
extern void linuxkpi_jiffies_sync(void) __attribute__((weak));

#ifndef LINUXKPI_LINUX_IMPORTED
/* Native mprotect also pins its deferred TLB queue across the unlock/drain
 * window. Keep that protection available without the Linux import. */
static unsigned int native_preempt_counts[MAX_CPUS];

static unsigned int *native_preempt_counter(void) {
  struct cpu_info *cpu = cpu_get_current();
  unsigned int id = cpu ? cpu->cpu_id : 0;
  return &native_preempt_counts[id < MAX_CPUS ? id : 0];
}

void __kpi_preempt_disable(void) {
  hal_irq_state_t flags = hal_irq_save();
  __atomic_add_fetch(native_preempt_counter(), 1, __ATOMIC_ACQ_REL);
  hal_irq_restore(flags);
}

void __kpi_preempt_enable(void) {
  hal_irq_state_t flags = hal_irq_save();
  unsigned int *count = native_preempt_counter();
  if (__atomic_load_n(count, __ATOMIC_RELAXED))
    __atomic_sub_fetch(count, 1, __ATOMIC_ACQ_REL);
  hal_irq_restore(flags);
}

bool linuxkpi_preempt_allowed(void) {
  hal_irq_state_t flags = hal_irq_save();
  bool allowed = !__atomic_load_n(native_preempt_counter(), __ATOMIC_RELAXED);
  hal_irq_restore(flags);
  return allowed;
}
#endif

void linuxkpi_report_stalled_gpu_wait(void) {
  static unsigned int reported;
  if (!__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL))
    lockdiag_dump_all("Nouveau synchronous VM_BIND fence stalled for 5 seconds");
}

void *linuxkpi_current_thread(void) { return sched_get_current(); }

void linuxkpi_yield(void) { sched_yield(); }

void linuxkpi_wake_poll_queue(void *native_wq) {
  wait_queue_wake_all((wait_queue_t *)native_wq);
}

/* sched_get_current() is only valid once the per-CPU GS base is installed;
 * the IRQ hooks run from the first timer interrupt, which can precede
 * cpu_init(). */
static struct thread *kpi_current_thread_or_null(void) {
  struct cpu_info *cpu = cpu_get_current();
  return cpu ? cpu->current_thread : NULL;
}

bool linuxkpi_need_resched(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? __atomic_load_n(&t->need_resched, __ATOMIC_ACQUIRE) : false;
}

/* ── hardirq context tracking (in_interrupt() backing) ──────────────────── */

/* The native ISR calls these around hardware-IRQ dispatch (src/cpu/isr.c).
 * Depth is tracked on the interrupted thread, so a different thread scheduled
 * in from inside the handler sees depth 0; when the handler's context resumes
 * on its original thread, the matching exit runs there too. */
void linuxkpi_irq_enter(void) {
  struct thread *t = kpi_current_thread_or_null();
  if (t)
    __atomic_add_fetch(&t->kpi_irq_depth, 1, __ATOMIC_ACQ_REL);
}

void linuxkpi_irq_exit(void) {
  struct thread *t = kpi_current_thread_or_null();
  if (t && __atomic_load_n(&t->kpi_irq_depth, __ATOMIC_RELAXED))
    __atomic_sub_fetch(&t->kpi_irq_depth, 1, __ATOMIC_ACQ_REL);
}

int linuxkpi_irq_depth(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? (int)__atomic_load_n(&t->kpi_irq_depth, __ATOMIC_RELAXED) : 0;
}

int linuxkpi_softirq_depth(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? (int)__atomic_load_n(&t->kpi_softirq_depth, __ATOMIC_RELAXED) : 0;
}

static void linuxkpi_kthread_trampoline(void) {
  struct thread *t = sched_get_current();
  struct linuxkpi_kthread_boot *boot =
      t ? (struct linuxkpi_kthread_boot *)t->kpi_data : NULL;

  if (!boot)
    return;

  /* Hand the thread its long-lived argument (the LinuxKPI kthread control
   * block) before invoking the entry point, then free the start record. */
  void (*fn)(void *) = boot->fn;
  void *arg = boot->arg;

  if (t)
    t->kpi_data = arg;
  kfree(boot);

  fn(arg);
}

void *linuxkpi_kthread_create(void (*fn)(void *), void *arg, const char *name) {
  struct linuxkpi_kthread_boot *boot = kmalloc(sizeof(*boot));
  if (!boot)
    return NULL;

  boot->fn = fn;
  boot->arg = arg;

  /* Create suspended so kpi_data/comm are set before it can run. */
  struct thread *t =
      sched_create_kernel_thread(linuxkpi_kthread_trampoline, NULL, false);
  if (!t) {
    kfree(boot);
    return NULL;
  }

  t->kpi_data = boot;
  if (name) {
    strncpy(t->comm, name, sizeof(t->comm) - 1);
    t->comm[sizeof(t->comm) - 1] = '\0';
  }

  sched_enqueue_thread(t, NULL);
  return t;
}

void linuxkpi_kthread_cancel_create(void *thread) {
  struct thread *t = thread;
  if (!t)
    return;
  /* The caller has never woken this thread: kpi_data is still the boot
   * record, rather than the argument installed by the trampoline. */
  kfree(t->kpi_data);
  t->kpi_data = NULL;
  sched_terminate_thread(t->tid);
}

void linuxkpi_set_current_state(int state) {
  struct thread *t = sched_get_current();
  if (!t)
    return;
  /* Upstream performs this before checking the wait condition. Clear an old
   * notification first, then publish the new wait; a wake before publication
   * is observed by that condition check, while a later wake arms the flag. */
  __atomic_store_n(&t->kpi_block_pending, false, __ATOMIC_RELEASE);
  if (state) {
    __atomic_store_n(&t->kpi_wake_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&t->kpi_wait_prepared, true, __ATOMIC_SEQ_CST);
  } else {
    __atomic_store_n(&t->kpi_wait_prepared, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->kpi_wake_pending, false, __ATOMIC_RELEASE);
  }
}

void linuxkpi_thread_block(void) {
  struct thread *t = sched_get_current();
  if (!t)
    return;

  /* TASK_RUNNING schedule() is a yield, not a blocking wait. Mutex callers
   * publish their wait under wait_lock before reaching this bridge. */
  if (!__atomic_load_n(&t->kpi_wait_prepared, __ATOMIC_SEQ_CST)) {
    sched_yield();
    return;
  }

  /* Same rendezvous as the timeout sleep: publish the blocking decision
   * under the run-queue lock and consume a wake that raced the caller's
   * condition check.  Without this, a waker that runs while this thread is
   * still THREAD_RUNNING is ignored by sched_wakeup() (it only wakes
   * BLOCKED/SLEEPING threads) and the wait would never be satisfied. */
  hal_irq_state_t irq_flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (cpu)
    spinlock_acquire(&cpu->queue_lock);
  if (__atomic_exchange_n(&t->kpi_block_pending, false, __ATOMIC_ACQ_REL)) {
    if (cpu)
      spinlock_release(&cpu->queue_lock);
    hal_irq_restore(irq_flags);
    /* A wake already arrived: the caller re-checks its condition. */
    linuxkpi_set_current_state(0);
    return;
  }
  __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
  if (cpu)
    spinlock_release(&cpu->queue_lock);
  hal_irq_restore(irq_flags);

  sched_yield();

  linuxkpi_set_current_state(0);

  /* If no wake re-marked us runnable (e.g. the yield resumed us directly),
   * do not stay marked blocked while running; sched_wakeup() would then try
   * to enqueue a running thread for a later wake. */
  uint8_t st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
  if (st == THREAD_BLOCKED || st == THREAD_SLEEPING)
    __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);
}

void linuxkpi_mutex_wait_set(void *mutex) {
  struct thread *t = sched_get_current();
  if (t)
    __atomic_store_n(&t->kpi_wait_mutex, mutex, __ATOMIC_RELEASE);
}

int linuxkpi_schedule_timeout_ms(unsigned long ms) {
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  if (ms == 0) {
    sched_yield();
    return 0;
  }

  uint64_t deadline = lapic_timer_get_ticks() + (uint64_t)ms;
  bool woken = false;

  t->wakeup_ticks = deadline;
  __atomic_store_n(&t->kpi_timeout_active, true, __ATOMIC_RELEASE);

  /* Sleep until the deadline, not just for one scheduler pass.
   *
   * The native scheduler can hand the CPU back before the deadline when the
   * caller is a per-CPU idle task: sched_schedule() treats cpu->idle_thread as
   * the always-available fallback and resumes it regardless of its state or
   * wakeup_ticks.  kmain_high_half runs in exactly that context before the
   * first real kernel thread is scheduled, so a single yield would return
   * immediately and every msleep()/schedule_timeout() would be a no-op.
   *
   * A genuine wakeup (linuxkpi_wake_thread() -> sched_wakeup()) clears
   * wakeup_ticks, which also cancels the sleep here; otherwise the loop
   * re-blocks until the clock reaches the deadline.
   *
   * The kpi_wake_pending flag catches the other interleaving: the waker may
   * run while this thread is still THREAD_RUNNING (it has committed to sleep
   * but not yielded yet), in which case sched_wakeup() ignores it.  The flag
   * is checked and the sleep state is published under the same run-queue lock
   * sched_wakeup() takes, so either the waker sees THREAD_SLEEPING or this
   * loop sees the pending wake. */
  for (;;) {
    hal_irq_state_t irq_flags = hal_irq_save();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu)
      spinlock_acquire(&cpu->queue_lock);
    if (__atomic_exchange_n(&t->kpi_wake_pending, false, __ATOMIC_ACQ_REL)) {
      if (cpu)
        spinlock_release(&cpu->queue_lock);
      hal_irq_restore(irq_flags);
      woken = true;
      break;
    }
    __atomic_store_n(&t->state, THREAD_SLEEPING, __ATOMIC_RELEASE);
    if (cpu)
      spinlock_release(&cpu->queue_lock);
    hal_irq_restore(irq_flags);

    sched_yield();

    if (!t->wakeup_ticks)
      break;
    if (lapic_timer_get_ticks() >= deadline)
      break;
  }

  bool timed_out = !woken && lapic_timer_get_ticks() >= deadline;
  t->wakeup_ticks = 0;
  __atomic_store_n(&t->kpi_wake_pending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&t->kpi_timeout_active, false, __ATOMIC_RELEASE);
  linuxkpi_set_current_state(0);
  /* Timed-out sleeping calls can leave the state as SLEEPING when the idle
   * fallback never switched away; the caller is definitely running now. */
  uint8_t st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
  if (st == THREAD_SLEEPING || st == THREAD_BLOCKED)
    __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);

  /* jiffies advances with the native tick, which may not have fired between
   * the last sync and this wakeup (the LAPIC is one-shot and armed at the
   * next scheduler deadline).  Bring it current so msleep()/schedule_timeout()
   * callers observe the interval they slept. */
  if (linuxkpi_jiffies_sync)
    linuxkpi_jiffies_sync();

  return timed_out ? 0 : 1;
}

void linuxkpi_wake_thread(void *thread) {
  struct thread *t = (struct thread *)thread;
  if (!t)
    return;

  /* Only a published wait owns a notification. Kthread-start and unrelated
   * wakes must not leak into a later mutex wait or schedule_timeout(). */
  if (__atomic_load_n(&t->kpi_timeout_active, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&t->kpi_wait_prepared, __ATOMIC_SEQ_CST)) {
    __atomic_store_n(&t->kpi_block_pending, true, __ATOMIC_RELEASE);
    __atomic_store_n(&t->kpi_wake_pending, true, __ATOMIC_RELEASE);
  }

  /* sched_wakeup() ignores idle threads: they are the per-CPU fallback task
   * and are never runqueue members.  KPI code can still sleep in the boot
   * thread's idle context before the first real kernel thread is scheduled
   * (e.g. the bounded wait in linuxkpi_run_boot_tests()), so cancel the
   * timeout here instead.  The scheduler resumes the idle fallback as soon as
   * nothing else is runnable, and the sleep loop then sees wakeup_ticks==0
   * and returns. */
  if (t->is_idle) {
    t->wakeup_ticks = 0;
    if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED)
      __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);
    return;
  }

  sched_wakeup(t);
}

bool linuxkpi_thread_has_pending_signal(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? thread_has_pending_signal(t) : false;
}

unsigned long linuxkpi_current_tgid(void) {
  struct thread *t = sched_get_current();
  return t ? (unsigned long)t->tgid : 0;
}

unsigned long linuxkpi_thread_tgid(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? (unsigned long)t->tgid : 0;
}

unsigned long linuxkpi_thread_pid(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? (unsigned long)t->tid : 0;
}

void linuxkpi_thread_comm(void *thread, char *buf, unsigned long size) {
  struct thread *t = (struct thread *)thread;

  if (!buf || size == 0)
    return;
  if (!t) {
    buf[0] = '\0';
    return;
  }
  __builtin_strncpy(buf, t->comm, size - 1);
  buf[size - 1] = '\0';
}

/* The shadow allocator is implemented on the Linux side (task.c), which knows
 * the struct task_struct layout; weak so this file stays native-only. */
extern void *linuxkpi_task_shadow_new(void *thread) __attribute__((weak));
extern void linuxkpi_task_shadow_release(void *shadow) __attribute__((weak));

static void *task_for_thread_locked(struct thread *t) {
  if (!t)
    return 0;
  if (!t->kpi_task && linuxkpi_task_shadow_new)
    t->kpi_task = linuxkpi_task_shadow_new(t);
  return t->kpi_task;
}

void *linuxkpi_current_task(void) {
  return task_for_thread_locked(sched_get_current());
}

void *linuxkpi_task_for_thread(void *thread) {
  return task_for_thread_locked((struct thread *)thread);
}

void *linuxkpi_task_thread(void *task) {
  /* The shadow stores the native thread pointer in its first field
   * (kernel/linuxkpi/src/task.c); keep this in sync with struct task_struct. */
  return task ? *(void **)task : 0;
}

void linuxkpi_thread_exiting(void *thread) {
  struct thread *t = (struct thread *)thread;

  if (!t || !t->kpi_task)
    return;

  void *shadow = t->kpi_task;
  t->kpi_task = NULL;
  if (linuxkpi_task_shadow_release)
    linuxkpi_task_shadow_release(shadow);
  else
    *(void **)shadow = NULL;

}

unsigned long long linuxkpi_monotonic_ms(void) { return lapic_timer_get_ms(); }

unsigned long long linuxkpi_monotonic_ns(void) { return lapic_timer_get_ns(); }

int linuxkpi_cpu_count(void) { return (int)cpu_get_count(); }

void linuxkpi_udelay_ns(unsigned long long ns) {
  unsigned long long start = lapic_timer_get_ns();
  while (lapic_timer_get_ns() - start < ns)
    hal_cpu_relax();
}

void *linuxkpi_thread_data(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? t->kpi_data : NULL;
}

void *linuxkpi_thread_self_data(void) {
  struct thread *t = sched_get_current();
  return t ? t->kpi_data : NULL;
}

/* Symbol expected by imported <asm/current.h>/<asm/processor.h>; uniprocessor
 * emulation, `current` itself is served by linuxkpi_current_thread(). */
struct linuxkpi_pcpu_hot {
  void *current_task;
  unsigned int preempt_count;
  unsigned int cpu_number;
  unsigned long top_of_stack;
  void *hardirq_stack_ptr;
  unsigned short softirq_pending;
  _Bool hardirq_stack_inuse;
  unsigned char pad[64 - 8 - 4 - 4 - 8 - 8 - 2 - 1];
} __attribute__((aligned(64)));

struct linuxkpi_pcpu_hot pcpu_hot;

void linuxkpi_vma_set_pending(void *linux_vma) {
  struct thread *t = sched_get_current();
  if (t)
    t->kpi_pending_vma = linux_vma;
}

void *linuxkpi_vma_take_pending(void) {
  struct thread *t = sched_get_current();
  void *v;

  if (!t)
    return NULL;
  v = t->kpi_pending_vma;
  t->kpi_pending_vma = NULL;
  return v;
}
