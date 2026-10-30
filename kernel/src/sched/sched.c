#include "sched.h"
#include "sched_internal.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../cpu/gdt.h"
#include "../cpu/fpu.h"
#include "../cpu/idt.h"
#include "../cpu/msr.h"
#include "../fs/procfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/numa.h"
#include "../smp/cpu.h"

extern void switch_context(struct thread *old_t, struct thread *new_t);
extern void tss_set_rsp0(uint64_t rsp0);
extern void signal_send(struct thread *, int);
extern void signal_notify_thread(struct thread *t, int sig);

static void ipi_reschedule_handler(struct registers *regs) {
  (void)regs;
  /* An IPI is a request, not a scheduler entry.  Switching here would
   * suspend the handler before the common ISR path has completed EOI and
   * LinuxKPI irq-exit bookkeeping.  The common ISR epilogue services this
   * after those steps, including when returning to preemptible kernel code. */
  struct cpu_info *cpu = cpu_get_current();
  struct thread *running = cpu ? cpu->current_thread : NULL;
  if (running)
    __atomic_store_n(&running->need_resched, true, __ATOMIC_RELEASE);
}

/* Called with cpu->queue_lock held from both scheduler entries and timer
 * ticks. This keeps /proc/stat's per-CPU lines tied to the actual CPU that
 * ran the interval instead of repeating an evenly split system average. */
static void sched_account_cpu_time_locked(struct cpu_info *cpu, uint64_t now_ms)
{
  if (!cpu || !cpu->stats_last_ms) {
    if (cpu)
      cpu->stats_last_ms = now_ms ? now_ms : 1;
    return;
  }
  uint64_t elapsed = now_ms - cpu->stats_last_ms;
  cpu->stats_last_ms = now_ms;
  if (!elapsed)
    return;
  struct thread *current = cpu->current_thread;
  if (current && current->is_idle) {
    __atomic_fetch_add(&cpu->stats_idle_ms, elapsed, __ATOMIC_RELAXED);
  } else if (current) {
    bool system = current->cr3 == 0 ||
                  __atomic_load_n(&current->in_syscall, __ATOMIC_RELAXED);
    if (system) {
      __atomic_fetch_add(&cpu->stats_system_ms, elapsed, __ATOMIC_RELAXED);
      __atomic_fetch_add(&current->runtime_system_ms, elapsed,
                         __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&cpu->stats_user_ms, elapsed, __ATOMIC_RELAXED);
      __atomic_fetch_add(&current->runtime_user_ms, elapsed,
                         __ATOMIC_RELAXED);
    }
  }
}

/* Timed waiters live in a per-CPU red-black tree ordered by wakeup_ticks, so
 * parking a poll/nanosleep and waking a sleeper are O(log n) under queue_lock
 * instead of walking a sorted list of every sleeper on the CPU. The cached
 * leftmost node is the earliest deadline, which keeps timer arming and expiry
 * O(1) for the common no-op case. All helpers require cpu->queue_lock. */
void sched_deadline_remove_locked(struct cpu_info *cpu,
                                  struct thread *t) {
  if (!t->deadline_queued)
    return;

  if (cpu->deadline_leftmost == &t->deadline_node)
    cpu->deadline_leftmost = rb_next(&t->deadline_node);
  rb_erase(&t->deadline_node, &cpu->deadline_tree);
  t->deadline_node.__rb_parent_color = 0;
  t->deadline_node.rb_left = NULL;
  t->deadline_node.rb_right = NULL;
  t->deadline_queued = false;
}

void sched_deadline_insert_locked(struct cpu_info *cpu,
                                  struct thread *t) {
  if (!t->wakeup_ticks)
    return;
  if (t->deadline_queued)
    sched_deadline_remove_locked(cpu, t);

  struct rb_node **link = &cpu->deadline_tree.rb_node;
  struct rb_node *parent = NULL;
  bool leftmost = true;
  while (*link) {
    parent = *link;
    struct thread *owner = rb_entry(parent, struct thread, deadline_node);
    if (t->wakeup_ticks < owner->wakeup_ticks) {
      link = &parent->rb_left;
    } else {
      /* Equal deadlines go right; membership is tracked per thread, not by
       * key lookup, so duplicates need no tie-breaker. */
      link = &parent->rb_right;
      leftmost = false;
    }
  }

  rb_link_node(&t->deadline_node, parent, link);
  rb_insert_color(&t->deadline_node, &cpu->deadline_tree);
  if (leftmost || !cpu->deadline_leftmost)
    cpu->deadline_leftmost = &t->deadline_node;
  t->deadline_queued = true;
}

void sched_deadline_expire_locked(struct cpu_info *cpu, uint64_t now) {
  while (cpu->deadline_leftmost &&
         rb_entry(cpu->deadline_leftmost, struct thread,
                  deadline_node)->wakeup_ticks <= now) {
    struct thread *t =
        rb_entry(cpu->deadline_leftmost, struct thread, deadline_node);
    sched_deadline_remove_locked(cpu, t);

    if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
      bool still_current = cpu->current_thread == t;
      t->state = still_current ? THREAD_RUNNING : THREAD_READY;
      t->wakeup_ticks = 0;
      t->ready_since_ms = still_current ? 0 : now;
      if (!still_current) {
        /* The runqueue that receives the thread is the one whose deadline
         * queue it was just popped from, but a concurrent sched_wakeup() may
         * have re-targeted it to another CPU.  Pin cpu_index to the runqueue
         * it actually lands in, otherwise a later remove_from_runqueue() /
         * sched_set_priority() operates on the wrong CPU's tree.
         * Also never insert an entity a racing wakeup already linked: a
         * second rb_insert turns the runqueue tree into a cycle. */
        if (!t->se.on_rq) {
          t->cpu_index = cpu->cpu_id;
          eevfd_place_entity(&cpu->eevfd, &t->se, false);
          eevfd_enqueue_entity(&cpu->eevfd, &t->se);
          t->on_runqueue = true;
          __atomic_add_fetch(&cpu->runnable_count, 1, __ATOMIC_RELEASE);
        }
      }
    } else {
      t->wakeup_ticks = 0;
    }
  }
}

void sched_arm_next_deadline(struct cpu_info *cpu,
                             struct thread *next_t) {
  uint64_t now = lapic_timer_get_ms();
  uint64_t deadline = 0;
  if (next_t && !next_t->is_idle) {
    /* Ceiling division: round up to avoid firing late */
    uint64_t slice_ms = (next_t->se.slice_ns + 999999ULL) / 1000000ULL;
    if (slice_ms < 1) slice_ms = 1;
    cpu->quantum_deadline_ms = now + slice_ms;
    deadline = cpu->quantum_deadline_ms;
  } else {
    cpu->quantum_deadline_ms = 0;
  }
  if (cpu->deadline_leftmost) {
    uint64_t earliest =
        rb_entry(cpu->deadline_leftmost, struct thread, deadline_node)
            ->wakeup_ticks;
    if (!deadline || earliest < deadline)
      deadline = earliest;
  }
  if (deadline) lapic_timer_rearm_if_earlier(deadline);
}

static uint32_t sched_runnable_snapshot(struct cpu_info *cpu) {
  return cpu ? __atomic_load_n(&cpu->runnable_count, __ATOMIC_ACQUIRE)
             : UINT32_MAX;
}

