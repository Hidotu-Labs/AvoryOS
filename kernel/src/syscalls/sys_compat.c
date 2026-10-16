/* Initial Linux i386 ABI. Only forward calls whose user layouts are identical;
 * unsupported compat structures return ENOSYS rather than corrupting memory.
 * Numbering: Linux arch/x86/entry/syscalls/syscall_32.tbl. */
#include "sys_io_shared.h"
#include "../cpu/isr.h"
#include "../cpu/features.h"
#define PAGE_SIZE 4096
extern void syscall_dispatcher(struct syscall_regs *regs);
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../console/klog.h"
#include "../sched/elf.h"
#include "../sched/seccomp.h"

static int compat_syscall_number(uint32_t number) {
  switch (number) {
  case 1: return SYS_EXIT;
  case 2: return SYS_FORK;
  case 3: return SYS_READ;
  case 4: return SYS_WRITE;
  case 5: return SYS_OPEN;
  case 6: return SYS_CLOSE;
  case 7: return SYS_WAIT4;
  case 9: return SYS_LINK;
  case 10: return SYS_UNLINK;
  case 11: return SYS_EXECVE;
  case 12: return SYS_CHDIR;
  case 15: return SYS_CHMOD;
  case 19: return SYS_LSEEK;
  case 20: return SYS_GETPID;
  case 33: return SYS_ACCESS;
  case 37: return SYS_KILL;
  case 38: return SYS_RENAME;
  case 39: return SYS_MKDIR;
  case 40: return SYS_RMDIR;
  case 41: return SYS_DUP;
  case 42: return SYS_PIPE;
  case 45: return SYS_BRK;
  case 55: case 221: return SYS_FCNTL;
  case 57: return SYS_SETPGID;
  case 60: return SYS_UMASK;
  case 61: return SYS_CHROOT;
  case 63: return SYS_DUP2;
  case 64: return SYS_GETPPID;
  case 65: return SYS_GETPGRP;
  case 66: return SYS_SETSID;
  case 83: return SYS_SYMLINK;
  case 85: return SYS_READLINK;
  case 90: case 192: return SYS_MMAP;
  case 91: return SYS_MUNMAP;
  case 93: return SYS_FTRUNCATE;
  case 94: return SYS_FCHMOD;
  case 118: return SYS_FSYNC;
  case 120: return SYS_CLONE;
  case 122: return SYS_UNAME;
  case 125: return SYS_MPROTECT;
  case 132: return SYS_GETPGID;
  case 133: return SYS_FCHDIR;
  case 143: return SYS_FLOCK;
  case 144: return SYS_MSYNC;
  case 147: return SYS_GETSID;
  case 148: return SYS_FDATASYNC;
  case 150: return SYS_MLOCK;
  case 151: return SYS_MUNLOCK;
  case 158: return SYS_SCHED_YIELD;
  case 168: return SYS_POLL;
  case 175: return SYS_RT_SIGPROCMASK;
  case 180: return SYS_PREAD64;
  case 181: return SYS_PWRITE64;
  case 183: return SYS_GETCWD;
  case 190: return SYS_VFORK;
  case 199: return SYS_GETUID;
  case 200: return SYS_GETGID;
  case 201: return SYS_GETEUID;
  case 202: return SYS_GETEGID;
  case 213: return SYS_SETUID;
  case 214: return SYS_SETGID;
  case 219: return SYS_MADVISE;
  case 220: return SYS_GETDENTS64;
  case 224: return SYS_GETTID;
  case 252: return SYS_EXIT_GROUP;
  case 258: return SYS_SET_TID_ADDRESS;
  case 295: return SYS_OPENAT;
  case 301: return SYS_UNLINKAT;
  case 302: return SYS_RENAMEAT;
  case 303: return SYS_LINKAT;
  case 304: return SYS_SYMLINKAT;
  case 305: return SYS_READLINKAT;
  case 306: return SYS_FCHMODAT;
  case 307: return SYS_FACCESSAT;
  case 330: return SYS_DUP3;
  case 331: return SYS_PIPE2;
  case 355: return SYS_GETRANDOM;
  case 403: return SYS_CLOCK_GETTIME; /* i386 __kernel_timespec (time64) */
  default: return -1;
  }
}

/* Keep the unmodified i386 registers in the return shadows for fork/clone;
 * their arguments use a different register order than x86-64. */
