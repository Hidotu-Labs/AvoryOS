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

extern void fork_return_to_userspace(struct syscall_regs *regs)
    __attribute__((noreturn));

#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

static int process_namespace_inherit(struct thread *child, struct thread *parent,
                                      struct mount_namespace *mount,
                                      struct pid_namespace *pid, uint64_t flags) {
  int err = pid_namespace_inherit(child, parent, pid, (flags & CLONE_THREAD) != 0);
  if (err) return err;
  net_namespace_inherit(child, parent);
  vfs_namespace_clone(child, parent, false);
  if (mount) vfs_namespace_install(child, mount);
  if (child->pid_ns && pid_namespace_tid(child, child) == 1)
    child->pgid = child->sid = child->tid;
  return 0;
}

/* Called before publishing a new userspace child, for fork/clone/vfork alike. */
static void process_inherit_scheduler(struct thread *child,
                                       const struct thread *parent) {
  int nice = parent->nice_value;
  uint8_t policy = parent->sched_policy;
  uint8_t rt_priority = parent->sched_rt_priority;
  if (parent->sched_reset_on_fork) {
    if (policy == 1 || policy == 2) {
      policy = 0;
      rt_priority = 0;
    }
    if (nice < 0)
      nice = 0;
  }
  child->priority = process_nice_to_sched_priority(nice);
  child->static_priority = child->priority;
  child->nice_value = (int8_t)nice;
  child->sched_policy = policy;
  child->sched_rt_priority = rt_priority;
  child->sched_reset_on_fork = false;
  child->se.custom_slice = false;
  eevfd_set_nice(&child->se, nice);
  if (parent->se.custom_slice && policy == parent->sched_policy)
    eevfd_set_slice(&child->se, parent->se.slice_ns);
}

/* Real-vfork rendezvous release (see struct thread::vfork_done). The child's
 * exec success and _exit paths call this; sched_reap_thread() calls it as a
 * backstop so a child killed any other way (SIGKILL via terminate) cannot
 * leave the parent blocked forever. The flag is set before the wakeup and the
 * child's VFORK mark cleared, so repeats and races are harmless. */
void vfork_release_parent(struct thread *child) {
  if (!child || !(child->clone_flags & CLONE_VFORK))
    return;
  struct thread *parent = child->parent;
  if (parent) {
    /* Pair completion with the waiter's check-and-block under its runqueue
     * lock. Publishing the flag and inspecting state without that handshake
     * can miss a parent that is just about to sleep. Started threads remain
     * on their owning CPU (sched_wakeup uses the same queue_lock). */
    struct cpu_info *cpu = cpu_get_info(parent->cpu_index);
    spinlock_acquire(&cpu->queue_lock);
    parent->vfork_release_tsc = rdtsc_fence();
    __atomic_store_n(&parent->vfork_done, true, __ATOMIC_RELEASE);
    bool wake = parent->state == THREAD_BLOCKED;
    spinlock_release(&cpu->queue_lock);
    ptrace_vfork_done(parent, child->tid);
    /* sched_wakeup takes queue_lock itself; never call it while holding it. */
    if (wake) {
      sched_wakeup(parent);
      /* The same-CPU child should give the waiting launcher a scheduling
       * opportunity at syscall exit, even when normal wakeup fairness would
       * let the child keep its slice. No context switch while holding locks. */
      if (cpu == cpu_get_current() && cpu->current_thread == child)
        __atomic_store_n(&child->need_resched, true, __ATOMIC_RELEASE);
    }
  }
  child->clone_flags &= ~CLONE_VFORK;
}

