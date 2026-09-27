#include "sched.h"
#include "sched_internal.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/features.h"
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
#include "../smp/cpu.h"

extern void switch_context(struct thread *old_t, struct thread *new_t);
extern void tss_set_rsp0(uint64_t rsp0);
extern void signal_send(struct thread *, int);

static void ipi_reschedule_handler(struct registers *regs) {
  (void)regs;
  /*
   * sched_yield() may switch away from this interrupt context indefinitely.
   * Acknowledging the IPI in the common ISR epilogue is therefore too late:
   * the LAPIC keeps the reschedule vector in-service and can withhold later
   * timer/IPI delivery from this CPU.  This is especially easy to trigger
   * when an exiting child wakes its parent on another CPU under KVM.
   *
   * The common epilogue will issue a second EOI if this context eventually
   * resumes; as with the LAPIC timer handler, that redundant EOI is harmless.
   */
  lapic_send_eoi();
  sched_yield();
}

/* Timed waits are rare compared with scheduler entries. Keep insertion O(n)
 * and make the yield/timer hot path O(1) by maintaining the earliest deadline
 * at the head. All helpers require cpu->queue_lock. */
void sched_deadline_remove_locked(struct cpu_info *cpu,
                                  struct thread *t) {
  if (!t->deadline_queued)
    return;

  struct thread **link = &cpu->deadline_head;
  while (*link && *link != t)
    link = &(*link)->deadline_next;
  if (*link == t)
    *link = t->deadline_next;

  t->deadline_next = NULL;
  t->deadline_queued = false;
}

void sched_deadline_insert_locked(struct cpu_info *cpu,
                                  struct thread *t) {
  if (!t->wakeup_ticks)
    return;
  if (t->deadline_queued)
    sched_deadline_remove_locked(cpu, t);

  struct thread **link = &cpu->deadline_head;
  while (*link && (*link)->wakeup_ticks <= t->wakeup_ticks)
    link = &(*link)->deadline_next;
  t->deadline_next = *link;
  *link = t;
  t->deadline_queued = true;
}

void sched_deadline_expire_locked(struct cpu_info *cpu, uint64_t now) {
  while (cpu->deadline_head && cpu->deadline_head->wakeup_ticks <= now) {
    struct thread *t = cpu->deadline_head;
    cpu->deadline_head = t->deadline_next;
    t->deadline_next = NULL;
    t->deadline_queued = false;

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
          cpu->runnable_count++;
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
  if (cpu->deadline_head &&
      (!deadline || cpu->deadline_head->wakeup_ticks < deadline))
    deadline = cpu->deadline_head->wakeup_ticks;
  if (deadline) lapic_timer_rearm_if_earlier(deadline);
}

void sched_init(void) {
  // We expect this to be called after cpu_init() which populates the CPU list
  uint32_t count = cpu_get_count();

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu)
      continue;

    spinlock_init(&cpu->queue_lock);
    eevfd_rq_init(&cpu->eevfd);
    cpu->deadline_head = NULL;

    // Register reschedule IPI handler once on BSP
    if (i == 0) {
      register_interrupt_handler(IPI_VECTOR_RESCHEDULE, ipi_reschedule_handler);
    }

    // Create the idle thread for this specific CPU
    struct thread *idle_thread = kmalloc(sizeof(struct thread));
    memset(idle_thread, 0, sizeof(struct thread));
    idle_thread->cwd_path[0] = '/';
    idle_thread->cwd_node = fs_root;
    if (fs_root)
      vfs_open(fs_root);

    // Assign a proper TID to the idle thread (don't use 0)
    spinlock_acquire(&tid_lock);
    idle_thread->tid = next_tid++;
    spinlock_release(&tid_lock);
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

    spinlock_acquire(&tid_lock);
    idle_thread->global_next = global_thread_list;
    global_thread_list = idle_thread;
    cpu->idle_thread = idle_thread;
    cpu->current_thread = idle_thread;
    idle_thread->cpu_affinity = (1ULL << count) - 1;
    if (count == 64)
      idle_thread->cpu_affinity = ~0ULL;
    spinlock_release(&tid_lock);
  }
}

