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
#include "sys_ptrace.h"
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
extern void signal_notify_parent_exit(struct thread *child);
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count);
static void robust_handle_death(struct thread *t);
void process_do_exit(uint64_t status) __attribute__((noreturn));

static void process_clear_child_tid(struct thread *current) {
  uint32_t *tidptr = (uint32_t *)current->tid_address;
  current->tid_address = NULL;
  if (!tidptr)
    return;
  uint32_t zero = 0;
  /* Exit also runs from a user exception, where the syscall's coarse SMAP
   * window is absent. The address may have been unmapped since registration;
   * a guarded user copy handles both that race and demand/CoW faults. */
  if (vmm_is_user_addr_range_writable((uint64_t)tidptr, sizeof(zero)) &&
      copy_to_user(tidptr, &zero, sizeof(zero)) == 0)
    futex_wake_user(tidptr, 1);
}

void process_do_exit(uint64_t status) {
  /* Fatal user exceptions enter with IF clear. Cleanup may wait for I/O or
   * IPIs and must run with the same interrupt state as ordinary sys_exit. */
  hal_irq_enable();
  struct thread *current = sched_get_current();
  if (!current) {
    while (1)
      sched_yield();
  }
  if (current->state == THREAD_DEAD || current->state == THREAD_ZOMBIE) {
    while (1) {
      sched_yield();
    }
  }
  ptrace_tracer_exit(current);
  current->exit_status = (int)status;
  ptrace_exit_event(current, (int)status);
  // Robust mutexes held by this thread must be marked OWNER_DIED and woken
  // while the address space is still live; otherwise the next locker spins
  // into musl's a_crash (HLT) on impossible owner state.
  if (current && current->robust_list)
    robust_handle_death(current);
  process_clear_child_tid(current);

  if (current) {
    pid_namespace_exit(current);
    sched_reparent_children(current);
  }

  if (current && !current->is_main_session) {
    klog_debug_puts("\n[SYSCALL] Process exited with status: ");
    klog_debug_uint64(status);
    klog_debug_puts("\n");
  }

  klog_debug_puts("[EXITDBG] begin cleanup\n");

  // Common cleanup. CLONE_FILES tables remain alive until the final thread
  // drops its reference; closing every fd on each pthread exit would break
  // descriptors still in use by its siblings.
  TSC_BEGIN(exit_files);
  if (current) {
    sched_release_files(current);
    klog_debug_puts("[EXITDBG] files released\n");

    if (current->cwd_node) {
      vfs_close(current->cwd_node);
      current->cwd_node = NULL;
    }
    klog_debug_puts("[EXITDBG] cwd released\n");
  }
  TSC_END(exit_files);

  if (current && current->is_forked_child) {
    TSC_BEGIN(exit_publish);
    current->exit_status = (int)status;

    struct thread *parent_to_wake = NULL;

    // Thread-group members are not waitable children. Keeping them as
    // zombies retains their large struct thread and kernel stack forever.
    // Arrange for the scheduler to reclaim them after this context switches
    // away; ordinary fork/clone processes remain zombies for wait4().
    if ((current->clone_flags & CLONE_THREAD) && !current->tracer_tid) {
      spinlock_acquire(&tid_lock);
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t != current && t->tgid == current->tgid && t->tid == current->tgid) {
          t->exit_status = (int)status;
          break;
        }
      }
      spinlock_release(&tid_lock);
      current->state = THREAD_DEAD;
      pidfd_wake_waiters();
      sched_queue_reap(current);
    } else {
      /* Serialize zombie publication with wait4's transition to BLOCKED. */
      klog_debug_puts("[EXITDBG] publishing zombie\n");
      spinlock_acquire(&tid_lock);
      struct thread *parent = current->parent;
      int exit_signal = (int)(current->clone_flags & 0xff);
      bool auto_reap = false;
      if (parent && exit_signal == SIGCHLD) {
        struct k_sigaction *chld_action =
            &parent->signal_handlers[SIGCHLD - 1];
        auto_reap = chld_action->sa_handler == (void *)SIG_IGN ||
                    (chld_action->sa_flags & SA_NOCLDWAIT) != 0;
        /* Keep default-disposition children waitable, including when the
         * parent is a GUI/daemon using pidfds, signalfd, or a later waitpid.
         * Process names and whether a wait syscall is currently blocked do
         * not establish that the parent has abandoned the exit status.
         * Only retain the existing PID-1 fallback for orphan collection. */
        bool parent_group_waiting = false;
        if (parent->waiting_for_child) {
          parent_group_waiting = true;
        } else {
          for (struct thread *w = global_thread_list; w; w = w->global_next) {
            if (w->tgid == parent->tgid && w->waiting_for_child) {
              parent_group_waiting = true;
              break;
            }
          }
        }
        if (!auto_reap &&
            (chld_action->sa_handler == (void *)SIG_DFL || chld_action->sa_handler == NULL) &&
            !parent_group_waiting) {
          if (parent->tgid == 1 || parent->tid == 1) {
            auto_reap = true;
          }
        }
      }
      /* A traced task must stay available to its tracer through the exit
       * wait. Its parent may reap it after the tracer consumes that status. */
      if (current->tracer_tid)
        auto_reap = false;
      current->state = auto_reap ? THREAD_DEAD : THREAD_ZOMBIE;
      if (current->parent) {
        uint32_t parent_tgid = current->parent->tgid;
        /* O(1) fast path: the single-threaded parent waiting in wait4() is
         * itself blocked with the flag set.  Checking it first avoids a full
         * global-list walk per exit (wake-one order was already arbitrary).
         * Anything else falls back to the scan below. */
        if (current->parent->tgid == parent_tgid &&
            current->parent->waiting_for_child &&
            current->parent->state == THREAD_BLOCKED) {
          parent_to_wake = current->parent;
        } else {
          for (struct thread *waiter = global_thread_list; waiter;
               waiter = waiter->global_next) {
            if (waiter->tgid == parent_tgid && waiter->waiting_for_child &&
                waiter->state == THREAD_BLOCKED) {
              parent_to_wake = waiter;
              break;
            }
          }
        }
      }
      if (auto_reap && parent) {
        /* No waitable child should remain when SIGCHLD is explicitly ignored
         * or SA_NOCLDWAIT is set. Unlink it before waking waiters so their
         * rescan observes ECHILD instead of parking on a zombie that will be
         * removed asynchronously by the reaper. Keep parent until notification
         * completes; the reaper clears the back-pointer before freeing. */
        if (parent->children == current) {
          parent->children = current->sibling_next;
        } else {
          struct thread *sibling = parent->children;
          while (sibling && sibling->sibling_next != current)
            sibling = sibling->sibling_next;
          if (sibling)
            sibling->sibling_next = current->sibling_next;
        }
        current->sibling_next = NULL;
      }
      spinlock_release(&tid_lock);
      klog_debug_puts("[EXITDBG] exit state published\n");
      ptrace_tracee_exit(current);
      /* Anyone waiting on a pidfd for this process (poll or waitid(P_PIDFD))
       * is woken here to observe its terminal state. */
      pidfd_wake_waiters();
      /* ... and the parent's SIGCHLD handler runs here, the way Linux sends
       * do_notify_parent() right after publishing the zombie. */
      signal_notify_parent_exit(current);
      if (auto_reap)
        sched_queue_reap(current);
    }

    /* A vfork parent is blocked in sys_vfork(), before it can enter wait4().
     * Publish the zombie before releasing it so an immediate waitpid() can
     * observe and reap this child. */
    vfork_release_parent(current);

    if (parent_to_wake) {
      klog_debug_puts("[EXITDBG] waking parent\n");
      sched_wakeup(parent_to_wake);
      klog_debug_puts("[EXITDBG] parent wake returned\n");
    }

    // Reaping handles the address-space reference after this task is off-CPU.
    TSC_END(exit_publish);
    if (current->cr3) {
      struct cpu_info *cpu = cpu_get_current();
      cpu_set_active_cr3(cpu->kernel_cr3);
      __asm__ volatile("mov %0, %%cr3" :: "r"(cpu->kernel_cr3) : "memory");
    }

    // Sleep forever; the parent will reap us.
    klog_debug_puts("[EXITDBG] yielding zombie\n");
    while (1) {
      sched_yield();
    }
  }

  // Non-forked (main) process exit
  extern void restart_main_session(void);
  restart_main_session();
  while (1)
    ;
}