static void vfork_wait_for_child(struct thread *parent) {
  TSC_BEGIN(vfork_wait);
  uint64_t released_at = 0;
  for (;;) {
    struct cpu_info *cpu = cpu_get_current();
    spinlock_acquire(&cpu->queue_lock);
    if (__atomic_load_n(&parent->vfork_done, __ATOMIC_ACQUIRE)) {
      released_at = parent->vfork_release_tsc;
      spinlock_release(&cpu->queue_lock);
      break;
    }
    parent->state = THREAD_BLOCKED;
    spinlock_release(&cpu->queue_lock);
    /* A completion after unlock makes us runnable before yield, or wakes us
     * after the switch. Spurious signal wakeups simply repeat the handshake. */
    sched_yield();
  }
  parent->vfork_child = 0;
  uint64_t resumed_at = rdtsc_fence();
  TSC_END(vfork_wait);
  /* Separate scheduling/wakeup latency from time spent waiting for the
   * child to finish. Clamp defensively if cross-CPU TSCs are not aligned. */
  if (released_at && resumed_at >= released_at) {
    int slot = tsc_probe_register("vfork_resume");
    tsc_probe_record(slot, resumed_at - released_at);
  }

  /* Borrowing children skip their exec dump; show completed rendezvous
   * samples from the parent. This is elapsed time, not CPU time. */
  static uint32_t waits;
  uint32_t n = __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
  if (n <= 4 || n == 16 || n == 64)
    tsc_probe_dump();
}
// Fork child kernel thread entry point
// This function runs as a kernel thread.  When scheduled, it switches to the
// child's page table and sysrets to the user-space address where the parent
// called fork().  RAX will be 0 (child return value).
static void fork_child_entry(void) {
  struct thread *self = sched_get_current();
  struct syscall_regs *child_regs = (struct syscall_regs *)self->fork_ctx;

  // Switch to the child's cloned address space if not already loaded by sched_schedule
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & CR3_ADDR_MASK) != (self->cr3 & CR3_ADDR_MASK)) {
    cpu_set_active_cr3(self->cr3);
    __asm__ volatile("mov %0, %%cr3" ::"r"(self->cr3) : "memory");
  }

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Restore user TLS bases — child inherits parent's FS_BASE (musl needs TLS)
  wrmsr(IA32_KERNEL_GS_BASE, self->gs_base);
  wrmsr(IA32_FS_BASE, self->fs_base);

  if (self->child_set_tid) {
    uint32_t visible_tid = pid_namespace_tid(self, self);
    copy_to_user((void *)self->child_set_tid, &visible_tid, sizeof(visible_tid));
    self->child_set_tid = 0;
  }

  /* Traced children must hit their initial SIGSTOP before the first
   * userspace instruction. This also drains any synchronous signal state
   * prepared while the child was unpublished. */
  signal_deliver_syscall(child_regs);

  // Jump to userspace — this never returns
  fork_return_to_userspace(child_regs);
}

/* Fork is on the critical path of every shell command (bash does fork+exec).
 * Print the cumulative probe table once a representative number of forks has
 * happened so the clone cost is visible alongside the exec sections. */
static void fork_probe_maybe_dump(void) {
  static uint32_t forks;
  uint32_t n = __atomic_add_fetch(&forks, 1, __ATOMIC_RELAXED);
  if (n == 16 || n == 64)
    tsc_probe_dump();
  /* Zero-pool hit rate next to the TSC table: boot is pool-cold by
   * construction (no idle time during the exec storm), steady state should
   * show hits climbing once idle refills. Every 64th fork, additive. */
  if ((n % 64) == 0) {
    uint64_t hits = 0, misses = 0, refilled = 0;
    size_t cached = 0;
    pmm_zero_pool_stats(&hits, &misses, &refilled, &cached);
    klog_puts("[PMM] zero-pool hits=");
    klog_uint64(hits);
    klog_puts(" misses=");
    klog_uint64(misses);
    klog_puts(" refilled=");
    klog_uint64(refilled);
    klog_puts(" cached=");
    klog_uint64((uint64_t)cached);
    klog_puts("\n");
  }
}