__attribute__((optimize("O3"))) void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu) {
  if (!t || t->is_idle || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
    return;

  struct cpu_info *target_cpu = explicit_cpu;

  if (!target_cpu) {
    struct cpu_info *prev_cpu = cpu_get_info(t->cpu_index);

    // 1. Strong preference for the previous CPU (cache/TLB warm).
    //    Accept it unconditionally if it is online and within affinity —
    //    unless it is heavily overloaded (≥3 extra tasks vs the least-loaded
    //    peer).  This prevents gears and Xorg from migrating on every IPC
    //    round-trip which would otherwise thrash L1/L2 and cause TLB flushes
    //    on every socket send/recv cycle.
    if (prev_cpu && prev_cpu->status != CPU_STATUS_OFFLINE &&
        (t->cpu_affinity & (1ULL << prev_cpu->cpu_id))) {
      // Find min load first so we can check the overload threshold
      uint32_t min_threads = 0xFFFFFFFF;
      uint32_t count = cpu_get_count();
      for (uint32_t i = 0; i < count; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!c || c->status == CPU_STATUS_OFFLINE) continue;
        if (!(t->cpu_affinity & (1ULL << i))) continue;
        if (c->runnable_count < min_threads) min_threads = c->runnable_count;
      }
      // Stay on prev_cpu unless it is ≥3 tasks more loaded than the minimum
      if (prev_cpu->runnable_count <= min_threads + 2) {
        target_cpu = prev_cpu;
      }
    }

    // 2. Fallback: Search for the least-loaded CPU
    if (!target_cpu) {
      uint32_t min_threads = 0xFFFFFFFF;
      uint32_t count = cpu_get_count();

      for (uint32_t i = 0; i < count; i++) {
        struct cpu_info *cpu = cpu_get_info(i);
        if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
          continue;

        if (!(t->cpu_affinity & (1ULL << i)))
          continue;

        if (cpu->runnable_count < min_threads) {
          min_threads = cpu->runnable_count;
          target_cpu = cpu;
        }
      }
    }
  }

  if (!target_cpu || target_cpu->status == CPU_STATUS_OFFLINE) {
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
    target_cpu->runnable_count++;

  t->cpu_index = target_cpu->cpu_id;

  struct thread *running = target_cpu->current_thread;
  bool kick_cpu = !running || running == target_cpu->idle_thread || running->is_idle ||
                  eevfd_check_preempt(&target_cpu->eevfd, &running->se, &t->se);
  spinlock_release(&target_cpu->queue_lock);

  if (kick_cpu && lapic_is_ready()) {
    struct cpu_info *self = cpu_get_current();
    if (target_cpu == self) {
      /* Prefer an immediate hand-over at the next reschedule point over
       * waiting a full tick; the rearm stays as the backstop. */
      if (running)
        __atomic_store_n(&running->need_resched, true, __ATOMIC_RELEASE);
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    } else {
      lapic_send_ipi(target_cpu->apic_id, IPI_VECTOR_RESCHEDULE);
    }
  }

  /* Restore the caller's interrupt state: some callers (wait-queue wakes from
   * IRQ context) enter with IF=0, and unconditionally enabling here left an
   * interrupt window inside their critical section. */
  hal_irq_restore(irq_flags);
}

__attribute__((optimize("O3"))) static void sched_balance(struct cpu_info *cpu) {
  uint32_t count = cpu_get_count();
  if (count <= 1)
    return;

  // Find the CPU with highest load
  struct cpu_info *richest_cpu = NULL;
  uint32_t max_threads = 0;

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *other = cpu_get_info(i);
    if (!other || other == cpu || other->status == CPU_STATUS_OFFLINE)
      continue;

    uint32_t nr = other->eevfd.nr_running;
    if (nr > max_threads) {
      max_threads = nr;
      richest_cpu = other;
    }
  }

  // Imbalance threshold: only steal if richest has 2+ more tasks than us
  if (!richest_cpu || max_threads < 2 ||
      (max_threads - cpu->eevfd.nr_running) < 2)
    return;

  if (!spinlock_try_acquire(&richest_cpu->queue_lock))
    return;

  // Steal from RIGHTMOST (highest vruntime = most indebted task).
  // This minimizes disruption to the source CPU's fairness.
  struct thread *stolen = NULL;
  struct rb_node *n = rb_last(&richest_cpu->eevfd.tasks_tree);
  while (n) {
    struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
    struct thread *curr = rb_entry(se, struct thread, se);
    if (curr->state == THREAD_READY && !curr->is_idle &&
        curr != __atomic_load_n(&richest_cpu->current_thread, __ATOMIC_ACQUIRE) &&
        curr != __atomic_load_n(&richest_cpu->switching_from, __ATOMIC_ACQUIRE) &&
        (curr->cpu_affinity & (1ULL << cpu->cpu_id))) {
      stolen = curr;
      eevfd_dequeue_entity(&richest_cpu->eevfd, &stolen->se);
      stolen->on_runqueue = false;
      if (richest_cpu->runnable_count)
        richest_cpu->runnable_count--;
      break;
    }
    n = rb_prev(n);
  }

  if (stolen) {
    stolen->cpu_index = cpu->cpu_id;
    eevfd_migrate_entity(&richest_cpu->eevfd, &cpu->eevfd, &stolen->se);
    eevfd_enqueue_entity(&cpu->eevfd, &stolen->se);
    stolen->on_runqueue = true;
    cpu->runnable_count++;
  }

  spinlock_release(&richest_cpu->queue_lock);
}