static bool sched_cpu_accepting_work(struct cpu_info *cpu) {
  if (!cpu)
    return false;
  uint8_t status = __atomic_load_n(&cpu->status, __ATOMIC_ACQUIRE);
  return status == CPU_STATUS_BSP || status == CPU_STATUS_ONLINE;
}

static uint32_t sched_cpu_load(struct cpu_info *cpu) {
  if (!cpu)
    return UINT32_MAX;
  uint32_t load = sched_runnable_snapshot(cpu);
  struct thread *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
  if (curr && !curr->is_idle && load == 0) {
    load = 1;
  }
  return load;
}

static struct cpu_info *sched_least_loaded_cpu(uint64_t affinity,
                                                int preferred_node,
                                                uint32_t *load_out) {
  static volatile uint32_t sched_rr_seed = 0;
  uint32_t count = cpu_get_count();
  if (count == 0) {
    if (load_out)
      *load_out = UINT32_MAX;
    return NULL;
  }

  // 1. Try CPUs in preferred NUMA node first if valid and completely idle
  if (preferred_node >= 0 && preferred_node < MAX_NUMA_NODES) {
    struct numa_node *pnode = numa_get_node(preferred_node);
    if (pnode && pnode->online && pnode->cpu_mask) {
      uint64_t node_affinity = affinity & pnode->cpu_mask;
      if (node_affinity) {
        for (uint32_t i = 0; i < count; i++) {
          if (!(node_affinity & (1ULL << i)))
            continue;
          struct cpu_info *cpu = cpu_get_info(i);
          if (!sched_cpu_accepting_work(cpu))
            continue;
          uint32_t load = sched_cpu_load(cpu);
          if (load == 0) {
            if (load_out)
              *load_out = 0;
            return cpu;
          }
        }
      }
    }
  }

  // 2. Global search
  struct cpu_info *best = NULL;
  uint32_t best_load = UINT32_MAX;
  uint32_t start =
      __atomic_fetch_add(&sched_rr_seed, 1, __ATOMIC_RELAXED) % count;

  for (uint32_t step = 0; step < count; step++) {
    uint32_t i = (start + step) % count;
    struct cpu_info *cpu = cpu_get_info(i);
    if (!sched_cpu_accepting_work(cpu) || !(affinity & (1ULL << i)))
      continue;
    uint32_t load = sched_cpu_load(cpu);
    if (load < best_load) {
      best_load = load;
      best = cpu;
      if (best_load == 0) {
        /* Completely idle CPU found; distribute immediately */
        break;
      }
    }
  }
  if (load_out)
    *load_out = best_load;
  return best;
}

void sched_init(void) {
  kstack_cache_init();

  // We expect this to be called after cpu_init() which populates the CPU list
  uint32_t count = cpu_get_count();

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu)
      continue;

    spinlock_init(&cpu->queue_lock);
    eevfd_rq_init(&cpu->eevfd);
    cpu->deadline_tree.rb_node = NULL;
    cpu->deadline_leftmost = NULL;
    cpu->stats_user_ms = 0;
    cpu->stats_system_ms = 0;
    cpu->stats_idle_ms = 0;
    cpu->stats_last_ms = lapic_timer_get_ms();

    // Register reschedule IPI handler once on BSP
    if (i == 0) {
      register_interrupt_handler(IPI_VECTOR_RESCHEDULE, ipi_reschedule_handler);
    }

    // Create the idle thread for this specific CPU
    struct thread *idle_thread = kmalloc(sizeof(struct thread));
    memset(idle_thread, 0, sizeof(struct thread));
    mempolicy_init(&idle_thread->numa_policy);
    idle_thread->cwd_path[0] = '/';
    idle_thread->cwd_node = fs_root;
    if (fs_root)
      vfs_open(fs_root);

    // Assign a dedicated TID in a high range so PID 1 is reserved for userspace init
    idle_thread->tid = 0x7FFFFFF0 + i;
    idle_thread->tgid = idle_thread->tid;
    idle_thread->ss_flags = SS_DISABLE;
    idle_thread->is_idle = true;
    idle_thread->pgid = idle_thread->tid;
    idle_thread->state = THREAD_RUNNING;

    // Idle thread initialized with nice 19 (lowest priority)
    eevfd_entity_init(&idle_thread->se, 19, EEVFD_MAX_SLICE_NS);
    idle_thread->priority = SCHED_PRIORITY_IDLE;
    idle_thread->static_priority = SCHED_PRIORITY_IDLE;
    idle_thread->nice_value = 19;
    idle_thread->time_slice = 100;
    idle_thread->runtime_total = 0;
    idle_thread->runtime_burst = 0;
    strcpy(idle_thread->comm, "idle");

    idle_thread->mm = kmalloc(sizeof(struct mm_struct));
    if (idle_thread->mm) {
      memset(idle_thread->mm, 0, sizeof(struct mm_struct));
      vma_list_init(&idle_thread->mm->vmas);
      idle_thread->mm->ref_count = 1;
      spinlock_init(&idle_thread->mm->lock);
    }

    idle_thread->stack_size = CPU_STACK_SIZE;
    idle_thread->stack_base = cpu->stack_top - CPU_STACK_SIZE;
    idle_thread->parent = NULL;
    fpu_init_thread(idle_thread);

    spinlock_acquire(&tid_lock);
    idle_thread->global_next = global_thread_list;
    idle_thread->global_prev = NULL;
    if (global_thread_list) {
      global_thread_list->global_prev = idle_thread;
    }
    global_thread_list = idle_thread;
    __atomic_add_fetch(&global_thread_count, 1, __ATOMIC_RELAXED);
    cpu->idle_thread = idle_thread;
    __atomic_store_n(&cpu->current_thread, idle_thread, __ATOMIC_RELEASE);
    __atomic_store_n(&idle_thread->on_cpu, true, __ATOMIC_RELEASE);
    idle_thread->cpu_affinity = (1ULL << count) - 1;
    if (count == 64)
      idle_thread->cpu_affinity = ~0ULL;
    spinlock_release(&tid_lock);
  }
  fpu_init_cpu();
}

/* Lock two runqueues in deterministic cpu_id order to prevent AB-BA deadlocks
 * across concurrent wakeup migrations, work-stealing, and affinity changes. */
static inline void sched_lock_two_rq(struct cpu_info *a, struct cpu_info *b) {
  if (a == b) {
    if (a)
      spinlock_acquire(&a->queue_lock);
    return;
  }
  if (!a) {
    spinlock_acquire(&b->queue_lock);
    return;
  }
  if (!b) {
    spinlock_acquire(&a->queue_lock);
    return;
  }
  if (a->cpu_id < b->cpu_id) {
    spinlock_acquire(&a->queue_lock);
    spinlock_acquire(&b->queue_lock);
  } else {
    spinlock_acquire(&b->queue_lock);
    spinlock_acquire(&a->queue_lock);
  }
}

static inline void sched_unlock_two_rq(struct cpu_info *a, struct cpu_info *b) {
  if (a == b) {
    if (a)
      spinlock_release(&a->queue_lock);
    return;
  }
  if (a)
    spinlock_release(&a->queue_lock);
  if (b)
    spinlock_release(&b->queue_lock);
}

/* Acquire the queue_lock of whichever CPU currently owns `t`, retrying if a
 * concurrent wakeup or work-steal migrated `t->cpu_index` while we waited. */