static uint64_t __attribute__((noreturn)) sys_exit(uint64_t status, uint64_t a1,
                                                   uint64_t a2, uint64_t a3,
                                                   uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit((int)status);
}

static uint64_t __attribute__((noreturn))
sys_exit_group(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3,
               uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_terminate_thread_group(sched_get_current());
  process_do_exit(status);
}

static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return 0;
  if (tidptr && !vmm_is_user_addr_range_writable(tidptr, sizeof(uint32_t)))
    return (uint64_t)-14;

  // Store the pointer to the user-space TID variable
  // This is used by musl/glibc for thread exit notification
  current->tid_address = (uint64_t *)tidptr;

  // Return the current thread's ID
  return pid_namespace_tid(current, current);
}

/* sizeof(struct robust_list_head) on x86-64: list.next, futex_offset and
 * list_op_pending.  Linux rejects every other length with EINVAL. */
#define ROBUST_LIST_HEAD_LEN 24
#define ROBUST_MAX_WALK 4096

static void robust_handle_futex_death(uint64_t futex_uaddr, uint32_t dying_tid) {
  if (!vmm_is_user_addr_range_writable(futex_uaddr, sizeof(uint32_t)))
    return;
  if (futex_uaddr & 3)
    return;
  uint32_t *uaddr = (uint32_t *)futex_uaddr;
  for (;;) {
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0)
      return;
    if ((cur & 0x3fffffff) != dying_tid)
      return; // Not owned by the dying thread.
    uint32_t newv = (cur & 0x80000000) | 0x40000000; // keep WAITERS, set OWNER_DIED
    uint32_t expected = cur;
    int exchanged = cmpxchg_user_u32(uaddr, &expected, newv);
    if (exchanged < 0)
      return;
    if (exchanged) {
      futex_wake_user(uaddr, 1);
      return;
    }
  }
}

