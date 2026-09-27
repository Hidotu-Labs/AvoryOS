#include "sched.h"
#include "sched_internal.h"
#include "wait.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/fpu.h"
#include "../fs/procfs.h"
#include "../fs/vfs.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"

// Futex waiter records are stack-resident and must be detached before a
// forced-exit thread stack is released.
void futex_remove_thread_waiters(struct thread *thread);

// Wakes poll()/waitid(P_PIDFD) waiters; see sys_process.c.
void pidfd_wake_waiters(void);

// Threads in a CLONE_THREAD group are not wait4() children. Queue them for
// destruction after they have switched off their kernel stacks.
static struct thread *reap_queue = NULL;
static spinlock_t reap_queue_lock = SPINLOCK_INIT;
static spinlock_t reap_worker_lock = SPINLOCK_INIT;

bool sched_thread_off_cpu(struct thread *t) {
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;
    if (__atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t ||
        __atomic_load_n(&cpu->switching_from, __ATOMIC_ACQUIRE) == t)
      return false;
  }
  return true;
}

void sched_queue_reap(struct thread *t) {
  if (!t || t->is_idle)
    return;

  spinlock_acquire(&reap_queue_lock);
  t->reap_next = reap_queue;
  reap_queue = t;
  spinlock_release(&reap_queue_lock);
}

void sched_queue_reap_and_wait(struct thread *t) {
  if (!t || t->is_idle)
    return;

  sched_queue_reap(t);

  for (;;) {
    sched_yield();

    spinlock_acquire(&reap_worker_lock);
    spinlock_acquire(&reap_queue_lock);
    bool pending = false;
    for (struct thread *it = reap_queue; it; it = it->reap_next) {
      if (it == t) {
        pending = true;
        break;
      }
    }
    spinlock_release(&reap_queue_lock);
    spinlock_release(&reap_worker_lock);

    if (!pending)
      return;
  }
}

void sched_process_reap_queue(struct thread *prev) {
  if (__atomic_load_n(&reap_queue, __ATOMIC_RELAXED) != NULL &&
      spinlock_try_acquire(&reap_worker_lock)) {
    while (1) {
      struct thread *victim = NULL;
      spinlock_acquire(&reap_queue_lock);
      struct thread **link = &reap_queue;
      while (*link) {
        if (*link != prev && sched_thread_off_cpu(*link)) {
          victim = *link;
          *link = victim->reap_next;
          victim->reap_next = NULL;
          break;
        }
        link = &(*link)->reap_next;
      }
      spinlock_release(&reap_queue_lock);

      if (!victim)
        break;
      sched_reap_thread(victim);
    }
    spinlock_release(&reap_worker_lock);
  }
}

void sched_reap_thread(struct thread *t) {
  if (!t || t->is_idle)
    return;

  klog_debug_puts("[REAP] Reaping thread ");
  klog_debug_uint64(t->tid);
  klog_debug_puts("\n");

  futex_remove_thread_waiters(t);
  wait_queue_cleanup_thread(t);

  /* The reap-queue claimant already verified sched_thread_off_cpu() while
   * holding reap_queue_lock. DEAD threads cannot become runnable again, so
   * repeating that check here can only spin forever on a stale hazard. */

  /* Ensure reaped thread is removed from its CPU's runqueue before freeing */
  remove_from_runqueue(t);

  klog_debug_puts("[REAP] Step 1: remove from lists\n");
  spinlock_acquire(&tid_lock);

  // 1.5 Remove from global thread list
  if (global_thread_list == t) {
    global_thread_list = t->global_next;
  } else {
    struct thread *prev_g = global_thread_list;
    while (prev_g && prev_g->global_next != t)
      prev_g = prev_g->global_next;
    if (prev_g)
      prev_g->global_next = t->global_next;
  }

  // 1.75 Remove from parent's children list
  bool parent_is_live = false;
  if (t->parent) {
    for (struct thread *it = global_thread_list; it; it = it->global_next) {
      if (it == t->parent) {
        parent_is_live = true;
        break;
      }
    }
  }
  if (parent_is_live) {
    if (t->parent->children == t) {
      t->parent->children = t->sibling_next;
    } else {
      struct thread *p = t->parent->children;
      while (p && p->sibling_next != t)
        p = p->sibling_next;
      if (p)
        p->sibling_next = t->sibling_next;
    }
  }

  // exit_group can reap related clone threads in any order. Clear every
  // surviving back-pointer before the thread object is freed.
  for (struct thread *it = global_thread_list; it; it = it->global_next) {
    if (it->parent == t) {
      it->parent = NULL;
      it->sibling_next = NULL;
    }
  }
  t->parent = NULL;
  t->children = NULL;
  t->sibling_next = NULL;
  spinlock_release(&tid_lock);

  /* Monitoring tools poll /proc continuously. Drop the cached PID tree
   * after the task has been removed from global lookup. */
  procfs_release_pid_dir(t->tid);

  // 3. Free fork_ctx (saved register state)
  klog_debug_puts("[REAP] Step 3: free fork_ctx\n");
  if (t->fork_ctx) {
    kfree(t->fork_ctx);
    t->fork_ctx = NULL;
  }

  // Normally released by process_do_exit(); retain this as a safety net for
  // kernel-thread and abnormal teardown paths.
  sched_release_files(t);
  if (t->cwd_node) {
    vfs_close(t->cwd_node);
    t->cwd_node = NULL;
  }

  // 4. Free user page tables (CR3) and MM if last thread
  if (t->mm) {
    /* Lifetime references are independent of VMA mutations. An atomic drop
     * avoids waiting on an mm lock from scheduler/reaper context and makes
     * exactly one reaper responsible for final destruction. */
    int refs = __atomic_sub_fetch(&t->mm->ref_count, 1, __ATOMIC_ACQ_REL);
    if (refs == 0) {
      klog_debug_puts("[REAP] Last thread, freeing MM resources\n");
      if (t->cr3) {
        vmm_free_user_pages_vma(t->cr3, &t->mm->vmas);
        t->cr3 = 0;
      }
      if (t->mm->pcid) {
        pcid_free(t->mm->pcid);
        t->mm->pcid = 0;
      }
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);

    } else {
      klog_debug_puts("[REAP] MM still shared, skipping CR3 free\n");
      t->cr3 = 0; // Don't free for THIS thread
    }
    t->mm = NULL;
  }

  /* HHDM-backed stack pages need no page-table unmap during release. */
  thread_stack_release(t->stack_base);
  /* Clear any per-CPU fpu_owner references pointing at this thread
     before the struct is freed, to avoid stale pointer dereferences. */
  fpu_forget_thread(t);
  kfree(t);

  klog_debug_puts("[REAP] Done\n");
}