__attribute__((optimize("O3"))) static void sched_schedule(bool voluntary_yield) {
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    hal_irq_enable();
    return;
  }

  struct thread *prev = cpu->current_thread;

  // Reap detached threads from a different scheduler context.
  sched_process_reap_queue(prev);

  spinlock_acquire(&cpu->queue_lock);

  uint64_t now = lapic_timer_get_ms();
  uint64_t now_ns = lapic_timer_get_ns();

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
    hal_irq_enable();
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
    if (cpu->runnable_count)
      cpu->runnable_count--;
  } else if (prev->state == THREAD_BLOCKED || prev->state == THREAD_SLEEPING) {
    if (prev->se.on_rq) {
      eevfd_dequeue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = false;
    }
    if (cpu->runnable_count)
      cpu->runnable_count--;
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
      prev->cpu_index = cpu->cpu_id;
      eevfd_enqueue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = true;
    }
  }

  // 2. Select earliest eligible virtual deadline task
  struct sched_entity *next_se = eevfd_pick_next_entity(&cpu->eevfd);
  struct thread *next_t = next_se ? rb_entry(next_se, struct thread, se) : NULL;

  // 2.5 Load Balancing (Work Stealing)
  if (!next_t) {
    sched_balance(cpu);
    next_se = eevfd_pick_next_entity(&cpu->eevfd);
    next_t = next_se ? rb_entry(next_se, struct thread, se) : NULL;
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
    __atomic_store_n(&cpu->switching_from, prev, __ATOMIC_RELEASE);
    cpu->current_thread = next_t;

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
          __asm__ volatile("mov %0, %%cr3" ::"r"(cr3_val) : "memory");
        } else {
          /* No PCID support — plain flushing CR3 load */
          __asm__ volatile("mov %0, %%cr3" ::"r"(nxt_base) : "memory");
        }
      }
    }
    /* Set TLS base for the incoming thread with hardware FSGSBASE fast-path */
    if (prev->fs_base != next_t->fs_base) {
      if (cpu_has_fsgsbase()) {
        wrfsbase(next_t->fs_base);
      } else {
        wrmsr(0xC0000100, next_t->fs_base);
      }
    }
    if (prev->gs_base != next_t->gs_base) {
      wrmsr(0xC0000102, next_t->gs_base);
    }

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  __atomic_store_n(&cpu->switching_from, NULL, __ATOMIC_RELEASE);
  hal_irq_enable();
}

__attribute__((optimize("O3"))) void sched_yield(void) {
  sched_schedule(false);
}

