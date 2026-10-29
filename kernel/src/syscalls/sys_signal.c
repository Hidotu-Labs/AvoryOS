#include "../sched/pid_namespace.h"
#include "../sched/capability.h"
// Signal Syscalls: rt_sigaction, rt_sigprocmask
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../cpu/fpu.h"
#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../include/arch/uaccess.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "../socket/epoll.h"
#include "sys_io_shared.h"
#include "sys_ptrace.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>


#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define X86_64_RED_ZONE_SIZE 128
#define UC_SIGCONTEXT_SS 0x2
#define UC_STRICT_RESTORE_SS 0x4

// Forward declarations
static bool on_sig_stack(struct thread *t, uint64_t sp);
#define SIG_SETMASK 2

struct kernel_siginfo {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  int32_t __pad0;
  union {
    struct {
      uint32_t si_pid;
      uint32_t si_uid;
    } _kill;
    struct {
      uint64_t si_addr;
    } _sigfault;
    struct {
      uint64_t si_call_addr;
      int32_t si_syscall;
      uint32_t si_arch;
    } _sigsys;
    uint8_t data[112];
  };
} __attribute__((packed));

/* Keep complete x86-64 siginfo payloads: crash rethrows carry positive fault
 * codes and addresses, while SI_QUEUE carries a sigval at union offset 8. */
struct queued_siginfo {
  struct queued_siginfo *next;
  struct kernel_siginfo info;
};
#define MAX_QUEUED_SIGINFO 64

void signal_release_queued_info(struct thread *t) {
  /* Reaper owns the unlinked task; no signal sender can find it anymore. */
  while (t->queued_siginfo) {
    struct queued_siginfo *entry = t->queued_siginfo;
    t->queued_siginfo = entry->next;
    kfree(entry);
  }
}

static bool signal_dequeue_locked(struct thread *current, uint64_t bit,
                                  struct kernel_siginfo *info);

struct kernel_sigaltstack {
  uint64_t ss_sp;
  int32_t ss_flags;
  uint32_t __pad;
  uint64_t ss_size;
};

struct kernel_mcontext {
  uint64_t gregs[23];
  uint64_t fpregs;
  uint64_t reserved[8];
};

struct kernel_ucontext {
  uint64_t uc_flags;
  uint64_t uc_link;
  struct kernel_sigaltstack uc_stack;
  struct kernel_mcontext uc_mcontext;
  uint64_t uc_sigmask[16];
  uint64_t fpregs_mem[64];
};

struct sigframe {
  struct registers regs;
  uint64_t mask;
  struct kernel_siginfo info;
  struct kernel_ucontext ucontext;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct kernel_siginfo) == 128,
               "x86_64 siginfo ABI size");
_Static_assert(offsetof(struct kernel_siginfo, _sigsys.si_call_addr) == 16 &&
               offsetof(struct kernel_siginfo, _sigsys.si_syscall) == 24 &&
               offsetof(struct kernel_siginfo, _sigsys.si_arch) == 28,
               "x86_64 SIGSYS siginfo ABI offsets");
_Static_assert(offsetof(struct sigframe, ucontext) == 312,
               "x86_64 signal frame ucontext offset");

static void restore_signal_context(struct registers *regs,
                                   const struct kernel_ucontext *uc) {
  const uint64_t *g = uc->uc_mcontext.gregs;

  regs->r8 = g[0];
  regs->r9 = g[1];
  regs->r10 = g[2];
  regs->r11 = g[3];
  regs->r12 = g[4];
  regs->r13 = g[5];
  regs->r14 = g[6];
  regs->r15 = g[7];
  regs->rdi = g[8];
  regs->rsi = g[9];
  regs->rbp = g[10];
  regs->rbx = g[11];
  regs->rdx = g[12];
  regs->rax = g[13];
  regs->rcx = g[14];
  regs->rsp = g[15];
  regs->rip = g[16];
  regs->rflags = g[17] | 0x200;

  uint16_t cs = (uint16_t)g[18];
  regs->cs = ((cs & 3) == 3 && cs != 0) ? cs : 0x2B;
  regs->err_code = g[19];
  regs->int_no = g[20];
  uint16_t ss = (uint16_t)(g[18] >> 48);
  /* Legacy contexts can leave SS unset. Strict contexts must retain an
   * invalid selector so sigreturn rejects it before attempting IRETQ. */
  regs->ss = (ss == 0x23 || (uc->uc_flags & UC_STRICT_RESTORE_SS) ||
              regs->cs != 0x2B) ? ss : 0x23;
}

static void fill_signal_context(struct sigframe *frame, int sig,
                                struct thread *current) {
  struct kernel_siginfo *info = &frame->info;
  struct kernel_ucontext *uc = &frame->ucontext;
  /* info and uc are already zero-initialized by the caller (staged_frame). */

  info->si_signo = sig;
  uint32_t sender_pid = pid_namespace_visible(current, current->signal_sender_pid[sig - 1]);
  if (sender_pid) {
    /* glibc's SIGSETXID handler requires SI_TKILL for tgkill/tkill;
     * reporting SI_USER makes it ignore the credential-change request
     * while the initiating thread waits forever for its acknowledgement. */
    info->si_code = current->signal_sender_code[sig - 1];
    info->_kill.si_pid = sender_pid;
    info->_kill.si_uid = 0;
  } else {
    info->si_code = 128; /* SI_KERNEL */
  }

  if (sig == SIGSEGV || sig == SIGBUS) {
    uint64_t fault_address = current->fault_addr;
    /* Linux reports SEGV_MAPERR iff no VMA covers the address, else
     * SEGV_ACCERR. The old P-bit test misclassifies PROT_NONE guards
     * (JVM yellow/red zones, pthread guard pages): they have no PTE
     * (P=0) but a VMA exists, so they must be ACCERR. JVM's stack-overflow
     * handler checks this and loops forever on MAPERR. */
    int code = (current->fault_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */;
    if (current->mm && spinlock_try_acquire(&current->mm->lock)) {
      struct vma *v = vma_find(&current->mm->vmas, fault_address);
      code = v ? 2 : 1;
      spinlock_release(&current->mm->lock);
    }
    info->si_code = code;
    info->_sigfault.si_addr = fault_address;
  } else if (sig == SIGILL || sig == SIGFPE) {
    uint64_t fault_address = frame->regs.rip;
    info->si_code = 1;
    info->_sigfault.si_addr = fault_address;
  } else if (sig == SIGSYS && current->seccomp_sigsys) {
    info->si_code = 1; /* SYS_SECCOMP */
    info->si_errno = current->seccomp_trap_data;
    info->_sigsys.si_call_addr = current->syscall_dispatch_call_addr;
    info->_sigsys.si_syscall = current->syscall_dispatch_number;
    info->_sigsys.si_arch = current->seccomp_trap_arch;
    current->seccomp_sigsys = false;
  } else if (sig == SIGSYS && current->syscall_dispatch_sigsys) {
    info->si_code = 2; /* SYS_USER_DISPATCH */
    info->_sigsys.si_call_addr = current->syscall_dispatch_call_addr;
    info->_sigsys.si_syscall = current->syscall_dispatch_number;
    info->_sigsys.si_arch = 0xC000003E; /* AUDIT_ARCH_X86_64 */
    current->syscall_dispatch_sigsys = false;
  }

  uc->uc_stack.ss_sp = current->ss_sp;
  uc->uc_stack.ss_flags = current->ss_flags;
  uc->uc_stack.ss_size = current->ss_size;

  uint64_t *g = uc->uc_mcontext.gregs;
  g[0] = frame->regs.r8;
  g[1] = frame->regs.r9;
  g[2] = frame->regs.r10;
  g[3] = frame->regs.r11;
  g[4] = frame->regs.r12;
  g[5] = frame->regs.r13;
  g[6] = frame->regs.r14;
  g[7] = frame->regs.r15;
  g[8] = frame->regs.rdi;
  g[9] = frame->regs.rsi;
  g[10] = frame->regs.rbp;
  g[11] = frame->regs.rbx;
  g[12] = frame->regs.rdx;
  g[13] = frame->regs.rax;
  g[14] = frame->regs.rcx;
  g[15] = frame->regs.rsp;
  g[16] = frame->regs.rip;
  g[17] = frame->regs.rflags;
  /* Linux REG_CSGSFS packs four 16-bit selectors: CS, GS, FS, SS.
   * Wine tests saved SS to distinguish native faults from 16-bit faults. */
  uint16_t fs, gs;
  __asm__ volatile("movw %%fs, %0; movw %%gs, %1" : "=r"(fs), "=r"(gs));
  g[18] = (uint16_t)frame->regs.cs | ((uint64_t)gs << 16) |
          ((uint64_t)fs << 32) | ((uint64_t)(uint16_t)frame->regs.ss << 48);
  uc->uc_flags = UC_SIGCONTEXT_SS;
  if (frame->regs.cs == 0x2B)
    uc->uc_flags |= UC_STRICT_RESTORE_SS;
  g[19] = frame->regs.err_code;
  g[20] = frame->regs.int_no;
  uint64_t saved_mask = current->has_saved_signal_mask ? current->saved_signal_mask : current->signal_mask;
  g[21] = saved_mask;
  if (sig == SIGSEGV || sig == SIGBUS)
    g[22] = current->fault_addr;
  uc->uc_sigmask[0] = saved_mask;
  uc->uc_mcontext.fpregs = (uint64_t)&uc->fpregs_mem[0];
  /* With lazy FPU, hardware registers may belong to a different thread.
     Ensure this thread's FPU state is live in hardware before saving. */
  fpu_ensure_loaded(current);
  if (cpu_has_xsave_flag) {
    uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
    uint32_t eax = (uint32_t)mask, edx = (uint32_t)(mask >> 32);
    __asm__ volatile("xsave64 %0" : "=m"(current->fpu_state) : "a"(eax), "d"(edx) : "memory");
    memcpy(uc->fpregs_mem, (const void *)current->fpu_state, 512);
  } else {
    uint8_t aligned_fpregs[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave64 %0" : "=m"(aligned_fpregs) : : "memory");
    memcpy(uc->fpregs_mem, aligned_fpregs, sizeof(aligned_fpregs));
    memcpy(current->fpu_state, aligned_fpregs, 512);
  }
}


// rt_sigaction: Set or get signal action
static uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_ptr,
                                 uint64_t oldact_ptr, uint64_t sigsetsize,
                                 uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (signum > 64 || signum < 1 || sigsetsize != 8)
    return (uint64_t)-22;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;
  uint64_t idx = signum - 1;

  /* Never dereference userspace after a separate VMA validation. Another
   * thread can unmap or reprotect the range between the check and access.
   * The uaccess helpers have exception-table fixups for that race and apply
   * the required SMAP access window. */
  struct k_sigaction new_action;
  if (act_ptr && copy_from_user(&new_action, (const void *)act_ptr,
                                sizeof(new_action)) != 0)
    return (uint64_t)-14; // EFAULT

  if (oldact_ptr) {
    struct k_sigaction old_action = current->signal_handlers[idx];
    if (copy_to_user((void *)oldact_ptr, &old_action,
                     sizeof(old_action)) != 0)
      return (uint64_t)-14; // EFAULT
  }

  if (act_ptr) {
    if (signum == SIGKILL || signum == SIGSTOP)
      return (uint64_t)-22;
    current->signal_handlers[idx] = new_action;
    /* If this process shares its VM with sibling threads (CLONE_THREAD/CLONE_VM),
     * synchronize the handler change across the thread group. */
    if (current->mm && current->mm->ref_count > 1) {
      extern struct thread *global_thread_list;
      extern spinlock_t tid_lock;
      spinlock_acquire(&tid_lock);
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t != current && t->tgid == current->tgid) {
          t->signal_handlers[idx] = new_action;
        }
      }
      spinlock_release(&tid_lock);
    }
  }
  return 0;
}

