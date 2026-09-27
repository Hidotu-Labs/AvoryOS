#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define IA32_EFER 0xC0000080
#define IA32_STAR 0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define IA32_EFER_SCE 0x01

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_LSTAT 6
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_BRK 12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_RT_SIGSUSPEND 130
#define SYS_RT_SIGTIMEDWAIT 128
#define SYS_PAUSE 34
#define SYS_IOCTL 16
#define SYS_PREAD64 17
#define SYS_PWRITE64 18
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21
#define SYS_PIPE 22
#define SYS_SELECT 23
#define SYS_SCHED_YIELD 24
#define SYS_MREMAP 25
#define SYS_MSYNC 26
#define SYS_MINCORE 27
#define SYS_MADVISE 28
#define SYS_SHMGET 29
#define SYS_SHMAT 30
#define SYS_SHMCTL 31
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_NANOSLEEP 35
#define SYS_ALARM 37
#define SYS_SETITIMER 38
#define SYS_GETPID 39
#define SYS_SENDFILE 40
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_SENDMSG 46
#define SYS_RECVMSG 47
#define SYS_SHUTDOWN 48
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SOCKETPAIR 53
#define SYS_SETSOCKOPT 54
#define SYS_GETSOCKOPT 55
#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_VFORK 58
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_WAITID 247
#define SYS_KILL 62
#define SYS_UNAME 63
#define SYS_SEMGET 64
#define SYS_SEMOP 65
#define SYS_SEMCTL 66
#define SYS_SHMDT 67
#define SYS_SEMTIMEDOP 220
#define SYS_FCNTL 72
#define SYS_FLOCK 73
#define SYS_FSYNC 74
#define SYS_FDATASYNC 75
#define SYS_FTRUNCATE 77
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_FCHDIR 81
#define SYS_RENAME 82
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_LINK 86
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_FCHMOD 91
#define SYS_CHOWN 92
#define SYS_FCHOWN 93
#define SYS_LCHOWN 94
#define SYS_UMASK 95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT 97
#define SYS_GETRUSAGE 98
#define SYS_SYSINFO 99
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_SETUID 105
#define SYS_SETGID 106
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_SETREUID 113
#define SYS_SETREGID 114
#define SYS_GETGROUPS 115
#define SYS_SETGROUPS 116
#define SYS_SETRESUID 117
#define SYS_GETRESUID 118
#define SYS_SETRESGID 119
#define SYS_GETRESGID 120
#define SYS_GETPGID 121
#define SYS_SETFSUID 122
#define SYS_SETFSGID 123
#define SYS_GETSID 124
#define SYS_CAPGET 125
#define SYS_CAPSET 126
#define SYS_SIGALTSTACK 131
#define SYS_SECCOMP 317
#define SYS_STATFS 137
#define SYS_GETDENTS 78
#define SYS_FSTATFS 138
#define SYS_GETPRIORITY 140
#define SYS_SETPRIORITY 141
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_MLOCK 149
#define SYS_MUNLOCK 150
#define SYS_MLOCKALL 151
#define SYS_MUNLOCKALL 152
#define SYS_MLOCK2 325
#define SYS_SCHED_SETPARAM 142
#define SYS_SCHED_GETPARAM 143
#define SYS_SCHED_GET_PRIORITY_MAX 146
#define SYS_SCHED_GET_PRIORITY_MIN 147
#define SYS_PRCTL 157
#define SYS_ARCH_PRCTL 158
#define SYS_GETTID 186
#define SYS_TKILL 200
#define SYS_TGKILL 234
#define SYS_FUTEX 202
#define SYS_SCHED_SETAFFINITY 203
#define SYS_SCHED_GETAFFINITY 204
#define SYS_EPOLL_CREATE 213
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_FADVISE64 221
#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES 229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP 231
#define SYS_EPOLL_WAIT 232
#define SYS_EPOLL_CTL 233
#define SYS_UTIMENSAT 235
#define SYS_INOTIFY_INIT 253
#define SYS_INOTIFY_ADD_WATCH 254
#define SYS_INOTIFY_RM_WATCH 255
#define SYS_OPENAT 257
#define SYS_MKDIRAT 258
#define SYS_FCHOWNAT 260
#define SYS_FUTIMESAT 261
#define SYS_NEWFSTATAT 262
#define SYS_UNLINKAT 263
#define SYS_LINKAT   265
#define SYS_FCHMODAT 268
#define SYS_READLINKAT 267
#define SYS_PSELECT6 270
#define SYS_PPOLL 271
#define SYS_UNSHARE 272
#define SYS_UTIMES 280
#define SYS_EPOLL_PWAIT 281
#define SYS_SIGNALFD 282
#define SYS_TIMERFD_CREATE 283
#define SYS_FALLOCATE 285
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMERFD_GETTIME 287
#define SYS_ACCEPT4 288
#define SYS_SIGNALFD4 289
#define SYS_EVENTFD2 290
#define SYS_EPOLL_CREATE1 291
#define SYS_PIPE2 293
#define SYS_INOTIFY_INIT1 294
#define SYS_RECVMMSG 299
#define SYS_PRLIMIT64 302
#define SYS_NAME_TO_HANDLE_AT 303
#define SYS_OPEN_BY_HANDLE_AT 304
#define SYS_SENDMMSG 307
#define SYS_GETCPU    309
#define SYS_GETRANDOM 318
#define SYS_MEMFD_CREATE 319
#define SYS_MEMBARRIER 324
#define SYS_STATX 332
#define SYS_RENAMEAT 264
#define SYS_SYMLINKAT 266
#define SYS_SET_ROBUST_LIST 273
#define SYS_GET_ROBUST_LIST 274
#define SYS_RENAMEAT2 316
#define SYS_RSEQ 334
#define SYS_MOUNT 165
#define SYS_REBOOT 169
#define SYS_UPTIME 399
#define SYS_PIDFD_OPEN 434
#define SYS_CLONE3 435
#define SYS_CLOSE_RANGE 436
#define SYS_FACCESSAT 269
#define SYS_FACCESSAT2 439
#define SYS_FCHMODAT2  452