__attribute__((optimize("O3"))) void sched_yield_user(void) {
  sched_schedule(true);
}

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
 * Deliberately narrow so it cannot destabilise anything:
 *   - to_user == false only switches when this CPU is sitting in its idle loop,
 *     i.e. there is no kernel-mode driver state to preempt.  Preempting an
 *     arbitrary kernel context remains the job of the tick and of the
 *     reschedule IPI, both of which already worked.
 *   - the LAPIC rearm in the wakeup paths is left in place, so a request that
 *     is missed for any reason costs latency, never a lost wakeup.
 */
__attribute__((optimize("O3"))) void sched_check_resched(bool to_user) {
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu || !cpu->current_thread)
    return;

  struct thread *t = cpu->current_thread;
  if (!__atomic_load_n(&t->need_resched, __ATOMIC_ACQUIRE))
    return;

  if (!to_user && !t->is_idle)
    return;

  /* Dying or already-blocked threads are leaving the CPU on their own. */
  if (t->state != THREAD_RUNNING)
    return;

  /* Never re-enter the scheduler from inside a context switch. */
  if (__atomic_load_n(&cpu->switching_from, __ATOMIC_ACQUIRE))
    return;

  __atomic_store_n(&t->need_resched, false, __ATOMIC_RELEASE);
  sched_yield();
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
      /* ITIMER_REAL expiry has to be walked under tid_lock.  The list is only
       * unlinked under that lock (sched_thread.c), but sched_reap_thread()
       * releases it and then kfree()s the thread, so the previous lock-free
       * walk could follow t->global_next through a freed slab object - and
       * signal_send() would then write into memory the allocator had already
       * handed to someone else.  signal_send_pgid() below signals threads the
       * same way, under the same lock, so this is the established order and
       * not a new one. */
      extern spinlock_t tid_lock;
      spinlock_acquire(&tid_lock);
      struct thread *t = global_thread_list;
      while (t) {
        if (t->it_real_next && now >= t->it_real_next) {
          signal_send(t, SIGALRM);
          if (t->it_real_interval) {
            t->it_real_next = now + t->it_real_interval;
            lapic_timer_rearm_if_earlier(t->it_real_next);
          } else {
            t->it_real_next = 0;
            t->it_real_value = 0;
          }
        }
        t = t->global_next;
      }
      spinlock_release(&tid_lock);
    }

    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);

    sched_deadline_expire_locked(cpu, now);

    if (!curr->is_idle) {
      eevfd_update_curr(&cpu->eevfd, now_ns);
      curr->runtime_total = curr->se.prev_sum_exec_ns / 1000000ULL;
    }

    bool preempt = false;
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
      sched_schedule(false);
    }
  }
}

struct thread *sched_get_current(void) {
  return cpu_get_current()->current_thread;
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

bool sched_set_priority(struct thread *t, uint8_t priority, int8_t nice_value) {
  if (!t || t->is_idle) return false;
  struct cpu_info *cpu = cpu_get_info(t->cpu_index);
  if (!cpu || cpu->status == CPU_STATUS_OFFLINE) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  t->static_priority = priority;
  t->priority = priority;
  t->nice_value = nice_value;
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu->eevfd, &t->se);
    eevfd_set_nice(&t->se, (int)nice_value);
    eevfd_enqueue_entity(&cpu->eevfd, &t->se);
  } else {
    eevfd_set_nice(&t->se, (int)nice_value);
  }
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
  return true;
}

void sched_print_tasks(void) {
  console_puts("TID  CPU  NICE  WEIGHT  VRUNTIME(ms)  DEADLINE(ms)  STATE       COMM\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    hal_irq_disable();
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
    hal_irq_enable();
  }
}

// Helper: remove thread from its CPU's runqueue.
void remove_from_runqueue(struct thread *t) {
  if (!t) return;
  struct cpu_info *cpu_local = cpu_get_info(t->cpu_index);
  if (!cpu_local || cpu_local->status == CPU_STATUS_OFFLINE) return;
  spinlock_acquire(&cpu_local->queue_lock);
  sched_deadline_remove_locked(cpu_local, t);
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu_local->eevfd, &t->se);
    t->on_runqueue = false;
    if (cpu_local->runnable_count)
      cpu_local->runnable_count--;
  }
  if (cpu_local->eevfd.curr == &t->se) {
    cpu_local->eevfd.curr = NULL;
  }
  spinlock_release(&cpu_local->queue_lock);
}