static struct cpu_info *sched_lock_task_rq(struct thread *t,
                                           hal_irq_state_t *flags_out) {
  for (;;) {
    uint32_t idx = __atomic_load_n(&t->cpu_index, __ATOMIC_ACQUIRE);
    struct cpu_info *cpu = cpu_get_info(idx);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      cpu = cpu_get_bsp();
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      return NULL;
    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);
    if (__atomic_load_n(&t->cpu_index, __ATOMIC_RELAXED) == cpu->cpu_id) {
      *flags_out = flags;
      return cpu;
    }
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(flags);
  }
}

/* Pull one runnable, off-CPU task from the busiest runqueue onto `dst_cpu`.
 * Caller holds NO queue_locks and has interrupts disabled. */
static bool sched_steal_one_task(struct cpu_info *dst_cpu) {
  if (!sched_cpu_accepting_work(dst_cpu))
    return false;

  uint32_t count = cpu_get_count();
  if (count <= 1)
    return false;

  struct cpu_info *busiest = NULL;
  uint32_t max_load = 1; /* Need at least 2 runnable tasks on donor to steal */

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *c = cpu_get_info(i);
    if (!c || c == dst_cpu || !sched_cpu_accepting_work(c))
      continue;
    uint32_t load = sched_runnable_snapshot(c);
    if (load > max_load) {
      max_load = load;
      busiest = c;
    }
  }

  if (!busiest)
    return false;

  sched_lock_two_rq(dst_cpu, busiest);

  bool stolen = false;
  if (busiest->eevfd.nr_running >= 1 &&
      __atomic_load_n(&busiest->runnable_count, __ATOMIC_RELAXED) >= 2) {
    for (struct rb_node *n = rb_first(&busiest->eevfd.tasks_tree); n;
         n = rb_next(n)) {
      struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
      struct thread *cand = rb_entry(se, struct thread, se);

      if (cand->is_idle || cand == busiest->current_thread)
        continue;
      if (__atomic_load_n(&cand->on_cpu, __ATOMIC_ACQUIRE))
        continue;
      if (__atomic_load_n(&cand->state, __ATOMIC_ACQUIRE) != THREAD_READY)
        continue;
      if (cand->deadline_queued || cand->se.critical_depth > 0)
        continue;
      uint64_t aff = __atomic_load_n(&cand->cpu_affinity, __ATOMIC_RELAXED);
      if (aff && !(aff & (1ULL << dst_cpu->cpu_id)))
        continue;

      /* Migrate `cand` from `busiest` to `dst_cpu` */
      eevfd_dequeue_entity(&busiest->eevfd, &cand->se);
      cand->on_runqueue = false;
      if (__atomic_load_n(&busiest->runnable_count, __ATOMIC_RELAXED))
        __atomic_sub_fetch(&busiest->runnable_count, 1, __ATOMIC_RELEASE);

      __atomic_store_n(&cand->cpu_index, dst_cpu->cpu_id, __ATOMIC_RELEASE);
      eevfd_migrate_entity(&busiest->eevfd, &dst_cpu->eevfd, &cand->se);
      eevfd_enqueue_entity(&dst_cpu->eevfd, &cand->se);
      cand->on_runqueue = true;
      __atomic_add_fetch(&dst_cpu->runnable_count, 1, __ATOMIC_RELEASE);
      stolen = true;
      break;
    }
  }

  sched_unlock_two_rq(dst_cpu, busiest);
  return stolen;
}

/* Called after releasing runqueue locks, with local IRQs still masked.
 * A local task-context wake has no IRQ epilogue of its own. Queue a self IPI
 * so it reaches the same guarded scheduling boundary as a remote wake, rather
 * than waiting for the one-millisecond timer backstop. */
static void sched_notify_runnable(struct cpu_info *target) {
  if (!lapic_is_ready())
    return;
  struct cpu_info *self = cpu_get_current();
  if (target == self) {
    struct thread *running = self->current_thread;
    if (running)
      __atomic_store_n(&running->need_resched, true, __ATOMIC_RELEASE);
    lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    /* The current hardirq already calls sched_check_resched after EOI and
     * irq_exit. Avoid an extra interrupt for every fence in an MSI batch. */
    if (running && running->kpi_irq_depth)
      return;
  }
  lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);
}

__attribute__((optimize("O3"))) void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu) {
  if (!t || t->is_idle || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
    return;

  uint64_t affinity = __atomic_load_n(&t->cpu_affinity, __ATOMIC_RELAXED);
  if (!affinity)
    affinity = ~0ULL;

  struct cpu_info *target_cpu = NULL;
  if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
    target_cpu = cpu_get_info(__atomic_load_n(&t->cpu_index, __ATOMIC_ACQUIRE));
  } else if (explicit_cpu && sched_cpu_accepting_work(explicit_cpu) &&
             (affinity & (1ULL << explicit_cpu->cpu_id))) {
    target_cpu = explicit_cpu;
  } else {
    int pref_node = -1;
    if (t->numa_policy.mode != MPOL_DEFAULT) {
      pref_node = mempolicy_select_node(&t->numa_policy, 0);
    }
    target_cpu = sched_least_loaded_cpu(affinity, pref_node, NULL);
  }

  if (!sched_cpu_accepting_work(target_cpu)) {
    target_cpu = cpu_get_bsp();
  }

  hal_irq_state_t irq_flags = hal_irq_save();
  spinlock_acquire(&target_cpu->queue_lock);

  if (t->on_runqueue || t->se.on_rq) {
    spinlock_release(&target_cpu->queue_lock);
    hal_irq_restore(irq_flags);
    return;
  }

  t->ready_since_ms = lapic_timer_get_ms();
  eevfd_place_entity(&target_cpu->eevfd, &t->se, (t->se.vruntime == 0));
  eevfd_enqueue_entity(&target_cpu->eevfd, &t->se);
  t->on_runqueue = true;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    __atomic_add_fetch(&target_cpu->runnable_count, 1, __ATOMIC_RELEASE);

  __atomic_store_n(&t->cpu_index, target_cpu->cpu_id, __ATOMIC_RELEASE);

  struct thread *running = target_cpu->current_thread;
  bool kick_cpu = !running || running == target_cpu->idle_thread || running->is_idle ||
                  eevfd_check_preempt(&target_cpu->eevfd, &running->se, &t->se);
  spinlock_release(&target_cpu->queue_lock);

  if (kick_cpu)
    sched_notify_runnable(target_cpu);

  /* Restore the caller's interrupt state: some callers (wait-queue wakes from
   * IRQ context) enter with IF=0, and unconditionally enabling here left an
   * interrupt window inside their critical section. */
  hal_irq_restore(irq_flags);
}