bool sched_terminate_thread(uint32_t tid) {
  struct thread *t = sched_get_thread_by_tid(tid);
  if (!t || t->is_idle || t->state == THREAD_DEAD ||
      t->state == THREAD_ZOMBIE)
    return false;
  __atomic_store_n(&t->state, THREAD_DEAD, __ATOMIC_RELEASE);
  struct cpu_info *cpu = cpu_get_info(t->cpu_index);
  if (cpu && cpu->status != CPU_STATUS_OFFLINE &&
      __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t)
    lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);
  sched_queue_reap(t);
  return true;
}

void sched_terminate_thread_group(struct thread *current) {
  if (!current)
    return;

  uint32_t killed = 0;
  struct thread *leader = NULL;

  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t == current || t->tgid != current->tgid || t->is_idle)
      continue;

    // Identify the thread-group leader
    if (t->tid == current->tgid) {
      leader = t;
      continue;
    }

    if (t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
      continue;

    /* exit_group is process-wide. Mark siblings unschedulable before making
     * them visible to the asynchronous reaper. The reaper waits until a
     * running sibling is off-CPU before touching any of its resources. */
    t->reap_remove_runqueue = true;
    __atomic_store_n(&t->state, THREAD_DEAD, __ATOMIC_RELEASE);

    struct cpu_info *cpu = cpu_get_info(t->cpu_index);
    if (cpu && cpu->status != CPU_STATUS_OFFLINE &&
        __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t)
      lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);

    sched_queue_reap(t);
    killed++;
  }

  // If leader was found and is not current, preserve leader as ZOMBIE
  if (leader && leader != current) {
    leader->reap_remove_runqueue = true;
    remove_from_runqueue(leader);
    leader->state = THREAD_ZOMBIE;
    leader->exit_status = current->exit_status ? current->exit_status : 0;
    struct thread *parent_to_wake = NULL;
    if (leader->parent) {
      uint32_t parent_tgid = leader->parent->tgid;
      for (struct thread *waiter = global_thread_list; waiter;
           waiter = waiter->global_next) {
        if (waiter->tgid == parent_tgid && waiter->waiting_for_child &&
            waiter->state == THREAD_BLOCKED) {
          parent_to_wake = waiter;
          break;
        }
      }
    }
    spinlock_release(&tid_lock);
    if (parent_to_wake)
      sched_wakeup(parent_to_wake);
  } else {
    spinlock_release(&tid_lock);
  }

  /* The leader is now a zombie and the siblings are DEAD: tell anyone waiting
   * on a pidfd for this process group. */
  pidfd_wake_waiters();

  if (killed) {
    klog_debug_puts("[EXIT_GROUP] queued sibling threads: ");
    klog_debug_uint64(killed);
    klog_debug_puts("\n");
  }
}