static int compat_prepare(struct registers *r, struct syscall_regs *s) {
  uint32_t nr = r->rax;
  int native = compat_syscall_number(nr);
  if (native < 0) return -38;
  *s = (struct syscall_regs){
      .rdi=(uint32_t)r->rbx, .rsi=(uint32_t)r->rcx,
      .rdx=(uint32_t)r->rdx, .r10=(uint32_t)r->rsi,
      .r8=(uint32_t)r->rdi, .r9=(uint32_t)r->rbp,
      .rax=(uint64_t)native, .rbx=(uint32_t)r->rbx, .rbp=(uint32_t)r->rbp,
      .rip=r->rip, .rsp=r->rsp, .rflags=r->rflags, .user_cs=r->cs,
      .user_rcx=(uint32_t)r->rcx, .user_rsi=(uint32_t)r->rsi,
      .user_rdi=(uint32_t)r->rdi };
  if (nr == 55 || nr == 221) {
    /* Lock commands use i386 flock layouts; scalar commands are identical. */
    if (s->rsi > 4 && s->rsi != 1030) return -38;
  }
  if (nr == 7) { s->rdi=(int32_t)r->rbx; s->r10=0; }
  if (nr == 37) s->rdi=(int32_t)r->rbx;
  if (nr == 19) s->rsi=(int32_t)r->rcx;
  if (nr == 168) s->rdx=(int32_t)r->rdx; /* poll timeout, including -1 */
  if (nr == 90) {
    uint32_t args[6];
    if (copy_from_user(args, (void *)(uintptr_t)(uint32_t)r->rbx, sizeof(args)))
      return -14;
    s->rdi=args[0]; s->rsi=args[1]; s->rdx=args[2];
    s->r10=args[3]; s->r8=(int32_t)args[4]; s->r9=args[5];
  }
  if (nr == 192) {
    if (s->r9 > UINT32_MAX / PAGE_SIZE) return -75;
    s->r9 *= PAGE_SIZE; /* mmap2 takes a page offset, not bytes. */
    s->r8=(int32_t)r->rdi;
  }
  if (nr == 120) {
    if (s->rdi & CLONE_SETTLS) return -38; /* user_desc TLS needs its own ABI */
    s->r10=(uint32_t)r->rdi; /* child_tid */
    s->r8=0;
  }
  if (nr == 180 || nr == 181)
    s->r10=(uint32_t)r->rsi | ((uint64_t)(uint32_t)r->rdi << 32);
  return 0;
}

void syscall_compat_dispatch(struct registers *regs) {
  user_access_begin();
  hal_irq_enable();
  struct syscall_regs native;
  uint32_t number = regs->rax;
  struct thread *task = sched_get_current();
  struct syscall_regs filtered = {.rax = number, .rip = regs->rip};
  uint64_t args[6] = {(uint32_t)regs->rbx, (uint32_t)regs->rcx,
      (uint32_t)regs->rdx, (uint32_t)regs->rsi, (uint32_t)regs->rdi,
      (uint32_t)regs->rbp};
  if (!seccomp_syscall(task, &filtered, number, 0x40000003, args)) {
    regs->rax = (uint32_t)filtered.rax;
    goto compat_finished;
  }
  if (regs->cs != 0x1b) {
    regs->rax = (uint32_t)-38;
    goto compat_finished;
  }
  int err = compat_prepare(regs, &native);
  if (err) {
    regs->rax=(uint32_t)err;
    static uint32_t missing_logs;
    if (err == -38 && __atomic_add_fetch(&missing_logs, 1, __ATOMIC_RELAXED) <= 16)
      klogf("[COMPAT32] unsupported i386 syscall %u at %llx\n",
            number, (unsigned long long)regs->rip);
  } else {
    bool exec = native.rax == SYS_EXECVE;
    if (task) task->seccomp_compat_dispatch = true;
    syscall_dispatcher(&native);
    if (task) task->seccomp_compat_dispatch = false;
    if (exec && native.rax == 0) {
      /* exec can change both ABI and address space. Return to the new image
       * through this interrupt's IRETQ, including a new CS and stack. */
      *regs=(struct registers){ .rip=native.rip, .rsp=native.rsp,
          .rflags=native.rflags, .cs=native.user_cs, .ss=0x23, .int_no=0x80 };
    } else {
      regs->rax=(uint32_t)native.rax;
    }
  }
compat_finished:;
  struct thread *t = sched_get_current();
  if (t) __atomic_store_n(&t->in_syscall, false, __ATOMIC_RELAXED);
  /* Only default signal actions can be installed by this initial i386 ABI. */
  signal_deliver(regs);
  hal_irq_disable();
  user_access_end();
}