__attribute__((optimize("O3"))) static void sched_schedule(bool voluntary_yield) {
  /* Preserve the caller's IF state across a schedule. Timer/IPI handlers
   * enter with IF clear and must keep it clear until their ISR epilogue has
   * finished; a syscall or voluntary yield may have entered with IF set. Each
   * suspended scheduler continuation keeps its own value on its thread stack. */
  hal_irq_state_t irq_flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    hal_irq_restore(irq_flags);
    return;
  }

  struct thread *prev = cpu->current_thread;

  // Reap detached threads from a different scheduler context.
  sched_process_reap_queue(prev);

  spinlock_acquire(&cpu->queue_lock);

  uint64_t now = lapic_timer_get_ms();
  uint64_t now_ns = lapic_timer_get_ns();
  sched_account_cpu_time_locked(cpu, now);

  if ((prev->state == THREAD_SLEEPING || prev->state == THREAD_BLOCKED) &&
      prev->wakeup_ticks) {
    if (prev->wakeup_ticks <= now) {
      prev->state = THREAD_RUNNING;
      prev->wakeup_ticks = 0;
    } else {
      sched_deadline_insert_locked(cpu, prev);
    }
  }
  sched_deadline_expire_locked(cpu, now);

  /* Fast-path: single runnable task on this core. Zero tree overhead.
   * Only applicable on non-voluntary preemption/ticks. */
  if (!voluntary_yield && prev->state == THREAD_RUNNING && !prev->is_idle && cpu->eevfd.nr_running == 0) {
    eevfd_update_curr(&cpu->eevfd, now_ns);
    prev->runtime_total = prev->se.prev_sum_exec_ns / 1000000ULL;
    sched_arm_next_deadline(cpu, prev);
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(irq_flags);
    return;
  }

  if (!prev->is_idle) {
    eevfd_update_curr(&cpu->eevfd, now_ns);
    prev->runtime_total = prev->se.prev_sum_exec_ns / 1000000ULL;
    eevfd_put_prev_entity(&cpu->eevfd, &prev->se);
  }

  if (prev->state == THREAD_ZOMBIE || prev->state == THREAD_DEAD) {
    sched_deadline_remove_locked(cpu, prev);
    if (prev->se.on_rq) {
      eevfd_dequeue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = false;
    }
    if (__atomic_load_n(&cpu->runnable_count, __ATOMIC_RELAXED))
      __atomic_sub_fetch(&cpu->runnable_count, 1, __ATOMIC_RELEASE);
  } else if (prev->state == THREAD_BLOCKED || prev->state == THREAD_SLEEPING) {
    if (prev->se.on_rq) {
      eevfd_dequeue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = false;
    }
    if (__atomic_load_n(&cpu->runnable_count, __ATOMIC_RELAXED))
      __atomic_sub_fetch(&cpu->runnable_count, 1, __ATOMIC_RELEASE);
  } else {
    /* prev is still running or ready */
    if (!prev->is_idle && !prev->se.on_rq) {
      if (voluntary_yield) {
        /* Linux yield_task_fair: advance vruntime so other waiting tasks run next */
        if (cpu->eevfd.rb_leftmost) {
          struct sched_entity *left = rb_entry(cpu->eevfd.rb_leftmost, struct sched_entity, rb_node);
          if (prev->se.vruntime < left->vruntime + prev->se.slice_ns)
            prev->se.vruntime = left->vruntime + prev->se.slice_ns;
        }
        prev->se.deadline = calc_deadline(prev->se.vruntime, prev->se.slice_ns, prev->se.weight, prev->se.wmult);
        prev->se.min_deadline = prev->se.deadline;
      } else {
        /* Involuntary preemption/tick: only recalculate deadline if expired */
        if (prev->se.deadline <= prev->se.vruntime) {
          prev->se.deadline = calc_deadline(prev->se.vruntime, prev->se.slice_ns, prev->se.weight, prev->se.wmult);
          prev->se.min_deadline = prev->se.deadline;
        }
      }
      __atomic_store_n(&prev->cpu_index, cpu->cpu_id, __ATOMIC_RELEASE);
      eevfd_enqueue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = true;
    }
  }

  /* Work-stealing: if this core has no runnable tasks, try pulling one off-CPU
   * task from a busy core before falling back to the idle thread.
   * CRITICAL: Never drop cpu->queue_lock here if `prev` is blocking/sleeping!
   * A concurrent sched_wakeup(prev) checks prev's state under cpu->queue_lock;
   * dropping queue_lock while prev->state is THREAD_BLOCKED/SLEEPING but before
   * cpu->current_thread is switched away would let sched_wakeup() see
   * still_current == true and set prev->state = THREAD_RUNNING without
   * enqueuing prev on the runqueue, causing a lost wakeup when sched_schedule
   * subsequently switches to idle_thread! */
  if (cpu->eevfd.nr_running == 0 &&
      (prev->is_idle || prev->state == THREAD_RUNNING ||
       prev->state == THREAD_READY)) {
    spinlock_release(&cpu->queue_lock);
    sched_steal_one_task(cpu);
    spinlock_acquire(&cpu->queue_lock);
    now = lapic_timer_get_ms();
    now_ns = lapic_timer_get_ns();
    sched_deadline_expire_locked(cpu, now);
  }

  // 2. Select earliest eligible virtual deadline task. A remote kill can
  // publish a terminal state while the task is still linked on this runqueue.
  // Never dispatch such an entity: unlink it under the same lock that
  // serializes dispatch with sched_prepare_thread_reap().
  struct sched_entity *next_se;
  struct thread *next_t = NULL;
  while ((next_se = eevfd_pick_next_entity(&cpu->eevfd)) != NULL) {
    struct thread *candidate = rb_entry(next_se, struct thread, se);
    uint8_t state = __atomic_load_n(&candidate->state, __ATOMIC_ACQUIRE);
    if (state != THREAD_DEAD && state != THREAD_ZOMBIE) {
      next_t = candidate;
      break;
    }

    sched_deadline_remove_locked(cpu, candidate);
    eevfd_dequeue_entity(&cpu->eevfd, &candidate->se);
    candidate->on_runqueue = false;
    if (__atomic_load_n(&cpu->runnable_count, __ATOMIC_RELAXED))
      __atomic_sub_fetch(&cpu->runnable_count, 1, __ATOMIC_RELEASE);
  }

  // 3. Perform the switch
  if (!next_t) {
    next_t = cpu->idle_thread;
  }

  if (next_t && !next_t->is_idle) {
    next_t->ready_since_ms = 0;
    eevfd_set_next_entity(&cpu->eevfd, &next_t->se);
    next_t->se.exec_start_ns = now_ns;
    next_t->on_runqueue = false;
  }

  sched_arm_next_deadline(cpu, next_t);

  if (next_t && next_t != prev) {
    if (!prev->is_idle && prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
      prev->ready_since_ms = now;
    }
    next_t->state = THREAD_RUNNING;
    __atomic_store_n(&next_t->has_started, true, __ATOMIC_RELEASE);
    /* Mark next_t on_cpu before releasing queue_lock.  prev->on_cpu and
     * cpu->switching_from are cleared inside switch_context (switch.asm) ONLY
     * after prev->rsp is saved and the CPU has switched to next_t->rsp, so
     * another CPU can never steal or dispatch prev while its stack is still
     * in use here. */
    __atomic_store_n(&next_t->on_cpu, true, __ATOMIC_RELEASE);
    __atomic_store_n(&cpu->switching_from, prev, __ATOMIC_RELEASE);
    fpu_on_context_switch();
    __atomic_store_n(&cpu->current_thread, next_t, __ATOMIC_RELEASE);

    cpu->stack_top = (next_t->stack_base + next_t->stack_size) & ~0xFULL;
    tss_set_rsp0(cpu->stack_top);

    /* Lazy CR3 + PCID-aware context switch:
     *
     * Case 1: next_t is a kernel/idle thread (cr3 == 0).
     *   Keep whatever CR3 is loaded.  There is no user TLB to flush and
     *   reloading kernel_cr3 would pointlessly evict every cached user
     *   translation, causing a full TLB miss burst on the next user switch.
     *
     * Case 2: next_t is a user process with the *same* CR3 as prev.
     *   Nothing to do — already using the right page tables.
     *
     * Case 3: Different user CR3.
     *   If PCID is available and this CPU already has the PCID entry warm,
     *   use CR3_NOFLUSH (bit 63) to avoid evicting all translations for
     *   this address space from the TLB.  Otherwise flush and mark cached.
     */
    if (next_t->cr3) {
      uint64_t current_cr3;
      __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
      uint64_t cur_base = current_cr3 & CR3_ADDR_MASK;
      uint64_t nxt_base = next_t->cr3  & CR3_ADDR_MASK;
      if (cur_base != nxt_base) {
        if (cpu_has_pcid() && next_t->mm && next_t->mm->pcid) {
          uint16_t pcid = next_t->mm->pcid;
          uint64_t cr3_val = nxt_base | pcid;
          if (cpu_pcid_is_cached(cpu, pcid)) {
            /* PCID already warm on this CPU — suppress TLB flush */
            cr3_val |= CR3_NOFLUSH;
          } else {
            /* First time running this PCID on this CPU — flushing load */
            cpu_pcid_mark_cached(cpu, pcid);
          }
          cpu_set_active_cr3(cr3_val);
          __asm__ volatile("mov %0, %%cr3" ::"r"(cr3_val) : "memory");
        } else {
          /* No PCID support — plain flushing CR3 load */
          cpu_set_active_cr3(nxt_base);
          __asm__ volatile("mov %0, %%cr3" ::"r"(nxt_base) : "memory");
        }
      }
    } else {
      /* Switching to kernel/idle thread (cr3 == 0).
       * Must reload kernel_cr3 if not already using it.
       * If an idle CPU or kthread were to keep a user process's CR3,
       * that process's address space can be concurrently freed or unmapped
       * by another CPU, leaving this CPU running on a freed PML4. Any subsequent
       * access or interrupt would fault on freed page tables -> triple fault.
       */
      uint64_t current_cr3;
      __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
      if ((current_cr3 & CR3_ADDR_MASK) != (cpu->kernel_cr3 & CR3_ADDR_MASK)) {
        cpu_set_active_cr3(cpu->kernel_cr3);
        __asm__ volatile("mov %0, %%cr3" ::"r"(cpu->kernel_cr3) : "memory");
      }
    }
    if (prev && prev->mm) {
      /* FSGSBASE is enabled for user mode, so a native process can update
       * its TLS bases with WRFSBASE/WRGSBASE without entering the kernel.
       * Save the live values on every switch, not only for compat tasks, or
       * a preempted thread can resume with stale TLS and corrupt its runtime
       * state.  While in the kernel, SWAPGS has put the user GS base in
       * IA32_KERNEL_GS_BASE (0xC0000102). */
      if (prev->mm->compat32) {
        __asm__ volatile("mov %%fs, %0" : "=rm"(prev->compat_fs));
        __asm__ volatile("mov %%gs, %0" : "=rm"(prev->compat_gs));
      }
      prev->fs_base = rdmsr(0xC0000100);
      prev->gs_base = rdmsr(0xC0000102);
    }
    gdt_set_compat_tls(next_t->compat_tls);
    /* Loading GS changes its base. Preserve the kernel per-CPU base until
     * the interrupt/syscall return swaps in the incoming user base. */
    uint64_t kernel_gs = rdmsr(0xC0000101);
    uint16_t fs = next_t->compat_fs, gs = next_t->compat_gs;
    __asm__ volatile("mov %0, %%fs; mov %1, %%gs" :: "rm"(fs), "rm"(gs) : "memory");
    wrmsr(0xC0000101, kernel_gs);
    /* Set TLS base for the incoming thread with hardware FSGSBASE fast-path.
     * Always write on switch because a migrated thread's fs_base/gs_base may
     * differ from what this CPU last loaded even if prev had the same value
     * on another core. */
    if (cpu_has_fsgsbase()) {
      wrfsbase(next_t->fs_base);
    } else {
      wrmsr(0xC0000100, next_t->fs_base);
    }
    wrmsr(0xC0000102, next_t->gs_base);

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  hal_irq_restore(irq_flags);
}