// sys_fork (raw handler — receives full register frame)
uint64_t sys_fork(struct syscall_regs *regs) {
  // 1. Get current parent state
  uint64_t *parent_pml4_phys = vmm_get_active_pml4();
  struct thread *parent = sched_get_current();
  if (!regs || !parent || !parent->mm)
    return (uint64_t)-22;

  // 2. Clone the user address space with VMA awareness
  //    Shared mappings share physical pages, private mappings get copied
  TSC_BEGIN(fork_clone);
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(
      parent_pml4_phys, &parent->mm->vmas);
  TSC_END(fork_clone);
  fork_probe_maybe_dump();
  /* Never publish a child that would enter fork_child_entry with CR3 == 0.
   * The VMM clone returns zero when it cannot allocate the child page tables. */
  if (!child_cr3)
    return (uint64_t)-12;

  // 3. Allocate and populate the child's saved register state.
  //    RAX = 0 so the child sees fork() returning 0.
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)(-12);
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  /* Prepare every fallible process resource before publishing a thread. */
  struct mm_struct *child_mm = kmalloc(sizeof(*child_mm));
  if (!child_mm) {
    kfree(child_regs);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)-12;
  }
  memset(child_mm, 0, sizeof(*child_mm));
  vma_list_init(&child_mm->vmas);
  TSC_BEGIN(fork_vma);
  if (!vma_list_clone(&child_mm->vmas, &parent->mm->vmas)) {
    kfree(child_mm);
    kfree(child_regs);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)-12;
  }
  TSC_END(fork_vma);
  child_mm->brk_base = parent->mm->brk_base;
  child_mm->brk_current = parent->mm->brk_current;
  child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
    child_mm->compat32 = parent->mm->compat32;
  child_mm->ref_count = 1;
  child_mm->pcid = pcid_alloc();
  spinlock_init(&child_mm->lock);
  memcpy(child_mm->saved_auxv, parent->mm->saved_auxv,
         sizeof(child_mm->saved_auxv));
  child_mm->auxv_count = parent->mm->auxv_count;
  memcpy(child_mm->saved_cmdline, parent->mm->saved_cmdline,
         sizeof(child_mm->saved_cmdline));
  child_mm->cmdline_len = parent->mm->cmdline_len;
  child_mm->arg_start = parent->mm->arg_start;
  child_mm->arg_end = parent->mm->arg_end;

  // 4. Create a kernel thread for the child process, not yet runnable.
  TSC_BEGIN(fork_thread);
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, NULL, false);
  if (!child) {
    kfree(child_regs);
    pcid_free(child_mm->pcid);
    vma_list_destroy(&child_mm->vmas);
    kfree(child_mm);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)(-12);
  }

  // Clear or free the default MM allocated by sched_create_kernel_thread
  // as we're about to replace it with a cloned one.
  if (child->mm) {
    pcid_free(child->mm->pcid);
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }
  child->mm = child_mm;
  TSC_END(fork_thread);

  // 5. Configure child thread
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->tgid = child->tid; // Fork creates a new process (new thread group)
  /* Linux implements fork() as a clone whose exit signal is SIGCHLD, and the
   * parent is notified through it.  Keep that visible here so the notification
   * does not depend on which entry point created the child. */
  child->clone_flags = SIGCHLD;

  // 6. Copy file descriptors from parent to child (with reference counting)
  TSC_BEGIN(fork_fds);
  if (parent) {
    sched_copy_files(child, parent);
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    // sched_create_kernel_thread() already inherited and referenced the
    // parent's CWD. Do not take a second, unmatched reference here.
    child->cwd_node = parent->cwd_node;
    memcpy(child->signal_handlers, parent->signal_handlers,
           sizeof(child->signal_handlers));
    child->fs_base = parent->fs_base;
    child->gs_base = parent->gs_base;
  memcpy(child->compat_tls, parent->compat_tls, sizeof(child->compat_tls));
  child->compat_fs = parent->compat_fs;
  child->compat_gs = parent->compat_gs;
    child->umask = parent->umask;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->supplementary_group_count = parent->supplementary_group_count;
    memcpy(child->supplementary_groups, parent->supplementary_groups,
           sizeof(child->supplementary_groups));
    child->ctty = parent->ctty; // Inherit controlling terminal

    // Inherit comm name — forked child keeps the parent's name until exec
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));

    // Inherit alternate signal stack
    child->ss_sp = parent->ss_sp;
    child->ss_size = parent->ss_size;
    child->ss_flags = parent->ss_flags;

#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
    child->trace_syscalls = parent->trace_syscalls;
