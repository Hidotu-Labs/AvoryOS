#include "sys_ptrace.h"

#include "syscall.h"
#include "../cpu/features.h"
#include "../include/arch/uaccess.h"
#include "../include/linux/errno.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../mm/zram.h"
#include "../mm/tlb_shootdown.h"
#include "../sched/capability.h"
#include "../sched/pid_namespace.h"
#include "../sched/sched.h"
#include "../sched/sched_internal.h"
#include "../smp/cpu.h"

#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_PEEKUSR 3
#define PTRACE_POKETEXT 4
#define PTRACE_POKEDATA 5
#define PTRACE_POKEUSR 6
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_GETFPREGS 14
#define PTRACE_SETFPREGS 15
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_GETFPXREGS 18
#define PTRACE_SETFPXREGS 19
#define PTRACE_SYSCALL 24
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO 0x4202
#define PTRACE_SETSIGINFO 0x4203
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SETREGSET 0x4205
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define PTRACE_LISTEN 0x4208
#define PTRACE_GETSIGMASK 0x420a
#define PTRACE_SETSIGMASK 0x420b
#define PTRACE_GET_SYSCALL_INFO 0x420e

#define PTRACE_O_TRACESYSGOOD 1ULL
#define PTRACE_O_TRACEFORK (1ULL << 1)
#define PTRACE_O_TRACEVFORK (1ULL << 2)
#define PTRACE_O_TRACECLONE (1ULL << 3)
#define PTRACE_O_TRACEEXEC (1ULL << 4)
#define PTRACE_O_TRACEVFORKDONE (1ULL << 5)
#define PTRACE_O_TRACEEXIT (1ULL << 6)
#define PTRACE_O_EXITKILL (1ULL << 20)
#define PTRACE_O_MASK (0x000000ffULL | (1ULL << 20) | (1ULL << 21))
#define PTRACE_O_SUPPORTED (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | \
                            PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |     \
                            PTRACE_O_TRACEEXEC | PTRACE_O_TRACEVFORKDONE |  \
                            PTRACE_O_TRACEEXIT |                            \
                            PTRACE_O_EXITKILL)
#define PTRACE_EVENT_FORK 1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_EXIT 6
#define PTRACE_EVENT_STOP 128
#define PTRACE_SYSCALL_INFO_ENTRY 1
#define PTRACE_SYSCALL_INFO_EXIT 2
#define NT_PRSTATUS 1
#define NT_FPREGSET 2
#define NT_X86_XSTATE 0x202
#define SIGKILL_NUM 9
#define SIGSTOP_NUM 19
#define SIGTRAP_NUM 5
#define SI_USER_CODE 0
#define SI_KERNEL_CODE 128
#define USER_REG_COUNT 27
#define USER_REG_BYTES (USER_REG_COUNT * sizeof(uint64_t))
#define FPREG_BYTES 512
#define MAX_PROCESS_VM_IOV 1024

struct user_iovec {
  void *base;
  size_t len;
};

struct ptrace_syscall_info {
  uint8_t op;
  uint8_t reserved;
  uint16_t flags;
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t stack_pointer;
  union {
    struct {
      uint64_t nr;
      uint64_t args[6];
    } entry;
    struct {
      int64_t rval;
      uint8_t is_error;
    } exit;
    struct {
      uint64_t nr;
      uint64_t args[6];
      uint32_t ret_data;
      uint32_t reserved2;
    } seccomp;
  } data;
};

_Static_assert(sizeof(struct ptrace_syscall_info) == 88,
               "Linux x86-64 ptrace syscall info ABI");

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;

static inline uint64_t neg_errno(int error) {
  return (uint64_t)-(int64_t)error;
}

static struct thread *find_visible_tid_locked(struct thread *caller,
                                               uint32_t visible_tid) {
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (pid_namespace_tid(caller, t) == visible_tid)
      return t;
  }
  return NULL;
}

static bool ptrace_access_allowed(const struct thread *caller,
                                  const struct thread *target) {
  if (!caller || !target)
    return false;
  if (capability_has((struct thread *)caller, CAP_SYS_PTRACE))
    return true;
  return caller->uid == target->uid && caller->uid == target->suid &&
         caller->euid == target->uid && caller->euid == target->suid;
}

static void user_regs_from_syscall(uint64_t out[USER_REG_COUNT],
                                   const struct syscall_regs *r,
                                   const struct thread *t) {
  memset(out, 0, USER_REG_BYTES);
  out[0] = r->r15; out[1] = r->r14; out[2] = r->r13; out[3] = r->r12;
  out[4] = r->rbp; out[5] = r->rbx; out[6] = r->rflags;
  out[7] = r->r10; out[8] = r->r9; out[9] = r->r8; out[10] = r->rax;
  out[11] = r->rip; out[12] = r->rdx; out[13] = r->rsi; out[14] = r->rdi;
  out[15] = t->last_syscall_num; out[16] = r->rip;
  out[17] = r->user_cs; out[18] = r->rflags; out[19] = r->rsp;
  out[20] = 0x23; out[21] = t->fs_base; out[22] = t->gs_base;
  out[23] = out[24] = out[25] = out[26] = 0;
}

static void user_regs_from_isr(uint64_t out[USER_REG_COUNT],
                               const struct registers *r,
                               const struct thread *t) {
  memset(out, 0, USER_REG_BYTES);
  out[0] = r->r15; out[1] = r->r14; out[2] = r->r13; out[3] = r->r12;
  out[4] = r->rbp; out[5] = r->rbx; out[6] = r->r11; out[7] = r->r10;
  out[8] = r->r9; out[9] = r->r8; out[10] = r->rax; out[11] = r->rcx;
  out[12] = r->rdx; out[13] = r->rsi; out[14] = r->rdi;
  out[15] = t->last_syscall_num; out[16] = r->rip; out[17] = r->cs;
  out[18] = r->rflags; out[19] = r->rsp; out[20] = r->ss;
  out[21] = t->fs_base; out[22] = t->gs_base;
}