__attribute__((optimize("O3"))) void sched_yield(void) {
  sched_schedule(false);
}

__attribute__((optimize("O3"))) void sched_yield_user(void) {
  sched_schedule(true);
}

/* Priority-ceiling protocol for kernel locks that may sleep with ownership.
 * Keep a resumed owner dispatchable even when userland marked it nice 19 and
 * its home CPU runs a nice -20 task. Started continuations remain CPU-local. */
static void sched_critical_change(bool enter) {
  struct cpu_info *cpu = cpu_get_current();
  struct thread *t = cpu ? cpu->current_thread : NULL;
  if (!t || t->is_idle)
    return;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  struct sched_entity *se = &t->se;
  eevfd_update_curr(&cpu->eevfd, lapic_timer_get_ns());
  bool change = false;
  if (enter) {
    if (se->critical_depth != UINT16_MAX)
      change = se->critical_depth++ == 0;
  } else if (se->critical_depth) {
    change = --se->critical_depth == 0;
  }
  if (change) {
    bool queued = se->on_rq;
    if (queued) eevfd_dequeue_entity(&cpu->eevfd, se);
    /* Rebase into the new weight domain without carrying user-priority debt
     * into the protected section or manufacturing credit when it ends. */
    se->vruntime = cpu->eevfd.vtime;
    se->lag = 0;
    eevfd_set_nice(se, se->nice_value);
    if (queued) eevfd_enqueue_entity(&cpu->eevfd, se);
  }
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
}

void sched_critical_enter(void) { sched_critical_change(true); }
void sched_critical_exit(void) { sched_critical_change(false); }

/*
 * Deferred preemption point.
 *
 * sched_wakeup()/sched_enqueue_thread() set need_resched on whatever thread is
 * running on this CPU when a more eligible thread becomes runnable here.  That
 * is only a request: the CPU is handed over at the next safe boundary, which is
 * what this function is for.  Callers must have already issued their EOI,
 * because switching away here can keep the vector in-service for a long time
 * (see ipi_reschedule_handler).
 *
 * Kernel callers must be returning to an IRQ-enabled context. Atomic KPI
 * sections and nested interrupt contexts retain the request until safe.
 */
__attribute__((optimize("O3"))) void sched_check_resched(bool to_user) {
  (void)to_user;
  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || !cpu->current_thread)
    goto out;

  struct thread *t = cpu->current_thread;
  if (!__atomic_load_n(&t->need_resched, __ATOMIC_ACQUIRE))
    goto out;

  uint8_t state = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
  bool terminal = state == THREAD_DEAD || state == THREAD_ZOMBIE;
  if (state != THREAD_RUNNING && !terminal)
    goto out;

  /* Even terminal threads cannot abandon a CPU pin or an outer handler. */
  extern bool linuxkpi_preempt_allowed(void) __attribute__((weak));
  if (t->kpi_irq_depth || t->kpi_softirq_depth ||
      (linuxkpi_preempt_allowed && !linuxkpi_preempt_allowed()))
    goto out;

  if (__atomic_load_n(&cpu->switching_from, __ATOMIC_ACQUIRE))
    goto out;

  __atomic_store_n(&t->need_resched, false, __ATOMIC_RELEASE);
  sched_yield();
out:
  hal_irq_restore(flags);
}

static uint32_t active_itimer_count = 0;
static uint64_t earliest_itimer_deadline = 0;

