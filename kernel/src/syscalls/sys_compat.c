/* Initial Linux i386 ABI. Only forward calls whose user layouts are identical;
 * unsupported compat structures return ENOSYS rather than corrupting memory.
 * Numbering: Linux arch/x86/entry/syscalls/syscall_32.tbl. */
#include "sys_io_shared.h"
#include "../cpu/isr.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../cpu/features.h"
#define PAGE_SIZE 4096
extern void syscall_dispatcher(struct syscall_regs *regs);
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../console/klog.h"
#include "../sched/elf.h"
#include "../sched/seccomp.h"
#include "../apic/lapic_timer.h"
#include "../drivers/timer/rtc.h"

int compat_clone_tls(uint64_t pointer, uint32_t *entry, uint32_t *base, uint64_t *value) {
  struct { uint32_t entry, base, limit, flags; } d;
  if (pointer > UINT32_MAX || copy_from_user(&d, (void *)pointer, sizeof(d))) return -14;
  unsigned contents = (d.flags >> 1) & 3;
  if (d.entry < 6 || d.entry > 8 || !(d.flags & 1) || contents > 1 ||
      (d.flags & 0x20) || (d.flags & ~0xffu)) return -22;
  uint64_t access = 0xf0 | (contents << 2) | ((d.flags & 8) ? 0 : 2);
  uint64_t gran = 4 | ((d.flags & 0x10) ? 8 : 0) | ((d.flags & 0x40) ? 1 : 0);
  *value = (d.limit & 0xffffULL) | ((d.base & 0xffffffULL) << 16) |
           (access << 40) | (((d.limit >> 16) & 15ULL) << 48) |
           (gran << 52) | ((uint64_t)(d.base >> 24) << 56);
  *entry = d.entry; *base = d.base;
  return 0;
}