static void user_regs_to_syscall(const uint64_t in[USER_REG_COUNT],
                                 struct syscall_regs *r,
                                 struct thread *t) {
  r->r15 = in[0]; r->r14 = in[1]; r->r13 = in[2]; r->r12 = in[3];
  r->rbp = in[4]; r->rbx = in[5]; r->rflags = in[18]; r->r10 = in[7];
  r->r9 = in[8]; r->r8 = in[9]; r->rax = in[10]; r->rip = in[16];
  r->rdx = in[12]; r->rsi = in[13]; r->rdi = in[14]; r->rsp = in[19];
  if (t->ptrace_syscall_entry_stop)
    r->rax = in[15];
  t->fs_base = in[21]; t->gs_base = in[22];
}

static void user_regs_to_isr(const uint64_t in[USER_REG_COUNT],
                             struct registers *r, struct thread *t) {
  r->r15 = in[0]; r->r14 = in[1]; r->r13 = in[2]; r->r12 = in[3];
  r->rbp = in[4]; r->rbx = in[5]; r->r11 = in[6]; r->r10 = in[7];
  r->r9 = in[8]; r->r8 = in[9]; r->rax = in[10]; r->rcx = in[11];
  r->rdx = in[12]; r->rsi = in[13]; r->rdi = in[14]; r->rip = in[16];
  r->cs = in[17]; r->rflags = in[18]; r->rsp = in[19]; r->ss = in[20];
  t->fs_base = in[21]; t->gs_base = in[22];
}

static void syscall_info_entry(struct thread *t, const struct syscall_regs *r) {
  struct ptrace_syscall_info info;
  memset(&info, 0, sizeof(info));
  info.op = PTRACE_SYSCALL_INFO_ENTRY;
  info.arch = 0xC000003E;
  info.instruction_pointer = r->rip;
  info.stack_pointer = r->rsp;
  info.data.entry.nr = r->rax;
  info.data.entry.args[0] = r->rdi; info.data.entry.args[1] = r->rsi;
  info.data.entry.args[2] = r->rdx; info.data.entry.args[3] = r->r10;
  info.data.entry.args[4] = r->r8; info.data.entry.args[5] = r->r9;
  memcpy(t->ptrace_syscall_info, &info, sizeof(info));
}

static void syscall_info_exit(struct thread *t, const struct syscall_regs *r) {
  struct ptrace_syscall_info info;
  memset(&info, 0, sizeof(info));
  info.op = PTRACE_SYSCALL_INFO_EXIT;
  info.arch = 0xC000003E;
  info.instruction_pointer = r->rip;
  info.stack_pointer = r->rsp;
  info.data.exit.rval = (int64_t)r->rax;
  info.data.exit.is_error = (int64_t)r->rax < 0 && (int64_t)r->rax >= -4095;
  memcpy(t->ptrace_syscall_info, &info, sizeof(info));
}

/* The stop frame remains on the tracee's kernel stack while it is blocked.
 * The tracer updates the private register snapshot and publishes the resume
 * under tid_lock before waking this loop. */
static int ptrace_stop_frame(struct registers *regs,
                             struct syscall_regs *sysregs, int signal,
                             int event, bool signal_delivery,
                             const void *siginfo) {
  struct thread *task = sched_get_current();
  if (!task)
    return -1;

  struct thread *tracer = NULL;
  spinlock_acquire(&tid_lock);
  if (!task->tracer_tid) {
    spinlock_release(&tid_lock);
    return -1;
  }
  if (sysregs)
    user_regs_from_syscall(task->ptrace_regs, sysregs, task);
  else if (regs)
    user_regs_from_isr(task->ptrace_regs, regs, task);
  task->ptrace_live_regs = regs;
  task->ptrace_live_syscall_regs = sysregs;
  task->ptrace_stop_signal = signal;
  task->ptrace_stop_event = event;
  task->ptrace_signal_delivery = signal_delivery;
  task->ptrace_stop_waited = false;
  task->ptrace_stopped = true;
  task->ptrace_stop_pending = false;
  task->ptrace_pending_event = 0;
  if (siginfo)
    memcpy(task->ptrace_siginfo, siginfo, sizeof(task->ptrace_siginfo));
  else {
    memset(task->ptrace_siginfo, 0, sizeof(task->ptrace_siginfo));
    ((int32_t *)task->ptrace_siginfo)[0] = signal;
    ((int32_t *)task->ptrace_siginfo)[2] = SI_KERNEL_CODE;
  }
  task->state = THREAD_BLOCKED;
  tracer = find_thread_by_tid_locked(task->tracer_tid);
  spinlock_release(&tid_lock);
  if (tracer)
    sched_wakeup(tracer);

  for (;;) {
    sched_yield();
    spinlock_acquire(&tid_lock);
    if (!task->ptrace_stopped) {
      int resume_signal = task->ptrace_resume_signal;
      task->ptrace_live_regs = NULL;
      task->ptrace_live_syscall_regs = NULL;
      task->ptrace_resume_signal = 0;
      spinlock_release(&tid_lock);
      return resume_signal;
    }
    task->state = THREAD_BLOCKED;
    spinlock_release(&tid_lock);
  }
}

void ptrace_syscall_enter(struct thread *task, struct syscall_regs *regs) {
  if (!task || !regs || !task->tracer_tid || !task->ptrace_syscall_trace)
    return;
  syscall_info_entry(task, regs);
  task->ptrace_syscall_entry_stop = true;
  (void)ptrace_stop_frame(NULL, regs, SIGTRAP_NUM, 0, false, NULL);
  task->ptrace_syscall_entry_stop = false;
}