void sched_itimer_arm(struct thread *t, uint64_t next_ms, uint64_t interval_ms, uint64_t value_ms) {
  if (!t)
    return;
  bool was_active = (t->it_real_next != 0);
  t->it_real_value = value_ms;
  t->it_real_interval = interval_ms;
  t->it_real_next = next_ms;

  if (next_ms != 0) {
    if (!was_active)
      __atomic_add_fetch(&active_itimer_count, 1, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&earliest_itimer_deadline, __ATOMIC_RELAXED);
    if (cur == 0 || next_ms < cur)
      __atomic_store_n(&earliest_itimer_deadline, next_ms, __ATOMIC_RELAXED);
    lapic_timer_rearm_if_earlier(next_ms);
  } else if (was_active) {
    sched_itimer_disarm(t);
  }
}

void sched_itimer_disarm(struct thread *t) {
  if (!t)
    return;
  if (t->it_real_next != 0) {
    t->it_real_next = 0;
    t->it_real_value = 0;
    t->it_real_interval = 0;
    if (__atomic_load_n(&active_itimer_count, __ATOMIC_RELAXED) > 0)
      __atomic_sub_fetch(&active_itimer_count, 1, __ATOMIC_RELAXED);
  }
}

__attribute__((optimize("O3"))) void sched_tick(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  struct thread *curr = cpu->current_thread;
  if (curr) {
    uint64_t now = lapic_timer_get_ms();
    uint64_t now_ns = lapic_timer_get_ns();
    cpu->ticks = now;

    if (cpu == cpu_get_bsp()) {
      extern void posix_timer_tick(void);
      posix_timer_tick();
      /* Skip tid_lock and thread list traversal entirely when no itimers are
       * active or when the earliest deadline has not arrived yet. */
      if (__atomic_load_n(&active_itimer_count, __ATOMIC_RELAXED) > 0) {
        uint64_t earliest = __atomic_load_n(&earliest_itimer_deadline, __ATOMIC_RELAXED);
        if (earliest == 0 || now >= earliest) {
          extern spinlock_t tid_lock;
          /* Same snapshot-then-wake order as posix_timer_tick/signal_send_pgid:
           * queue the bit under tid_lock, wake after releasing it. Waking
           * takes runqueue/wait-queue locks and must never run with tid_lock
           * held (sys_kill hung this way). No kmalloc in tick context. */
          struct thread *itimer_notify[64];
          uint32_t itimer_notify_count = 0;
          spinlock_acquire(&tid_lock);
          uint64_t next_earliest = 0;
          struct thread *t = global_thread_list;
          while (t) {
            if (t->it_real_next) {
              if (now >= t->it_real_next) {
                uint64_t bit = 1ULL << (SIGALRM - 1);
                t->pending_signals |= bit;
                /* signal_send() records sender as current; in tick context
                 * there is no sender, keep any existing SI_USER info. */
                if (itimer_notify_count < 64)
                  itimer_notify[itimer_notify_count++] = t;
                if (t->it_real_interval) {
                  t->it_real_next = now + t->it_real_interval;
                  lapic_timer_rearm_if_earlier(t->it_real_next);
                } else {
                  t->it_real_next = 0;
                  t->it_real_value = 0;
                  if (__atomic_load_n(&active_itimer_count, __ATOMIC_RELAXED) > 0)
                    __atomic_sub_fetch(&active_itimer_count, 1, __ATOMIC_RELAXED);
                }
              }
              if (t->it_real_next) {
                if (next_earliest == 0 || t->it_real_next < next_earliest)
                  next_earliest = t->it_real_next;
              }
            }
            t = t->global_next;
          }
          __atomic_store_n(&earliest_itimer_deadline, next_earliest, __ATOMIC_RELAXED);
          spinlock_release(&tid_lock);
          for (uint32_t k = 0; k < itimer_notify_count; k++)
            signal_notify_thread(itimer_notify[k], SIGALRM);
        }
      }
    }

    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);

    sched_account_cpu_time_locked(cpu, now);

    sched_deadline_expire_locked(cpu, now);

    if (!curr->is_idle) {
      eevfd_update_curr(&cpu->eevfd, now_ns);
      curr->runtime_total = curr->se.prev_sum_exec_ns / 1000000ULL;
    }

    bool preempt = false;
    if (curr->is_idle && cpu->eevfd.nr_running == 0) {
      spinlock_release(&cpu->queue_lock);
      sched_steal_one_task(cpu);
      spinlock_acquire(&cpu->queue_lock);
    }

    if (curr->is_idle && cpu->eevfd.nr_running > 0) {
      preempt = true;
    } else if (!curr->is_idle && cpu->eevfd.nr_running > 0) {
      uint64_t exec_delta = (curr->se.exec_start_ns && now_ns > curr->se.exec_start_ns)
                                ? (now_ns - curr->se.exec_start_ns)
                                : 0;
      if (curr->se.vruntime >= curr->se.deadline || exec_delta >= curr->se.slice_ns) {
        preempt = true;
      } else if (exec_delta >= EEVFD_MIN_GRANULARITY_NS && cpu->eevfd.rb_leftmost) {
        struct sched_entity *left = rb_entry(cpu->eevfd.rb_leftmost, struct sched_entity, rb_node);
        if (left && eevfd_check_preempt(&cpu->eevfd, &curr->se, left)) {
          preempt = true;
        }
      }
    }

    if (!preempt) {
      sched_arm_next_deadline(cpu, curr);
    }
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(flags);

    if (preempt) {
      /* Never switch away from a KPI thread in an atomic section: rearm so
       * the request is retried on the next tick (or serviced earlier by
       * preempt_enable_resched()). */
      extern bool linuxkpi_preempt_allowed(void) __attribute__((weak));
      if (linuxkpi_preempt_allowed && !linuxkpi_preempt_allowed()) {
        lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
        return;
      }
      sched_schedule(false);
    }
  }
}

struct thread *sched_get_current(void) {
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu)
    return NULL;
  return cpu->current_thread;
}

bool sched_validate_runqueues(struct cpu_info *cpu) {
  if (!cpu) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  bool valid = eevfd_validate_rq(&cpu->eevfd);
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
  return valid;
}

static bool sched_update_priority(struct thread *t, uint8_t priority,
                                  int8_t nice_value, int policy,
                                  uint8_t rt_priority, bool reset_on_fork) {
  if (!t || t->is_idle) return false;
  hal_irq_state_t flags;
  struct cpu_info *cpu = sched_lock_task_rq(t, &flags);
  if (!cpu) return false;
  t->static_priority = priority;
  t->priority = priority;
  t->nice_value = nice_value;
  bool queued = t->se.on_rq;
  if (queued)
    eevfd_dequeue_entity(&cpu->eevfd, &t->se);
  if (policy >= 0) {
    t->sched_policy = (uint8_t)policy;
    t->sched_rt_priority = rt_priority;
    t->sched_reset_on_fork = reset_on_fork;
    t->se.custom_slice = false;
  }
  eevfd_set_nice(&t->se, (int)nice_value);
  if (policy == 1 || policy == 2)
    eevfd_set_slice(&t->se, EEVFD_LATENCY_SLICE_NS);
  else if (policy == 3)
    eevfd_set_slice(&t->se, EEVFD_BATCH_SLICE_NS);
  else if (policy == 5)
    eevfd_set_slice(&t->se, EEVFD_MAX_SLICE_NS);
  if (queued)
    eevfd_enqueue_entity(&cpu->eevfd, &t->se);
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
  return true;
}

bool sched_set_priority(struct thread *t, uint8_t priority, int8_t nice_value) {
  return sched_update_priority(t, priority, nice_value, -1, 0, false);
}