// rt_sigprocmask: Set or get signal mask
static int sigtimedwait_dequeue(struct thread *t, uint64_t mask,
                                struct kernel_siginfo *info) {
  uint64_t pending = t->pending_signals & mask;
  if (!pending)
    return 0;

  while (pending) {
    int i = __builtin_ctzll(pending);
    uint64_t bit = 1ULL << i;
    pending &= ~bit;
    int sig = i + 1;
    extern spinlock_t tid_lock;
    spinlock_acquire(&tid_lock);
    if (!(t->pending_signals & bit)) {
      spinlock_release(&tid_lock);
      continue;
    }
    bool supplied = signal_dequeue_locked(t, bit, info);
    if (info && !supplied) {
      memset(info, 0, sizeof(*info));
      info->si_signo = sig;
      if (t->signal_sender_pid[i]) {
        info->si_code = t->signal_sender_code[i];
        info->_kill.si_pid = pid_namespace_visible_locked(t, t->signal_sender_pid[i]);
      } else {
        info->si_code = 128; /* SI_KERNEL */
      }
    }
    spinlock_release(&tid_lock);
    return sig;
  }
  return 0;
}

// rt_sigtimedwait: used by musl sigwait(); VLC main thread blocks here.
static uint64_t sys_rt_sigtimedwait(uint64_t set_ptr, uint64_t info_ptr,
                                    uint64_t timeout_ptr, uint64_t sigsetsize,
                                    uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  if (sigsetsize != 8)
    return (uint64_t)-22; // EINVAL
  uint64_t mask;
  if (!set_ptr || !is_user_range((const void *)set_ptr, sizeof(uint64_t)) ||
      copy_from_user(&mask, (const void *)set_ptr, sizeof(uint64_t)) != 0)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

  // Linux rejects sets containing signals that are not blocked in the waiter.
  if (mask & ~current->signal_mask)
    return (uint64_t)-22; // EINVAL

  uint64_t deadline = 0;
  bool have_deadline = false;
  if (timeout_ptr) {
    uint64_t ts[2];
    if (!is_user_range((const void *)timeout_ptr, sizeof(ts)) ||
        copy_from_user(ts, (const void *)timeout_ptr, sizeof(ts)) != 0)
      return (uint64_t)-14; // EFAULT
    uint64_t sec = ts[0];
    uint64_t nsec = ts[1];
    if (nsec >= 1000000000ULL)
      return (uint64_t)-22; // EINVAL
    if (sec == 0 && nsec == 0) {
      struct kernel_siginfo info;
      int sig = sigtimedwait_dequeue(current, mask, &info);
      if (!sig)
        return (uint64_t)-11; // EAGAIN
      if (info_ptr) {
        if (!is_user_range((const void *)info_ptr, sizeof(info)) ||
            copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
          return (uint64_t)-14;
      }
      return (uint64_t)sig;
    }
    uint64_t ms = sec * 1000 + nsec / 1000000;
    if (nsec % 1000000ULL)
      ms++;
    if (ms == 0)
      ms = 1;
    deadline = lapic_timer_get_ticks() + ms;
    have_deadline = true;
  }

  for (;;) {
    struct kernel_siginfo info;
    int sig = sigtimedwait_dequeue(current, mask, &info);
    if (sig) {
      if (info_ptr) {
        if (!is_user_range((const void *)info_ptr, sizeof(info)) ||
            copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
          return (uint64_t)-14;
      }
      return (uint64_t)sig;
    }

    if (have_deadline && lapic_timer_get_ticks() >= deadline)
      return (uint64_t)-11; // EAGAIN

    current->state = THREAD_BLOCKED;
    current->wakeup_ticks = have_deadline ? deadline : 0;
    if (current->pending_signals & mask) {
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;
      continue;
    }
    sched_yield();
    current->state = THREAD_RUNNING;
    current->wakeup_ticks = 0;
  }
}

static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
                                   uint64_t oldset_ptr, uint64_t sigsetsize,
                                   uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (sigsetsize != 8)
    return (uint64_t)-22;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  if (oldset_ptr) {
    if (!is_user_range((const void *)oldset_ptr, sizeof(uint64_t)) ||
        copy_to_user((void *)oldset_ptr, &current->signal_mask, sizeof(uint64_t)) != 0)
      return (uint64_t)-14;
  }

  if (set_ptr) {
    uint64_t newset;
    if (!is_user_range((const void *)set_ptr, sizeof(uint64_t)) ||
        copy_from_user(&newset, (const void *)set_ptr, sizeof(uint64_t)) != 0)
      return (uint64_t)-14;

    newset &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    switch (how) {
    case SIG_BLOCK:
      current->signal_mask |= newset;
      break;
    case SIG_UNBLOCK:
      current->signal_mask &= ~newset;
      break;
    case SIG_SETMASK:
      current->signal_mask = newset;
      break;
    default:
      return (uint64_t)-22;
    }
  }
  return 0;
}

// rt_sigpending: examine the calling thread's pending (blocked-raised) set.
// Linux reports all pending signals, whether blocked or not; this kernel
// keeps the pending set per thread, so a plain copy is exact.
static uint64_t sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (sigsetsize != 8)
    return (uint64_t)-22;
  if (!set_ptr)
    return (uint64_t)-14;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;
  if (!is_user_range((const void *)set_ptr, sizeof(uint64_t)) ||
      copy_to_user((void *)set_ptr, &current->pending_signals, sizeof(uint64_t)) != 0)
    return (uint64_t)-14;
  return 0;
}