static int compat_set_thread_area(uint32_t pointer) {
  struct { uint32_t entry, base, limit, flags; } desc;
  if (copy_from_user(&desc, (void *)(uintptr_t)pointer, sizeof(desc))) return -14;
  bool empty = !desc.base && !desc.limit &&
               (!desc.flags || desc.flags == 0x28);
  unsigned contents = (desc.flags >> 1) & 3;
  if (!empty && (!(desc.flags & 1) || contents > 1 ||
                 (desc.flags & 0x20) || (desc.flags & ~0xffu))) return -22;
  struct thread *t = sched_get_current();
  if (!t) return -1;
  uint32_t entry = desc.entry;
  if (entry == UINT32_MAX) {
    for (entry = 6; entry < 9 && t->compat_tls[entry - 6]; entry++);
    if (entry == 9) return -3;
  }
  if (entry < 6 || entry > 8) return -22;
  uint64_t value = 0;
  if (!empty) {
    uint64_t access = 0xf0 | (contents << 2) | ((desc.flags & 8) ? 0 : 2);
    uint64_t gran = 4 | ((desc.flags & 0x10) ? 8 : 0) |
                    ((desc.flags & 0x40) ? 1 : 0);
    value = (desc.limit & 0xffffULL) | ((desc.base & 0xffffffULL) << 16) |
            (access << 40) | (((desc.limit >> 16) & 15ULL) << 48) |
            (gran << 52) | ((uint64_t)(desc.base >> 24) << 56);
  }
  if (copy_to_user((void *)(uintptr_t)pointer, &entry, sizeof(entry))) return -14;
  hal_irq_state_t irq = hal_irq_save();
  t->compat_tls[entry - 6] = value;
  gdt_set_compat_tls(t->compat_tls);
  if ((t->compat_fs >> 3) == entry) {
    uint16_t selector = empty ? 0 : t->compat_fs;
    __asm__ volatile("mov %0, %%fs" :: "rm"(selector) : "memory");
    t->compat_fs = selector;
    t->fs_base = empty ? 0 : desc.base;
  }
  if ((t->compat_gs >> 3) == entry) {
    uint64_t kernel_gs = rdmsr(0xC0000101);
    uint16_t selector = empty ? 0 : t->compat_gs;
    __asm__ volatile("mov %0, %%gs" :: "rm"(selector) : "memory");
    wrmsr(0xC0000101, kernel_gs);
    t->compat_gs = selector;
    t->gs_base = empty ? 0 : desc.base;
    wrmsr(0xC0000102, t->gs_base);
  }
  hal_irq_restore(irq);
  return 0;
}

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
  case 54: return SYS_IOCTL;
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
  case 97: return SYS_SETPRIORITY;
  case 172: return SYS_PRCTL;
  case 308: return SYS_PSELECT6;
  case 309: return SYS_PPOLL; /* i386 ppoll has a timespec32 timeout */
  case 265: return -2; /* handled with legacy timespec32 below */
  case 266: return -2; /* handled with legacy timespec32 below */
  case 267: return SYS_CLOCK_NANOSLEEP;
  case 268: return SYS_STATFS;
  case 269: return SYS_FSTATFS;
  case 116: return SYS_SYSINFO;
  case 118: return SYS_FSYNC;
  case 120: return SYS_CLONE;
  case 122: return SYS_UNAME;
  case 125: return SYS_MPROTECT;
  case 132: return SYS_GETPGID;
  case 133: return SYS_FCHDIR;
  case 140: return SYS_LSEEK;
  case 143: return SYS_FLOCK;
  case 144: return SYS_MSYNC;
  case 145: return SYS_READV;
  case 146: return SYS_WRITEV;
  case 147: return SYS_GETSID;
  case 148: return SYS_FDATASYNC;
  case 150: return SYS_MLOCK;
  case 151: return SYS_MUNLOCK;
  case 158: return SYS_SCHED_YIELD;
  case 168: return SYS_POLL;
  case 174: return SYS_RT_SIGACTION;
  case 175: return SYS_RT_SIGPROCMASK;
  case 176: return SYS_RT_SIGPENDING;
  case 179: return SYS_RT_SIGSUSPEND;
  case 180: return SYS_PREAD64;
  case 181: return SYS_PWRITE64;
  case 183: return SYS_GETCWD;
  case 186: return SYS_SIGALTSTACK;
  case 194: return SYS_FTRUNCATE;
  case 190: return SYS_VFORK;
  case 191: return SYS_GETRLIMIT;
  case 195: case 196: return SYS_NEWFSTATAT;
  case 197: return SYS_FSTAT;
  case 199: return SYS_GETUID;
  case 200: return SYS_GETGID;
  case 201: return SYS_GETEUID;
  case 202: return SYS_GETEGID;
  case 213: return SYS_SETUID;
  case 214: return SYS_SETGID;
  case 219: return SYS_MADVISE;
  case 220: return SYS_GETDENTS64;
  case 224: return SYS_GETTID;
  case 240: return SYS_FUTEX;
  case 252: return SYS_EXIT_GROUP;
  case 258: return SYS_SET_TID_ADDRESS;
  case 270: return SYS_TGKILL;
  case 295: return SYS_OPENAT;
  case 300: return SYS_NEWFSTATAT; /* converted to i386 stat64 on output */
  case 301: return SYS_UNLINKAT;
  case 302: return SYS_RENAMEAT;
  case 303: return SYS_LINKAT;
  case 304: return SYS_SYMLINKAT;
  case 305: return SYS_READLINKAT;
  case 306: return SYS_FCHMODAT;
  case 307: return SYS_FACCESSAT;
  case 311: return SYS_SET_ROBUST_LIST;
  case 330: return SYS_DUP3;
  case 331: return SYS_PIPE2;
  case 355: return SYS_GETRANDOM;
  case 359: return SYS_SOCKET;
  case 360: return SYS_SOCKETPAIR;
  case 361: return SYS_BIND;
  case 362: return SYS_CONNECT;
  case 363: return SYS_LISTEN;
  case 364: return SYS_ACCEPT4;
  case 365: return SYS_GETSOCKOPT;
  case 366: return SYS_SETSOCKOPT;
  case 367: return SYS_GETSOCKNAME;
  case 368: return SYS_GETPEERNAME;
  case 369: return SYS_SENDTO;
  case 370: return SYS_SENDMSG;
  case 372: return SYS_RECVMSG;
  case 371: return SYS_RECVFROM;
  case 373: return SYS_SHUTDOWN;
  case 435: return SYS_CLONE3;
  case 386: return SYS_RSEQ;
  case 383: return SYS_STATX; /* fixed-width statx layout on both ABIs */
  case 403: return SYS_CLOCK_GETTIME; /* i386 __kernel_timespec (time64) */
  case 406: return SYS_CLOCK_GETRES; /* i386 __kernel_timespec (time64) */
  case 340: return SYS_PRLIMIT64; /* fixed-width rlimit64 */
  case 8: return SYS_CREAT; /* creat */
  case 14: return SYS_MKNOD; /* mknod */
  case 21: return SYS_MOUNT; /* mount */
  case 52: return SYS_UMOUNT2; /* umount2 */
  case 74: return SYS_SETHOSTNAME; /* sethostname */
  case 121: return SYS_SETDOMAINNAME; /* setdomainname */
  case 152: return SYS_MLOCKALL; /* mlockall */
  case 153: return SYS_MUNLOCKALL; /* munlockall */
  case 154: return SYS_SCHED_SETPARAM; /* sched_setparam */
  case 155: return SYS_SCHED_GETPARAM; /* sched_getparam */
  case 156: return SYS_SCHED_SETSCHEDULER; /* sched_setscheduler */
  case 157: return SYS_SCHED_GETSCHEDULER; /* sched_getscheduler */
  case 159: return SYS_SCHED_GET_PRIORITY_MAX; /* sched_get_priority_max */
  case 160: return SYS_SCHED_GET_PRIORITY_MIN; /* sched_get_priority_min */
  case 198: return SYS_LCHOWN; /* lchown32 */
  case 203: return SYS_SETREUID; /* setreuid32 */
  case 204: return SYS_SETREGID; /* setregid32 */
  case 207: return SYS_FCHOWN; /* fchown32 */
  case 208: return SYS_SETRESUID; /* setresuid32 */
  case 209: return SYS_GETRESUID; /* getresuid32 */
  case 210: return SYS_SETRESGID; /* setresgid32 */
  case 211: return SYS_GETRESGID; /* getresgid32 */
  case 212: return SYS_CHOWN; /* chown32 */
  case 215: return SYS_SETFSUID; /* setfsuid32 */
  case 216: return SYS_SETFSGID; /* setfsgid32 */
  case 217: return SYS_PIVOT_ROOT; /* pivot_root */
  case 226: return SYS_SETXATTR; /* setxattr */
  case 227: return SYS_LSETXATTR; /* lsetxattr */
  case 228: return SYS_FSETXATTR; /* fsetxattr */
  case 229: return SYS_GETXATTR; /* getxattr */
  case 230: return SYS_LGETXATTR; /* lgetxattr */
  case 231: return SYS_FGETXATTR; /* fgetxattr */
  case 232: return SYS_LISTXATTR; /* listxattr */
  case 233: return SYS_LLISTXATTR; /* llistxattr */
  case 234: return SYS_FLISTXATTR; /* flistxattr */
  case 235: return SYS_REMOVEXATTR; /* removexattr */
  case 236: return SYS_LREMOVEXATTR; /* lremovexattr */
  case 237: return SYS_FREMOVEXATTR; /* fremovexattr */
  case 238: return SYS_TKILL; /* tkill */
  case 241: return SYS_SCHED_SETAFFINITY; /* sched_setaffinity */
  case 242: return SYS_SCHED_GETAFFINITY; /* sched_getaffinity */
  case 254: return SYS_EPOLL_CREATE; /* epoll_create */
  case 255: return SYS_EPOLL_CTL; /* epoll_ctl */
  case 256: return SYS_EPOLL_WAIT; /* epoll_wait */
  case 291: return SYS_INOTIFY_INIT; /* inotify_init */
  case 292: return SYS_INOTIFY_ADD_WATCH; /* inotify_add_watch */
  case 293: return SYS_INOTIFY_RM_WATCH; /* inotify_rm_watch */
  case 296: return SYS_MKDIRAT; /* mkdirat */
  case 297: return SYS_MKNODAT; /* mknodat */
  case 298: return SYS_FCHOWNAT; /* fchownat */
  case 310: return SYS_UNSHARE; /* unshare */
  case 315: return SYS_TEE; /* tee */
  case 318: return SYS_GETCPU; /* getcpu */
  case 328: return SYS_EVENTFD2; /* eventfd2 */
  case 329: return SYS_EPOLL_CREATE1; /* epoll_create1 */
  case 332: return SYS_INOTIFY_INIT1; /* inotify_init1 */
  case 353: return SYS_RENAMEAT2; /* renameat2 */
  case 356: return SYS_MEMFD_CREATE; /* memfd_create */
  case 374: return SYS_USERFAULTFD; /* userfaultfd */
  case 375: return SYS_MEMBARRIER; /* membarrier */
  case 434: return SYS_PIDFD_OPEN; /* pidfd_open */
  case 436: return SYS_CLOSE_RANGE; /* close_range */
  case 438: return SYS_PIDFD_GETFD; /* pidfd_getfd */
  case 439: return SYS_FACCESSAT2; /* faccessat2 */
  case 444: return SYS_LANDLOCK_CREATE_RULESET; /* landlock_create_ruleset */
  case 446: return SYS_LANDLOCK_RESTRICT_SELF; /* landlock_restrict_self */
  case 452: return SYS_FCHMODAT2; /* fchmodat2 */
  default: return -1;
  }
}