bool sched_set_attributes(struct thread *t, uint8_t priority, int8_t nice_value,
                          uint8_t policy, uint8_t rt_priority, bool reset_on_fork) {
  return sched_update_priority(t, priority, nice_value, policy, rt_priority,
                               reset_on_fork);
}

void sched_print_tasks(void) {
  console_puts("TID  CPU  NICE  WEIGHT  VRUNTIME(ms)  DEADLINE(ms)  STATE       COMM\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    hal_irq_state_t irq_flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);

    struct rb_node *n = rb_first(&cpu->eevfd.tasks_tree);
    while (n) {
      struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
      struct thread *curr = rb_entry(se, struct thread, se);

      klog_puts("TID: ");
      klog_uint64(curr->tid);
      klog_puts(" CPU: ");
      klog_uint64(cpu->cpu_id);
      klog_puts(" Nice: ");
      if (curr->nice_value < 0) {
        klog_puts("-");
        klog_uint64((uint64_t)(-curr->nice_value));
      } else {
        klog_uint64((uint64_t)curr->nice_value);
      }
      klog_puts(" W: ");
      klog_uint64(curr->se.weight);
      klog_puts(" V: ");
      klog_uint64(curr->se.vruntime / 1000000ULL);
      klog_puts(" D: ");
      klog_uint64(curr->se.deadline / 1000000ULL);
      klog_puts(" State: ");
      klog_puts(curr->state == THREAD_RUNNING ? "RUN " : "RDY ");
      klog_puts(" Comm: ");
      klog_puts(curr->comm);
      klog_puts("\n");

      n = rb_next(n);
    }
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(irq_flags);
  }
}

// Helper: remove thread from its CPU's runqueue.
void remove_from_runqueue(struct thread *t) {
  if (!t) return;
  hal_irq_state_t flags;
  struct cpu_info *cpu_local = sched_lock_task_rq(t, &flags);
  if (!cpu_local) return;
  sched_deadline_remove_locked(cpu_local, t);
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu_local->eevfd, &t->se);
    t->on_runqueue = false;
    if (__atomic_load_n(&cpu_local->runnable_count, __ATOMIC_RELAXED))
      __atomic_sub_fetch(&cpu_local->runnable_count, 1, __ATOMIC_RELEASE);
  }
  if (cpu_local->eevfd.curr == &t->se) {
    cpu_local->eevfd.curr = NULL;
  }
  spinlock_release(&cpu_local->queue_lock);
  hal_irq_restore(flags);
}

/* A lockless "not current" check is not sufficient for reclamation: another
 * CPU can select the runnable thread immediately after the check, while the
 * reaper frees its stack. Serialize the final eligibility check and dequeue
 * with dispatch on the owning runqueue. A false return means the thread is
 * still executing / crossing stacks, or its terminal state is not stable;
 * the reaper leaves it queued for a later pass. */
bool sched_prepare_thread_reap(struct thread *t) {
  if (!t || t->is_idle)
    return false;

  hal_irq_state_t irq_flags;
  struct cpu_info *cpu = sched_lock_task_rq(t, &irq_flags);
  if (!cpu)
    return false;

  uint8_t state = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
  if ((state != THREAD_DEAD && state != THREAD_ZOMBIE) ||
      __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t ||
      __atomic_load_n(&cpu->switching_from, __ATOMIC_ACQUIRE) == t) {
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(irq_flags);
    return false;
  }

  sched_deadline_remove_locked(cpu, t);
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu->eevfd, &t->se);
    t->on_runqueue = false;
    if (__atomic_load_n(&cpu->runnable_count, __ATOMIC_RELAXED))
      __atomic_sub_fetch(&cpu->runnable_count, 1, __ATOMIC_RELEASE);
  }
  if (cpu->eevfd.curr == &t->se)
    cpu->eevfd.curr = NULL;

  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(irq_flags);
  return true;
}