// rt_sigsuspend: Temporarily replace signal mask and suspend execution until signal
static uint64_t sys_rt_sigsuspend(uint64_t unewset, uint64_t sigsetsize,
                                  uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (sigsetsize != 8)
    return (uint64_t)-22; // EINVAL
  uint64_t newset;
  if (!unewset || !is_user_range((const void *)unewset, sizeof(uint64_t)) ||
      copy_from_user(&newset, (const void *)unewset, sizeof(uint64_t)) != 0)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  newset &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

  current->saved_signal_mask = current->signal_mask;
  current->has_saved_signal_mask = true;
  current->signal_mask = newset;

  for (;;) {
    if (current->pending_signals & ~current->signal_mask) {
      break;
    }

    current->state = THREAD_BLOCKED;
    current->wakeup_ticks = 0;
    if (current->pending_signals & ~current->signal_mask) {
      current->state = THREAD_RUNNING;
      break;
    }
    sched_yield();
    current->state = THREAD_RUNNING;
  }

  return (uint64_t)-4; // -EINTR
}

// pause: Suspend execution until any unblocked signal arrives
static uint64_t sys_pause(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  for (;;) {
    if (current->pending_signals & ~current->signal_mask) {
      break;
    }

    current->state = THREAD_BLOCKED;
    current->wakeup_ticks = 0;
    if (current->pending_signals & ~current->signal_mask) {
      current->state = THREAD_RUNNING;
      break;
    }
    sched_yield();
    current->state = THREAD_RUNNING;
  }

  return (uint64_t)-4; // -EINTR
}

// sigreturn
static uint64_t sys_rt_sigreturn(struct syscall_regs *sregs) {
  struct thread *current = sched_get_current();
  // Frame is on user stack.
  struct sigframe *frame = (struct sigframe *)sregs->rsp;
  if (!vmm_is_user_addr_range_valid((uint64_t)frame, sizeof(struct sigframe))) {
    klog_puts("[SIGNAL] sigreturn: invalid frame pointer\n");
    process_do_exit(11); // SIGSEGV
  }

  /*
   * Linux defines the ucontext as the restorable signal state. Restore into
   * kernel-side current->sigreturn_regs so IRETQ restores from safe kernel memory.
   */
  restore_signal_context(&current->sigreturn_regs, &frame->ucontext);
  current->signal_mask = frame->ucontext.uc_sigmask[0];
  current->signal_mask &=
      ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
  current->fault_regs_valid = false;
  current->fault_addr = 0;
 if ((current->sigreturn_regs.cs & 3) != 3 || current->sigreturn_regs.ss != 0x23 ||
      current->sigreturn_regs.rip >= 0x0000800000000000ULL ||
      current->sigreturn_regs.rsp >= 0x0000800000000000ULL) {
    klog_puts("[SIGNAL] sigreturn: invalid user context rip=");
    klog_uint64(current->sigreturn_regs.rip);
    klog_puts(" rsp=");
    klog_uint64(current->sigreturn_regs.rsp);
    klog_puts(" cs=");
    klog_uint64(current->sigreturn_regs.cs);
    klog_puts("\n");
    process_do_exit(11);
  }
 __asm__ volatile("cli" ::: "memory");
  fpu_ensure_loaded(current);
  if (cpu_has_xsave_flag) {
    // Preserve extended AVX/YMM state: update only the legacy 512-byte region
    // at the beginning of current->fpu_state without clearing upper YMM registers
    memcpy(&current->fpu_state, (const void *)frame->ucontext.fpregs_mem, 512);
    uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
    uint32_t eax = (uint32_t)mask, edx = (uint32_t)(mask >> 32);
    __asm__ volatile("xrstor64 %0" : : "m"(current->fpu_state), "a"(eax), "d"(edx) : "memory");
  } else {
    uint8_t aligned_fpregs[512] __attribute__((aligned(16)));
    memcpy(aligned_fpregs, (const void *)frame->ucontext.fpregs_mem, sizeof(aligned_fpregs));
    __asm__ volatile("fxrstor64 %0" : : "m"(aligned_fpregs) : "memory");
    memcpy(&current->fpu_state, aligned_fpregs, 512);
  }

  /* The dispatcher publishes this frame only after pending-signal delivery.
   * Delivery can exit/schedule, so a per-CPU pointer must not be set here. */
  return 0;
}

// Signal Delivery
extern bool process_core_dump_enabled;
extern void process_dump_core(struct thread *t, struct registers *regs,
                              int sig);

/* Signals whose default disposition is to be discarded rather than to terminate
 * or stop the process.  Both the delivery path and anyone queueing a signal have
 * to agree on this list: Linux drops such signals before they ever reach a
 * pending mask, and a pending-but-inert signal is not harmless here because a
 * blocking poll() reports EINTR for it. */
static bool signal_default_is_ignore(int sig) {
  return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH;
}

/* tid_lock held. Process signals are mirrored on siblings and consumed once;
 * tgkill/tkill signals belong to each recipient independently. In particular,
 * glibc's setxid barrier sends SIGSETXID to every worker and waits for all of
 * them: consuming one worker's signal must not discard the others' copies. */
static bool signal_dequeue_locked(struct thread *current, uint64_t bit,
                                  struct kernel_siginfo *info) {
  if (current->thread_directed_signals & bit) {
    bool supplied = false;
    unsigned sig = __builtin_ctzll(bit) + 1;
    struct queued_siginfo **link = &current->queued_siginfo;
    while (*link) {
      struct queued_siginfo *entry = *link;
      if (entry->info.si_signo == (int)sig) {
        if (info) *info = entry->info;
        *link = entry->next;
        kfree(entry);
        supplied = true;
        break;
      }
      link = &entry->next;
    }
    current->pending_signals &= ~bit;
    current->thread_directed_signals &= ~bit;
    /* Real-time sends of the same signal remain pending in FIFO order. */
    for (struct queued_siginfo *e = current->queued_siginfo; e; e = e->next) {
      if (e->info.si_signo == (int)sig) {
        current->pending_signals |= bit;
        current->thread_directed_signals |= bit;
        break;
      }
    }
    return supplied;
  }
  if (current->mm && current->mm->ref_count <= 1) {
    current->pending_signals &= ~bit;
    return false;
  }
  extern struct thread *global_thread_list;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid == current->tgid && !(t->thread_directed_signals & bit))
      t->pending_signals &= ~bit;
  }
  return false;
}

static void signal_frame_terminate(struct thread *current) {
  /* An undeliverable signal is fatal to the process, including siblings
   * sharing the failed address space. Leaving those workers alive retains
   * the memory whose exhaustion may have prevented frame construction. */
  current->exit_status = 128 + SIGSEGV;
  sched_terminate_thread_group(current);
  process_do_exit(current->exit_status);
}