void ptrace_syscall_exit(struct thread *task, struct syscall_regs *regs) {
  if (!task || !regs || !task->tracer_tid || !task->ptrace_syscall_trace)
    return;
  syscall_info_exit(task, regs);
  task->ptrace_syscall_exit_stop = true;
  (void)ptrace_stop_frame(NULL, regs, SIGTRAP_NUM, 0, false, NULL);
  task->ptrace_syscall_exit_stop = false;
}

void ptrace_user_return_stop(struct registers *regs) {
  struct thread *task = sched_get_current();
  if (!task || !regs || !task->ptrace_stop_pending)
    return;
  int event = task->ptrace_pending_event ? (int)task->ptrace_pending_event
                                         : PTRACE_EVENT_STOP;
  (void)ptrace_stop_frame(regs, NULL, SIGTRAP_NUM, event, false, NULL);
}

void ptrace_vfork_done(struct thread *parent, uint32_t child_tid) {
  if (!parent)
    return;
  spinlock_acquire(&tid_lock);
  if (parent->tracer_tid &&
      (parent->ptrace_options & PTRACE_O_TRACEVFORKDONE)) {
    parent->ptrace_message = child_tid;
    parent->ptrace_pending_event = 5; /* PTRACE_EVENT_VFORK_DONE */
    parent->ptrace_stop_pending = true;
  }
  spinlock_release(&tid_lock);
}

void ptrace_fork_event(struct syscall_regs *regs, struct thread *child,
                       uint32_t child_tid, uint64_t clone_flags) {
  struct thread *parent = sched_get_current();
  if (!parent || !child || !regs || (clone_flags & CLONE_UNTRACED))
    return;
  uint64_t event_option;
  int event;
  if (clone_flags & CLONE_VFORK) {
    event = PTRACE_EVENT_VFORK;
    event_option = PTRACE_O_TRACEVFORK;
  } else if (!(clone_flags & CLONE_THREAD) &&
             (clone_flags & 0xff) == SIGCHLD) {
    event = PTRACE_EVENT_FORK;
    event_option = PTRACE_O_TRACEFORK;
  } else {
    event = PTRACE_EVENT_CLONE;
    event_option = PTRACE_O_TRACECLONE;
  }

  spinlock_acquire(&tid_lock);
  uint32_t tracer_tid = parent->tracer_tid;
  if (!tracer_tid || !(parent->ptrace_options & event_option)) {
    spinlock_release(&tid_lock);
    return;
  }
  child->tracer_tid = tracer_tid;
  child->ptrace_options = parent->ptrace_options;
  child->ptrace_seized = parent->ptrace_seized;
  child->ptrace_exit_waited = false;
  child->pending_signals |= 1ULL << (SIGSTOP_NUM - 1);
  child->thread_directed_signals |= 1ULL << (SIGSTOP_NUM - 1);
  struct thread *tracer = find_thread_by_tid_locked(tracer_tid);
  child->signal_sender_pid[SIGSTOP_NUM - 1] =
      tracer ? (tracer->tgid ? tracer->tgid : tracer->tid) : parent->tid;
  child->signal_sender_code[SIGSTOP_NUM - 1] = SI_USER_CODE;
  parent->ptrace_message = child_tid;
  spinlock_release(&tid_lock);
  (void)ptrace_stop_frame(NULL, regs, SIGTRAP_NUM, event, false, NULL);
}

void ptrace_exec_event(struct syscall_regs *regs) {
  struct thread *task = sched_get_current();
  if (!task || !regs)
    return;
  spinlock_acquire(&tid_lock);
  if (!task->tracer_tid) {
    spinlock_release(&tid_lock);
    return;
  }
  bool event_stop = (task->ptrace_options & PTRACE_O_TRACEEXEC) != 0;
  if (event_stop) {
    task->ptrace_message = 0;
  } else {
    task->pending_signals |= 1ULL << (SIGTRAP_NUM - 1);
    task->thread_directed_signals |= 1ULL << (SIGTRAP_NUM - 1);
    task->signal_sender_pid[SIGTRAP_NUM - 1] = 0;
    task->signal_sender_code[SIGTRAP_NUM - 1] = SI_KERNEL_CODE;
  }
  spinlock_release(&tid_lock);
  if (event_stop)
    (void)ptrace_stop_frame(NULL, regs, SIGTRAP_NUM, PTRACE_EVENT_EXEC, false,
                            NULL);
}

void ptrace_exit_event(struct thread *tracee, int status) {
  if (!tracee)
    return;
  spinlock_acquire(&tid_lock);
  bool event_stop = tracee->tracer_tid &&
                    (tracee->ptrace_options & PTRACE_O_TRACEEXIT);
  if (event_stop) {
    tracee->ptrace_message = tracee->termination_signal
        ? tracee->termination_signal : ((uint64_t)(status & 0xff) << 8);
  }
  spinlock_release(&tid_lock);
  if (event_stop)
    (void)ptrace_stop_frame(NULL, NULL, SIGTRAP_NUM, PTRACE_EVENT_EXIT, false,
                            NULL);
}