__attribute__((optimize("O3"))) void sched_wakeup(struct thread *t) {
  if (!t || t->is_idle || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
    return;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    return; // Already runnable, nothing to do

  hal_irq_state_t rflags = hal_irq_save();

  struct cpu_info *self = cpu_get_current();
  struct cpu_info *prev_cpu = cpu_get_info(t->cpu_index);
  struct cpu_info *target = NULL;

  /* If the thread is currently executing on any CPU, keep it targeted on that CPU to prevent duplicate execution */
  uint32_t count = cpu_get_count();
  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *c = cpu_get_info(i);
    if (c && c->status != CPU_STATUS_OFFLINE &&
        __atomic_load_n(&c->current_thread, __ATOMIC_ACQUIRE) == t) {
      target = c;
      break;
    }
  }

  if (!target)
    target = prev_cpu;

  /* Preserve thread CPU parallelism: keep the thread on its previously assigned CPU (prev_cpu)
   * so parallel worker threads stay distributed across separate cores rather than stacking on one. */
  if (!target || target->status == CPU_STATUS_OFFLINE || !(t->cpu_affinity & (1ULL << target->cpu_id))) {
    if (self && self->status != CPU_STATUS_OFFLINE && (t->cpu_affinity & (1ULL << self->cpu_id))) {
      target = self;
    } else {
      target = cpu_get_bsp();
    }
  }

  if (target && target->status != CPU_STATUS_OFFLINE) {
    bool send_ipi = false;
    bool rearm_local = false;

    if (target != prev_cpu && prev_cpu && prev_cpu->status != CPU_STATUS_OFFLINE && t->deadline_queued) {
      spinlock_acquire(&prev_cpu->queue_lock);
      sched_deadline_remove_locked(prev_cpu, t);
      spinlock_release(&prev_cpu->queue_lock);
    }

    spinlock_acquire(&target->queue_lock);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
      bool still_current =
          __atomic_load_n(&target->current_thread, __ATOMIC_ACQUIRE) == t;
      sched_deadline_remove_locked(target, t);
      t->state = still_current ? THREAD_RUNNING : THREAD_READY;
      t->wakeup_ticks = 0;
      if (!still_current && !t->se.on_rq) {
        /* Publish the target only once the enqueue is serialized under its
         * queue_lock; a stale/racy cpu_index makes the deadline and removal
         * paths operate on another CPU's list/tree.  The on_rq check refuses
         * to link an entity that a racing expire/wakeup already enqueued. */
        t->cpu_index = target->cpu_id;
        eevfd_place_entity(&target->eevfd, &t->se, false);
        eevfd_enqueue_entity(&target->eevfd, &t->se);
        t->on_runqueue = true;
        target->runnable_count++;

        struct thread *running = target->current_thread;
        bool should_kick = !running || running->is_idle || running == target->idle_thread ||
                           eevfd_check_preempt(&target->eevfd, &running->se, &t->se);

        if (target->apic_id != self->apic_id) {
          if (should_kick)
            send_ipi = true;
        } else if (should_kick) {
          /* Same CPU: ask the running thread to hand over at its next
           * interrupt/syscall exit instead of waiting up to a full tick.
           * `running` is never t here (that case is still_current above). */
          if (running)
            __atomic_store_n(&running->need_resched, true, __ATOMIC_RELEASE);
          rearm_local = true;
        }
      } else {
        t->ready_since_ms = 0;
      }
    }
    spinlock_release(&target->queue_lock);

    if (send_ipi && lapic_is_ready()) {
      lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);
    } else if (rearm_local && lapic_is_ready()) {
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    }
  } else {
    t->state = THREAD_READY;
    t->wakeup_ticks = 0;
    sched_enqueue_thread(t, cpu_get_current());
  }

  hal_irq_restore(rflags);
}