static void signal_deliver_frame(struct registers *regs, uint64_t syscall_number,
                                  uint64_t syscall_return_ip) {
  struct thread *current = sched_get_current();
  if (!current)
    return;

  // Never deliver signals when interrupted while in kernel mode!
  // Signals must only be delivered when transitioning back to user space (CPL 3).
  if ((regs->cs & 3) != 3)
    return;

  /* PTRACE_INTERRUPT creates an event-stop even without a pending Unix
   * signal. This hook is deliberately before ordinary signal selection. */
  ptrace_user_return_stop(regs);

  uint64_t unmaskable = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));
  uint64_t pending = current->pending_signals & (~current->signal_mask | unmaskable);
  if (!pending) {
    syscall_restart_return(regs, syscall_number, syscall_return_ip, false, 0);
    return;
  }

  int sig = (current->syscall_dispatch_sigsys || current->seccomp_sigsys) ? SIGSYS : 0;
  if (!sig) {
    sig = __builtin_ctzll(pending) + 1;
  }

  struct kernel_siginfo queued_info;
  bool has_queued_info = false;
  uint64_t bit = (1ULL << (sig - 1));
  if (current->syscall_dispatch_sigsys || current->seccomp_sigsys) {
    /* Dispatch is synchronous and thread-directed, even if other ordinary
     * signals are pending or siblings also have SIGSYS queued. */
    current->pending_signals &= ~bit;
    current->thread_directed_signals &= ~bit;
  } else {
    extern spinlock_t tid_lock;
    spinlock_acquire(&tid_lock);
    has_queued_info = signal_dequeue_locked(current, bit, &queued_info);
    spinlock_release(&tid_lock);
  }
  /* Synchronous faults can remain pending while masked and be noticed here
   * at syscall exit. Restore the original exception frame before building a
   * signal frame or reporting the default action. */
  if (!has_queued_info &&
      (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) &&
      current->fault_regs_valid) {
    *regs = current->fault_regs;
    syscall_number = UINT64_MAX; /* this frame belongs to the original fault */
  }

  if (!current->syscall_dispatch_sigsys && !current->seccomp_sigsys) {
    struct kernel_siginfo trace_info;
    const struct kernel_siginfo *trace_info_ptr = NULL;
    if (has_queued_info) {
      trace_info_ptr = &queued_info;
    } else {
      memset(&trace_info, 0, sizeof(trace_info));
      trace_info.si_signo = sig;
      uint32_t sender = current->signal_sender_pid[sig - 1];
      trace_info.si_code = sender ? current->signal_sender_code[sig - 1]
                                  : 128; /* SI_KERNEL */
      if (sender) {
        trace_info._kill.si_pid = pid_namespace_visible(current, sender);
        trace_info._kill.si_uid = 0;
      }
      if (sig == SIGSEGV || sig == SIGBUS)
        trace_info._sigfault.si_addr = current->fault_addr;
      if (sig == SIGTRAP && regs->int_no == 3) {
        trace_info.si_code = 1; /* TRAP_BRKPT */
        trace_info._sigfault.si_addr = regs->rip ? regs->rip - 1 : 0;
      } else if (sig == SIGTRAP && regs->int_no == 1) {
        trace_info.si_code = 2; /* TRAP_TRACE */
        trace_info._sigfault.si_addr = regs->rip;
      }
      trace_info_ptr = &trace_info;
    }
    int replacement = ptrace_signal_delivery_stop(
        regs, sig, trace_info_ptr);
    if (replacement >= 0) {
      if (replacement == 0) {
        /* Suppressing a signal still has to translate internal restart
         * results before SYSRET; otherwise libc sees a kernel-only errno. */
        syscall_restart_return(regs, syscall_number, syscall_return_ip,
                               false, 0);
        return;
      }
      sig = replacement;
      has_queued_info = false;
    }
  }

  /* A sibling can reset a group disposition during frame construction.
   * Keep one action snapshot through handler selection, restorer and mask. */
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct k_sigaction action = current->signal_handlers[sig - 1];
  spinlock_release(&tid_lock);
  struct k_sigaction *sa = &action;

  /* Adjust the saved user context before constructing a signal frame. A
   * handler returning through sigreturn then either re-executes SYSCALL or
   * resumes its caller with EINTR, according to its SA_RESTART policy. */
  bool handler = sa->sa_handler != (void *)SIG_IGN &&
                 sa->sa_handler != (void *)SIG_DFL;
  syscall_restart_return(regs, syscall_number, syscall_return_ip,
                         handler, sa->sa_flags);

  if (sa->sa_handler == (void *)SIG_IGN)
    return;
  if (sa->sa_handler == (void *)SIG_DFL) {
    // Signals whose default action is "ignore"
    if (signal_default_is_ignore(sig))
      return;
    /* SIGCONT's default action is continuation, never termination. The
     * scheduler has no job-control stopped state yet, so an already-running
     * thread needs no state change here. Installed handlers still run below. */
    if (sig == SIGCONT)
      return;
    // SIGTTIN (21) and SIGTTOU (22): default action is STOP.
    // We don't have a full STOP/CONT implementation yet, so we ignore these
    // rather than terminating the process. This lets bash open job control
    // without being killed when it reads from the controlling terminal.
    if (sig == 21 || sig == 22 || sig == 32 || sig == 33)
      return;
    uint32_t sender_pid = current->signal_sender_pid[sig - 1];
    int32_t sender_code = current->signal_sender_code[sig - 1];
    if (has_queued_info) {
      sender_pid = queued_info._kill.si_pid;
      sender_code = queued_info.si_code;
    }
    sender_pid = pid_namespace_visible(current, sender_pid);
    klogf("[SIGNAL] Default action (terminate) for sig %llu comm=%s tid=%u sender=%u si_code=%d rip=%llu rsp=%llu rax=%llu\n",
          (unsigned long long)sig, current->comm, current->tid,
          sender_pid, sender_code,
          (unsigned long long)regs->rip, (unsigned long long)regs->rsp,
          (unsigned long long)regs->rax);

    // Print full registers, backtrace, and memory inspection only for fatal crash signals
    if (sig == SIGQUIT || sig == SIGILL || sig == SIGTRAP || sig == SIGABRT ||
        sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGSYS) {
      uint64_t report_addr = current->fault_regs_valid ? current->fault_addr : regs->rip;
      /* #BP/#DB were already recorded by their exception handlers before
       * reaching this default-action path. Re-reporting SIGTRAP here duplicates
       * the fault record and, historically, repeated a large dump with IF=0. */
      bool trap_already_reported =
          sig == SIGTRAP && (regs->int_no == 1 || regs->int_no == 3);
      if (!trap_already_reported)
        isr_report_user_fault(regs, sig, report_addr);
      if (process_core_dump_enabled) {
        process_dump_core(current, regs, sig);
      }
    }

    current->exit_status = 128 + sig;
    current->termination_signal = (uint8_t)sig;
    sched_terminate_thread_group(current);
    process_do_exit(128 + sig);
    return;
  }

  // Determine which stack to deliver the signal on
  bool using_altstack = on_sig_stack(current, regs->rsp);
  uint64_t rsp = regs->rsp;
  if ((sa->sa_flags & SA_ONSTACK) && current->ss_size &&
      !(current->ss_flags & SS_DISABLE) &&
      !using_altstack) {
    // Switch to the alternate signal stack (top = base + size)
    rsp = current->ss_sp + current->ss_size;
    using_altstack = true;
  }

  /*
   * Preserve the interrupted function's SysV x86-64 red zone. Leaf
   * functions may keep live data in the 128 bytes immediately below RSP.
   */
  rsp -= X86_64_RED_ZONE_SIZE;

  // Push frame to the chosen stack
  rsp -= sizeof(struct sigframe);
  rsp &= ~0xFULL; // 16-byte align for SysV ABI

  /* Check only a frame actually using the alternate stack, including its
   * return address. A normal pthread stack may be below the registered
   * alternate stack; that address ordering is not an overflow. Nested
   * handlers still need this check even without SA_ONSTACK. */
  if (using_altstack && (rsp < 8 || !on_sig_stack(current, rsp - 8))) {
    klog_puts("[SIGNAL] Altstack underflow during delivery\n");
    signal_frame_terminate(current);
  }

  if (!vmm_is_user_addr_range_writable(rsp, sizeof(struct sigframe))) {
    klogf("[SIGNAL] Stack overflow/invalid during delivery sig=%d tid=%u "
          "frame=%#llx size=%llu interrupted_rsp=%#llx alt=%d "
          "ss_sp=%#llx ss_size=%llu ss_flags=%#x sa_flags=%#llx\n",
          sig, current->tid, (unsigned long long)rsp,
          (unsigned long long)sizeof(struct sigframe),
          (unsigned long long)regs->rsp, using_altstack,
          (unsigned long long)current->ss_sp,
          (unsigned long long)current->ss_size, (unsigned)current->ss_flags,
          (unsigned long long)sa->sa_flags);
    signal_frame_terminate(current); // SIGSEGV
  }

  /* VMA permissions do not guarantee that a user page is resident or that
   * breaking fork CoW can allocate a frame. Build on the kernel stack, then
   * use exception-table protected copies so OOM/unmap kills this task rather
   * than panicking at an unguarded memset/store into user memory. */
  uint64_t user_frame = rsp;
  struct sigframe staged_frame;
  struct sigframe *frame = &staged_frame;
  memset(frame, 0, sizeof(struct sigframe));
  frame->regs = *regs;
  uint64_t mask_to_save = current->has_saved_signal_mask ? current->saved_signal_mask : current->signal_mask;
  frame->mask = mask_to_save;
  fill_signal_context(frame, sig, current);
  if (has_queued_info)
    frame->info = queued_info;
  frame->ucontext.uc_mcontext.fpregs =
      user_frame + offsetof(struct sigframe, ucontext.fpregs_mem);
  if (copy_to_user((void *)user_frame, frame, sizeof(*frame))) {
    klog_puts("[SIGNAL] Cannot write signal frame\n");
    signal_frame_terminate(current);
    return;
  }
  current->has_saved_signal_mask = false;

  regs->rip = (uint64_t)sa->sa_handler;
  regs->rdi = sig;
  if (sa->sa_flags & SA_SIGINFO) {
    regs->rsi = user_frame + offsetof(struct sigframe, info);
    regs->rdx = user_frame + offsetof(struct sigframe, ucontext);
  }

  // Set up return
  rsp -= 8;
  if (!vmm_is_user_addr_range_writable(rsp, 8)) {
    klog_puts("[SIGNAL] Stack overflow during return setup\n");
    signal_frame_terminate(current);
  }

  if (sa->sa_flags & SA_RESTORER) {
    uint64_t restorer = (uint64_t)sa->sa_restorer;
    if (copy_to_user((void *)rsp, &restorer, sizeof(restorer))) {
      klog_puts("[SIGNAL] Cannot write signal restorer\n");
      signal_frame_terminate(current);
      return;
    }
  } else {
    // If no restorer, we'd need a kernel trampoline. We'll warn for now.
    klog_puts("[SIGNAL] No SA_RESTORER for sig ");
    klog_uint64(sig);
    klog_puts("\n");
  }
  regs->rsp = rsp;

  current->signal_mask |= sa->sa_mask;
  if (!(sa->sa_flags & SA_NODEFER)) {
    current->signal_mask |= (1ULL << (sig - 1));
  }
  current->last_report_sig = 0;
}