void ptrace_tracer_exit(struct thread *tracer) {
  if (!tracer)
    return;
  for (;;) {
    struct thread *target = NULL;
    bool wake = false;
    spinlock_acquire(&tid_lock);
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t->tracer_tid != tracer->tid)
        continue;
      target = t;
      bool exit_kill = (t->ptrace_options & (1ULL << 20)) != 0;
      if (t->ptrace_stopped) {
        if (t->ptrace_live_syscall_regs)
          user_regs_to_syscall(t->ptrace_regs, t->ptrace_live_syscall_regs, t);
        if (t->ptrace_live_regs)
          user_regs_to_isr(t->ptrace_regs, t->ptrace_live_regs, t);
        t->ptrace_resume_signal = exit_kill ? SIGKILL_NUM :
            (t->ptrace_signal_delivery && t->ptrace_stop_signal != SIGSTOP_NUM
                 ? t->ptrace_stop_signal : 0);
        t->ptrace_resume_pid = tracer->tgid ? tracer->tgid : tracer->tid;
        if (t->ptrace_resume_signal && !t->ptrace_signal_delivery) {
          unsigned sig = (unsigned)t->ptrace_resume_signal;
          t->pending_signals |= 1ULL << (sig - 1);
          t->thread_directed_signals |= 1ULL << (sig - 1);
          t->signal_sender_pid[sig - 1] = tracer->tgid ? tracer->tgid : tracer->tid;
          t->signal_sender_code[sig - 1] = SI_USER_CODE;
        }
        t->ptrace_stopped = false;
        wake = true;
      } else if (exit_kill) {
        t->pending_signals |= 1ULL << (SIGKILL_NUM - 1);
        t->thread_directed_signals |= 1ULL << (SIGKILL_NUM - 1);
        t->signal_sender_pid[SIGKILL_NUM - 1] = tracer->tgid ? tracer->tgid : tracer->tid;
        t->signal_sender_code[SIGKILL_NUM - 1] = SI_USER_CODE;
        wake = true;
      }
      if (t->signal_sender_pid[SIGSTOP_NUM - 1] ==
          (tracer->tgid ? tracer->tgid : tracer->tid)) {
        t->pending_signals &= ~(1ULL << (SIGSTOP_NUM - 1));
        t->thread_directed_signals &= ~(1ULL << (SIGSTOP_NUM - 1));
      }
      t->tracer_tid = 0;
      t->ptrace_options = 0;
      t->ptrace_seized = false;
      t->ptrace_syscall_trace = false;
      t->ptrace_stop_pending = false;
      t->ptrace_pending_event = 0;
      break;
    }
    spinlock_release(&tid_lock);
    if (!target)
      break;
    if (wake)
      sched_wakeup(target);
  }
}

void ptrace_tracee_exit(struct thread *tracee) {
  if (!tracee)
    return;
  struct thread *tracer = NULL;
  spinlock_acquire(&tid_lock);
  if (tracee->tracer_tid) {
    tracee->ptrace_exit_waited = false;
    tracer = find_thread_by_tid_locked(tracee->tracer_tid);
  }
  spinlock_release(&tid_lock);
  if (tracer)
    sched_wakeup(tracer);
}

int ptrace_signal_delivery_stop(struct registers *regs, int signal,
                                const void *siginfo) {
  struct thread *task = sched_get_current();
  if (!task || signal == SIGKILL_NUM || !task->tracer_tid)
    return -1;
  int replacement = ptrace_stop_frame(regs, NULL, signal, 0, true, siginfo);
  if (replacement > 0 && replacement <= 64) {
    task->signal_sender_pid[replacement - 1] = task->ptrace_resume_pid;
    task->signal_sender_code[replacement - 1] = SI_USER_CODE;
  }
  return replacement;
}

static bool tracee_mm_get(struct thread *caller, uint32_t visible_tid,
                          bool stopped_only,
                          struct thread **task_out, struct mm_struct **mm_out,
                          uint64_t *cr3_out) {
  struct thread *target = NULL;
  struct mm_struct *mm = NULL;
  uint64_t cr3 = 0;
  spinlock_acquire(&tid_lock);
  target = find_visible_tid_locked(caller, visible_tid);
  if (target && target->state != THREAD_DEAD &&
      target->state != THREAD_ZOMBIE && ptrace_access_allowed(caller, target) &&
      (!stopped_only || (target->tracer_tid == caller->tid &&
                         target->ptrace_stopped)) &&
      target->mm) {
    mm = target->mm;
    cr3 = target->cr3;
    __atomic_add_fetch(&mm->ref_count, 1, __ATOMIC_ACQ_REL);
  }
  spinlock_release(&tid_lock);
  if (!mm)
    return false;
  *task_out = target;
  *mm_out = mm;
  *cr3_out = cr3;
  return true;
}

/* PTRACE_POKEDATA and process_vm_writev have Linux's forced-write behavior
 * for private COW pages. Create a private 4 KiB copy before changing it; leave
 * the user's original protection bits intact so debugger writes do not turn
 * read-only text into executable writable memory. */
static int force_private_write_page(struct mm_struct *mm, uint64_t cr3,
                                    uint64_t va) {
  bool changed = false;
  uint64_t old_phys = 0;
  spinlock_acquire(&mm->lock);
  vmm_lock_acquire_for((uint64_t *)cr3, va);
  uint64_t *leaf = vmm_get_leaf_entry_ptr((uint64_t *)cr3, va);
  if (!leaf || !(*leaf & PAGE_FLAG_PRESENT) || !(*leaf & PAGE_FLAG_USER)) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return -14;
  }
  if (vmm_is_huge_page((uint64_t *)cr3, va)) {
    bool writable = (*leaf & PAGE_FLAG_RW) != 0;
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return writable ? 0 : -5;
  }
  if (*leaf & PAGE_FLAG_RW) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return 0;
  }
  struct vma *vma = vma_find(&mm->vmas, va);
  if (!vma || !(vma->flags & MAP_PRIVATE)) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return -14;
  }
  old_phys = *leaf & PAGE_MASK;
  if (!pmm_is_managed(old_phys)) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return -5;
  }
  void *new_page = pmm_alloc_page();
  if (!new_page) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return -12;
  }
  memcpy((void *)((uint64_t)new_page + pmm_get_hhdm_offset()),
         (void *)(old_phys + pmm_get_hhdm_offset()), PAGE_SIZE);
  *leaf = ((uint64_t)new_page & PAGE_MASK) | (*leaf & ~PAGE_MASK &
            ~(PAGE_FLAG_COW | PAGE_FLAG_REFAULT));
  changed = true;
  vmm_lock_release();
  spinlock_release(&mm->lock);
  if (changed) {
    tlb_shootdown_page_for_pcid(va, cr3, mm->pcid);
    if (old_phys != pmm_get_zero_page_phys() && pmm_is_managed(old_phys))
      pmm_decref((void *)old_phys);
  }
  return 0;
}