#endif
  }
  TSC_END(fork_fds);

  // 8. Enqueue child only after its address space and inherited state are set.
  // Keep the return value independent of the child allocation in case it exits
  // quickly after being scheduled.
  int ns_error = process_namespace_inherit(child, parent, NULL, NULL, 0);
  if (ns_error) {
    while (!sched_reap_thread(child))
      sched_yield();
    return (uint64_t)ns_error;
  }
  uint32_t child_tid = pid_namespace_tid(parent, child);
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  ptrace_fork_event(regs, child, child_tid, SIGCHLD);
  sched_enqueue_thread(child, NULL);

  // 9. Return child PID to parent
  return child_tid;
}

static uint64_t sys_clone_internal(struct syscall_regs *regs, uint64_t flags,
                                   uint64_t child_stack, uint64_t ptid,
                                   uint64_t ctid, uint64_t newtls,
                                   uint64_t pidfd_slot) {
  struct thread *parent = sched_get_current();
  if (!parent || !parent->mm)
    return (uint64_t)-22;

  uint32_t compat_entry = 0, compat_base = 0;
  uint64_t compat_descriptor = 0;
  bool compat_tls = parent->seccomp_compat_dispatch && (flags & CLONE_SETTLS);
  if (compat_tls) {
    extern int compat_clone_tls(uint64_t, uint32_t *, uint32_t *, uint64_t *);
    int error = compat_clone_tls(newtls, &compat_entry, &compat_base, &compat_descriptor);
    if (error) return (uint64_t)(int64_t)error;
  }

  /* Linux clone flag dependencies. */
  if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
    return (uint64_t)-22;
  if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
    return (uint64_t)-22;
  /* Reject combined sandbox requests until every requested namespace exists.
   * In particular, accepting NEWNS with a global mount table is not safe. */
  if (flags & (CLONE_NEWCGROUP | CLONE_NEWUSER | CLONE_NEWTIME))
    return (uint64_t)-1; // EPERM
  if ((flags & CLONE_NEWIPC) && (flags & CLONE_SYSVSEM)) return (uint64_t)-22;
  if ((flags & CLONE_NEWPID) && (flags & (CLONE_THREAD | CLONE_PARENT))) return (uint64_t)-22;
  if ((flags & CLONE_NEWNS) && (flags & CLONE_FS)) return (uint64_t)-22;
  if ((flags & (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET)) &&
      !capability_has(parent, CAP_SYS_ADMIN))
    return (uint64_t)-1;
  struct uts_namespace *new_uts
      __attribute__((cleanup(uts_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWUTS) {
    new_uts = uts_namespace_copy(parent);
    if (!new_uts) return (uint64_t)-12;
  }
  struct ipc_namespace *new_ipc
      __attribute__((cleanup(ipc_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWIPC) {
    new_ipc = ipc_namespace_create();
    if (!new_ipc) return (uint64_t)-12;
  }
  struct mount_namespace *new_mount __attribute__((cleanup(process_mount_cleanup))) = NULL;
  struct pid_namespace *new_pid __attribute__((cleanup(pid_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNS) {
    new_mount = vfs_namespace_prepare(parent);
    if (!new_mount) return (uint64_t)-12;
  }
  if (flags & CLONE_NEWPID) {
    new_pid = pid_namespace_create(parent);
    if (!new_pid) return (uint64_t)-12;
  }
  struct net_namespace *new_net __attribute__((cleanup(net_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNET) {
    new_net = net_namespace_create();
    if (!new_net) return (uint64_t)-12;
  }
  if ((flags & CLONE_VM) && !(flags & CLONE_VFORK) && child_stack == 0)
    return (uint64_t)-22;
  if ((flags & CLONE_PARENT_SETTID) &&
      (!ptid || !vmm_is_user_addr_range_writable(ptid, sizeof(uint32_t))))
    return (uint64_t)-14;
  if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
      (!ctid || !vmm_is_user_addr_range_writable(ctid, sizeof(uint32_t))))
    return (uint64_t)-14;

  /* clone(CLONE_PIDFD) hands the descriptor back through the parent_tid slot
   * (clone3() has a field of its own for it).  Reserve it here, before the
   * child exists, because no allocation may fail once the child has been
   * created; it is installed only after the child copied the parent's
   * descriptor table, so the pidfd does not leak into the child. */
  int pidfd_fd = -1;
  vfs_node_t *pidfd_node = NULL;
  if (pidfd_slot) {
    /* Linux refuses a pidfd for a thread, and one slot cannot report both the
     * pidfd and the parent_tid. */
    if (flags & CLONE_THREAD)
      return (uint64_t)-22;
    if ((flags & CLONE_PARENT_SETTID) && pidfd_slot == ptid)
      return (uint64_t)-22;
    if (!vmm_is_user_addr_range_writable(pidfd_slot, sizeof(int)))
      return (uint64_t)-14;
    int64_t pr = process_pidfd_reserve(parent, &pidfd_fd, &pidfd_node);
    if (pr < 0)
      return (uint64_t)pr;
  }
#define PIDFD_UNRESERVE()                                                      \
  do {                                                                         \
    if (pidfd_fd >= 0) {                                                       \
      process_pidfd_abort(parent, pidfd_fd, pidfd_node);                               \
      pidfd_fd = -1;                                                           \
      pidfd_node = NULL;                                                       \
    }                                                                          \
  } while (0)

  struct syscall_regs *child_regs = kmalloc(sizeof(*child_regs));
  if (!child_regs) {
    PIDFD_UNRESERVE();
    return (uint64_t)-12;
  }
  memcpy(child_regs, regs, sizeof(*child_regs));
  child_regs->rax = 0;
  if (child_stack)
    child_regs->rsp = child_stack;

  uint64_t child_cr3 = parent->cr3;
  struct mm_struct *child_mm = parent->mm;
  bool vfork = (flags & CLONE_VFORK) != 0;
  /* CLONE_VFORK borrows the address space (Linux vfork semantics, same as
   * sys_vfork): the child shares mm/CR3 and the caller blocks until the child
   * execs or exits. Only the calling thread sleeps; sibling threads keep
   * running. The child must only call execve or _exit — the standard vfork
   * contract musl posix_spawn honors. exec() unshares via its shared_mm path;
   * _exit/kill release via vfork_release_parent() plus mm refcounting, and
   * the reaper NULLs survivor back-pointers so a reaped parent can't be
   * touched by a late release. VFORK-without-VM keeps the old private-CoW
   * clone as a fallback instead of growing a new EINVAL. */
  bool private_mm = !(flags & CLONE_VM);

  if (private_mm) {
    TSC_BEGIN(fork_clone);
    child_cr3 = vmm_clone_user_mappings_vma(
        (uint64_t *)parent->cr3, &parent->mm->vmas);
    TSC_END(fork_clone);
    if (!child_cr3) {
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }

    child_mm = kmalloc(sizeof(*child_mm));
    if (!child_mm) {
      vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }
    memset(child_mm, 0, sizeof(*child_mm));
    vma_list_init(&child_mm->vmas);
    if (!vma_list_clone(&child_mm->vmas, &parent->mm->vmas)) {
      kfree(child_mm);
      vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }
    child_mm->brk_base = parent->mm->brk_base;
    child_mm->brk_current = parent->mm->brk_current;
    child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
    child_mm->compat32 = parent->mm->compat32;
    child_mm->ref_count = 1;
    child_mm->pcid = pcid_alloc();
    spinlock_init(&child_mm->lock);
  }

  struct cpu_info *target_cpu = vfork ? cpu_get_current() : NULL;
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, target_cpu, false);
  if (!child) {
    PIDFD_UNRESERVE();
    if (private_mm) {
      vmm_free_user_pages_vma(child_cr3, &child_mm->vmas);
      if (child_mm->pcid) {
        pcid_free(child_mm->pcid);
        child_mm->pcid = 0;
      }
      vma_list_destroy(&child_mm->vmas);
      kfree(child_mm);
    }
    kfree(child_regs);
    return (uint64_t)-12;
  }

  /* Replace the generic kernel-thread MM only after every clone-owned
   * allocation has succeeded. From this point initialization cannot fail. */
  if (child->mm) {
    if (child->mm->pcid) {
      pcid_free(child->mm->pcid);
      child->mm->pcid = 0;
    }
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }

  if (!private_mm)
    __atomic_add_fetch(&child_mm->ref_count, 1, __ATOMIC_ACQ_REL);

  child->cr3 = child_cr3;
  child->mm = child_mm;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->clone_flags = flags;
  if (new_ipc) {
    ipc_namespace_install(child, new_ipc);
    new_ipc = NULL;
  }
  if (new_uts) {
    uts_namespace_install(child, new_uts);
    new_uts = NULL;
  }
  child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->tid;
  child->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;
  child->gs_base = parent->gs_base;
  memcpy(child->compat_tls, parent->compat_tls, sizeof(child->compat_tls));
  child->compat_fs = parent->compat_fs;
  child->compat_gs = parent->compat_gs;
  if (compat_tls) {
    child->compat_tls[compat_entry - 6] = compat_descriptor;
    child->compat_gs = (compat_entry << 3) | 3;
    child->gs_base = compat_base;
    child->fs_base = parent->fs_base;
  }
  child->tid_address =
      (flags & CLONE_CHILD_CLEARTID) ? (uint64_t *)ctid : NULL;
  child->child_set_tid = (flags & CLONE_CHILD_SETTID) ? ctid : 0;
  // A new task starts from the parent's robust list, exactly like Linux; a
  // fresh thread installs its own through set_robust_list(2).
  child->robust_list = parent->robust_list;
  // rseq follows Linux copy_process()/rseq_fork(): the child's registration
  // is always cleared, even for CLONE_THREAD. Each thread owns a struct rseq
  // in its own TLS at a different virtual address; inheriting the parent's
  // pointer makes the child's subsequent rseq(2) registration of its own TLS
  // area fail with EBUSY, which glibc treats as fatal ("rseq registration
  // failed") and aborts (observed as Xorg + all threaded glibc programs
  // dying with SIGABRT). The child re-registers on thread startup.
  child->rseq_addr = 0;
  child->rseq_len = 0;
  child->rseq_sig = 0;
  child->rseq_last_cpu_id = 0xFFFFFFFFu;

  if (flags & CLONE_FILES) {
    sched_share_files(child, parent);
  } else {
    sched_copy_files(child, parent);
  }

  memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  child->uid = parent->uid;
  child->gid = parent->gid;
  child->euid = parent->euid;
  child->egid = parent->egid;
  child->suid = parent->suid;
  child->sgid = parent->sgid;
  child->fsuid = parent->fsuid;
  child->fsgid = parent->fsgid;
  child->supplementary_group_count = parent->supplementary_group_count;
  memcpy(child->supplementary_groups, parent->supplementary_groups,
         sizeof(child->supplementary_groups));
  child->pgid = parent->pgid;
  memcpy(child->signal_handlers, parent->signal_handlers,
         sizeof(child->signal_handlers));
  if ((flags & CLONE_VM) && !(flags & CLONE_VFORK)) {
    child->ss_sp = 0;
    child->ss_size = 0;
    child->ss_flags = SS_DISABLE;
  } else {
    child->ss_sp = parent->ss_sp;
    child->ss_size = parent->ss_size;
    child->ss_flags = parent->ss_flags;
  }
  memcpy(child->comm, parent->comm, sizeof(child->comm));
  memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
  child->trace_syscalls = parent->trace_syscalls;
#endif

  int ns_error = process_namespace_inherit(child, parent, new_mount, new_pid, flags);
  if (ns_error) {
    PIDFD_UNRESERVE();
    while (!sched_reap_thread(child))
      sched_yield();
    return (uint64_t)ns_error;
  }
  new_mount = NULL;
  if (new_net) { net_namespace_install(child, new_net); new_net = NULL; }
  uint32_t visible_child_tid = pid_namespace_tid(parent, child);
  if (flags & CLONE_PARENT_SETTID)
    __atomic_store_n((uint32_t *)ptid, visible_child_tid, __ATOMIC_RELEASE);
  if ((flags & CLONE_CHILD_SETTID) && (flags & CLONE_VM))
    __atomic_store_n((uint32_t *)ctid, pid_namespace_tid(child, child), __ATOMIC_RELEASE);
  if (pidfd_fd >= 0) {
    /* The child has copied the descriptor table by now, so the pidfd is only
     * the parent's.  Publish it before the child runs: the whole point of a
     * pidfd is to be able to watch a process that may already be gone. */
    process_pidfd_publish(parent, pidfd_fd, pidfd_node, child->tid, 0);
    int pidfd_user = pidfd_fd;
    copy_to_user((void *)pidfd_slot, &pidfd_user, sizeof(int));
  }
  /* Runqueue publication is the final creation step. */
  /* Snapshot the result before making the child runnable. */
  uint32_t child_tid = child->tid;
  if (vfork) {
    /* Publish the rendezvous before the child is runnable: like sys_vfork's
     * child, it may exec/exit on another CPU immediately after enqueue and
     * must observe valid state. Release comes from the child's exec/_exit
     * (or the reap backstop for kills). */
    parent->vfork_child = child_tid;
    parent->vfork_release_tsc = 0;
    __atomic_store_n(&parent->vfork_done, false, __ATOMIC_RELEASE);
  }
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  ptrace_fork_event(regs, child, visible_child_tid, flags);
  sched_enqueue_thread(child, target_cpu);

  if (vfork) {
    vfork_wait_for_child(parent);
  }

  return visible_child_tid;
#undef PIDFD_UNRESERVE
}
// sys_clone (syscall 56)
uint64_t sys_clone(struct syscall_regs *regs) {
  uint64_t flags = regs->rdi;
  uint64_t child_stack = regs->rsi;
  uint64_t ptid = regs->rdx;
  uint64_t ctid = regs->r10;
  uint64_t newtls = regs->r8;

  klog_debugf("[CLONE] flags=%llu stack=0x%llx\n",
              (unsigned long long)flags, (unsigned long long)child_stack);

  /* Legacy clone() has no pidfd field: CLONE_PIDFD reports the descriptor
   * through the parent_tid argument. */
  uint64_t pidfd_slot = (flags & CLONE_PIDFD) ? ptid : 0;

  return sys_clone_internal(regs, flags, child_stack, ptid, ctid, newtls,
                            pidfd_slot);
}

// sys_vfork (syscall 58) — real vfork semantics, shared with
// sys_clone_internal()'s CLONE_VFORK path (same borrow + rendezvous).
//
// The child borrows the parent's address space (same mm, same CR3, refcounted)
// and the calling thread blocks until the child execs or exits. Only the
// calling thread sleeps; sibling threads keep running, matching Linux. The
// child must only call execve or _exit (never return or touch the stack) —
// the standard vfork contract, which musl/glibc posix_spawn honor.
//
uint64_t sys_vfork(struct syscall_regs *regs) {
  struct thread *parent = sched_get_current();
  if (!regs || !parent || !parent->mm)
    return (uint64_t)-22;

  // Child register state: returns 0 in the child, same user stack as parent.
  struct syscall_regs *child_regs = kmalloc(sizeof(*child_regs));
  if (!child_regs)
    return (uint64_t)-12;
  memcpy(child_regs, regs, sizeof(*child_regs));
  child_regs->rax = 0;

  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, NULL, false);
  if (!child) {
    kfree(child_regs);
    return (uint64_t)-12;
  }

  // Drop the placeholder MM and share the parent's address space instead of
  // cloning page tables / VMAs (the whole point of vfork).
  if (child->mm) {
    if (child->mm->pcid)
      pcid_free(child->mm->pcid);
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }
  spinlock_acquire(&parent->mm->lock);
  parent->mm->ref_count++;
  spinlock_release(&parent->mm->lock);
  child->mm = parent->mm;
  child->cr3 = parent->cr3;

  // Configure the child exactly like sys_fork (own fd-table copy, inherited
  // credentials/handlers/name), so the only behavioral delta is the shared mm.
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->tgid = child->tid;
  child->clone_flags = CLONE_VFORK | CLONE_VM | SIGCHLD;
  sched_copy_files(child, parent);
  memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  // sched_create_kernel_thread() already inherited and referenced the
  // parent's CWD. Do not take a second, unmatched reference here.
  child->cwd_node = parent->cwd_node;
  memcpy(child->signal_handlers, parent->signal_handlers,
         sizeof(child->signal_handlers));
  child->fs_base = parent->fs_base;
  child->gs_base = parent->gs_base;
  memcpy(child->compat_tls, parent->compat_tls, sizeof(child->compat_tls));
  child->compat_fs = parent->compat_fs;
  child->compat_gs = parent->compat_gs;
  child->umask = parent->umask;
  child->uid = parent->uid;
  child->gid = parent->gid;
  child->euid = parent->euid;
  child->egid = parent->egid;
  child->suid = parent->suid;
  child->sgid = parent->sgid;
  child->fsuid = parent->fsuid;
  child->fsgid = parent->fsgid;
  child->supplementary_group_count = parent->supplementary_group_count;
  memcpy(child->supplementary_groups, parent->supplementary_groups,
         sizeof(child->supplementary_groups));
  child->ctty = parent->ctty;
  memcpy(child->comm, parent->comm, sizeof(child->comm));
  memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
  child->ss_sp = parent->ss_sp;
  child->ss_size = parent->ss_size;
  child->ss_flags = parent->ss_flags;
#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
  child->trace_syscalls = parent->trace_syscalls;
#endif

  int ns_error = process_namespace_inherit(child, parent, NULL, NULL, 0);
  if (ns_error) {
    while (!sched_reap_thread(child))
      sched_yield();
    return (uint64_t)ns_error;
  }
  uint32_t visible_child_tid = pid_namespace_tid(parent, child);
  uint32_t child_tid = child->tid;
  // Publish the rendezvous before the child is runnable: it may exec/exit
  // on another CPU immediately after enqueue and must observe valid state.
  parent->vfork_child = child_tid;
  parent->vfork_release_tsc = 0;
  __atomic_store_n(&parent->vfork_done, false, __ATOMIC_RELEASE);
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  ptrace_fork_event(regs, child, visible_child_tid, child->clone_flags);
  sched_enqueue_thread(child, NULL);

  vfork_wait_for_child(parent);
  return visible_child_tid;
}

struct clone_args {
  uint64_t flags;
  uint64_t pidfd;
  uint64_t child_tid;
  uint64_t parent_tid;
  uint64_t exit_signal;
  uint64_t stack;
  uint64_t stack_size;
  uint64_t tls;
  uint64_t set_tid;
  uint64_t set_tid_size;
  uint64_t cgroup;
};

// sys_clone3 (syscall 435)
uint64_t sys_clone3(struct syscall_regs *regs) {
  uint64_t uargs_ptr = regs->rdi;
  size_t size = (size_t)regs->rsi;

  if (size < 64 || !uargs_ptr)
    return (uint64_t)-22; // EINVAL

  if (!is_user_ptr(uargs_ptr) || !vmm_is_user_addr_range_valid(uargs_ptr, size))
    return (uint64_t)-14; // EFAULT

  struct clone_args kargs;
  memset(&kargs, 0, sizeof(kargs));
  size_t copy_len = size < sizeof(kargs) ? size : sizeof(kargs);
  if (copy_from_user(&kargs, (void *)uargs_ptr, copy_len) != 0)
    return (uint64_t)-14;

  uint64_t flags = kargs.flags | (kargs.exit_signal & 0xff);
  uint64_t child_stack = 0;
  if (kargs.stack) {
    child_stack = kargs.stack + kargs.stack_size;
  }

  uint64_t ptid = kargs.parent_tid;
  uint64_t ctid = kargs.child_tid;
  uint64_t newtls = kargs.tls;

  klog_debugf("[CLONE3] flags=0x%llx stack=0x%llx size=0x%llx ptid=0x%llx ctid=0x%llx tls=0x%llx\n",
              (unsigned long long)flags, (unsigned long long)kargs.stack,
              (unsigned long long)kargs.stack_size, (unsigned long long)ptid,
              (unsigned long long)ctid, (unsigned long long)newtls);

  uint64_t ret = sys_clone_internal(regs, flags, child_stack, ptid, ctid, newtls,
                                    (kargs.flags & CLONE_PIDFD) ? kargs.pidfd : 0);
  return ret;
}


void syscall_register_process_clone(void) {
  syscall_register_raw(SYS_FORK, sys_fork);
  syscall_register_raw(SYS_VFORK, sys_vfork);
  syscall_register_raw(SYS_CLONE, sys_clone);
  syscall_register_raw(SYS_CLONE3, sys_clone3);
}