// Helper to deliver from syscall context
void signal_deliver(struct registers *regs) {
  signal_deliver_frame(regs, UINT64_MAX, 0);
}

static void signal_syscall_return(struct syscall_regs *sregs, uint64_t number,
                                   uint64_t return_ip) {
  /* int 0x80 delivers default actions using its original interrupt frame.
   * i386 custom signal frames are not implemented yet. */
  if (sregs->user_cs == 0x1b) {
    /* Compat translation does not retain the original i386 syscall number.
     * It cannot replay an int 0x80 here; do not expose restart codes. */
    if (number != UINT64_MAX && syscall_restart_error(sregs->rax))
      sregs->rax = (uint64_t)-4;
    return;
  }
  /* rt_sigreturn has already selected the restored context for IRETQ.
   * A signal requeued inside a handler becomes unblocked here: deliver it
   * against that context, rather than libc's now-finished restorer syscall.
   * Handler frame updates must reach the same registers assembly restores. */
  struct thread *current = sched_get_current();
  if (current && number == SYS_RT_SIGRETURN) {
    signal_deliver_frame(&current->sigreturn_regs, UINT64_MAX, 0);
    return;
  }
  struct registers regs = {0};
  regs.rdi = sregs->rdi;
  regs.rsi = sregs->rsi;
  regs.rdx = sregs->rdx;
  regs.r10 = sregs->r10;
  regs.r8 = sregs->r8;
  regs.r9 = sregs->r9;
  regs.rax = sregs->rax;
  regs.rbx = sregs->rbx;
  regs.rbp = sregs->rbp;
  regs.r12 = sregs->r12;
  regs.r13 = sregs->r13;
  regs.r14 = sregs->r14;
  regs.r15 = sregs->r15;
  regs.rip = sregs->rip;
  regs.rflags = sregs->rflags;
  regs.rcx = sregs->rip; /* Architectural SYSCALL clobbers */
  regs.r11 = sregs->rflags;
  regs.rsp = sregs->rsp;
  regs.cs = 0x2B;
  regs.ss = 0x23; // Standard user segments

  signal_deliver_frame(&regs, number, return_ip);

  // Sync back
  sregs->rdi = regs.rdi;
  sregs->rsi = regs.rsi;
  sregs->rdx = regs.rdx;
  sregs->r10 = regs.r10;
  sregs->r8 = regs.r8;
  sregs->r9 = regs.r9;
  sregs->rax = regs.rax;
  sregs->rbx = regs.rbx;
  sregs->rbp = regs.rbp;
  sregs->r12 = regs.r12;
  sregs->r13 = regs.r13;
  sregs->r14 = regs.r14;
  sregs->r15 = regs.r15;
  sregs->rip = regs.rip;
  sregs->rflags = regs.rflags;
  sregs->rsp = regs.rsp;
}

void signal_deliver_syscall(struct syscall_regs *sregs) {
  signal_syscall_return(sregs, UINT64_MAX, 0);
}

void signal_finish_syscall(struct syscall_regs *sregs, uint64_t number,
                            uint64_t return_ip) {
  signal_syscall_return(sregs, number, return_ip);
}

// tgkill, sigaltstack, and helpers
// Forward declaration - defined after signalfd types below
void signal_notify_thread(struct thread *t, int sig);

/* Wake signalfd readers/pollers in `t` for `sig` without waking the thread
 * itself.  A consumer that blocks a signal and reads it through signalfd is
 * exactly the case signal_notify_thread's thread wakeup is skipped for, but
 * its poll()/read() still has to end.  Defined after the signalfd types. */
static void signal_notify_signalfds(struct thread *t, int sig);

static uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  // Validate signal number (0-64 are valid; 0 is existence check)
  if (sig > 64)
    return (uint64_t)-22; // EINVAL

  // tgid and tid must both be positive
  if ((int64_t)tgid <= 0 || (int64_t)tid <= 0)
    return (uint64_t)-22; // EINVAL

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;
  uint32_t sender_tid = sender ? sender->tid : 0;

  /* Lookup + queue under tid_lock, wake after releasing it. The old code used
   * sched_get_thread_by_tid() (lock, unlock, then dereference) and queued
   * without any lock: the target could be reaped in between (UAF) and a
   * concurrent delivery could lose the bit. */
  spinlock_acquire(&tid_lock);
  struct thread *target = NULL;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (pid_namespace_tid(sender, t) == (uint32_t)tid) {
      target = t;
      break;
    }
  }
  if (!target) {
    spinlock_release(&tid_lock);
    return (uint64_t)-3; // ESRCH — no such thread
  }

  // Validate that the target thread belongs to the specified thread group
  if (pid_namespace_visible_locked(sender, target->tgid) != (uint32_t)tgid) {
    spinlock_release(&tid_lock);
    return (uint64_t)-3; // ESRCH — tid exists but not in this tgid
  }

  if (!capability_has(sender, CAP_KILL) && sender->uid != target->uid &&
      sender->euid != target->uid && sender->uid != target->suid &&
      sender->euid != target->suid) {
    spinlock_release(&tid_lock); return (uint64_t)-1;
  }
  // Signal 0 is used for existence check only — don't deliver
  if (sig == 0) {
    spinlock_release(&tid_lock);
    return 0;
  }

  if (!pid_namespace_signal_allowed(sender, target, sig)) {
    spinlock_release(&tid_lock); return 0;
  }
  target->pending_signals |= (1ULL << (sig - 1));
  target->thread_directed_signals |= (1ULL << (sig - 1));
  target->signal_sender_pid[sig - 1] = sender_pid;
  target->signal_sender_code[sig - 1] = -6; /* SI_TKILL */
  spinlock_release(&tid_lock);

  /* Log without tid_lock held (see sys_kill). */

  if (sig == SIGKILL) {
    klog_puts("[SIGNAL] tgkill sender=");
    klog_uint64(sender_tid);
    klog_puts(" target=");
    klog_uint64((uint32_t)tid);
    klog_puts(" tgid=");
    klog_uint64((uint32_t)tgid);
    klog_puts("\n");
  }
  signal_notify_thread(target, (int)sig);

  return 0;
}

/* Linux rt_tgsigqueueinfo(2), x86-64 syscall 297. A positive si_code is
 * allowed only for a rethrow to the calling thread, as used by Crashpad. */