static size_t remote_memory_copy(struct mm_struct *mm, uint64_t cr3,
                                 uint64_t remote, void *local, size_t length,
                                 bool write_remote) {
  uint8_t *bytes = (uint8_t *)local;
  size_t done = 0;
  if (!is_user_range((void *)remote, length))
    return 0;
  while (done < length) {
    uint64_t va = remote + done;
    size_t chunk = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
    if (chunk > length - done)
      chunk = length - done;
    int fault = zram_fault(mm, cr3, va, false, false);
    if (fault < 0)
      break;
    if (write_remote && force_private_write_page(mm, cr3, va) < 0)
      break;
    vmm_lock_acquire_for((uint64_t *)cr3, va);
    uint64_t *leaf = vmm_get_leaf_entry_ptr((uint64_t *)cr3, va);
    if (!leaf || !(*leaf & PAGE_FLAG_PRESENT) || !(*leaf & PAGE_FLAG_USER)) {
      vmm_lock_release();
      break;
    }
    uint64_t phys = vmm_virt_to_phys((uint64_t *)cr3, va);
    if (!phys) {
      vmm_lock_release();
      break;
    }
    void *remote_ptr = (void *)(phys + pmm_get_hhdm_offset());
    if (write_remote)
      memcpy(remote_ptr, bytes + done, chunk);
    else
      memcpy(bytes + done, remote_ptr, chunk);
    vmm_lock_release();
    done += chunk;
  }
  return done;
}

static uint64_t do_ptrace_memory(struct thread *caller, uint32_t pid,
                                 uint64_t remote, uint64_t data,
                                 bool write_remote) {
  struct thread *target;
  struct mm_struct *mm;
  uint64_t cr3;
  if (!tracee_mm_get(caller, pid, true, &target, &mm, &cr3))
    return neg_errno(ESRCH);
  (void)target;
  if (!is_user_range((void *)remote, sizeof(uint64_t))) {
    mm_put(mm, cr3);
    return neg_errno(EIO);
  }
  uint64_t word = data;
  size_t n = remote_memory_copy(mm, cr3, remote, &word, sizeof(word),
                                write_remote);
  mm_put(mm, cr3);
  if (n != sizeof(word))
    return neg_errno(EIO);
  if (!write_remote && copy_to_user((void *)data, &word, sizeof(word)))
    return neg_errno(EFAULT);
  return 0;
}

static uint64_t process_vm(uint64_t pid_val, uint64_t local_iov_ptr,
                           uint64_t liovcnt_val, uint64_t remote_iov_ptr,
                           uint64_t riovcnt_val, uint64_t flags,
                           bool write_remote) {
  struct thread *caller = sched_get_current();
  size_t liovcnt = (size_t)liovcnt_val;
  size_t riovcnt = (size_t)riovcnt_val;
  if (!caller)
    return neg_errno(ESRCH);
  if (flags || liovcnt > MAX_PROCESS_VM_IOV || riovcnt > MAX_PROCESS_VM_IOV)
    return neg_errno(EINVAL);
  if (!liovcnt || !riovcnt)
    return 0;
  struct user_iovec *local = kmalloc(liovcnt * sizeof(*local));
  struct user_iovec *remote = kmalloc(riovcnt * sizeof(*remote));
  if (!local || !remote) {
    if (local) kfree(local);
    if (remote) kfree(remote);
    return neg_errno(ENOMEM);
  }
  if (copy_from_user(local, (void *)local_iov_ptr, liovcnt * sizeof(*local)) ||
      copy_from_user(remote, (void *)remote_iov_ptr,
                     riovcnt * sizeof(*remote))) {
    kfree(local); kfree(remote);
    return neg_errno(EFAULT);
  }
  struct thread *target;
  struct mm_struct *mm;
  uint64_t cr3;
  if (!tracee_mm_get(caller, (uint32_t)pid_val, false, &target, &mm, &cr3)) {
    kfree(local); kfree(remote);
    return neg_errno(ESRCH);
  }
  (void)target;
  size_t total = 0, li = 0, ri = 0, lo = 0, ro = 0;
  while (li < liovcnt && ri < riovcnt) {
    size_t amount = local[li].len - lo;
    if (amount > remote[ri].len - ro)
      amount = remote[ri].len - ro;
    if (amount > 0x7ffff000U - total)
      amount = 0x7ffff000U - total;
    if (!amount)
      break;
    if (!is_user_range((uint8_t *)local[li].base + lo, amount) ||
        !is_user_range((void *)(remote[ri].base + ro), amount))
      break;
    size_t copied;
    if (write_remote) {
      uint8_t page[PAGE_SIZE];
      copied = 0;
      while (copied < amount) {
        size_t n = amount - copied;
        if (n > sizeof(page)) n = sizeof(page);
        if (copy_from_user(page, (uint8_t *)local[li].base + lo + copied, n))
          break;
        size_t wrote = remote_memory_copy(mm, cr3,
            (uint64_t)(uintptr_t)remote[ri].base + ro + copied,
            page, n, true);
        copied += wrote;
        if (wrote != n) break;
      }
    } else {
      uint8_t page[PAGE_SIZE];
      copied = 0;
      while (copied < amount) {
        size_t n = amount - copied;
        if (n > sizeof(page)) n = sizeof(page);
        size_t got = remote_memory_copy(mm, cr3,
            (uint64_t)(uintptr_t)remote[ri].base + ro + copied,
            page, n, false);
        if (got && copy_to_user((uint8_t *)local[li].base + lo + copied,
                                page, got))
          got = 0;
        copied += got;
        if (got != n) break;
      }
    }
    total += copied;
    if (copied != amount)
      break;
    lo += amount; ro += amount;
    if (lo == local[li].len) { li++; lo = 0; }
    if (ro == remote[ri].len) { ri++; ro = 0; }
  }
  mm_put(mm, cr3);
  kfree(local); kfree(remote);
  return total ? total : (li < liovcnt && ri < riovcnt ? neg_errno(EFAULT) : 0);
}