__attribute__((optimize("O3"))) void sched_wakeup(struct thread *t) {
  if (!t || t->is_idle || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
    return;
  /* The blocked-state publication and wake decision must share queue_lock.
   * A runnable check here races a waiter which already consumed its wake
   * flag but has not yet stored BLOCKED/SLEEPING under that lock. */

  hal_irq_state_t rflags = hal_irq_save();

  struct cpu_info *self = cpu_get_current();
  uint32_t prev_idx = __atomic_load_n(&t->cpu_index, __ATOMIC_ACQUIRE);
  struct cpu_info *prev_cpu = cpu_get_info(prev_idx);
  if (!sched_cpu_accepting_work(prev_cpu))
    prev_cpu = cpu_get_bsp();

  uint64_t affinity = __atomic_load_n(&t->cpu_affinity, __ATOMIC_RELAXED);
  if (!affinity)
    affinity = ~0ULL;

  struct cpu_info *target = NULL;

  /* If t is still on a CPU (e.g. wakeup raced the tail of sched_schedule()
   * before switch_context cleared t->on_cpu), keep it on that CPU. */
  if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
    uint32_t count = cpu_get_count();
    for (uint32_t i = 0; i < count; i++) {
      struct cpu_info *c = cpu_get_info(i);
      if (!c || c->status == CPU_STATUS_OFFLINE)
        continue;
      if (__atomic_load_n(&c->current_thread, __ATOMIC_ACQUIRE) == t ||
          __atomic_load_n(&c->switching_from, __ATOMIC_ACQUIRE) == t) {
        target = c;
        break;
      }
    }
    if (!target)
      target = prev_cpu;
  } else {
    /* Off-CPU wakeup load balancing:
     * 1. Keep cache/TLB locality on prev_cpu if it is allowed and either
     *    completely idle (load == 0) or has no queued tasks waiting behind
     *    the currently running task (nr_running == 0), or if t holds a
     *    priority-ceiling kernel lock (critical_depth > 0).
     * 2. Otherwise, if the waker's own CPU (`self`) is allowed and idle, wake
     *    locally for warm IPC/pipe cache locality without IPI overhead.
     * 3. Otherwise, select the least-loaded allowed CPU across the system. */
    bool prev_allowed = prev_cpu && (affinity & (1ULL << prev_cpu->cpu_id));
    uint32_t prev_load = prev_allowed ? sched_cpu_load(prev_cpu) : UINT32_MAX;
    if (prev_allowed &&
        (t->se.critical_depth > 0 || prev_load == 0 ||
         (prev_cpu == self && prev_load <= 1) ||
         prev_cpu->eevfd.nr_running == 0)) {
      target = prev_cpu;
    } else if (self && sched_cpu_accepting_work(self) &&
               (affinity & (1ULL << self->cpu_id)) &&
               sched_cpu_load(self) == 0) {
      target = self;
    } else {
      int pref_node = -1;
      if (t->numa_policy.mode != MPOL_DEFAULT)
        pref_node = mempolicy_select_node(&t->numa_policy, 0);
      uint32_t best_load = UINT32_MAX;
      struct cpu_info *best =
          sched_least_loaded_cpu(affinity, pref_node, &best_load);
      if (prev_allowed && best && best != prev_cpu && prev_load <= best_load) {
        target = prev_cpu;
      } else {
        target = best;
      }
    }
  }

  if (!sched_cpu_accepting_work(target)) {
    if (self && sched_cpu_accepting_work(self) &&
        (affinity & (1ULL << self->cpu_id))) {
      target = self;
    } else {
      target = prev_cpu ? prev_cpu : cpu_get_bsp();
    }
  }

  if (target && target->status != CPU_STATUS_OFFLINE) {
    bool notify = false;

    /* Lock both prev_cpu and target in deterministic cpu_id order so we can
     * safely inspect t->on_cpu, unlink any deadline timer on prev_cpu, and
     * enqueue on target atomically.  Also verify that if t->on_cpu is true,
     * prev_cpu is the core actually running/switching t (in case a prior
     * wakeup migrated t->cpu_index while t was still running on its old core). */
    for (;;) {
      uint32_t cur_idx = __atomic_load_n(&t->cpu_index, __ATOMIC_ACQUIRE);
      prev_cpu = cpu_get_info(cur_idx);
      if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        uint32_t count = cpu_get_count();
        for (uint32_t i = 0; i < count; i++) {
          struct cpu_info *c = cpu_get_info(i);
          if (c && c->status != CPU_STATUS_OFFLINE &&
              (__atomic_load_n(&c->current_thread, __ATOMIC_ACQUIRE) == t ||
               __atomic_load_n(&c->switching_from, __ATOMIC_ACQUIRE) == t)) {
            prev_cpu = c;
            target = c;
            break;
          }
        }
      }
      if (!prev_cpu || prev_cpu->status == CPU_STATUS_OFFLINE)
        prev_cpu = target;
      sched_lock_two_rq(prev_cpu, target);
      bool idx_ok = (__atomic_load_n(&t->cpu_index, __ATOMIC_RELAXED) ==
                     prev_cpu->cpu_id);
      bool on_prev =
          (__atomic_load_n(&prev_cpu->current_thread, __ATOMIC_RELAXED) == t ||
           __atomic_load_n(&prev_cpu->switching_from, __ATOMIC_RELAXED) == t);
      if (on_prev || (idx_ok && !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE))) {
        break;
      }
      sched_unlock_two_rq(prev_cpu, target);
    }

    /* If t is still on prev_cpu's stack (wakeup raced the outgoing schedule
     * before switch.asm cleared on_cpu), do not migrate it across CPUs; wake
     * it in place on prev_cpu instead. */
    struct cpu_info *dest = target;
    if (dest != prev_cpu &&
        (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&prev_cpu->current_thread, __ATOMIC_ACQUIRE) == t ||
         __atomic_load_n(&prev_cpu->switching_from, __ATOMIC_ACQUIRE) == t)) {
      dest = prev_cpu;
    }

    uint8_t cur_state = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (cur_state == THREAD_BLOCKED || cur_state == THREAD_SLEEPING) {
      bool still_current =
          __atomic_load_n(&dest->current_thread, __ATOMIC_ACQUIRE) == t;
      sched_deadline_remove_locked(prev_cpu, t);
      if (dest != prev_cpu)
        sched_deadline_remove_locked(dest, t);
      __atomic_store_n(&t->state,
                       still_current ? THREAD_RUNNING : THREAD_READY,
                       __ATOMIC_RELEASE);
      t->wakeup_ticks = 0;
      /* Timestamp the transition, including a waiter already linked by a
       * racing deadline expiry. Repeated wakeups must not erase its age. */
      if (still_current)
        t->ready_since_ms = 0;
      else if (!t->ready_since_ms)
        t->ready_since_ms = lapic_timer_get_ms();
      if (!still_current && !t->se.on_rq) {
        __atomic_store_n(&t->cpu_index, dest->cpu_id, __ATOMIC_RELEASE);
        if (dest != prev_cpu)
          eevfd_migrate_entity(&prev_cpu->eevfd, &dest->eevfd, &t->se);
        else
          eevfd_place_entity(&dest->eevfd, &t->se, false);
        eevfd_enqueue_entity(&dest->eevfd, &t->se);
        t->on_runqueue = true;
        __atomic_add_fetch(&dest->runnable_count, 1, __ATOMIC_RELEASE);

        struct thread *running = dest->current_thread;
        bool should_kick =
            !running || running->is_idle || running == dest->idle_thread ||
            eevfd_check_preempt(&dest->eevfd, &running->se, &t->se);

        notify = should_kick;
      }
    } else {
      /* A timeout, signal, or earlier wake may have made the thread runnable
       * while it was still linked on an external wait queue. Wakeups are
       * idempotent: do not turn a live RUNNING thread into READY and enqueue
       * its active stack a second time. */
    }
    sched_unlock_two_rq(prev_cpu, target);

    if (notify)
      sched_notify_runnable(dest);
  } else {
    uint8_t cur_state = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (cur_state == THREAD_BLOCKED || cur_state == THREAD_SLEEPING) {
      t->state = THREAD_READY;
      t->wakeup_ticks = 0;
      sched_enqueue_thread(t, cpu_get_current());
    }
  }

  hal_irq_restore(rflags);
}

void sched_enforce_affinity(struct thread *t) {
  if (!t || t->is_idle)
    return;

  uint64_t affinity = __atomic_load_n(&t->cpu_affinity, __ATOMIC_ACQUIRE);
  if (!affinity)
    return;

  uint32_t cur_idx = __atomic_load_n(&t->cpu_index, __ATOMIC_ACQUIRE);
  if (affinity & (1ULL << cur_idx))
    return;

  int pref_node = -1;
  if (t->numa_policy.mode != MPOL_DEFAULT)
    pref_node = mempolicy_select_node(&t->numa_policy, 0);
  struct cpu_info *dest = sched_least_loaded_cpu(affinity, pref_node, NULL);
  if (!sched_cpu_accepting_work(dest))
    return;

  hal_irq_state_t flags = hal_irq_save();
  struct cpu_info *src = cpu_get_info(cur_idx);
  if (!src || src->status == CPU_STATUS_OFFLINE || src == dest) {
    hal_irq_restore(flags);
    return;
  }

  sched_lock_two_rq(src, dest);
  bool migrated = false;
  bool kick_src = false;

  if (__atomic_load_n(&t->cpu_index, __ATOMIC_RELAXED) == src->cpu_id) {
    if (!__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_READY &&
        t->se.on_rq && !t->deadline_queued) {
      eevfd_dequeue_entity(&src->eevfd, &t->se);
      t->on_runqueue = false;
      if (__atomic_load_n(&src->runnable_count, __ATOMIC_RELAXED))
        __atomic_sub_fetch(&src->runnable_count, 1, __ATOMIC_RELEASE);

      __atomic_store_n(&t->cpu_index, dest->cpu_id, __ATOMIC_RELEASE);
      eevfd_migrate_entity(&src->eevfd, &dest->eevfd, &t->se);
      eevfd_enqueue_entity(&dest->eevfd, &t->se);
      t->on_runqueue = true;
      __atomic_add_fetch(&dest->runnable_count, 1, __ATOMIC_RELEASE);
      migrated = true;
    } else if (__atomic_load_n(&src->current_thread, __ATOMIC_ACQUIRE) == t) {
      __atomic_store_n(&t->need_resched, true, __ATOMIC_RELEASE);
      kick_src = true;
    }
  }

  sched_unlock_two_rq(src, dest);
  hal_irq_restore(flags);

  if (migrated && lapic_is_ready()) {
    struct cpu_info *self = cpu_get_current();
    if (dest != self)
      lapic_send_ipi(dest->apic_id, IPI_VECTOR_RESCHEDULE);
  } else if (kick_src) {
    struct cpu_info *self = cpu_get_current();
    if (src == self) {
      sched_yield();
    } else if (lapic_is_ready()) {
      lapic_send_ipi(src->apic_id, IPI_VECTOR_RESCHEDULE);
    }
  }
}