static uint64_t queue_siginfo(uint64_t tgid_arg, uint64_t tid_arg,
                              uint64_t sig_arg, uint64_t info_ptr,
                              bool process_directed) {
  int32_t tgid = (int32_t)tgid_arg, tid = (int32_t)tid_arg;
  int32_t sig = (int32_t)sig_arg;
  if (tgid <= 0 || tid <= 0 || sig < 0 || sig > 64)
    return (uint64_t)-22;
  struct kernel_siginfo info;
  if (copy_from_user(&info, (const void *)info_ptr, sizeof(info)))
    return (uint64_t)-14;
  struct thread *sender = sched_get_current();
  if (!sender) return (uint64_t)-3;
  if ((info.si_code >= 0 || info.si_code == -6 /* SI_TKILL */) &&
      (process_directed || tid != (int32_t)pid_namespace_tid(sender, sender)))
    return (uint64_t)-1;
  info.si_signo = sig;
  struct queued_siginfo *entry = sig ? kmalloc(sizeof(*entry)) : NULL;
  if (sig && !entry) return (uint64_t)-11; /* EAGAIN */
  if (entry) { entry->next = NULL; entry->info = info; }

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *target = global_thread_list;
  while (target && pid_namespace_tid(sender, target) != (uint32_t)tid) target = target->global_next;
  if (process_directed) {
    target = NULL;
    uint64_t bit = sig ? 1ULL << (sig - 1) : 0;
    for (struct thread *member = global_thread_list; member; member = member->global_next) {
      if (pid_namespace_visible_locked(sender, member->tgid ? member->tgid : member->tid) != (uint32_t)tgid ||
          member->state == THREAD_DEAD || member->state == THREAD_ZOMBIE) continue;
      if (!target) target = member;
      if (!(member->signal_mask & bit)) { target = member; break; }
    }
  }
  int error = 0;
  if (!target || pid_namespace_visible_locked(sender, target->tgid ? target->tgid : target->tid) != (uint32_t)tgid ||
      target->state == THREAD_DEAD || target->state == THREAD_ZOMBIE)
    error = -3;
  else if (pid_namespace_visible_locked(sender, sender->tgid ? sender->tgid : sender->tid) != (uint32_t)tgid &&
           !capability_has(sender, CAP_KILL) && sender->uid != target->uid &&
           sender->uid != target->suid && sender->euid != target->uid &&
           sender->euid != target->suid &&
           !(sig == SIGCONT && sender->sid == target->sid))
    error = -1;
  if (error || !sig) {
    spinlock_release(&tid_lock);
    kfree(entry);
    return (uint64_t)(int64_t)error;
  }
  if (!pid_namespace_signal_allowed(sender, target, sig)) {
    spinlock_release(&tid_lock); kfree(entry); return 0;
  }
  uint64_t bit = 1ULL << (sig - 1);
  if (target->signal_handlers[sig - 1].sa_handler == (void *)SIG_IGN &&
      !(target->signal_mask & bit)) {
    spinlock_release(&tid_lock);
    kfree(entry);
    return 0;
  }
  /* Standard signals coalesce and retain the first payload. */
  if (sig < 32 && (target->pending_signals & bit)) {
    spinlock_release(&tid_lock);
    kfree(entry);
    return 0;
  }
  unsigned count = 0;
  struct queued_siginfo **tail = &target->queued_siginfo;
  while (*tail) { count++; tail = &(*tail)->next; }
  if (count >= MAX_QUEUED_SIGINFO) {
    spinlock_release(&tid_lock);
    kfree(entry);
    return (uint64_t)-11;
  }
  *tail = entry;
  target->pending_signals |= bit;
  if (!process_directed) target->thread_directed_signals |= bit;
  target->signal_sender_pid[sig - 1] = info._kill.si_pid;
  target->signal_sender_code[sig - 1] = info.si_code;
  spinlock_release(&tid_lock);
  signal_notify_thread(target, sig);
  return 0;
}

static uint64_t sys_rt_tgsigqueueinfo(uint64_t tgid, uint64_t tid,
                                     uint64_t sig, uint64_t info,
                                     uint64_t a4, uint64_t a5) {
  (void)a4; (void)a5;
  return queue_siginfo(tgid, tid, sig, info, false);
}

static uint64_t sys_rt_sigqueueinfo(uint64_t pid, uint64_t sig, uint64_t info,
                                   uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  return queue_siginfo(pid, pid, sig, info, true);
}

#define MINSIGSTKSZ 2048

typedef struct {
  uint64_t ss_sp;
  uint32_t ss_flags;
  uint32_t __pad;
  uint64_t ss_size;
} stack_t;

// Helper: check if the current RSP is within the alternate signal stack
static bool on_sig_stack(struct thread *t, uint64_t sp) {
  if (t->ss_flags & SS_DISABLE)
    return false;
  return sp >= t->ss_sp && sp < (t->ss_sp + t->ss_size);
}

static uint64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  // Return the current alternate signal stack to userspace
  if (old_ss_ptr) {
    if (!vmm_is_user_addr_range_valid(old_ss_ptr, sizeof(stack_t)))
      return (uint64_t)-14; // EFAULT
    stack_t *old = (stack_t *)old_ss_ptr;
    old->ss_sp = current->ss_sp;
    old->ss_size = current->ss_size;
    old->ss_flags = current->ss_flags;
    // If we're currently executing on the alt stack, report SS_ONSTACK
    // We can't easily know for sure here, so check the saved RSP if available
  }

  // Set a new alternate signal stack
  if (ss_ptr) {
    if (!vmm_is_user_addr_range_valid(ss_ptr, sizeof(stack_t)))
      return (uint64_t)-14; // EFAULT
    stack_t *ss = (stack_t *)ss_ptr;

    // Can't change the alt stack while executing on it
    // (We approximate this — a real kernel would check current RSP)

    if (ss->ss_flags & SS_DISABLE) {
      // Disabling the alternate signal stack
      current->ss_sp = 0;
      current->ss_size = 0;
      current->ss_flags = SS_DISABLE;
    } else if (ss->ss_flags & ~(SS_AUTODISARM)) {
      // Only SS_DISABLE and SS_AUTODISARM are valid flags
      return (uint64_t)-22; // EINVAL
    } else {
      // Validate minimum stack size
      if (ss->ss_size < MINSIGSTKSZ)
        return (uint64_t)-12; // ENOMEM
      current->ss_sp = ss->ss_sp;
      current->ss_size = ss->ss_size;
      current->ss_flags =
          ss->ss_flags & SS_AUTODISARM; // Store valid flags, clear SS_DISABLE
    }
  }
  return 0;
}

static uint64_t __attribute__((unused)) sys_sigprocmask(uint64_t how, uint64_t set_ptr,
                                uint64_t oldset_ptr, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_rt_sigprocmask(how, set_ptr, oldset_ptr, 8, 0, 0);
}

// Send a signal to a specific thread
void signal_send(struct thread *t, int sig) {
  if (!t || sig <= 0 || sig > 64)
    return;
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;
  /* Queue under tid_lock after verifying the target is still linked, then
   * wake after releasing it (same order as sys_kill/pidfd_send_signal). */
  spinlock_acquire(&tid_lock);
  bool linked = false;
  for (struct thread *it = global_thread_list; it; it = it->global_next) {
    if (it == t) {
      linked = true;
      break;
    }
  }
  if (!linked) {
    spinlock_release(&tid_lock);
    return;
  }
  t->pending_signals |= (1ULL << (sig - 1));
  t->thread_directed_signals |= (1ULL << (sig - 1));
  t->signal_sender_pid[sig - 1] = sender_pid;
  t->signal_sender_code[sig - 1] = 0; /* SI_USER */
  spinlock_release(&tid_lock);
  signal_notify_thread(t, sig);
}

/* Report a child's death to its parent, as Linux's do_notify_parent() does: the
 * exit signal recorded at clone time (SIGCHLD for fork() and clone(SIGCHLD)) is
 * queued on every thread of the parent's thread group - a signal is a process
 * property, and this kernel keeps pending sets per thread and clears them
 * group-wide on delivery - and the threads that can take it are woken.
 *
 * This is what Qt's forkfd "fork fallback" is built on, and QProcess takes that
 * path for every spawn that installs a childProcessModifier, which is every
 * KPtyProcess/kpty spawn and therefore every Konsole session.  There is no pidfd
 * on that path at all: the parent blocks on a pipe that only the SIGCHLD handler
 * ever writes after reaping the child, and QProcess::waitForFinished() waits on
 * it with an infinite timeout.  Without SIGCHLD the pipe stays empty forever, so
 * Konsole's shell child used to park inside KPty::login() - which runs the
 * utempter helper through exactly such a nested QProcess - and never got as far
 * as execve("/bin/bash"), leaving the tab on "Could not start program".
 */
void signal_notify_parent_exit(struct thread *child) {
  if (!child || !child->parent)
    return;

  /* Threads of a thread group are not waitable children and are never reported
   * to the parent; Linux notifies for the group leader only. */
  if (child->clone_flags & CLONE_THREAD)
    return;

  int sig = (int)(child->clone_flags & 0xff); // CSIGNAL
  if (sig <= 0 || sig > 64)
    return;

  struct thread *parent = child->parent;

  /* An explicitly ignored SIGCHLD never becomes pending, as on Linux.  SIG_DFL
   * is different: its default action is "ignore", but Linux still queues the
   * signal, which is what signalfd consumers (bwrap and every glib spawn that
   * reaps through a signalfd) block it for.  signal_deliver() consumes a
   * default-ignore bit without delivering anything, so leaving it pending is
   * safe. */
  struct k_sigaction *pa = &parent->signal_handlers[sig - 1];
  if (pa->sa_handler == (void *)SIG_IGN)
    return;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  uint64_t bit = 1ULL << (sig - 1);
  uint32_t child_pid = child->tgid ? child->tgid : child->tid;

  /* Waking signalfd and scheduler waiters can acquire wait-queue/runqueue
   * locks. Snapshot the group under tid_lock, then perform wakeups after
   * releasing it to avoid inverting the scheduler's lock order. */
  uint32_t capacity = __atomic_load_n(&global_thread_count, __ATOMIC_ACQUIRE);
  struct thread **notify = capacity ? kmalloc(capacity * sizeof(*notify)) : NULL;
  if (capacity && !notify)
    return;
  uint32_t notify_count = 0;

  spinlock_acquire(&tid_lock);
  bool default_ignore =
      pa->sa_handler == (void *)SIG_DFL && signal_default_is_ignore(sig);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid != parent->tgid)
      continue;
    t->pending_signals |= bit;
    /* Delivered as SI_USER-style info from the child, which is what a
     * SIGCHLD handler expects to see in si_pid. */
    t->signal_sender_pid[sig - 1] = child_pid;
    t->signal_sender_code[sig - 1] = 0;
    if (notify_count < capacity)
      notify[notify_count++] = t;
  }
  spinlock_release(&tid_lock);

  for (uint32_t i = 0; i < notify_count; i++) {
    struct thread *t = notify[i];
    if ((t->signal_mask & bit) || default_ignore)
      signal_notify_signalfds(t, sig);
    else
      signal_notify_thread(t, sig);
  }
  kfree(notify);
}

