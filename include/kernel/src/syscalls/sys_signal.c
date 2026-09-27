// Signal Syscalls: rt_sigaction, rt_sigprocmask
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../cpu/fpu.h"
#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "../socket/epoll.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>


#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define X86_64_RED_ZONE_SIZE 128

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
    uint8_t data[112];
  };
} __attribute__((packed));

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
  regs->ss = 0x23;
}

static void fill_signal_context(struct sigframe *frame, int sig,
                                struct thread *current) {
  struct kernel_siginfo *info = &frame->info;
  struct kernel_ucontext *uc = &frame->ucontext;
  memset(info, 0, sizeof(*info));
  memset(uc, 0, sizeof(*uc));

  info->si_signo = sig;
  uint32_t sender_pid = current->signal_sender_pid[sig - 1];
  if (sender_pid) {
    info->si_code = 0; /* SI_USER */
    info->_kill.si_pid = sender_pid;
    info->_kill.si_uid = 0;
  } else {
    info->si_code = 128; /* SI_KERNEL */
  }

  if (sig == SIGSEGV || sig == SIGBUS) {
    uint64_t fault_address = current->fault_addr;
    info->si_code = (current->fault_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */;
    info->_sigfault.si_addr = fault_address;
  } else if (sig == SIGILL || sig == SIGFPE) {
    uint64_t fault_address = frame->regs.rip;
    info->si_code = 1;
    info->_sigfault.si_addr = fault_address;
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
  g[18] = frame->regs.cs;
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
    uint32_t eax = 0xFFFFFFFF, edx = 0xFFFFFFFF;
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

  if (oldact_ptr) {
    if (!vmm_is_user_addr_range_writable(oldact_ptr, sizeof(struct k_sigaction)))
      return (uint64_t)-14; // EFAULT
    struct k_sigaction *old = (struct k_sigaction *)oldact_ptr;
    *old = current->signal_handlers[idx];
  }

  if (act_ptr) {
    if (!vmm_is_user_addr_range_valid(act_ptr, sizeof(struct k_sigaction)))
      return (uint64_t)-14;
    struct k_sigaction *new = (struct k_sigaction *)act_ptr;
    if (signum == SIGKILL || signum == SIGSTOP)
      return (uint64_t)-22;
    extern struct thread *global_thread_list;
    extern spinlock_t tid_lock;
    spinlock_acquire(&tid_lock);
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t->tgid == current->tgid) {
        t->signal_handlers[idx] = *new;
      }
    }
    spinlock_release(&tid_lock);
  }
  return 0;
}

