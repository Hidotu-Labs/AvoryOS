#include "../sched/capability.h"
// Process syscall implementation.
#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/fpu.h"
#include "../cpu/msr.h"
#include "../drivers/storage/nvme.h"
#include "../drivers/net/e1000.h"
#include "../drivers/timer/rtc.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/numa.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"

#include "../sched/sched.h"
#include "../sched/namespace.h"
#include "../sched/pid_namespace.h"
#include "../sched/seccomp.h"
#include "../sched/net_namespace.h"
#include "../fs/mount_namespace.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "sysdiag.h"
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#ifdef LINUXKPI_LINUX_IMPORTED
#include <linuxkpi/native_pci.h>
#endif

#include "sys_process_internal.h"

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;

#define WNOHANG 1
#define __WNOTHREAD 0x20000000

static uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_ptr, uint64_t options,
                          uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)rusage;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int target_pid = (int)pid;

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;
    struct thread *zombie_owner = NULL;
    bool should_block = false;

    spinlock_acquire(&tid_lock);
    /* Clear the flag now that we hold the lock and are about to rescan.
     * This prevents a spurious re-block if a wakeup arrived between our
     * last sched_yield() return and this spinlock_acquire(). */
    current->waiting_for_child = false;
    struct thread *owner = (options & __WNOTHREAD) ? current : global_thread_list;
    while (owner) {
      if (owner->tgid != current->tgid) {
        owner = owner->global_next;
        continue;
      }

      struct thread *t = owner->children;
      while (t) {
        bool matches = false;
        if (target_pid == -1) {
          matches = true;
        } else if (target_pid > 0) {
          if (pid_namespace_tid(current, t) == (uint32_t)target_pid)
            matches = true;
        } else if (target_pid == 0) {
          if (t->pgid == current->pgid)
            matches = true;
        } else { // target_pid < -1
          if (pid_namespace_visible_locked(current, t->pgid) == (uint32_t)(-target_pid))
            matches = true;
        }

        if (matches) {
          has_matching_children = true;
          if (t->state == THREAD_ZOMBIE) {
            zombie = t;
            zombie_owner = owner;

            // Unlink from its owning thread list while holding the lock.
            if (zombie_owner->children == zombie) {
              zombie_owner->children = zombie->sibling_next;
            } else {
              struct thread *p = zombie_owner->children;
              while (p && p->sibling_next != zombie)
                p = p->sibling_next;
              if (p)
                p->sibling_next = zombie->sibling_next;
            }
            break;
          }
        }
        t = t->sibling_next;
      }

      if (zombie || (options & __WNOTHREAD))
        break;
      owner = owner->global_next;
    }
    /* Publish BLOCKED while child exit is excluded from publishing ZOMBIE.
     * Keep waiting_for_child set until we re-check under the lock so we
     * cannot miss a wakeup that fires between sched_yield() returning and
     * the next spinlock_acquire at the top of the loop. */
    if (!zombie && has_matching_children && !(options & WNOHANG)) {
      current->waiting_for_child = true;
      current->state = THREAD_BLOCKED;
      should_block = true;
    }
    spinlock_release(&tid_lock);

    if (zombie) {
      if (wstatus_ptr != 0) {
        int *wstatus = (int *)wstatus_ptr;
        *wstatus = zombie->termination_signal ? zombie->termination_signal :
                   (zombie->exit_status & 0xFF) << 8;
      }
      uint32_t reaped_pid = pid_namespace_tid(current, zombie);

      // Destruction must wait until the exiting child has switched off its
      // kernel stack.  On SMP the awakened parent can run concurrently with
      // process_do_exit(), so direct reaping here would free a live stack.
      TSC_BEGIN(wait4_reap);
      sched_queue_reap_and_wait(zombie);
      TSC_END(wait4_reap);

      current->waiting_for_child = false;
      return (uint64_t)reaped_pid;
    }

    if (!has_matching_children) {
      current->waiting_for_child = false;
      return (uint64_t)-10; // ECHILD
    }

    // WNOHANG: return 0 immediately if no zombie found
    if (options & WNOHANG) {
      current->waiting_for_child = false;
      return 0;
    }

    // Block and wait for a child to exit.
    // Note: waiting_for_child stays true across sched_yield() so that a
    // wakeup arriving while we are off-CPU is not lost.  It is cleared only
    // at the top of the next iteration once we hold tid_lock again and can
    // safely inspect the child list.
    if (should_block) {
      sched_yield();
      /* waiting_for_child is cleared at the top of the next loop iteration
       * after we reacquire tid_lock and rescan — do NOT clear it here. */
    }
  }
}