// Send signal to all processes in a process group
void signal_send_pgid(uint32_t pgid, int sig) {  if (sig <= 0 || sig > 64)
    return;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  uint32_t capacity = __atomic_load_n(&global_thread_count, __ATOMIC_ACQUIRE);
  struct thread **notify = capacity ? kmalloc(capacity * sizeof(*notify)) : NULL;
  if (capacity && !notify)
    return;
  uint32_t notify_count = 0;
  spinlock_acquire(&tid_lock);

  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;

  struct thread *t = global_thread_list;
  while (t) {
    if (t->pgid == pgid && (!sender || sender->euid == 0 ||
        sender->uid == t->uid || sender->euid == t->uid)) {
      t->pending_signals |= (1ULL << (sig - 1));
      t->signal_sender_pid[sig - 1] = sender_pid;
      t->signal_sender_code[sig - 1] = 0; /* SI_USER */
      if (notify_count < capacity)
        notify[notify_count++] = t;
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
  for (uint32_t i = 0; i < notify_count; i++)
    signal_notify_thread(notify[i], sig);
  kfree(notify);
}

static uint64_t sys_kill(uint64_t pid_val, uint64_t sig, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if (sig > 64) return (uint64_t)-22;
  int32_t pid = (int32_t)pid_val;
  struct thread *current = sched_get_current();
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  extern uint32_t global_thread_count;
  uint32_t capacity = __atomic_load_n(&global_thread_count, __ATOMIC_ACQUIRE);
  struct thread **notify = sig && capacity ? kmalloc(capacity * sizeof(*notify)) : NULL;
  if (sig && capacity && !notify) return (uint64_t)-12;
  uint32_t count = 0;
  bool found = false, allowed = false;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    uint32_t visible = pid_namespace_tid(current, t);
    if (!visible || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE) continue;
    if (pid == -1 && !t->is_forked_child && !t->is_main_session) continue;
    bool match = pid > 0 ? pid_namespace_visible_locked(current, t->tgid) == (uint32_t)pid :
      pid == 0 ? t->pgid == current->pgid :
      pid == -1 ? visible != 1 && t->tgid != current->tgid :
      pid_namespace_visible_locked(current, t->pgid) == (uint32_t)-(int64_t)pid;
    if (!match) continue;
    found = true;
    if (!capability_has(current, CAP_KILL) && current->uid != t->uid &&
        current->euid != t->uid && current->uid != t->suid && current->euid != t->suid &&
        !(sig == SIGCONT && current->sid == t->sid)) continue;
    allowed = true;
    if (!sig || !pid_namespace_signal_allowed(current, t, sig) || count >= capacity) continue;
    t->pending_signals |= 1ULL << (sig - 1);
    t->signal_sender_pid[sig - 1] = current->tgid;
    t->signal_sender_code[sig - 1] = 0;
    __atomic_add_fetch(&t->futex_wake_claims, 1, __ATOMIC_ACQ_REL);
    notify[count++] = t;
  }
  spinlock_release(&tid_lock);
  for (uint32_t i = 0; i < count; i++) {
    signal_notify_thread(notify[i], sig);
    __atomic_sub_fetch(&notify[i]->futex_wake_claims, 1, __ATOMIC_RELEASE);
  }
  kfree(notify);
  return allowed ? 0 : (uint64_t)(found ? -1 : -3);
}

typedef struct {
  uint64_t mask;
  wait_queue_t wq;
} signalfd_ctx_t;

struct signalfd_siginfo {
  uint32_t ssi_signo;
  int32_t ssi_errno;
  int32_t ssi_code;
  uint32_t ssi_pid;
  uint32_t ssi_uid;
  int32_t ssi_fd;
  uint32_t ssi_tid;
  uint32_t ssi_band;
  uint32_t ssi_overrun;
  uint32_t ssi_trapno;
  int32_t ssi_status;
  int32_t ssi_int;
  uint64_t ssi_ptr;
  uint64_t ssi_utime;
  uint64_t ssi_stime;
  uint64_t ssi_addr;
  uint16_t ssi_addr_lsb;
  uint8_t __pad[46];
};

static uint64_t get_pending_signals_tgid(struct thread *curr, uint64_t mask) {
  uint64_t pending = curr->pending_signals & mask;
  if (pending)
    return pending;

  /* If the process has no cloned threads sharing its VM, no other thread
   * can have pending process signals. */
  if (curr->mm && curr->mm->ref_count <= 1)
    return 0;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t) {
    if (t->tgid == curr->tgid &&
        (t->pending_signals & ~t->thread_directed_signals & mask)) {
      pending = t->pending_signals & ~t->thread_directed_signals & mask;
      break;
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
  return pending;
}

static bool consume_signal_tgid(struct thread *curr, int sig, uint32_t *out_sender_pid,
                                 struct kernel_siginfo *queued_info, bool *has_info) {
  uint64_t bit = (1ULL << (sig - 1));
  uint32_t sender = 0;
  bool consumed = false;

  /* Fast-path for single-threaded process: consume without locking global_thread_list */
  if (curr->mm && curr->mm->ref_count <= 1) {
    if (curr->pending_signals & bit) {
      if (curr->thread_directed_signals & bit) {
        *has_info = signal_dequeue_locked(curr, bit, queued_info);
      } else {
        curr->pending_signals &= ~bit;
        *has_info = false;
      }
      sender = curr->signal_sender_pid[sig - 1];
      curr->signal_sender_pid[sig - 1] = 0;
      consumed = true;
    }
    if (consumed && out_sender_pid)
      *out_sender_pid = sender;
    return consumed;
  }

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  if (curr->pending_signals & curr->thread_directed_signals & bit) {
    sender = curr->signal_sender_pid[sig - 1];
    *has_info = signal_dequeue_locked(curr, bit, queued_info);
    spinlock_release(&tid_lock);
    if (out_sender_pid)
      *out_sender_pid = sender;
    return true;
  }
  struct thread *t = global_thread_list;
  while (t) {
    if (t->tgid == curr->tgid &&
        (t->pending_signals & ~t->thread_directed_signals & bit)) {
      t->pending_signals &= ~bit;
      if (!sender && t->signal_sender_pid[sig - 1]) {
        sender = t->signal_sender_pid[sig - 1];
        t->signal_sender_pid[sig - 1] = 0;
      }
      consumed = true;
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);

  if (consumed && out_sender_pid)
    *out_sender_pid = sender;
  return consumed;
}

static uint32_t signalfd_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  (void)offset;
  if (size < sizeof(struct signalfd_siginfo)) {
    klogf("[SIGNALFD] size too small: %u node=0x%016llX\n", size,
          (unsigned long long)(uintptr_t)node);
    return (uint32_t)-22; // EINVAL
  }

  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
  struct thread *t = sched_get_current();

  while (1) {
    uint64_t pending = get_pending_signals_tgid(t, ctx->mask);
    if (pending) {
      int sig = __builtin_ctzll(pending) + 1;
      uint32_t sender_pid = 0;
      struct kernel_siginfo queued_info;
      bool has_info = false;
      if (sig && consume_signal_tgid(t, sig, &sender_pid, &queued_info, &has_info)) {
        klogf("[SIGNALFD] read sig=%d from sender_pid=%u\n", sig,
              sender_pid);
        struct signalfd_siginfo info;
        memset(&info, 0, sizeof(info));
        info.ssi_signo = sig;
        info.ssi_pid = pid_namespace_visible(t, sender_pid);
        if (has_info) {
          info.ssi_errno = queued_info.si_errno;
          info.ssi_code = queued_info.si_code;
          info.ssi_pid = queued_info._kill.si_pid;
          info.ssi_uid = queued_info._kill.si_uid;
          memcpy(&info.ssi_ptr, queued_info.data + 8, sizeof(info.ssi_ptr));
          info.ssi_int = (int32_t)info.ssi_ptr;
          info.ssi_addr = queued_info._sigfault.si_addr;
        }
        memcpy(buffer, &info, sizeof(info));
        return sizeof(info);
      }
    }

    // No signals: honour O_NONBLOCK like eventfd/timerfd do.
    int sig_fd = -1;
    if (t) {
      for (uint32_t i = 0; i < t->fd_capacity; i++) {
        if (t->fds[i] == node) {
          sig_fd = i;
          break;
        }
      }
    }
    if (sig_fd != -1 && t && (t->fd_flags[sig_fd] & 0x800))
      return (uint32_t)-11; // EAGAIN
    wait_queue_entry_t entry = {.thread = t, .next = NULL, .wq = NULL, .thread_next = NULL, .thread_prev = NULL};
    t->state = THREAD_BLOCKED;
    wait_queue_add(&ctx->wq, &entry);

    if (get_pending_signals_tgid(t, ctx->mask)) {
      t->state = THREAD_RUNNING;
      wait_queue_remove(&ctx->wq, &entry);
      continue;
    }

    sched_yield();
    t->state = THREAD_RUNNING;
    wait_queue_remove(&ctx->wq, &entry);
  }
}

static int signalfd_poll(vfs_node_t *node, int events) {
  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
  struct thread *t = sched_get_current();
  int revents = 0;

  if (get_pending_signals_tgid(t, ctx->mask))
    revents |= POLLIN;

  return revents & events;
}

static void signalfd_close(vfs_node_t *node) {
  if (node->device) {
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
    /* Wake and detach waiters before freeing the context that owns the queue. */
    wait_queue_wake_all(&ctx->wq);
    kfree(ctx);
    node->device = NULL;
  }
}

// signal_notify_signalfds/signal_notify_thread must be defined AFTER
// signalfd_ctx_t and signalfd_read.
static void signal_notify_signalfds(struct thread *t, int sig) {
  if (!t || sig <= 0 || sig > 64)
    return;

  extern spinlock_t tid_lock;
  /* Pin matching nodes while both table attachment and descriptor slots are
   * stable. Exit detaches under tid_lock, and close/dup mutate under files->lock.
   * Wake outside those locks: epoll and wait queues have their own lock order. */
  uint32_t next = 0;
  while (t->files && next < t->files->capacity) {
    vfs_node_t *notify[16];
    unsigned count = 0;
    spinlock_acquire(&tid_lock);
    struct fd_table *files = t->files;
    if (!files) {
      spinlock_release(&tid_lock);
      return;
    }
    spinlock_acquire(&files->lock);
    for (; next < files->capacity && count < 16; next++) {
      vfs_node_t *node = files->fds[next];
      if (fd_is_valid_node(node) && node->read == signalfd_read) {
        signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
        if (ctx && (ctx->mask & (1ULL << (sig - 1)))) {
          vfs_node_ref(node);
          notify[count++] = node;
        }
      }
    }
    spinlock_release(&files->lock);
    spinlock_release(&tid_lock);
    for (unsigned i = 0; i < count; i++) {
      vfs_node_t *node = notify[i];
      signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
      wait_queue_wake_all(&ctx->wq);
      epoll_notify_event(node, POLLIN);
      vfs_close(node);
    }
  }
}

void signal_notify_thread(struct thread *t, int sig) {
  if (!t || sig <= 0 || sig > 64)
    return;

  // Wake up thread if it's sleeping/blocked
  if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
    sched_wakeup(t);
  }

  signal_notify_signalfds(t, sig);
}

static uint64_t sys_signalfd4(uint64_t fd, uint64_t mask_ptr, uint64_t sizemask,
                              uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  (void)sizemask;
#ifndef SFD_NONBLOCK
#define SFD_NONBLOCK 04000
#endif
#ifndef SFD_CLOEXEC
#define SFD_CLOEXEC  02000000
#endif
  /* Same encodings the descriptor layer uses: bit 24 carries FD_CLOEXEC
   * (FD_FLAGS_CLOEXEC_BIT in sys_io_shared.h), O_NONBLOCK is a status flag. */
#define SIGNALFD_O_NONBLOCK 0x800
#define SIGNALFD_FD_FLAGS_CLOEXEC_BIT (1u << 24)
  if (flags & ~(uint64_t)(SFD_CLOEXEC | SFD_NONBLOCK))
    return (uint64_t)-22; // EINVAL

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  uint64_t mask = 0;
  if (mask_ptr && vmm_is_user_addr_range_valid(mask_ptr, 8))
    mask = *(uint64_t *)mask_ptr;

  // If fd != -1, update existing signalfd
  if (FD_IN_RANGE(t, fd) && fd_is_valid_node(t->fds[fd])) {
    vfs_node_t *node = t->fds[fd];
    if (node->read == signalfd_read) {
      signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
      ctx->mask = mask;
      return fd;
    }
  }

  // Allocate a new fd
  int new_fd = alloc_fd(t);
  if (new_fd < 0)
    return (uint64_t)-24; // EMFILE

  signalfd_ctx_t *ctx = kmalloc(sizeof(signalfd_ctx_t));
  if (!ctx)
    return (uint64_t)-12;
  ctx->mask = mask;
  wait_queue_init(&ctx->wq);

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    kfree(ctx);
    return (uint64_t)-12; // ENOMEM
  }
  vfs_node_init(node);

  node->flags = FS_CHARDEV; // Change to CHARDEV to avoid default POLLIN/OUT
  node->mask = 0600;
  node->length = 0;
  node->device = ctx;
  node->read = signalfd_read;
  node->poll = signalfd_poll;
  node->close = signalfd_close;
  node->wait_queue = &ctx->wq;

  t->fds[new_fd] = node;
  t->fd_offsets[new_fd] = 0;
  /* Explicit descriptor flags: the slot may have been reused.  Bit 24 is the
   * FD_CLOEXEC bit the descriptor layer uses (FD_FLAGS_CLOEXEC_BIT). */
  t->fd_flags[new_fd] =
      (flags & SFD_NONBLOCK ? SIGNALFD_O_NONBLOCK : 0) |
      (flags & SFD_CLOEXEC ? SIGNALFD_FD_FLAGS_CLOEXEC_BIT : 0);

  return (uint64_t)new_fd;
}