/* Keep the unmodified i386 registers in the return shadows for fork/clone;
 * their arguments use a different register order than x86-64. */
static int compat_prepare(struct registers *r, struct syscall_regs *s) {
  uint32_t nr = r->rax;
  int native = compat_syscall_number(nr);
  if (nr == 117) {
    switch ((uint32_t)r->rbx & 0xffff) {
    case 1: native=SYS_SEMOP; break;
    case 2: native=SYS_SEMGET; break;
    case 3: native=SYS_SEMCTL; break;
    case 21: native=SYS_SHMAT; break;
    case 22: native=SYS_SHMDT; break;
    case 23: native=SYS_SHMGET; break;
    case 24: native=SYS_SHMCTL; break;
    default: return -38;
    }
  }
  if (native < 0) return -38;
  *s = (struct syscall_regs){
      .rdi=(uint32_t)r->rbx, .rsi=(uint32_t)r->rcx,
      .rdx=(uint32_t)r->rdx, .r10=(uint32_t)r->rsi,
      .r8=(uint32_t)r->rdi, .r9=(uint32_t)r->rbp,
      .rax=(uint64_t)native, .rbx=(uint32_t)r->rbx, .rbp=(uint32_t)r->rbp,
      .rip=r->rip, .rsp=r->rsp, .rflags=r->rflags, .user_cs=r->cs,
      .user_rcx=(uint32_t)r->rcx, .user_rsi=(uint32_t)r->rsi,
      .user_rdi=(uint32_t)r->rdi };
  if (nr == 117) {
    uint32_t command = (uint32_t)r->rbx & 0xffff;
    s->rdi=(uint32_t)r->rcx; s->rsi=(uint32_t)r->rdx; s->rdx=(uint32_t)r->rsi;
    if (command == 1) { s->rsi=(uint32_t)r->rdi; s->rdx=(uint32_t)r->rdx; }
    if (command == 3) {
      if (((uint32_t)r->rsi & 0xff) < 11 && ((uint32_t)r->rsi & 0xff) != 0) return -38;
      uint32_t value;
      if (copy_from_user(&value, (void *)(uintptr_t)(uint32_t)r->rdi, 4)) return -14;
      s->r10=value;
    }
    if (command == 21) { s->rsi=(uint32_t)r->rdi; s->rdx=(uint32_t)r->rdx; }
    if (command == 22) s->rdi=(uint32_t)r->rdi;
    if (command == 24) {
      if (((uint32_t)r->rdx & 0xff) != 0) return -38;
      s->rdx=(uint32_t)r->rdi;
    }
  }
  if (nr == 93) s->rsi=(uint64_t)(int64_t)(int32_t)r->rcx;
  if (nr == 194)
    s->rsi=(uint32_t)r->rcx | ((uint64_t)(uint32_t)r->rdx << 32);
  if (nr == 195 || nr == 196) {
    s->rdi = (uint64_t)(int64_t)-100;
    s->rsi = (uint32_t)r->rbx;
    s->rdx = (uint32_t)r->rcx;
    s->r10 = nr == 196 ? 0x100 : 0;
  }
  if (nr == 97) { s->rsi=(int32_t)r->rcx; s->rdx=(int32_t)r->rdx; }
  if (nr == 172 && s->rdi == 0x41555856) return -95; /* AUXV needs narrow entries. */
  if (nr == 268 || nr == 269) {
    if (s->rsi != 84) return -22;
    s->rsi = s->rdx;
  }
  /* Native fcntl only accesses fixed-width l_type for lock queries;
   * other supported commands have scalar arguments. Unknown commands go
   * to the native EINVAL path rather than masquerading as absent syscalls. */
  if (nr == 7) { s->rdi=(int32_t)r->rbx; s->r10=0; }
  if (nr == 37) s->rdi=(int32_t)r->rbx;
  if (nr == 270) { s->rdi=(int32_t)r->rbx; s->rsi=(int32_t)r->rcx; }
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
    /* The page offset is 32-bit; its byte offset is 44-bit. In particular,
     * DRM's opaque mmap tokens need not fit a 32-bit byte offset. */
    s->r9 = (uint64_t)(uint32_t)r->rbp * PAGE_SIZE;
    s->r8=(int32_t)r->rdi;
  }
  if (nr == 120) {
    s->r10=(uint32_t)r->rdi; /* child_tid */
    s->r8=(uint32_t)r->rsi; /* i386 user_desc pointer */
  }
  if (nr == 140) {
    s->rsi = ((uint64_t)(uint32_t)r->rcx << 32) | (uint32_t)r->rdx;
    s->rdx = (uint32_t)r->rdi;
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
  if (task) {
    hal_irq_state_t irq = hal_irq_save();
    __asm__ volatile("mov %%fs, %0" : "=rm"(task->compat_fs));
    __asm__ volatile("mov %%gs, %0" : "=rm"(task->compat_gs));
    task->fs_base = rdmsr(0xC0000100);
    task->gs_base = rdmsr(0xC0000102);
    hal_irq_restore(irq);
  }
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
  if (number == 243) {
    regs->rax = (uint32_t)compat_set_thread_area((uint32_t)regs->rbx);
    goto compat_finished;
  }
  if (number == 119 || number == 173) {
    extern void signal_compat_sigreturn(struct registers *, bool);
    signal_compat_sigreturn(regs, number == 173);
    goto compat_finished;
  }
  if (number == 265 || number == 266) {
    /* Legacy i386 clock_gettime/getres use two 32-bit longs. */
    uint32_t clock_id = (uint32_t)regs->rbx;
    uint32_t ptr = (uint32_t)regs->rcx;
    struct { int32_t sec, nsec; } ts = {0, 1};
    if (clock_id > 11) regs->rax = (uint32_t)-22;
    else if (number == 265) {
      uint64_t ns = lapic_timer_get_ns();
      ts.sec = (int32_t)(ns / 1000000000ULL);
      ts.nsec = (int32_t)(ns % 1000000000ULL);
      if (clock_id == 0 || clock_id == 4)
        ts.sec += (int32_t)rtc_get_boot_timestamp();
      if (ptr && copy_to_user((void *)(uintptr_t)ptr, &ts, sizeof(ts)))
        regs->rax = (uint32_t)-14;
      else regs->rax = 0;
    } else if (ptr && copy_to_user((void *)(uintptr_t)ptr, &ts, sizeof(ts)))
      regs->rax = (uint32_t)-14;
    else regs->rax = 0;
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
    if (number == 117 && ((uint32_t)regs->rbx & 0xffff) == 21 &&
        (int64_t)native.rax >= 0) {
      uint32_t address = native.rax;
      native.rax = copy_to_user((void *)(uintptr_t)(uint32_t)regs->rsi,
                                &address, sizeof(address)) ? (uint64_t)-14 : 0;
    }
    if (number == 140 && (int64_t)native.rax >= 0) {
      uint64_t offset = native.rax;
      native.rax = copy_to_user((void *)(uintptr_t)(uint32_t)regs->rsi,
                                &offset, sizeof(offset)) ? (uint64_t)-14 : 0;
    }
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
  /* Deliver against the actual i386 interrupt frame, including sigreturn. */
  signal_deliver(regs);
  hal_irq_disable();
  user_access_end();
}