static int ptrace_regset_copy(struct thread *target, uint64_t note,
                              void *buffer, size_t *length, bool set) {
  size_t available;
  const void *source = NULL;
  void *destination = NULL;
  if (note == NT_PRSTATUS) {
    available = USER_REG_BYTES;
    source = target->ptrace_regs;
    destination = target->ptrace_regs;
  } else if (note == NT_FPREGSET) {
    available = FPREG_BYTES;
    source = target->fpu_state;
    destination = target->fpu_state;
  } else if (note == NT_X86_XSTATE) {
    available = cpu_has_xsave_flag ? sizeof(target->fpu_state) : FPREG_BYTES;
    source = target->fpu_state;
    destination = target->fpu_state;
  } else {
    return -EINVAL;
  }
  size_t amount = *length < available ? *length : available;
  if (set)
    memcpy(destination, buffer, amount);
  else
    memcpy(buffer, source, amount);
  *length = amount;
  return 0;
}

static bool set_user_register(uint64_t regs[USER_REG_COUNT], unsigned index,
                              uint64_t value) {
  if (index >= USER_REG_COUNT)
    return false;
  /* Linux protects the privilege and execution-mode bits in EFLAGS. */
  if (index == 18) {
    uint64_t allowed = 0x0000000000254dd5ULL;
    regs[index] = (regs[index] & ~allowed) | (value & allowed) | 0x202;
    return true;
  }
  if (index == 17 || index == 20 || (index >= 23 && index <= 26))
    return value == regs[index]; /* segment selectors are fixed by this ABI */
  regs[index] = value;
  return true;
}