static uint64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t sizemask,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_signalfd4(fd, mask_ptr, sizemask, 0, 0, 0);
}

static uint64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (sig > 64)
    return (uint64_t)-22;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;

  spinlock_acquire(&tid_lock);
  struct thread *target = NULL;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (pid_namespace_tid(sender, t) == (uint32_t)tid) {
      target = t;
      break;
    }
  }
  if (!target) { spinlock_release(&tid_lock); return (uint64_t)-3; }
  if (!capability_has(sender, CAP_KILL) && sender->uid != target->uid &&
      sender->euid != target->uid && sender->uid != target->suid &&
      sender->euid != target->suid) {
    spinlock_release(&tid_lock); return (uint64_t)-1;
  }
  if (!sig) { spinlock_release(&tid_lock); return 0; }
  if (!pid_namespace_signal_allowed(sender, target, sig)) {
    spinlock_release(&tid_lock); return 0;
  }
  target->pending_signals |= (1ULL << (sig - 1));
  target->thread_directed_signals |= (1ULL << (sig - 1));
  target->signal_sender_pid[sig - 1] = sender_pid;
  target->signal_sender_code[sig - 1] = -6; /* SI_TKILL */
  spinlock_release(&tid_lock);
  signal_notify_thread(target, (int)sig);
  return 0;
}

void syscall_register_signal(void) {
  syscall_register_ptrace();
  syscall_register(SYS_RT_SIGACTION, sys_rt_sigaction);
  syscall_register(SYS_RT_SIGPROCMASK, sys_rt_sigprocmask);
  syscall_register(SYS_RT_SIGPENDING, sys_rt_sigpending);
  syscall_register(SYS_RT_SIGSUSPEND, sys_rt_sigsuspend);
  syscall_register(SYS_RT_SIGTIMEDWAIT, sys_rt_sigtimedwait);
  syscall_register_raw(SYS_RT_SIGRETURN, sys_rt_sigreturn);
  syscall_register(SYS_PAUSE, sys_pause);
  syscall_register(SYS_SIGALTSTACK, sys_sigaltstack);
  syscall_register(SYS_TKILL, sys_tkill);
  syscall_register(SYS_TGKILL, sys_tgkill);
  syscall_register(SYS_RT_TGSIGQUEUEINFO, sys_rt_tgsigqueueinfo);
  syscall_register(SYS_RT_SIGQUEUEINFO, sys_rt_sigqueueinfo);
  syscall_register(SYS_KILL, sys_kill);
  syscall_register(SYS_SIGNALFD, sys_signalfd);
  syscall_register(SYS_SIGNALFD4, sys_signalfd4);
}