/* ── waitid (syscall 247) ──────────────────────────────────────────────────
 * int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options,
 * idtype values (POSIX / Linux):
 *   P_ALL   = 0  – wait for any child
 *   P_PID   = 1  – wait for specific pid
 *   P_PGID  = 2  – wait for any child in process group `id`
 *   P_PIDFD = 3  – wait for process referred to by pidfd
 *
 * We fill a minimal siginfo_t sufficient for glibc/musl to function.
 */
#define P_ALL   0
#define P_PID   1
#define P_PGID  2
#define P_PIDFD 3

#define WEXITED   4
#define WSTOPPED  2
#define WNOWAIT   0x01000000

/* siginfo_t layout – only the fields we need */
struct k_siginfo_child {
  int      si_signo;   /* always SIGCHLD = 17 */
  int      si_errno;
  int      si_code;    /* CLD_EXITED=1, CLD_KILLED=2 */
  int      _pad0;
  uint32_t si_pid;
  uint32_t si_uid;
  int      si_status;  /* exit code or signal */
  int      _pad1;
};
#define CLD_EXITED 1
#define CLD_KILLED 2

static uint64_t sys_waitid(uint64_t idtype_val, uint64_t id_val,
                           uint64_t infop_ptr, uint64_t options,
                           uint64_t rusage, uint64_t a5) {
  (void)rusage; (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int idtype = (int)idtype_val;
  uint32_t id = (uint32_t)id_val;
  uint32_t target_pid = id;

  if (idtype == P_PIDFD) {
    if (process_pidfd_target(current, id, &target_pid) < 0)
      return (uint64_t)-9; // EBADF
  }

  /* At least one of WEXITED/WSTOPPED must be requested */
  if (!(options & (WEXITED | WSTOPPED)))
    return (uint64_t)-22; // EINVAL

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;
    struct thread *zombie_owner = NULL;
    bool should_block = false;

    spinlock_acquire(&tid_lock);
    /* Clear here under the lock so a wakeup between yield-return and this
     * acquire is not missed — same pattern as sys_wait4. */
    current->waiting_for_child = false;

    /* Search every thread in this process's thread group for matching children */
    for (struct thread *owner = global_thread_list; owner;
         owner = owner->global_next) {
      if (owner->tgid != current->tgid)
        continue;

      for (struct thread *t = owner->children; t; t = t->sibling_next) {
        bool matches = false;
        switch (idtype) {
        case P_ALL:   matches = true; break;
        case P_PID:   matches = (pid_namespace_tid(current, t) == id); break;
        case P_PGID:  matches = (id ? pid_namespace_visible_locked(current, t->pgid) == id : t->pgid == current->pgid); break;
        case P_PIDFD: matches = (t->tid == target_pid || t->tgid == target_pid); break;
        default:
          spinlock_release(&tid_lock);
          return (uint64_t)-22; // EINVAL
        }

        if (!matches)
          continue;

        has_matching_children = true;

        if (t->state == THREAD_ZOMBIE) {
          zombie = t;
          zombie_owner = owner;

          /* Unlink from parent's children list while still holding tid_lock */
          if (!(options & WNOWAIT)) {
            if (zombie_owner->children == zombie) {
              zombie_owner->children = zombie->sibling_next;
            } else {
              struct thread *p = zombie_owner->children;
              while (p && p->sibling_next != zombie)
                p = p->sibling_next;
              if (p)
                p->sibling_next = zombie->sibling_next;
            }
          }
          break;
        }
      }
      if (zombie)
        break;
    }

    if (idtype == P_PIDFD && !has_matching_children) {
      /* P_PIDFD can wait on non-child processes */
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tid == target_pid || t->tgid == target_pid) {
          has_matching_children = true;
          if (t->state == THREAD_ZOMBIE || t->state == THREAD_DEAD) {
            zombie = t;
            zombie_owner = t->parent;
          }
          break;
        }
      }
    }

    if (!zombie && has_matching_children && !(options & WNOHANG)) {
      current->waiting_for_child = true;
      current->state = THREAD_BLOCKED;
      should_block = true;
    }

    spinlock_release(&tid_lock);

    if (zombie) {
      /* Fill in the siginfo_t if the caller supplied a buffer */
      if (infop_ptr) {
        struct k_siginfo_child *si = (struct k_siginfo_child *)infop_ptr;
        si->si_signo  = 17; /* SIGCHLD */
        si->si_errno  = 0;
        si->si_code   = zombie->termination_signal ? 2 /* CLD_KILLED */ : CLD_EXITED;
        si->si_pid    = pid_namespace_tid(current, zombie);
        si->si_uid    = (uint32_t)zombie->uid;
        si->si_status = zombie->termination_signal ? zombie->termination_signal :
                        zombie->exit_status & 0xFF;
        si->_pad0     = 0;
        si->_pad1     = 0;
      }

      if (!(options & WNOWAIT) && zombie_owner && zombie_owner->tgid == current->tgid) {
        /* Consume the zombie – wait until child is truly off-CPU first */
        sched_queue_reap_and_wait(zombie);
      }

      current->waiting_for_child = false;
      return 0; /* waitid returns 0 on success, not the pid */
    }

    if (!has_matching_children) {
      current->waiting_for_child = false;
      return (uint64_t)-10; // ECHILD
    }

    if (options & WNOHANG) {
      /* No zombie yet; zero out siginfo_t per POSIX/Linux specification */
      if (infop_ptr) {
        struct k_siginfo_child *si = (struct k_siginfo_child *)infop_ptr;
        memset(si, 0, sizeof(*si));
      }
      current->waiting_for_child = false;
      return 0;
    }

    if (should_block) {
      /* A pidfd can name a process this one is not the parent of, so the
       * parent-only wake above cannot be relied on here: queue on the shared
       * pidfd wake-up, and keep a bounded fallback timer so a missed wake can
       * never park the caller forever - the loop re-evaluates every round. */
      wait_queue_entry_t pidfd_entry;
      pidfd_entry.thread = current;
      pidfd_entry.next = NULL;
      pidfd_entry.wq = NULL;
      pidfd_entry.thread_next = NULL;
      pidfd_entry.thread_prev = NULL;
      bool on_pidfd_wq = false;

      if (idtype == P_PIDFD) {
        current->state = THREAD_BLOCKED;
        wait_queue_add(process_pidfd_wait_queue(), &pidfd_entry);
        on_pidfd_wq = true;
        current->wakeup_ticks = lapic_timer_get_ticks() + 10;
      }

      sched_yield();

      if (on_pidfd_wq) {
        wait_queue_remove(process_pidfd_wait_queue(), &pidfd_entry);
        current->wakeup_ticks = 0;
        if (current->state == THREAD_BLOCKED)
          current->state = THREAD_RUNNING;
      }
      /* waiting_for_child stays set — cleared at top of next iteration
       * after we reacquire tid_lock, same as sys_wait4. */
    }
  }
}


void syscall_register_process_wait(void) {
  syscall_register(SYS_WAIT4, sys_wait4);
  syscall_register(SYS_WAITID, sys_waitid);
}