static uint64_t ptrace_request(uint64_t request, uint64_t pid_val,
                               uint64_t addr, uint64_t data,
                               uint64_t a4, uint64_t a5) {
  (void)a4; (void)a5;
  struct thread *caller = sched_get_current();
  if (!caller)
    return neg_errno(ESRCH);
  if (request == PTRACE_TRACEME) {
    spinlock_acquire(&tid_lock);
    if (caller->tracer_tid) {
      spinlock_release(&tid_lock);
      return neg_errno(EPERM);
    }
    caller->tracer_tid = caller->parent ? caller->parent->tid : caller->tid;
    spinlock_release(&tid_lock);
    return 0;
  }

  if (!pid_val || pid_val > UINT32_MAX)
    return neg_errno(ESRCH);
  uint32_t pid = (uint32_t)pid_val;

  /* Stage all tracer-provided data before taking tid_lock. User copies may
   * fault and must never run while the global task-list lock is held. */
  uint64_t input_regs[USER_REG_COUNT];
  uint8_t input_fp[FPREG_BYTES];
  uint8_t input_siginfo[128];
  uint64_t input_sigmask = 0;
  struct user_iovec input_iov = {0};
  uint8_t input_regset[4096] __attribute__((aligned(64)));
  if (request == PTRACE_SETREGS &&
      copy_from_user(input_regs, (void *)data, sizeof(input_regs)))
    return neg_errno(EFAULT);
  if ((request == PTRACE_SETFPREGS || request == PTRACE_SETFPXREGS) &&
      copy_from_user(input_fp, (void *)data, sizeof(input_fp)))
    return neg_errno(EFAULT);
  if (request == PTRACE_SETSIGINFO &&
      copy_from_user(input_siginfo, (void *)data, sizeof(input_siginfo)))
    return neg_errno(EFAULT);
  if (request == PTRACE_SETSIGMASK) {
    if (addr != sizeof(uint64_t))
      return neg_errno(EINVAL);
    if (copy_from_user(&input_sigmask, (void *)data, sizeof(input_sigmask)))
      return neg_errno(EFAULT);
  }
  if (request == PTRACE_GETREGSET || request == PTRACE_SETREGSET) {
    if (copy_from_user(&input_iov, (void *)data, sizeof(input_iov)))
      return neg_errno(EFAULT);
    if (input_iov.len > sizeof(input_regset) ||
        (input_iov.len && !is_user_range(input_iov.base, input_iov.len)))
      return neg_errno(EFAULT);
    if (request == PTRACE_SETREGSET &&
        copy_from_user(input_regset, input_iov.base, input_iov.len))
      return neg_errno(EFAULT);
  }

  if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
    if (request == PTRACE_SEIZE && (data & ~PTRACE_O_SUPPORTED))
      return neg_errno(EINVAL);
    struct thread *target = NULL;
    spinlock_acquire(&tid_lock);
    target = find_visible_tid_locked(caller, pid);
    if (!target || target == caller || target->state == THREAD_DEAD ||
        target->state == THREAD_ZOMBIE) {
      spinlock_release(&tid_lock);
      return neg_errno(ESRCH);
    }
    if (!ptrace_access_allowed(caller, target)) {
      spinlock_release(&tid_lock);
      return neg_errno(EPERM);
    }
    if (target->tracer_tid) {
      spinlock_release(&tid_lock);
      return neg_errno(EPERM);
    }
    target->tracer_tid = caller->tid;
    target->ptrace_seized = request == PTRACE_SEIZE;
    target->ptrace_options = request == PTRACE_SEIZE ? data : 0;
    target->ptrace_syscall_trace = false;
    if (request == PTRACE_ATTACH) {
      target->pending_signals |= 1ULL << (SIGSTOP_NUM - 1);
      target->thread_directed_signals |= 1ULL << (SIGSTOP_NUM - 1);
      target->signal_sender_pid[SIGSTOP_NUM - 1] = caller->tgid ? caller->tgid : caller->tid;
      target->signal_sender_code[SIGSTOP_NUM - 1] = SI_USER_CODE;
    }
    spinlock_release(&tid_lock);
    if (request == PTRACE_ATTACH &&
        (target->state == THREAD_BLOCKED || target->state == THREAD_SLEEPING))
      sched_wakeup(target);
    return 0;
  }

  if (request == PTRACE_INTERRUPT) {
    struct thread *target = NULL;
    spinlock_acquire(&tid_lock);
    target = find_visible_tid_locked(caller, pid);
    if (!target || target->tracer_tid != caller->tid) {
      spinlock_release(&tid_lock);
      return neg_errno(ESRCH);
    }
    if (!target->ptrace_seized || target->ptrace_stopped) {
      spinlock_release(&tid_lock);
      return neg_errno(target->ptrace_stopped ? EIO : EIO);
    }
    target->ptrace_stop_pending = true;
    target->ptrace_pending_event = PTRACE_EVENT_STOP;
    spinlock_release(&tid_lock);
    if (target->state == THREAD_BLOCKED || target->state == THREAD_SLEEPING)
      sched_wakeup(target);
    return 0;
  }

  struct thread *target = NULL;
  spinlock_acquire(&tid_lock);
  target = find_visible_tid_locked(caller, pid);
  if (!target || target->tracer_tid != caller->tid) {
    spinlock_release(&tid_lock);
    return neg_errno(ESRCH);
  }
  bool stopped = target->ptrace_stopped;
  if (request == PTRACE_KILL && !stopped) {
    target->pending_signals |= 1ULL << (SIGKILL_NUM - 1);
    target->thread_directed_signals |= 1ULL << (SIGKILL_NUM - 1);
    spinlock_release(&tid_lock);
    sched_wakeup(target);
    return 0;
  }
  if (request == PTRACE_CONT || request == PTRACE_SYSCALL ||
      request == PTRACE_SINGLESTEP || request == PTRACE_DETACH ||
      request == PTRACE_KILL) {
    if (!stopped) {
      spinlock_release(&tid_lock);
      return neg_errno(ESRCH);
    }
    if (request != PTRACE_KILL && data > 64) {
      spinlock_release(&tid_lock);
      return neg_errno(EINVAL);
    }
    if (request == PTRACE_KILL) {
      data = SIGKILL_NUM;
      request = PTRACE_CONT;
    }
    target->ptrace_resume_signal = (int32_t)data;
    target->ptrace_resume_pid = caller->tgid ? caller->tgid : caller->tid;
    target->ptrace_syscall_trace = request == PTRACE_SYSCALL;
    if (request != PTRACE_SYSCALL)
      target->ptrace_syscall_exit_stop = false;
    if (request == PTRACE_SINGLESTEP)
      target->ptrace_regs[18] |= (1ULL << 8);
    else
      target->ptrace_regs[18] &= ~(1ULL << 8);
    if (target->ptrace_live_syscall_regs)
      user_regs_to_syscall(target->ptrace_regs,
                           target->ptrace_live_syscall_regs, target);
    if (target->ptrace_live_regs)
      user_regs_to_isr(target->ptrace_regs, target->ptrace_live_regs, target);
    bool queue_signal = data && !target->ptrace_signal_delivery;
    if (queue_signal) {
      target->pending_signals |= 1ULL << (data - 1);
      target->thread_directed_signals |= 1ULL << (data - 1);
      target->signal_sender_pid[data - 1] = target->ptrace_resume_pid;
      target->signal_sender_code[data - 1] = SI_USER_CODE;
    }
    bool detach = request == PTRACE_DETACH;
    if (detach) {
      target->tracer_tid = 0;
      target->ptrace_options = 0;
      target->ptrace_seized = false;
      target->ptrace_syscall_trace = false;
    }
    target->ptrace_stopped = false;
    target->ptrace_stop_waited = false;
    spinlock_release(&tid_lock);
    sched_wakeup(target);
    return 0;
  }

  if (!stopped) {
    spinlock_release(&tid_lock);
    return neg_errno(ESRCH);
  }

  uint64_t result = 0;
  switch (request) {
  case PTRACE_PEEKDATA:
  case PTRACE_PEEKTEXT:
    spinlock_release(&tid_lock);
    return do_ptrace_memory(caller, pid, addr, data, false);
  case PTRACE_POKEDATA:
  case PTRACE_POKETEXT:
    spinlock_release(&tid_lock);
    return do_ptrace_memory(caller, pid, addr, data, true);
  case PTRACE_GETREGS:
    memcpy(input_regs, target->ptrace_regs, USER_REG_BYTES);
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, input_regs, USER_REG_BYTES)
               ? neg_errno(EFAULT) : 0;
  case PTRACE_SETREGS: {
    uint64_t sanitized[USER_REG_COUNT];
    memcpy(sanitized, target->ptrace_regs, sizeof(sanitized));
    for (unsigned i = 0; i < USER_REG_COUNT; i++)
      if (!set_user_register(sanitized, i, input_regs[i])) {
        result = neg_errno(EIO); break;
      }
    if (!result)
      memcpy(target->ptrace_regs, sanitized, sizeof(sanitized));
    break;
  }
  case PTRACE_PEEKUSR: {
    if ((addr & 7) || addr >= USER_REG_BYTES) { result = neg_errno(EIO); break; }
    uint64_t value = target->ptrace_regs[addr / 8];
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, &value, sizeof(value))
               ? neg_errno(EFAULT) : 0;
  }
  case PTRACE_POKEUSR: {
    if ((addr & 7) || addr >= USER_REG_BYTES ||
        !set_user_register(target->ptrace_regs, (unsigned)(addr / 8), data))
      result = neg_errno(EIO);
    break;
  }
  case PTRACE_GETFPREGS:
  case PTRACE_GETFPXREGS:
    memcpy(input_fp, target->fpu_state, FPREG_BYTES);
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, input_fp, FPREG_BYTES)
               ? neg_errno(EFAULT) : 0;
  case PTRACE_SETFPREGS:
  case PTRACE_SETFPXREGS:
    memcpy(target->fpu_state, input_fp, FPREG_BYTES);
    break;
  case PTRACE_GETEVENTMSG:
    input_sigmask = target->ptrace_message;
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, &input_sigmask, sizeof(input_sigmask))
               ? neg_errno(EFAULT) : 0;
  case PTRACE_GETSIGINFO:
    memcpy(input_siginfo, target->ptrace_siginfo, sizeof(input_siginfo));
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, input_siginfo, sizeof(input_siginfo))
               ? neg_errno(EFAULT) : 0;
  case PTRACE_SETSIGINFO:
    if (*(int32_t *)input_siginfo <= 0 || *(int32_t *)input_siginfo > 64)
      result = neg_errno(EINVAL);
    else
      memcpy(target->ptrace_siginfo, input_siginfo, sizeof(input_siginfo));
    break;
  case PTRACE_SETOPTIONS:
    if (data & ~PTRACE_O_SUPPORTED)
      result = neg_errno(EINVAL);
    else
      target->ptrace_options = data;
    break;
  case PTRACE_GETREGSET:
  case PTRACE_SETREGSET: {
    size_t len = input_iov.len;
    bool set = request == PTRACE_SETREGSET;
    int rc = 0;
    if (set && addr == NT_PRSTATUS) {
      uint64_t sanitized[USER_REG_COUNT];
      memcpy(sanitized, target->ptrace_regs, sizeof(sanitized));
      size_t count = len < USER_REG_BYTES ? len / sizeof(uint64_t)
                                         : USER_REG_COUNT;
      uint64_t *incoming = (uint64_t *)input_regset;
      for (size_t i = 0; i < count; i++) {
        if (!set_user_register(sanitized, (unsigned)i, incoming[i])) {
          rc = -EIO;
          break;
        }
      }
      if (!rc)
        memcpy(input_regset, sanitized, count * sizeof(uint64_t));
    }
    if (!rc)
      rc = ptrace_regset_copy(target, addr, input_regset, &len, set);
    if (rc) { spinlock_release(&tid_lock); return neg_errno(-rc); }
    input_iov.len = len;
    spinlock_release(&tid_lock);
    if ((!set && copy_to_user(input_iov.base, input_regset, len)) ||
        copy_to_user((void *)data, &input_iov, sizeof(input_iov)))
      return neg_errno(EFAULT);
    return 0;
  }
  case PTRACE_GETSIGMASK: {
    if (addr != sizeof(uint64_t)) { result = neg_errno(EINVAL); break; }
    input_sigmask = target->signal_mask;
    spinlock_release(&tid_lock);
    return copy_to_user((void *)data, &input_sigmask, sizeof(input_sigmask))
               ? neg_errno(EFAULT) : 0;
  }
  case PTRACE_SETSIGMASK: {
    target->signal_mask = input_sigmask & ~((1ULL << (SIGKILL_NUM - 1)) |
                                            (1ULL << (SIGSTOP_NUM - 1)));
    break;
  }
  case PTRACE_GET_SYSCALL_INFO: {
    size_t len = addr < sizeof(target->ptrace_syscall_info) ?
                 (size_t)addr : sizeof(target->ptrace_syscall_info);
    memcpy(input_regset, target->ptrace_syscall_info, len);
    spinlock_release(&tid_lock);
    if (copy_to_user((void *)data, input_regset, len))
      return neg_errno(EFAULT);
    return len;
  }
  case PTRACE_LISTEN:
    result = neg_errno(EIO); /* job-control group-stop/listen is not modeled */
    break;
  default:
    result = neg_errno(EINVAL);
    break;
  }
  spinlock_release(&tid_lock);
  return result;
}

static uint64_t sys_ptrace(uint64_t request, uint64_t pid, uint64_t addr,
                           uint64_t data, uint64_t a4, uint64_t a5) {
  return ptrace_request(request, pid, addr, data, a4, a5);
}

static uint64_t sys_process_vm_readv(uint64_t pid, uint64_t local_iov,
                                    uint64_t liovcnt, uint64_t remote_iov,
                                    uint64_t riovcnt, uint64_t flags) {
  return process_vm(pid, local_iov, liovcnt, remote_iov, riovcnt, flags, false);
}

static uint64_t sys_process_vm_writev(uint64_t pid, uint64_t local_iov,
                                     uint64_t liovcnt, uint64_t remote_iov,
                                     uint64_t riovcnt, uint64_t flags) {
  return process_vm(pid, local_iov, liovcnt, remote_iov, riovcnt, flags, true);
}

void syscall_register_ptrace(void) {
  syscall_register(SYS_PTRACE, sys_ptrace);
  syscall_register(310, sys_process_vm_readv);
  syscall_register(311, sys_process_vm_writev);
}