/* Extended attributes (188..199) */
#define SYS_SETXATTR      188
#define SYS_LSETXATTR     189
#define SYS_FSETXATTR     190
#define SYS_GETXATTR      191
#define SYS_LGETXATTR     192
#define SYS_FGETXATTR     193
#define SYS_LISTXATTR     194
#define SYS_LLISTXATTR    195
#define SYS_FLISTXATTR    196
#define SYS_REMOVEXATTR   197
#define SYS_LREMOVEXATTR  198
#define SYS_FREMOVEXATTR  199

/* Moving data between descriptors */
#define SYS_SPLICE           275
#define SYS_COPY_FILE_RANGE  326

#define MAX_SYSCALL 512

#ifndef SYSCALL_LOG
#define SYSCALL_LOG 0
#endif

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_PIDFD 0x00001000
#define CLONE_PTRACE 0x00002000
#define CLONE_VFORK 0x00004000
#define CLONE_PARENT 0x00008000
#define CLONE_THREAD 0x00010000
#define CLONE_NEWNS 0x00020000
#define CLONE_SYSVSEM 0x00040000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED 0x00400000
#define CLONE_UNTRACED 0x00800000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWPID 0x20000000
#define CLONE_NEWNET 0x40000000
#define CLONE_IO 0x80000000

struct syscall_regs {
  uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
  uint64_t rip, rflags, rsp;
} __attribute__((packed));

// Standard handler: receives the 6 argument registers, returns result in rax.
typedef uint64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t);

// Raw handler: receives the full saved register frame.  Used by syscalls
// that need access to the caller's RIP/RSP/RFLAGS (e.g. fork).
typedef uint64_t (*syscall_raw_handler_t)(struct syscall_regs *regs);

void syscall_register(int num, syscall_handler_t handler);

void syscall_register_raw(int num, syscall_raw_handler_t handler);

void syscall_register_io(void);
void syscall_register_process(void);
void syscall_register_mm(void);
void syscall_register_arch(void);
void syscall_register_signal(void);
void signal_deliver_syscall(struct syscall_regs *regs);
void syscall_register_media(void);
void syscall_register_socket(void);
void syscall_register_epoll(void);
void syscall_register_poll(void);
void syscall_register_shm(void);
void syscall_register_sem(void);
void syscall_register_futex(void);

const char *syscall_get_name(uint64_t num);

void syscall_init(void);

uint64_t mm_alloc_mmap_region(uint64_t length);

#endif