static void robust_handle_death(struct thread *t) {
  if (!t)
    return;
  uint64_t head_addr = t->robust_list;
  if (!head_addr)
    return;
  t->robust_list = 0; // Prevent double handling on re-entry.
  // Verified musl ABI (pthread_impl.h: _m_lock=__vi[1] off 4,
  // _m_next=__p[4] off 32; pthread_create.c sets off=-28 and processes
  // private/detached lists in userspace, leaving pshared for the kernel).
  // Sanity range below still guards corrupted heads.
  if (!vmm_is_user_addr_range_valid(head_addr, ROBUST_LIST_HEAD_LEN))
    return;
  uint64_t head_next = 0;
  uint64_t futex_offset_u = 0;
  uint64_t pending = 0;
  if (copy_from_user(&head_next, (const void *)head_addr, 8) != 0)
    return;
  if (copy_from_user(&futex_offset_u, (const void *)(head_addr + 8), 8) != 0)
    return;
  if (copy_from_user(&pending, (const void *)(head_addr + 16), 8) != 0)
    return;
  int64_t futex_offset = (int64_t)futex_offset_u;
  // Corrupted heads (e.g. UAF) yield wild offsets; Linux offsets are tiny
  // (futex word inside the mutex). Refuse to walk instead of CASing heap.
  if (futex_offset < -32768 || futex_offset > 32768)
    return;
  uint32_t dying_tid = t->tid;
  // Lock operation in progress at death: handle it first, like Linux.
  if (pending && pending != head_addr &&
      vmm_is_user_addr_range_valid(pending, 8)) {
    uint64_t futex_uaddr = pending + (uint64_t)futex_offset;
    robust_handle_futex_death(futex_uaddr, dying_tid);
  }
  uint64_t entry = head_next;
  for (int i = 0; i < ROBUST_MAX_WALK && entry && entry != head_addr; i++) {
    if (!vmm_is_user_addr_range_valid(entry, 8))
      break;
    uint64_t next = 0;
    if (copy_from_user(&next, (const void *)entry, 8) != 0)
      break;
    uint64_t futex_uaddr = entry + (uint64_t)futex_offset;
    robust_handle_futex_death(futex_uaddr, dying_tid);
    entry = next;
  }
}

static uint64_t sys_set_robust_list(uint64_t head, uint64_t len, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-22; // EINVAL

  if (len != ROBUST_LIST_HEAD_LEN)
    return (uint64_t)-22; // EINVAL
  if (head && !vmm_is_user_addr_range_valid(head, len))
    return (uint64_t)-14; // EFAULT

  current->robust_list = head;
  return 0;
}

// get_robust_list(pid, &head, &len): read back the list registered with
// set_robust_list(2).  baloo_file, gdb and friends ask for it on startup;
// pid 0 refers to the calling thread.
static uint64_t sys_get_robust_list(uint64_t pid_val, uint64_t head_ptr,
                                    uint64_t len_ptr, uint64_t a3, uint64_t a4,
                                    uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-22; // EINVAL

  if (!head_ptr || !len_ptr ||
      !vmm_is_user_addr_range_writable(head_ptr, sizeof(uint64_t)) ||
      !vmm_is_user_addr_range_writable(len_ptr, sizeof(uint64_t)))
    return (uint64_t)-14; // EFAULT

  int32_t pid = (int32_t)pid_val;
  if (pid < 0)
    return (uint64_t)-22; // EINVAL

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  uint64_t head = 0;
  bool found = false;

  spinlock_acquire(&tid_lock);
  if (pid == 0) {
    head = current->robust_list;
    found = true;
  } else {
    // A tid, not a tgid: robust lists are per thread.
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (pid_namespace_tid(current, t) != (uint32_t)pid)
        continue;
      // Linux asks for ptrace_may_access(); same owner is the closest match.
      if (!capability_has(current, CAP_SYS_PTRACE) && t->uid != current->uid &&
          t->euid != current->euid)
        break;
      head = t->robust_list;
      found = true;
      break;
    }
  }
  spinlock_release(&tid_lock);

  if (!found)
    return (uint64_t)-3; // ESRCH

  uint64_t len = ROBUST_LIST_HEAD_LEN;
  copy_to_user((void *)head_ptr, &head, sizeof(head));
  copy_to_user((void *)len_ptr, &len, sizeof(len));
  return 0;
}

// ---------------------------------------------------------------------------

void syscall_register_process_exit(void) {
  syscall_register(SYS_EXIT, sys_exit);
  syscall_register(SYS_EXIT_GROUP, sys_exit_group);
  syscall_register(SYS_SET_TID_ADDRESS, sys_set_tid_address);
  syscall_register(SYS_SET_ROBUST_LIST, sys_set_robust_list);
  syscall_register(SYS_GET_ROBUST_LIST, sys_get_robust_list);
}