// rt_sigprocmask: Set or get signal mask
static int sigtimedwait_dequeue(struct thread *t, uint64_t mask,
                                struct kernel_siginfo *info) {
  uint64_t pending = t->pending_signals & mask;
  if (!pending)
    return 0;

  for (int i = 0; i < 64; i++) {
    if (!(pending & (1ULL << i)))
      continue;
    int sig = i + 1;
    t->pending_signals &= ~(1ULL << i);
    if (info) {
      memset(info, 0, sizeof(*info));
      info->si_signo = sig;
      info->si_code = 128; /* SI_KERNEL */
    }
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
  if (!set_ptr || !vmm_is_user_addr_range_valid(set_ptr, 8))
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  uint64_t mask = *(uint64_t *)set_ptr;
  mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

  // Linux rejects sets containing signals that are not blocked in the waiter.
  if (mask & ~current->signal_mask)
    return (uint64_t)-22; // EINVAL

  uint64_t deadline = 0;
  bool have_deadline = false;
  if (timeout_ptr) {
    if (!vmm_is_user_addr_range_valid(timeout_ptr, 16))
      return (uint64_t)-14; // EFAULT
    uint64_t sec = ((uint64_t *)timeout_ptr)[0];
    uint64_t nsec = ((uint64_t *)timeout_ptr)[1];
    if (nsec >= 1000000000ULL)
      return (uint64_t)-22; // EINVAL
    if (sec == 0 && nsec == 0) {
      struct kernel_siginfo info;
      int sig = sigtimedwait_dequeue(current, mask, &info);
      if (!sig)
        return (uint64_t)-11; // EAGAIN
      if (info_ptr) {
        if (!vmm_is_user_addr_range_writable(info_ptr,
                                             sizeof(struct kernel_siginfo)))
          return (uint64_t)-14;
        memcpy((void *)info_ptr, &info, sizeof(info));
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
        if (!vmm_is_user_addr_range_writable(info_ptr,
                                             sizeof(struct kernel_siginfo)))
          return (uint64_t)-14;
        memcpy((void *)info_ptr, &info, sizeof(info));
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
    if (!vmm_is_user_addr_range_writable(oldset_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    *(uint64_t *)oldset_ptr = current->signal_mask;
  }

  if (set_ptr) {
    if (!vmm_is_user_addr_range_valid(set_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    uint64_t newset = *(uint64_t *)set_ptr;
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

// rt_sigsuspend: Temporarily replace signal mask and suspend execution until signal
static uint64_t sys_rt_sigsuspend(uint64_t unewset, uint64_t sigsetsize,
                                  uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (sigsetsize != 8)
    return (uint64_t)-22; // EINVAL
  if (!unewset || !vmm_is_user_addr_range_valid(unewset, 8))
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  uint64_t newset = *(uint64_t *)unewset;
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
 if ((current->sigreturn_regs.cs & 3) != 3 || (current->sigreturn_regs.ss & 3) != 3 ||
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
    uint32_t eax = 0xFFFFFFFF, edx = 0xFFFFFFFF;
    __asm__ volatile("xrstor64 %0" : : "m"(current->fpu_state), "a"(eax), "d"(edx) : "memory");
  } else {
    uint8_t aligned_fpregs[512] __attribute__((aligned(16)));
    memcpy(aligned_fpregs, (const void *)frame->ucontext.fpregs_mem, sizeof(aligned_fpregs));
    __asm__ volatile("fxrstor64 %0" : : "m"(aligned_fpregs) : "memory");
    memcpy(&current->fpu_state, aligned_fpregs, 512);
  }

  /* syscall_entry.asm IRETQs from here with IF restored from user RFLAGS. */
  cpu_get_current()->sigreturn_frame = (uint64_t)&current->sigreturn_regs;
  __asm__ volatile("" ::: "memory");
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

void signal_deliver(struct registers *regs) {
  struct thread *current = sched_get_current();
  if (!current)
    return;

  // Never deliver signals when interrupted while in kernel mode!
  // Signals must only be delivered when transitioning back to user space (CPL 3).
  if ((regs->cs & 3) != 3)
    return;

  uint64_t pending = current->pending_signals & ~current->signal_mask;
  if (!pending)
    return;

  int sig = 0;
  for (int i = 0; i < 64; i++) {
    if (pending & (1ULL << i)) {
      sig = i + 1;
      break;
    }
  }
  if (!sig)
    return;

  uint64_t bit = (1ULL << (sig - 1));
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid == current->tgid) {
      t->pending_signals &= ~bit;
    }
  }
  spinlock_release(&tid_lock);
  struct k_sigaction *sa = &current->signal_handlers[sig - 1];

  if (sa->sa_handler == (void *)SIG_IGN)
    return;
  if (sa->sa_handler == (void *)SIG_DFL) {
    // Signals whose default action is "ignore"
    if (signal_default_is_ignore(sig))
      return;
    // SIGTTIN (21) and SIGTTOU (22): default action is STOP.
    // We don't have a full STOP/CONT implementation yet, so we ignore these
    // rather than terminating the process. This lets bash open job control
    // without being killed when it reads from the controlling terminal.
    if (sig == 21 || sig == 22 || sig == 32 || sig == 33)
      return;
    klog_puts("[SIGNAL] Default action (terminate) for sig ");
    klog_uint64(sig);
    klog_puts("\n");

    // Print full registers, backtrace, and memory inspection only for fatal crash signals
    if (sig == SIGQUIT || sig == SIGILL || sig == SIGTRAP || sig == SIGABRT ||
        sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGSYS) {
      isr_report_user_fault(regs, sig, current->fault_addr);
      if (process_core_dump_enabled) {
        process_dump_core(current, regs, sig);
      }
    }

    sched_terminate_thread_group(current);
    process_do_exit(128 + sig);
    return;
  }

  // Determine which stack to deliver the signal on
  uint64_t rsp;
  if ((sa->sa_flags & SA_ONSTACK) && !(current->ss_flags & SS_DISABLE) &&
      !on_sig_stack(current, regs->rsp)) {
    // Switch to the alternate signal stack (top = base + size)
    if (current->ss_size >= sizeof(struct sigframe) + X86_64_RED_ZONE_SIZE + 64) {
      rsp = current->ss_sp + current->ss_size;
    } else {
      rsp = regs->rsp;
    }
  } else {
    rsp = regs->rsp;
  }

  /*
   * Preserve the interrupted function's SysV x86-64 red zone. Leaf
   * functions may keep live data in the 128 bytes immediately below RSP.
   */
  rsp -= X86_64_RED_ZONE_SIZE;

  // Push frame to the chosen stack
  rsp -= sizeof(struct sigframe);
  rsp &= ~0xFULL; // 16-byte align for SysV ABI

  if (current->ss_sp && on_sig_stack(current, current->ss_sp + current->ss_size - 1) &&
      rsp < current->ss_sp) {
    klog_puts("[SIGNAL] Altstack underflow during delivery\n");
    process_do_exit(11);
  }

  if (!vmm_is_user_addr_range_writable(rsp, sizeof(struct sigframe))) {
    klog_puts("[SIGNAL] Stack overflow/invalid during delivery\n");
    process_do_exit(11); // SIGSEGV
  }

  struct sigframe *frame = (struct sigframe *)rsp;
  memset(frame, 0, sizeof(struct sigframe));
  frame->regs = *regs;
  uint64_t mask_to_save = current->has_saved_signal_mask ? current->saved_signal_mask : current->signal_mask;
  frame->mask = mask_to_save;
  fill_signal_context(frame, sig, current);
  current->has_saved_signal_mask = false;

  regs->rip = (uint64_t)sa->sa_handler;
  regs->rdi = sig;
  if (sa->sa_flags & SA_SIGINFO) {
    regs->rsi = (uint64_t)&frame->info;
    regs->rdx = (uint64_t)&frame->ucontext;
  }

  // Set up return
  rsp -= 8;
  if (!vmm_is_user_addr_range_writable(rsp, 8)) {
    klog_puts("[SIGNAL] Stack overflow during return setup\n");
    process_do_exit(11);
  }

  if (sa->sa_flags & SA_RESTORER) {
    *(uint64_t *)rsp = (uint64_t)sa->sa_restorer;
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
void signal_deliver_syscall(struct syscall_regs *sregs) {
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
  regs.rsp = sregs->rsp;
  regs.cs = 0x2B;
  regs.ss = 0x23; // Standard user segments

  signal_deliver(&regs);

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

// tgkill, sigaltstack, and helpers
// Forward declaration - defined after signalfd types below
void signal_notify_thread(struct thread *t, int sig);

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

  // Find the target thread by tid
  struct thread *target = sched_get_thread_by_tid((uint32_t)tid);
  if (!target)
    return (uint64_t)-3; // ESRCH — no such thread

  // Validate that the target thread belongs to the specified thread group
  if (target->tgid != (uint32_t)tgid)
    return (uint64_t)-3; // ESRCH — tid exists but not in this tgid

  // Signal 0 is used for existence check only — don't deliver
  if (sig == 0)
    return 0;

  // Queue the signal on the target thread
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;
  if (sig == SIGKILL) {
    klog_puts("[SIGNAL] tgkill sender=");
    klog_uint64(sender ? sender->tid : 0);
    klog_puts(" target=");
    klog_uint64(target->tid);
    klog_puts(" tgid=");
    klog_uint64(target->tgid);
    klog_puts("\n");
  }
  target->pending_signals |= (1ULL << (sig - 1));
  target->signal_sender_pid[sig - 1] = sender_pid;
  signal_notify_thread(target, (int)sig);

  return 0;
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
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;
  t->pending_signals |= (1ULL << (sig - 1));
  t->signal_sender_pid[sig - 1] = sender_pid;
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

  /* Never queue a signal the parent will not act on: Linux drops ignored signals
   * before they reach the pending mask, and here a pending signal also means a
   * spurious -EINTR from a blocking poll(). */
  struct k_sigaction *pa = &parent->signal_handlers[sig - 1];
  if (pa->sa_handler == (void *)SIG_IGN)
    return;
  if (pa->sa_handler == (void *)SIG_DFL && signal_default_is_ignore(sig))
    return;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  uint64_t bit = 1ULL << (sig - 1);
  uint32_t child_pid = child->tgid ? child->tgid : child->tid;

  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid != parent->tgid)
      continue;
    t->pending_signals |= bit;
    /* Delivered as SI_USER-style info from the child, which is what a
     * SIGCHLD handler expects to see in si_pid. */
    t->signal_sender_pid[sig - 1] = child_pid;
    /* A thread that blocks the signal keeps it pending for the ones that do not,
     * so waking it would only churn the scheduler. */
    if (!(t->signal_mask & bit))
      signal_notify_thread(t, sig);
  }
  spinlock_release(&tid_lock);
}

// Send signal to all processes in a process group
void signal_send_pgid(uint32_t pgid, int sig) {  if (sig <= 0 || sig > 64)
    return;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);

  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;

  struct thread *t = global_thread_list;
  while (t) {
    if (t->pgid == pgid && (!sender || sender->euid == 0 ||
        sender->uid == t->uid || sender->euid == t->uid)) {
      t->pending_signals |= (1ULL << (sig - 1));
      t->signal_sender_pid[sig - 1] = sender_pid;
      signal_notify_thread(t, sig);
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
}

static uint64_t sys_kill(uint64_t pid_val, uint64_t sig, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  int32_t pid = (int32_t)pid_val;
  if (sig > 64)
    return (uint64_t)-22;

  struct thread *current = sched_get_current();
  if (sig == 0) {
    if (pid == 0 || pid == -1)
      return 0;

    bool found = false;
    extern struct thread *global_thread_list;
    extern spinlock_t tid_lock;
    spinlock_acquire(&tid_lock);
    struct thread *t = global_thread_list;
    while (t) {
      if (t->tid == (uint32_t)(pid > 0 ? pid : -pid)) {
        if (current && current->euid != 0 && current->uid != t->uid &&
            current->euid != t->uid) {
          spinlock_release(&tid_lock);
          return (uint64_t)-1;
        }
        found = true;
        break;
      }
      t = t->global_next;
    }
    spinlock_release(&tid_lock);
    return found ? 0 : (uint64_t)-3; // -ESRCH
  }

  if (pid == 0) {
    if (sig == SIGKILL) {
      klog_puts("[SIGNAL] killpg sender=");
      klog_uint64(current ? current->tid : 0);
      klog_puts(" pgid=");
      klog_uint64(current ? current->pgid : 0);
      klog_puts("\n");
    }
    signal_send_pgid(current->pgid, (int)sig);
    return 0;
  } else if (pid == -1) {
    // Send to everyone? Not implemented for safety.
    return 0;
  } else if (pid < -1) {
    if (sig == SIGKILL) {
      klog_puts("[SIGNAL] killpg sender=");
      klog_uint64(current ? current->tid : 0);
      klog_puts(" pgid=");
      klog_uint64((uint32_t)-pid);
      klog_puts("\n");
    }
    signal_send_pgid((uint32_t)-pid, (int)sig);
    return 0;
  }

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  bool found = false;
  uint32_t sender_pid = current ? (current->tgid ? current->tgid : current->tid) : 0;
  klog_puts("[SIGNAL] sys_kill sender=");
  klog_uint64(sender_pid);
  klog_puts(" target=");
  klog_uint64(pid);
  klog_puts(" sig=");
  klog_uint64(sig);
  klog_puts("\n");
  while (t) {
    if (t->tgid == (uint32_t)pid || t->tid == (uint32_t)pid) {
      if (current && current->euid != 0 && current->uid != t->uid &&
          current->euid != t->uid) {
        spinlock_release(&tid_lock);
        return (uint64_t)-1;
      }
      found = true;
      t->pending_signals |= (1ULL << (sig - 1));
      t->signal_sender_pid[sig - 1] = sender_pid;
      signal_notify_thread(t, (int)sig);
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
  return found ? 0 : (uint64_t)-3;
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

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t) {
    if (t->tgid == curr->tgid && (t->pending_signals & mask)) {
      pending = t->pending_signals & mask;
      break;
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
  return pending;
}

static bool consume_signal_tgid(struct thread *curr, int sig, uint32_t *out_sender_pid) {
  uint64_t bit = (1ULL << (sig - 1));
  uint32_t sender = 0;
  bool consumed = false;

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t) {
    if (t->tgid == curr->tgid && (t->pending_signals & bit)) {
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
    klog_puts("[SIGNALFD] size too small: ");
    klog_uint64(size);
    klog_puts(" node=");
    klog_uint64((uint64_t)node);
    klog_puts("\n");
    return (uint32_t)-22; // EINVAL
  }

  signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
  struct thread *t = sched_get_current();

  while (1) {
    uint64_t pending = get_pending_signals_tgid(t, ctx->mask);
    if (pending) {
      int sig = 0;
      for (int i = 0; i < 64; i++) {
        if (pending & (1ULL << i)) {
          sig = i + 1;
          break;
        }
      }
      uint32_t sender_pid = 0;
      if (sig && consume_signal_tgid(t, sig, &sender_pid)) {
        klog_puts("[SIGNALFD] read sig=");
        klog_uint64(sig);
        klog_puts(" from sender_pid=");
        klog_uint64(sender_pid);
        klog_puts("\n");
        struct signalfd_siginfo info;
        memset(&info, 0, sizeof(info));
        info.ssi_signo = sig;
        info.ssi_pid = sender_pid;
        memcpy(buffer, &info, sizeof(info));
        return sizeof(info);
      }
    }

    // No signals, block if needed
    // (In a real OS we'd check O_NONBLOCK, but for now we assume blocking)
    wait_queue_entry_t entry = {.thread = t, .next = NULL};
    wait_queue_add(&ctx->wq, &entry);
    t->state = THREAD_BLOCKED;

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

// signal_notify_thread must be defined AFTER signalfd_ctx_t and signalfd_read
void signal_notify_thread(struct thread *t, int sig) {
  if (!t || sig <= 0 || sig > 64)
    return;

  // Wake up thread if it's sleeping/blocked
  if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
    sched_wakeup(t);
  }

  /* Exit teardown clears the shared descriptor table before every possible
   * asynchronous signal source (notably ITIMER_REAL) has observed the dead
   * state.  There can be no signalfd to notify once the table is detached. */
  if (!t->fds)
    return;

  // Find all signalfds in this thread and wake them
  for (int i = 0; i < MAX_FDS; i++) {
    vfs_node_t *node = t->fds[i];
    if (node && node->read == signalfd_read) {
      signalfd_ctx_t *ctx = (signalfd_ctx_t *)node->device;
      if (ctx && (ctx->mask & (1ULL << (sig - 1)))) {
        // Wake up poll() and read() waiters
        wait_queue_wake_all(&ctx->wq);
        // Wake up epoll() waiters
        epoll_notify_event(node, POLLIN);
      }
    }
  }
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
  if ((int64_t)fd >= 0 && fd < MAX_FDS && t->fds[fd]) {
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
  if (sig == 0)
    return 0;

  struct thread *target = sched_get_thread_by_tid((uint32_t)tid);
  if (!target) {
    /* Fallback: deliver to current thread (single-threaded process) */
    target = sched_get_current();
    if (!target)
      return (uint64_t)-3; /* ESRCH */
  }
  struct thread *sender = sched_get_current();
  uint32_t sender_pid = sender ? (sender->tgid ? sender->tgid : sender->tid) : 0;
  target->pending_signals |= (1ULL << (sig - 1));
  target->signal_sender_pid[sig - 1] = sender_pid;
  signal_notify_thread(target, (int)sig);
  return 0;
}

void syscall_register_signal(void) {
  syscall_register(SYS_RT_SIGACTION, sys_rt_sigaction);
  syscall_register(SYS_RT_SIGPROCMASK, sys_rt_sigprocmask);
  syscall_register(SYS_RT_SIGSUSPEND, sys_rt_sigsuspend);
  syscall_register(SYS_RT_SIGTIMEDWAIT, sys_rt_sigtimedwait);
  syscall_register_raw(SYS_RT_SIGRETURN, sys_rt_sigreturn);
  syscall_register(SYS_PAUSE, sys_pause);
  syscall_register(SYS_SIGALTSTACK, sys_sigaltstack);
  syscall_register(SYS_TKILL, sys_tkill);
  syscall_register(SYS_TGKILL, sys_tgkill);
  syscall_register(SYS_KILL, sys_kill);
  syscall_register(SYS_SIGNALFD, sys_signalfd);
  syscall_register(SYS_SIGNALFD4, sys_signalfd4);
}
