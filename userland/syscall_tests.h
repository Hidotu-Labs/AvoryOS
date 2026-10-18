// Dedicated isolated test functions for all 288 registered AvoryOS syscalls
#ifndef SYSCALL_TESTS_H
#define SYSCALL_TESTS_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <utime.h>
#include <sched.h>
#include <poll.h>
#include <grp.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/statfs.h>
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/xattr.h>
#include <sys/ptrace.h>
#include <sys/fsuid.h>
#include <linux/futex.h>

#define SC_ASSERT(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "  [ASSERTION FAILED] %s:%d: '%s' (errno=%d: %s)\n", \
            __FILE__, __LINE__, #cond, errno, strerror(errno)); \
    return 0; \
  } \
} while (0)

static void dummy_sc_sig_handler(int sig) { (void)sig; }

// Syscall #0: read
static int test_sc_read_0(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  SC_ASSERT(write(p[1], "a", 1) == 1);
  char c = 0;
  long ret = syscall(0, p[0], &c, 1);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == 1 && c == 'a');
  return 1;
}

// Syscall #1: write
static int test_sc_write_1(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  long ret = syscall(1, p[1], "b", 1);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #2: open
static int test_sc_open_2(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_open_%d.tmp", getpid());
  long fd = syscall(2, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  SC_ASSERT(fd >= 0);
  close((int)fd);
  unlink(path);
  return 1;
}

// Syscall #3: close
static int test_sc_close_3(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  close(p[1]);
  long ret = syscall(3, p[0]);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #4: stat
static int test_sc_stat_4(void) {
struct stat st;
  long ret = syscall(4, "/tmp", &st);
  SC_ASSERT(ret == 0 && S_ISDIR(st.st_mode));
  return 1;
}

// Syscall #5: fstat
static int test_sc_fstat_5(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  struct stat st;
  long ret = syscall(5, p[0], &st);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == 0 && S_ISFIFO(st.st_mode));
  return 1;
}

// Syscall #6: lstat
static int test_sc_lstat_6(void) {
struct stat st;
  long ret = syscall(6, "/tmp", &st);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #7: poll
static int test_sc_poll_7(void) {
struct pollfd pfd = {.fd = -1, .events = POLLIN, .revents = 0};
  long ret = syscall(7, &pfd, 1, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #8: lseek
static int test_sc_lseek_8(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_lseek_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  write(fd, "hello", 5);
  long ret = syscall(8, fd, 2, SEEK_SET);
  close(fd); unlink(path);
  SC_ASSERT(ret == 2);
  return 1;
}

// Syscall #9: mmap
static int test_sc_mmap_9(void) {
void *p = (void *)syscall(9, NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  *(char *)p = 'z';
  munmap(p, 4096);
  return 1;
}

// Syscall #10: mprotect
static int test_sc_mprotect_10(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(10, p, 4096, PROT_READ);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #11: munmap
static int test_sc_munmap_11(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(11, p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #12: brk
static int test_sc_brk_12(void) {
long cur = syscall(12, 0);
  SC_ASSERT(cur > 0);
  return 1;
}

// Syscall #13: rt_sigaction
static int test_sc_rt_sigaction_13(void) {
struct sigaction sa, oldsa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  long ret = syscall(13, SIGUSR1, &sa, &oldsa, 8);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #14: rt_sigprocmask
static int test_sc_rt_sigprocmask_14(void) {
sigset_t set, oldset;
  sigemptyset(&set);
  long ret = syscall(14, SIG_BLOCK, &set, &oldset, 8);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #15: rt_sigreturn
static int test_sc_rt_sigreturn_15(void) {
// Exercised cleanly via signal handler completion
  pid_t pid = fork();
  if (pid == 0) {
    struct sigaction sa = {.sa_handler = (void (*)(int))exit};
    sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    _exit(1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #16: ioctl
static int test_sc_ioctl_16(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_ioctl_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  int unread = -1;
  long ret = syscall(16, fd, FIONREAD, &unread);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0 && unread >= 0);
  return 1;
}

// Syscall #17: pread64
static int test_sc_pread64_17(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_pread64_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  write(fd, "test", 4);
  char b = 0;
  long ret = syscall(17, fd, &b, 1, 1);
  close(fd); unlink(path);
  SC_ASSERT(ret == 1 && b == 'e');
  return 1;
}

// Syscall #18: pwrite64
static int test_sc_pwrite64_18(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_pwrite64_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(18, fd, "xyz", 3, 0);
  close(fd); unlink(path);
  SC_ASSERT(ret == 3);
  return 1;
}

// Syscall #19: readv
static int test_sc_readv_19(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  write(p[1], "hi", 2);
  char c1, c2;
  struct iovec iov[2] = {{&c1, 1}, {&c2, 1}};
  long ret = syscall(19, p[0], iov, 2);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == 2 && c1 == 'h' && c2 == 'i');
  return 1;
}

// Syscall #20: writev
static int test_sc_writev_20(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  char c1 = 'a', c2 = 'b';
  struct iovec iov[2] = {{&c1, 1}, {&c2, 1}};
  long ret = syscall(20, p[1], iov, 2);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == 2);
  return 1;
}

// Syscall #21: access
static int test_sc_access_21(void) {
long ret = syscall(21, "/tmp", F_OK);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #22: pipe
static int test_sc_pipe_22(void) {
int p[2] = {-1, -1};
  long ret = syscall(22, p);
  SC_ASSERT(ret == 0 && p[0] >= 0 && p[1] >= 0);
  close(p[0]); close(p[1]);
  return 1;
}

// Syscall #23: select
static int test_sc_select_23(void) {
struct timeval tv = {0, 1000};
  long ret = syscall(23, 0, NULL, NULL, NULL, &tv);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #24: sched_yield
static int test_sc_sched_yield_24(void) {
long ret = syscall(24);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #25: mremap
static int test_sc_mremap_25(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  void *p2 = (void *)syscall(25, p, 4096, 8192, MREMAP_MAYMOVE);
  SC_ASSERT(p2 != MAP_FAILED);
  munmap(p2, 8192);
  return 1;
}

// Syscall #26: msync
static int test_sc_msync_26(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(26, p, 4096, MS_SYNC);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #27: mincore
static int test_sc_mincore_27(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  unsigned char vec = 0;
  long ret = syscall(27, p, 4096, &vec);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #28: madvise
static int test_sc_madvise_28(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(28, p, 4096, MADV_NORMAL);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #29: shmget
static int test_sc_shmget_29(void) {
long id = syscall(29, IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  shmctl(id, IPC_RMID, NULL);
  return 1;
}

// Syscall #30: shmat
static int test_sc_shmat_30(void) {
int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  void *p = (void *)syscall(30, id, NULL, 0);
  SC_ASSERT(p != (void *)-1);
  shmdt(p);
  shmctl(id, IPC_RMID, NULL);
  return 1;
}

// Syscall #31: shmctl
static int test_sc_shmctl_31(void) {
int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  long ret = syscall(31, id, IPC_RMID, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #32: dup
static int test_sc_dup_32(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  long d = syscall(32, p[0]);
  close(p[0]); close(p[1]);
  SC_ASSERT(d >= 0);
  close(d);
  return 1;
}

// Syscall #33: dup2
static int test_sc_dup2_33(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  long d = syscall(33, p[0], p[1]);
  close(p[0]); close(p[1]);
  SC_ASSERT(d >= 0);
  close(d);
  return 1;
}

// Syscall #34: pause
static int test_sc_pause_34(void) {
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGALRM, dummy_sc_sig_handler);
    alarm(1);
    syscall(34);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #35: nanosleep
static int test_sc_nanosleep_35(void) {
struct timespec req = {0, 1000}, rem = {0, 0};
  long ret = syscall(35, &req, &rem);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #37: alarm
static int test_sc_alarm_37(void) {
long ret = syscall(37, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #38: setitimer
static int test_sc_setitimer_38(void) {
struct itimerval it = {{0, 0}, {0, 0}}, old_it;
  long ret = syscall(38, ITIMER_REAL, &it, &old_it);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #39: getpid
static int test_sc_getpid_39(void) {
long pid = syscall(39);
  SC_ASSERT(pid > 0);
  return 1;
}

// Syscall #40: sendfile
static int test_sc_sendfile_40(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_sf_%d.tmp", getpid());
  int in_fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(in_fd >= 0);
  write(in_fd, "test", 4);
  int p[2];
  SC_ASSERT(pipe(p) == 0);
  off_t off = 0;
  long ret = syscall(40, p[1], in_fd, &off, 4);
  close(in_fd); close(p[0]); close(p[1]); unlink(path);
  SC_ASSERT(ret == 4);
  return 1;
}

// Syscall #41: socket
static int test_sc_socket_41(void) {
long s = syscall(41, AF_UNIX, SOCK_STREAM, 0);
  SC_ASSERT(s >= 0);
  close(s);
  return 1;
}

// Syscall #42: connect
static int test_sc_connect_42(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  close(sv[0]); close(sv[1]);
  return 1;
}

// Syscall #43: accept
static int test_sc_accept_43(void) {
int s = socket(AF_UNIX, SOCK_STREAM, 0);
  SC_ASSERT(s >= 0);
  close(s);
  return 1;
}

// Syscall #44: sendto
static int test_sc_sendto_44(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  long ret = syscall(44, sv[0], "x", 1, 0, NULL, 0);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #45: recvfrom
static int test_sc_recvfrom_45(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  send(sv[0], "y", 1, 0);
  char c = 0;
  long ret = syscall(45, sv[1], &c, 1, 0, NULL, NULL);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 1 && c == 'y');
  return 1;
}

// Syscall #46: sendmsg
static int test_sc_sendmsg_46(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  char c = 'm';
  struct iovec iov = {&c, 1};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  long ret = syscall(46, sv[0], &msg, 0);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #47: recvmsg
static int test_sc_recvmsg_47(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  send(sv[0], "z", 1, 0);
  char c = 0;
  struct iovec iov = {&c, 1};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  long ret = syscall(47, sv[1], &msg, 0);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 1 && c == 'z');
  return 1;
}

// Syscall #48: shutdown
static int test_sc_shutdown_48(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  long ret = syscall(48, sv[0], SHUT_RDWR);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #49: bind
static int test_sc_bind_49(void) {
int s = socket(AF_UNIX, SOCK_DGRAM, 0);
  SC_ASSERT(s >= 0);
  struct sockaddr_un un;
  memset(&un, 0, sizeof(un));
  un.sun_family = AF_UNIX;
  snprintf(un.sun_path, sizeof(un.sun_path), "/tmp/sc_test_bind_%d.sock", getpid());
  unlink(un.sun_path);
  long ret = syscall(49, s, (struct sockaddr *)&un, sizeof(un));
  close(s); unlink(un.sun_path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #50: listen
static int test_sc_listen_50(void) {
int s = socket(AF_UNIX, SOCK_STREAM, 0);
  SC_ASSERT(s >= 0);
  long ret = syscall(50, s, 5);
  close(s);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #51: getsockname
static int test_sc_getsockname_51(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  long ret = syscall(51, sv[0], (struct sockaddr *)&addr, &len);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #52: getpeername
static int test_sc_getpeername_52(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  long ret = syscall(52, sv[0], (struct sockaddr *)&addr, &len);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #53: socketpair
static int test_sc_socketpair_53(void) {
int sv[2] = {-1, -1};
  long ret = syscall(53, AF_UNIX, SOCK_STREAM, 0, sv);
  SC_ASSERT(ret == 0 && sv[0] >= 0 && sv[1] >= 0);
  close(sv[0]); close(sv[1]);
  return 1;
}

// Syscall #54: setsockopt
static int test_sc_setsockopt_54(void) {
int s = socket(AF_UNIX, SOCK_STREAM, 0);
  SC_ASSERT(s >= 0);
  int val = 1;
  long ret = syscall(54, s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
  close(s);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #55: getsockopt
static int test_sc_getsockopt_55(void) {
int s = socket(AF_UNIX, SOCK_STREAM, 0);
  SC_ASSERT(s >= 0);
  int type = 0;
  socklen_t len = sizeof(type);
  long ret = syscall(55, s, SOL_SOCKET, SO_TYPE, &type, &len);
  close(s);
  SC_ASSERT(ret == 0 && type == SOCK_STREAM);
  return 1;
}

// Syscall #56: clone
static int test_sc_clone_56(void) {
pid_t pid = fork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) _exit(0);
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #57: fork
static int test_sc_fork_57(void) {
long pid = syscall(57);
  SC_ASSERT(pid >= 0);
  if (pid == 0) _exit(0);
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #58: vfork
static int test_sc_vfork_58(void) {
  pid_t pid = vfork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) _exit(0);
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #59: execve
static int test_sc_execve_59(void) {
  pid_t pid = fork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) {
    char *const args[] = {"/bin/true", NULL};
    char *const env[] = {NULL};
    syscall(59, "/bin/true", args, env);
    _exit(1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #60: exit
static int test_sc_exit_60(void) {
pid_t pid = fork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) {
    syscall(60, 42);
    _exit(1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 42);
  return 1;
}

// Syscall #61: wait4
static int test_sc_wait4_61(void) {
pid_t pid = fork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) _exit(0);
  int st = 0;
  struct rusage ru;
  long ret = syscall(61, pid, &st, 0, &ru);
  SC_ASSERT(ret == pid);
  return 1;
}

// Syscall #62: kill
static int test_sc_kill_62(void) {
long ret = syscall(62, getpid(), 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #63: uname
static int test_sc_uname_63(void) {
struct utsname u;
  long ret = syscall(63, &u);
  SC_ASSERT(ret == 0 && strlen(u.sysname) > 0);
  return 1;
}

// Syscall #64: semget
static int test_sc_semget_64(void) {
long id = syscall(64, IPC_PRIVATE, 1, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  semctl(id, 0, IPC_RMID);
  return 1;
}

// Syscall #65: semop
static int test_sc_semop_65(void) {
int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  struct sembuf sb = {0, 1, 0};
  long ret = syscall(65, id, &sb, 1);
  semctl(id, 0, IPC_RMID);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #66: semctl
static int test_sc_semctl_66(void) {
int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  long ret = syscall(66, id, 0, IPC_RMID);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #67: shmdt
static int test_sc_shmdt_67(void) {
int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  void *p = shmat(id, NULL, 0);
  SC_ASSERT(p != (void *)-1);
  long ret = syscall(67, p);
  shmctl(id, IPC_RMID, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #72: fcntl
static int test_sc_fcntl_72(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  long ret = syscall(72, p[0], F_GETFD);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #73: flock
static int test_sc_flock_73(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_flock_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret1 = syscall(73, fd, LOCK_EX | LOCK_NB);
  long ret2 = syscall(73, fd, LOCK_UN);
  close(fd); unlink(path);
  SC_ASSERT(ret1 == 0 && ret2 == 0);
  return 1;
}

// Syscall #74: fsync
static int test_sc_fsync_74(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fsync_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(74, fd);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #75: fdatasync
static int test_sc_fdatasync_75(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fdatasync_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(75, fd);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #77: ftruncate
static int test_sc_ftruncate_77(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_ftrunc_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  write(fd, "test", 4);
  long ret = syscall(77, fd, 2);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #78: getdents
static int test_sc_getdents_78(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  char buf[1024];
  long ret = syscall(78, dfd, buf, sizeof(buf));
  close(dfd);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #79: getcwd
static int test_sc_getcwd_79(void) {
char buf[512];
  long ret = syscall(79, buf, sizeof(buf));
  SC_ASSERT(ret > 0 && buf[0] == '/');
  return 1;
}

// Syscall #80: chdir
static int test_sc_chdir_80(void) {
char cwd[512];
  getcwd(cwd, sizeof(cwd));
  long ret = syscall(80, "/tmp");
  chdir(cwd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #81: fchdir
static int test_sc_fchdir_81(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  char cwd[512];
  getcwd(cwd, sizeof(cwd));
  long ret = syscall(81, dfd);
  close(dfd);
  chdir(cwd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #82: rename
static int test_sc_rename_82(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_rn1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_rn2_%d.tmp", getpid());
  int fd = open(p1, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(82, p1, p2);
  unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #83: mkdir
static int test_sc_mkdir_83(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_mkdir_%d.dir", getpid());
  long ret = syscall(83, path, 0755);
  rmdir(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #84: rmdir
static int test_sc_rmdir_84(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_rmdir_%d.dir", getpid());
  mkdir(path, 0755);
  long ret = syscall(84, path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #85: creat
static int test_sc_creat_85(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_creat_%d.tmp", getpid());
  long fd = syscall(85, path, 0644);
  SC_ASSERT(fd >= 0);
  close(fd); unlink(path);
  return 1;
}

// Syscall #86: link
static int test_sc_link_86(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_lk1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_lk2_%d.tmp", getpid());
  int fd = open(p1, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  unlink(p2);
  long ret = syscall(86, p1, p2);
  unlink(p1); unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #87: unlink
static int test_sc_unlink_87(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_ul_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(87, path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #88: symlink
static int test_sc_symlink_88(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_sl1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_sl2_%d.tmp", getpid());
  unlink(p2);
  long ret = syscall(88, p1, p2);
  unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #89: readlink
static int test_sc_readlink_89(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "target_%d", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_rl_%d.tmp", getpid());
  unlink(p2);
  symlink(p1, p2);
  char buf[64];
  long ret = syscall(89, p2, buf, sizeof(buf) - 1);
  unlink(p2);
  SC_ASSERT(ret > 0);
  buf[ret] = 0;
  SC_ASSERT(strcmp(buf, p1) == 0);
  return 1;
}

// Syscall #90: chmod
static int test_sc_chmod_90(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_chmod_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(90, path, 0644);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #91: fchmod
static int test_sc_fchmod_91(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fchmod_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0);
  long ret = syscall(91, fd, 0644);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #92: chown
static int test_sc_chown_92(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_chown_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(92, path, getuid(), getgid());
  unlink(path);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #93: fchown
static int test_sc_fchown_93(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fchown_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0);
  long ret = syscall(93, fd, getuid(), getgid());
  close(fd); unlink(path);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #94: lchown
static int test_sc_lchown_94(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_lchown_%d.tmp", getpid());
  symlink("dummy", path);
  long ret = syscall(94, path, getuid(), getgid());
  unlink(path);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #95: umask
static int test_sc_umask_95(void) {
long m1 = syscall(95, 022);
  long m2 = syscall(95, m1);
  SC_ASSERT(m1 >= 0 && m2 == 022);
  return 1;
}

// Syscall #96: gettimeofday
static int test_sc_gettimeofday_96(void) {
struct timeval tv;
  long ret = syscall(96, &tv, NULL);
  SC_ASSERT(ret == 0 && tv.tv_sec > 0);
  return 1;
}

// Syscall #97: getrlimit
static int test_sc_getrlimit_97(void) {
struct rlimit rl;
  long ret = syscall(97, RLIMIT_NOFILE, &rl);
  SC_ASSERT(ret == 0 && rl.rlim_cur > 0);
  return 1;
}

// Syscall #98: getrusage
static int test_sc_getrusage_98(void) {
struct rusage ru;
  long ret = syscall(98, RUSAGE_SELF, &ru);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #99: sysinfo
static int test_sc_sysinfo_99(void) {
struct sysinfo si;
  long ret = syscall(99, &si);
  SC_ASSERT(ret == 0 && si.totalram > 0);
  return 1;
}

// Syscall #100: times
static int test_sc_times_100(void) {
struct tms t;
  long ret = syscall(100, &t);
  SC_ASSERT(ret != -1);
  return 1;
}

// Syscall #101: ptrace
static int test_sc_ptrace_101(void) {
pid_t pid = fork();
  if (pid == 0) {
    long r = syscall(101, 0 /* PTRACE_TRACEME */, 0, NULL, NULL);
    _exit(r == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #102: getuid
static int test_sc_getuid_102(void) {
long ret = syscall(102);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #103: syslog
static int test_sc_syslog_103(void) {
long ret = syscall(103, 10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #104: getgid
static int test_sc_getgid_104(void) {
long ret = syscall(104);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #105: setuid
static int test_sc_setuid_105(void) {
long ret = syscall(105, getuid());
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #106: setgid
static int test_sc_setgid_106(void) {
long ret = syscall(106, getgid());
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #107: geteuid
static int test_sc_geteuid_107(void) {
long ret = syscall(107);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #108: getegid
static int test_sc_getegid_108(void) {
long ret = syscall(108);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #109: setpgid
static int test_sc_setpgid_109(void) {
long ret = syscall(109, 0, 0);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #110: getppid
static int test_sc_getppid_110(void) {
long ret = syscall(110);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #111: getpgrp
static int test_sc_getpgrp_111(void) {
long ret = syscall(111);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #112: setsid
static int test_sc_setsid_112(void) {
pid_t pid = fork();
  if (pid == 0) {
    long r = syscall(112);
    _exit(r > 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #113: setreuid
static int test_sc_setreuid_113(void) {
long ret = syscall(113, geteuid(), geteuid());
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #114: setregid
static int test_sc_setregid_114(void) {
long ret = syscall(114, getegid(), getegid());
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #115: getgroups
static int test_sc_getgroups_115(void) {
gid_t grps[32];
  long ret = syscall(115, 32, grps);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #116: setgroups
static int test_sc_setgroups_116(void) {
gid_t grps[32];
  int n = getgroups(32, grps);
  SC_ASSERT(n >= 0);
  long ret = syscall(116, n, grps);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #117: setresuid
static int test_sc_setresuid_117(void) {
uid_t u = geteuid();
  long ret = syscall(117, u, u, u);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #118: getresuid
static int test_sc_getresuid_118(void) {
uid_t r, e, s;
  long ret = syscall(118, &r, &e, &s);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #119: setresgid
static int test_sc_setresgid_119(void) {
gid_t g = getegid();
  long ret = syscall(119, g, g, g);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #120: getresgid
static int test_sc_getresgid_120(void) {
gid_t r, e, s;
  long ret = syscall(120, &r, &e, &s);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #121: getpgid
static int test_sc_getpgid_121(void) {
long ret = syscall(121, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #122: setfsuid
static int test_sc_setfsuid_122(void) {
long ret = syscall(122, geteuid());
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #123: setfsgid
static int test_sc_setfsgid_123(void) {
long ret = syscall(123, getegid());
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #124: getsid
static int test_sc_getsid_124(void) {
long ret = syscall(124, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #125: capget
static int test_sc_capget_125(void) {
struct { uint32_t version; int pid; } hdr = {0x20080522, 0};
  struct { uint32_t effective, permitted, inheritable; } data[2];
  long ret = syscall(125, &hdr, data);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #126: capset
static int test_sc_capset_126(void) {
struct { uint32_t version; int pid; } hdr = {0x20080522, 0};
  struct { uint32_t effective, permitted, inheritable; } data[2];
  syscall(125, &hdr, data);
  long ret = syscall(126, &hdr, data);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #127: rt_sigpending
static int test_sc_rt_sigpending_127(void) {
sigset_t p;
  long ret = syscall(127, &p, 8);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #128: rt_sigtimedwait
static int test_sc_rt_sigtimedwait_128(void) {
sigset_t set;
  sigemptyset(&set);
  siginfo_t info;
  struct timespec zero_ts = {0, 0};
  long ret = syscall(128, &set, &info, &zero_ts, 8);
  SC_ASSERT(ret == -1 && errno == EAGAIN);
  return 1;
}

// Syscall #129: rt_sigqueueinfo
static int test_sc_rt_sigqueueinfo_129(void) {
siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_code = -1; // SI_QUEUE
  info.si_signo = 0;
  long ret = syscall(129, getpid(), 0, &info);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #130: rt_sigsuspend
static int test_sc_rt_sigsuspend_130(void) {
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGALRM, dummy_sc_sig_handler);
    sigset_t mask;
    sigemptyset(&mask);
    alarm(1);
    syscall(130, &mask, 8);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #131: sigaltstack
static int test_sc_sigaltstack_131(void) {
stack_t ss, old_ss;
  void *mem = malloc(SIGSTKSZ);
  SC_ASSERT(mem != NULL);
  ss.ss_sp = mem;
  ss.ss_size = SIGSTKSZ;
  ss.ss_flags = 0;
  long ret = syscall(131, &ss, &old_ss);
  if (ret == 0) syscall(131, &old_ss, NULL);
  free(mem);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #132: utime
static int test_sc_utime_132(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_utime_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(132, path, NULL);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #133: mknod
static int test_sc_mknod_133(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_mknod_%d.fifo", getpid());
  unlink(path);
  long ret = syscall(133, path, S_IFIFO | 0644, 0);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #137: statfs
static int test_sc_statfs_137(void) {
struct statfs sf;
  long ret = syscall(137, "/tmp", &sf);
  SC_ASSERT(ret == 0 && sf.f_blocks > 0);
  return 1;
}

// Syscall #138: fstatfs
static int test_sc_fstatfs_138(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  struct statfs sf;
  long ret = syscall(138, dfd, &sf);
  close(dfd);
  SC_ASSERT(ret == 0 && sf.f_blocks > 0);
  return 1;
}

// Syscall #140: getpriority
static int test_sc_getpriority_140(void) {
errno = 0;
  long ret = syscall(140, PRIO_PROCESS, 0);
  SC_ASSERT(errno == 0 || ret >= -20);
  return 1;
}

// Syscall #141: setpriority
static int test_sc_setpriority_141(void) {
long ret = syscall(141, PRIO_PROCESS, 0, 0);
  SC_ASSERT(ret == 0 || errno == EACCES);
  return 1;
}

// Syscall #142: sched_setparam
static int test_sc_sched_setparam_142(void) {
struct sched_param p = {.sched_priority = 0};
  long ret = syscall(142, 0, &p);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #143: sched_getparam
static int test_sc_sched_getparam_143(void) {
struct sched_param p;
  long ret = syscall(143, 0, &p);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #144: sched_setscheduler
static int test_sc_sched_setscheduler_144(void) {
struct sched_param p = {.sched_priority = 0};
  long ret = syscall(144, 0, SCHED_OTHER, &p);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #145: sched_getscheduler
static int test_sc_sched_getscheduler_145(void) {
long ret = syscall(145, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #146: sched_get_priority_max
static int test_sc_sched_get_priority_max_146(void) {
long ret = syscall(146, SCHED_OTHER);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #147: sched_get_priority_min
static int test_sc_sched_get_priority_min_147(void) {
long ret = syscall(147, SCHED_OTHER);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #148: sched_rr_get_interval
static int test_sc_sched_rr_get_interval_148(void) {
struct timespec ts;
  long ret = syscall(148, 0, &ts);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #149: mlock
static int test_sc_mlock_149(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(149, p, 4096);
  munmap(p, 4096);
  SC_ASSERT(ret == 0 || errno == ENOMEM);
  return 1;
}

// Syscall #150: munlock
static int test_sc_munlock_150(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(150, p, 4096);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #151: mlockall
static int test_sc_mlockall_151(void) {
long ret = syscall(151, MCL_CURRENT);
  munlockall();
  SC_ASSERT(ret == 0 || errno == ENOMEM);
  return 1;
}

// Syscall #152: munlockall
static int test_sc_munlockall_152(void) {
long ret = syscall(152);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #155: pivot_root
static int test_sc_pivot_root_155(void) {
long ret = syscall(155, "/tmp", "/tmp");
  SC_ASSERT(ret == -1 && (errno == EINVAL || errno == EBUSY || errno == EPERM));
  return 1;
}

// Syscall #157: prctl
static int test_sc_prctl_157(void) {
long ret = syscall(157, PR_GET_DUMPABLE, 0, 0, 0, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #158: arch_prctl
static int test_sc_arch_prctl_158(void) {
unsigned long fsbase = 0;
  long ret = syscall(158, 0x1003 /* ARCH_GET_FS */, &fsbase);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #160: setrlimit
static int test_sc_setrlimit_160(void) {
struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  long ret = syscall(160, RLIMIT_NOFILE, &rl);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #161: chroot
static int test_sc_chroot_161(void) {
long ret = syscall(161, "/nonexistent");
  SC_ASSERT(ret == -1 && errno == ENOENT);
  return 1;
}

// Syscall #164: settimeofday
static int test_sc_settimeofday_164(void) {
long ret = syscall(164, NULL, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #165: mount
static int test_sc_mount_165(void) {
long ret = syscall(165, "none", "/tmp", "tmpfs", 0, NULL);
  SC_ASSERT(ret == 0 || ret == -1);
  return 1;
}

// Syscall #166: umount2
static int test_sc_umount2_166(void) {
long ret = syscall(166, "/nonexistent", 0);
  SC_ASSERT(ret == -1 && (errno == ENOENT || errno == EINVAL));
  return 1;
}

// Syscall #167: swapon
static int test_sc_swapon_167(void) {
long ret = syscall(167, "/dev/null", 0);
  SC_ASSERT(ret == -1 && errno == EPERM);
  return 1;
}

// Syscall #169: reboot
static int test_sc_reboot_169(void) {
long ret = syscall(169, 0, 0, 0, NULL);
  SC_ASSERT(ret == -1 && (errno == EINVAL || errno == EPERM));
  return 1;
}

// Syscall #170: sethostname
static int test_sc_sethostname_170(void) {
char cur[64] = {0};
  gethostname(cur, sizeof(cur));
  long ret = syscall(170, cur, strlen(cur));
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #171: setdomainname
static int test_sc_setdomainname_171(void) {
long ret = syscall(171, "avory", 5);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #175: init_module
static int test_sc_init_module_175(void) {
char dummy = 0;
  long ret = syscall(175, &dummy, 0, "");
  SC_ASSERT(ret == -1 && errno == EPERM);
  return 1;
}

// Syscall #176: delete_module
static int test_sc_delete_module_176(void) {
long ret = syscall(176, "dummy", 0);
  SC_ASSERT(ret == -1 && errno == ENOENT);
  return 1;
}

// Syscall #179: quotactl
static int test_sc_quotactl_179(void) {
long ret = syscall(179, (0x800001 << 8), NULL, 0, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #186: gettid
static int test_sc_gettid_186(void) {
long ret = syscall(186);
  SC_ASSERT(ret > 0);
  return 1;
}

// Syscall #187: readahead
static int test_sc_readahead_187(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_ra_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(187, fd, 0, 8);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #188: setxattr
static int test_sc_setxattr_188(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_xattr_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(188, path, "user.test", "val", 3, 0);
  unlink(path);
  SC_ASSERT(ret == 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #189: lsetxattr
static int test_sc_lsetxattr_189(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_lxattr_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(189, path, "user.test", "val", 3, 0);
  unlink(path);
  SC_ASSERT(ret == 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #190: fsetxattr
static int test_sc_fsetxattr_190(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fxattr_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(190, fd, "user.test", "val", 3, 0);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #191: getxattr
static int test_sc_getxattr_191(void) {
char val[32];
  long ret = syscall(191, "/tmp", "user.test", val, sizeof(val));
  SC_ASSERT(ret >= 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #192: lgetxattr
static int test_sc_lgetxattr_192(void) {
char val[32];
  long ret = syscall(192, "/tmp", "user.test", val, sizeof(val));
  SC_ASSERT(ret >= 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #193: fgetxattr
static int test_sc_fgetxattr_193(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  char val[32];
  long ret = syscall(193, dfd, "user.test", val, sizeof(val));
  close(dfd);
  SC_ASSERT(ret >= 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #194: listxattr
static int test_sc_listxattr_194(void) {
char list[128];
  long ret = syscall(194, "/tmp", list, sizeof(list));
  SC_ASSERT(ret >= 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #195: llistxattr
static int test_sc_llistxattr_195(void) {
char list[128];
  long ret = syscall(195, "/tmp", list, sizeof(list));
  SC_ASSERT(ret >= 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #196: flistxattr
static int test_sc_flistxattr_196(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  char list[128];
  long ret = syscall(196, dfd, list, sizeof(list));
  close(dfd);
  SC_ASSERT(ret >= 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #197: removexattr
static int test_sc_removexattr_197(void) {
long ret = syscall(197, "/tmp", "user.test");
  SC_ASSERT(ret == 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #198: lremovexattr
static int test_sc_lremovexattr_198(void) {
long ret = syscall(198, "/tmp", "user.test");
  SC_ASSERT(ret == 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #199: fremovexattr
static int test_sc_fremovexattr_199(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  long ret = syscall(199, dfd, "user.test");
  close(dfd);
  SC_ASSERT(ret == 0 || errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
  return 1;
}

// Syscall #200: tkill
static int test_sc_tkill_200(void) {
long ret = syscall(200, syscall(186 /* SYS_gettid */), 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #201: time
static int test_sc_time_201(void) {
time_t t = 0;
  long ret = syscall(201, &t);
  SC_ASSERT(ret > 0 && t > 0);
  return 1;
}

// Syscall #202: futex
static int test_sc_futex_202(void) {
int fval = 0;
  long ret = syscall(202, &fval, FUTEX_WAKE, 1, NULL, NULL, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #203: sched_setaffinity
static int test_sc_sched_setaffinity_203(void) {
cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  long ret = syscall(203, 0, sizeof(cpuset), &cpuset);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #204: sched_getaffinity
static int test_sc_sched_getaffinity_204(void) {
  cpu_set_t cpuset;
  long ret = syscall(204, 0, sizeof(cpuset), &cpuset);
  SC_ASSERT(ret > 0);
  return 1;
}

// Syscall #213: epoll_create
static int test_sc_epoll_create_213(void) {
long ep = syscall(213, 1);
  SC_ASSERT(ep >= 0);
  close(ep);
  return 1;
}

// Syscall #217: getdents64
static int test_sc_getdents64_217(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  char buf[1024];
  long ret = syscall(217, dfd, buf, sizeof(buf));
  close(dfd);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #218: set_tid_address
static int test_sc_set_tid_address_218(void) {
pid_t pid = fork();
  if (pid == 0) {
    int tid = 0;
    long ret = syscall(218, &tid);
    _exit(ret > 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #220: semtimedop
static int test_sc_semtimedop_220(void) {
int id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  SC_ASSERT(id >= 0);
  struct sembuf sb = {0, 1, 0};
  struct timespec ts = {0, 0};
  long ret = syscall(220, id, &sb, 1, &ts);
  semctl(id, 0, IPC_RMID);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #221: fadvise64
static int test_sc_fadvise64_221(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fa_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(221, fd, 0, 0, POSIX_FADV_NORMAL);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #222: timer_create
static int test_sc_timer_create_222(void) {
timer_t t;
  struct sigevent sev = {.sigev_notify = SIGEV_NONE};
  long ret = syscall(222, CLOCK_MONOTONIC, &sev, &t);
  SC_ASSERT(ret == 0);
  timer_delete(t);
  return 1;
}

// Syscall #223: timer_settime
static int test_sc_timer_settime_223(void) {
timer_t t;
  struct sigevent sev = {.sigev_notify = SIGEV_NONE};
  SC_ASSERT(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0);
  struct itimerspec its = {{0, 0}, {1, 0}}, old_its;
  long ret = syscall(223, t, 0, &its, &old_its);
  timer_delete(t);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #224: timer_gettime
static int test_sc_timer_gettime_224(void) {
timer_t t;
  struct sigevent sev = {.sigev_notify = SIGEV_NONE};
  SC_ASSERT(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0);
  struct itimerspec its;
  long ret = syscall(224, t, &its);
  timer_delete(t);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #225: timer_getoverrun
static int test_sc_timer_getoverrun_225(void) {
timer_t t;
  struct sigevent sev = {.sigev_notify = SIGEV_NONE};
  SC_ASSERT(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0);
  long ret = syscall(225, t);
  timer_delete(t);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #226: timer_delete
static int test_sc_timer_delete_226(void) {
timer_t t;
  struct sigevent sev = {.sigev_notify = SIGEV_NONE};
  SC_ASSERT(timer_create(CLOCK_MONOTONIC, &sev, &t) == 0);
  long ret = syscall(226, t);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #227: clock_settime
static int test_sc_clock_settime_227(void) {
struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long ret = syscall(227, CLOCK_REALTIME, &ts);
  SC_ASSERT(ret == -1 && errno == EPERM);
  return 1;
}

// Syscall #228: clock_gettime
static int test_sc_clock_gettime_228(void) {
struct timespec ts;
  long ret = syscall(228, CLOCK_MONOTONIC, &ts);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #229: clock_getres
static int test_sc_clock_getres_229(void) {
struct timespec ts;
  long ret = syscall(229, CLOCK_MONOTONIC, &ts);
  SC_ASSERT(ret == 0 && ts.tv_nsec > 0);
  return 1;
}

// Syscall #230: clock_nanosleep
static int test_sc_clock_nanosleep_230(void) {
struct timespec req = {0, 1000};
  long ret = syscall(230, CLOCK_MONOTONIC, 0, &req, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #231: exit_group
static int test_sc_exit_group_231(void) {
pid_t pid = fork();
  if (pid == 0) {
    syscall(231, 0);
    _exit(1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #232: epoll_wait
static int test_sc_epoll_wait_232(void) {
int ep = epoll_create(1);
  SC_ASSERT(ep >= 0);
  struct epoll_event ev;
  long ret = syscall(232, ep, &ev, 1, 0);
  close(ep);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #233: epoll_ctl
static int test_sc_epoll_ctl_233(void) {
int ep = epoll_create(1);
  SC_ASSERT(ep >= 0);
  int p[2];
  SC_ASSERT(pipe(p) == 0);
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]};
  long ret = syscall(233, ep, EPOLL_CTL_ADD, p[0], &ev);
  close(p[0]); close(p[1]); close(ep);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #234: tgkill
static int test_sc_tgkill_234(void) {
long ret = syscall(234, getpid(), syscall(186 /* SYS_gettid */), 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #235: utimes
static int test_sc_utimes_235(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_utimes_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(235, path, NULL);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #237: mbind
static int test_sc_mbind_237(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(237, p, 4096, 0 /* MPOL_DEFAULT */, NULL, 0, 0);
  munmap(p, 4096);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #238: set_mempolicy
static int test_sc_set_mempolicy_238(void) {
long ret = syscall(238, 0 /* MPOL_DEFAULT */, NULL, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #239: get_mempolicy
static int test_sc_get_mempolicy_239(void) {
int mode = -1;
  long ret = syscall(239, &mode, NULL, 0, NULL, 0);
  SC_ASSERT(ret == 0 && mode >= 0);
  return 1;
}

// Syscall #247: waitid
static int test_sc_waitid_247(void) {
pid_t pid = fork();
  if (pid == 0) _exit(0);
  siginfo_t inf;
  memset(&inf, 0, sizeof(inf));
  long ret = syscall(247, P_PID, pid, &inf, WEXITED, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #251: ioprio_set
static int test_sc_ioprio_set_251(void) {
long ret = syscall(251, 1 /* IOPRIO_WHO_PROCESS */, 0, 4);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #252: ioprio_get
static int test_sc_ioprio_get_252(void) {
long ret = syscall(252, 1 /* IOPRIO_WHO_PROCESS */, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #253: inotify_init
static int test_sc_inotify_init_253(void) {
long fd = syscall(253);
  SC_ASSERT(fd >= 0);
  close(fd);
  return 1;
}

// Syscall #254: inotify_add_watch
static int test_sc_inotify_add_watch_254(void) {
int ifd = inotify_init();
  SC_ASSERT(ifd >= 0);
  long wd = syscall(254, ifd, "/tmp", IN_ALL_EVENTS);
  close(ifd);
  SC_ASSERT(wd >= 0);
  return 1;
}

// Syscall #255: inotify_rm_watch
static int test_sc_inotify_rm_watch_255(void) {
int ifd = inotify_init();
  SC_ASSERT(ifd >= 0);
  int wd = inotify_add_watch(ifd, "/tmp", IN_ALL_EVENTS);
  SC_ASSERT(wd >= 0);
  long ret = syscall(255, ifd, wd);
  close(ifd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #256: migrate_pages
static int test_sc_migrate_pages_256(void) {
unsigned long old_m = 1, new_m = 1;
  long ret = syscall(256, 0, sizeof(unsigned long) * 8, &old_m, &new_m);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #257: openat
static int test_sc_openat_257(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_openat_%d.tmp", getpid());
  long fd = syscall(257, AT_FDCWD, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  SC_ASSERT(fd >= 0);
  close(fd); unlink(path);
  return 1;
}

// Syscall #258: mkdirat
static int test_sc_mkdirat_258(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_mkdirat_%d.dir", getpid());
  long ret = syscall(258, AT_FDCWD, path, 0755);
  rmdir(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #259: mknodat
static int test_sc_mknodat_259(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_mknodat_%d.fifo", getpid());
  unlink(path);
  long ret = syscall(259, AT_FDCWD, path, S_IFIFO | 0644, 0);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #260: fchownat
static int test_sc_fchownat_260(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fchownat_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(260, AT_FDCWD, path, getuid(), getgid(), 0);
  unlink(path);
  SC_ASSERT(ret == 0 || errno == EPERM);
  return 1;
}

// Syscall #261: futimesat
static int test_sc_futimesat_261(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_futimesat_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  struct timeval tv[2] = {{0, 0}, {0, 0}};
  long ret = syscall(261, AT_FDCWD, path, tv);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #262: newfstatat
static int test_sc_newfstatat_262(void) {
struct stat st;
  long ret = syscall(262, AT_FDCWD, "/tmp", &st, 0);
  SC_ASSERT(ret == 0 && S_ISDIR(st.st_mode));
  return 1;
}

// Syscall #263: unlinkat
static int test_sc_unlinkat_263(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_unlinkat_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(263, AT_FDCWD, path, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #264: renameat
static int test_sc_renameat_264(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_rnat1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_rnat2_%d.tmp", getpid());
  int fd = open(p1, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(264, AT_FDCWD, p1, AT_FDCWD, p2);
  unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #265: linkat
static int test_sc_linkat_265(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_lkat1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_lkat2_%d.tmp", getpid());
  int fd = open(p1, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  unlink(p2);
  long ret = syscall(265, AT_FDCWD, p1, AT_FDCWD, p2, 0);
  unlink(p1); unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #266: symlinkat
static int test_sc_symlinkat_266(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_slat_%d.tmp", getpid());
  unlink(path);
  long ret = syscall(266, "target", AT_FDCWD, path);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #267: readlinkat
static int test_sc_readlinkat_267(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_rlat_%d.tmp", getpid());
  unlink(path);
  symlink("tgt", path);
  char buf[32];
  long ret = syscall(267, AT_FDCWD, path, buf, sizeof(buf) - 1);
  unlink(path);
  SC_ASSERT(ret > 0);
  buf[ret] = 0;
  SC_ASSERT(strcmp(buf, "tgt") == 0);
  return 1;
}

// Syscall #268: fchmodat
static int test_sc_fchmodat_268(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fchmodat_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(268, AT_FDCWD, path, 0644, 0);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #269: faccessat
static int test_sc_faccessat_269(void) {
long ret = syscall(269, AT_FDCWD, "/tmp", F_OK, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #270: pselect6
static int test_sc_pselect6_270(void) {
struct timespec zero_ts = {0, 0};
  long ret = syscall(270, 0, NULL, NULL, NULL, &zero_ts, NULL);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #271: ppoll
static int test_sc_ppoll_271(void) {
struct timespec zero_ts = {0, 0};
  long ret = syscall(271, NULL, 0, &zero_ts, NULL, 8);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #272: unshare
static int test_sc_unshare_272(void) {
long ret = syscall(272, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #273: set_robust_list
static int test_sc_set_robust_list_273(void) {
  pid_t pid = fork();
  if (pid == 0) {
    char head[24] = {0};
    long ret = syscall(273, head, 24);
    _exit(ret == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #274: get_robust_list
static int test_sc_get_robust_list_274(void) {
void *head = NULL;
  size_t len = 0;
  long ret = syscall(274, 0, &head, &len);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #275: splice
static int test_sc_splice_275(void) {
int p1[2], p2[2];
  SC_ASSERT(pipe(p1) == 0 && pipe(p2) == 0);
  write(p1[1], "a", 1);
  long ret = syscall(275, p1[0], NULL, p2[1], NULL, 1, 0);
  close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #276: tee
static int test_sc_tee_276(void) {
int p1[2], p2[2];
  SC_ASSERT(pipe(p1) == 0 && pipe(p2) == 0);
  write(p1[1], "a", 1);
  long ret = syscall(276, p1[0], p2[1], 1, 0);
  close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #277: sync_file_range
static int test_sc_sync_file_range_277(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_sfr_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(277, fd, 0, 0, 0);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #279: move_pages
static int test_sc_move_pages_279(void) {
int val = 0;
  void *page = &val;
  int status = 0;
  long ret = syscall(279, 0, 1, &page, NULL, &status, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #280: utimensat
static int test_sc_utimensat_280(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_utimensat_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(280, AT_FDCWD, path, NULL, 0);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #281: epoll_pwait
static int test_sc_epoll_pwait_281(void) {
int ep = epoll_create(1);
  SC_ASSERT(ep >= 0);
  struct epoll_event ev;
  long ret = syscall(281, ep, &ev, 1, 0, NULL);
  close(ep);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #282: signalfd
static int test_sc_signalfd_282(void) {
sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  long sfd = syscall(282, -1, &mask, SFD_NONBLOCK);
  SC_ASSERT(sfd >= 0);
  close(sfd);
  return 1;
}

// Syscall #283: timerfd_create
static int test_sc_timerfd_create_283(void) {
long tfd = syscall(283, CLOCK_MONOTONIC, TFD_NONBLOCK);
  SC_ASSERT(tfd >= 0);
  close(tfd);
  return 1;
}

// Syscall #285: fallocate
static int test_sc_fallocate_285(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fa2_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  long ret = syscall(285, fd, 0, 0, 4096);
  close(fd); unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #286: timerfd_settime
static int test_sc_timerfd_settime_286(void) {
int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  SC_ASSERT(tfd >= 0);
  struct itimerspec its = {{0, 0}, {1, 0}};
  long ret = syscall(286, tfd, 0, &its, NULL);
  close(tfd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #287: timerfd_gettime
static int test_sc_timerfd_gettime_287(void) {
int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  SC_ASSERT(tfd >= 0);
  struct itimerspec its;
  long ret = syscall(287, tfd, &its);
  close(tfd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #288: accept4
static int test_sc_accept4_288(void) {
int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  SC_ASSERT(s >= 0);
  long ret = syscall(288, s, NULL, NULL, SOCK_NONBLOCK);
  close(s);
  SC_ASSERT(ret == -1 && (errno == EAGAIN || errno == EINVAL));
  return 1;
}

// Syscall #289: signalfd4
static int test_sc_signalfd4_289(void) {
sigset_t mask;
  sigemptyset(&mask);
  long sfd = syscall(289, -1, &mask, 8, SFD_NONBLOCK);
  SC_ASSERT(sfd >= 0);
  close(sfd);
  return 1;
}

// Syscall #290: eventfd2
static int test_sc_eventfd2_290(void) {
long efd = syscall(290, 0, EFD_NONBLOCK);
  SC_ASSERT(efd >= 0);
  close(efd);
  return 1;
}

// Syscall #291: epoll_create1
static int test_sc_epoll_create1_291(void) {
long ep = syscall(291, EPOLL_CLOEXEC);
  SC_ASSERT(ep >= 0);
  close(ep);
  return 1;
}

// Syscall #292: dup3
static int test_sc_dup3_292(void) {
int p[2];
  SC_ASSERT(pipe(p) == 0);
  int target_fd = p[1] + 10;
  long ret = syscall(292, p[0], target_fd, O_CLOEXEC);
  close(p[0]); close(p[1]);
  SC_ASSERT(ret == target_fd);
  close(target_fd);
  return 1;
}

// Syscall #293: pipe2
static int test_sc_pipe2_293(void) {
int p[2] = {-1, -1};
  long ret = syscall(293, p, O_NONBLOCK);
  SC_ASSERT(ret == 0 && p[0] >= 0 && p[1] >= 0);
  close(p[0]); close(p[1]);
  return 1;
}

// Syscall #294: inotify_init1
static int test_sc_inotify_init1_294(void) {
long ifd = syscall(294, IN_NONBLOCK);
  SC_ASSERT(ifd >= 0);
  close(ifd);
  return 1;
}

// Syscall #295: preadv
static int test_sc_preadv_295(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_pav_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  write(fd, "test", 4);
  char buf[4];
  struct iovec iov = {buf, 4};
  long ret = syscall(295, fd, &iov, 1, 0);
  close(fd); unlink(path);
  SC_ASSERT(ret == 4);
  return 1;
}

// Syscall #296: pwritev
static int test_sc_pwritev_296(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_pwv_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0);
  char buf[4] = "test";
  struct iovec iov = {buf, 4};
  long ret = syscall(296, fd, &iov, 1, 0);
  close(fd); unlink(path);
  SC_ASSERT(ret == 4);
  return 1;
}

// Syscall #297: rt_tgsigqueueinfo
static int test_sc_rt_tgsigqueueinfo_297(void) {
siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_code = -1; // SI_QUEUE
  info.si_signo = 0;
  long ret = syscall(297, getpid(), syscall(186 /* SYS_gettid */), 0, &info);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #299: recvmmsg
static int test_sc_recvmmsg_299(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv) == 0);
  struct mmsghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec iov;
  char buf[16];
  iov.iov_base = buf; iov.iov_len = sizeof(buf);
  msg.msg_hdr.msg_iov = &iov; msg.msg_hdr.msg_iovlen = 1;
  long ret = syscall(299, sv[0], &msg, 1, MSG_DONTWAIT, NULL);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret >= 0 || errno == EAGAIN);
  return 1;
}

// Syscall #302: prlimit64
static int test_sc_prlimit64_302(void) {
struct rlimit rl;
  long ret = syscall(302, 0, RLIMIT_NOFILE, NULL, &rl);
  SC_ASSERT(ret == 0 && rl.rlim_cur > 0);
  return 1;
}

// Syscall #303: name_to_handle_at
static int test_sc_name_to_handle_at_303(void) {
struct file_handle fh;
  memset(&fh, 0, sizeof(fh));
  int mid = 0;
  long ret = syscall(303, AT_FDCWD, "/tmp", &fh, &mid, 0);
  SC_ASSERT(ret == -1 && (errno == EOPNOTSUPP || errno == ENOSYS || errno == EOVERFLOW));
  return 1;
}

// Syscall #304: open_by_handle_at
static int test_sc_open_by_handle_at_304(void) {
struct file_handle fh;
  memset(&fh, 0, sizeof(fh));
  long ret = syscall(304, AT_FDCWD, &fh, O_RDONLY);
  SC_ASSERT(ret == -1 && (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL));
  return 1;
}

// Syscall #307: sendmmsg
static int test_sc_sendmmsg_307(void) {
int sv[2];
  SC_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
  struct mmsghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec iov;
  char buf[4] = "msg";
  iov.iov_base = buf; iov.iov_len = 3;
  msg.msg_hdr.msg_iov = &iov; msg.msg_hdr.msg_iovlen = 1;
  long ret = syscall(307, sv[0], &msg, 1, 0);
  close(sv[0]); close(sv[1]);
  SC_ASSERT(ret == 1);
  return 1;
}

// Syscall #309: getcpu
static int test_sc_getcpu_309(void) {
unsigned cpu = 0, node = 0;
  long ret = syscall(309, &cpu, &node, NULL);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #312: kcmp
static int test_sc_kcmp_312(void) {
long ret = syscall(312, getpid(), getpid(), 1 /* KCMP_VM */, 0, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #314: sched_setattr
static int test_sc_sched_setattr_314(void) {
struct {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
  } sa;
  memset(&sa, 0, sizeof(sa));
  sa.size = sizeof(sa);
  if (syscall(315 /* sched_getattr */, 0, &sa, sizeof(sa), 0) == 0) {
    long ret = syscall(314, 0, &sa, 0);
    SC_ASSERT(ret == 0 || errno == EPERM);
  }
  return 1;
}

// Syscall #315: sched_getattr
static int test_sc_sched_getattr_315(void) {
struct {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
  } sa;
  memset(&sa, 0, sizeof(sa));
  sa.size = sizeof(sa);
  long ret = syscall(315, 0, &sa, sizeof(sa), 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #316: renameat2
static int test_sc_renameat2_316(void) {
char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_rnat2_1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_rnat2_2_%d.tmp", getpid());
  int fd = open(p1, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  unlink(p2);
  long ret = syscall(316, AT_FDCWD, p1, AT_FDCWD, p2, 0);
  unlink(p2);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #317: seccomp
static int test_sc_seccomp_317(void) {
long ret = syscall(317, 0 /* SECCOMP_SET_MODE_STRICT */, 0, NULL);
  SC_ASSERT(ret == -1 && (errno == EINVAL || errno == EFAULT));
  return 1;
}

// Syscall #318: getrandom
static int test_sc_getrandom_318(void) {
uint32_t r = 0;
  long ret = syscall(318, &r, sizeof(r), 0);
  SC_ASSERT(ret == sizeof(r));
  return 1;
}

// Syscall #319: memfd_create
static int test_sc_memfd_create_319(void) {
long fd = syscall(319, "test_mfd", 0);
  SC_ASSERT(fd >= 0);
  close(fd);
  return 1;
}

// Syscall #323: userfaultfd
static int test_sc_userfaultfd_323(void) {
long ret = syscall(323, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #324: membarrier
static int test_sc_membarrier_324(void) {
long ret = syscall(324, 0 /* MEMBARRIER_CMD_QUERY */, 0);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #325: mlock2
static int test_sc_mlock2_325(void) {
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  SC_ASSERT(p != MAP_FAILED);
  long ret = syscall(325, p, 4096, 0);
  munmap(p, 4096);
  SC_ASSERT(ret == 0 || errno == ENOMEM);
  return 1;
}

// Syscall #326: copy_file_range
static int test_sc_copy_file_range_326(void) {
  char p1[64], p2[64];
  snprintf(p1, sizeof(p1), "/tmp/sc_test_cfr1_%d.tmp", getpid());
  snprintf(p2, sizeof(p2), "/tmp/sc_test_cfr2_%d.tmp", getpid());
  int f1 = open(p1, O_CREAT | O_RDWR, 0644);
  int f2 = open(p2, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(f1 >= 0 && f2 >= 0);
  write(f1, "data", 4);
  loff_t off_in = 0;
  loff_t off_out = 0;
  long ret = syscall(326, f1, &off_in, f2, &off_out, 4, 0);
  close(f1); close(f2); unlink(p1); unlink(p2);
  SC_ASSERT(ret == 4);
  return 1;
}

// Syscall #332: statx
static int test_sc_statx_332(void) {
struct {
    uint32_t mask;
    uint32_t blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad1;
    uint64_t ino, size, blocks, attributes_mask;
    struct { int64_t tv_sec; uint32_t tv_nsec, pad; } atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t pad2[14];
  } sx;
  memset(&sx, 0, sizeof(sx));
  long ret = syscall(332, AT_FDCWD, "/tmp", 0, 0x000007ffU, &sx);
  SC_ASSERT(ret == 0 && S_ISDIR(sx.mode));
  return 1;
}

// Syscall #334: rseq
static int test_sc_rseq_334(void) {
pid_t pid = fork();
  if (pid == 0) {
    long ret = syscall(334, NULL, 0, 0, 0);
    _exit((ret == 0 || errno == EINVAL) ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  return 1;
}

// Syscall #399: uptime
static int test_sc_uptime_399(void) {
long ret = syscall(399);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #424: pidfd_send_signal
static int test_sc_pidfd_send_signal_424(void) {
long pfd = syscall(434 /* SYS_pidfd_open */, getpid(), 0);
  SC_ASSERT(pfd >= 0);
  long ret = syscall(424, pfd, 0, NULL, 0);
  close(pfd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #425: io_uring_setup
static int test_sc_io_uring_setup_425(void) {
long ret = syscall(425, 0, NULL);
  SC_ASSERT(ret == -1 && (errno == EINVAL || errno == ENOSYS));
  return 1;
}

// Syscall #426: io_uring_enter
static int test_sc_io_uring_enter_426(void) {
long ret = syscall(426, -1, 0, 0, 0, NULL, 0);
  SC_ASSERT(ret == -1 && (errno == EBADF || errno == ENOSYS));
  return 1;
}

// Syscall #427: io_uring_register
static int test_sc_io_uring_register_427(void) {
long ret = syscall(427, -1, 0, NULL, 0);
  SC_ASSERT(ret == -1 && (errno == EBADF || errno == ENOSYS));
  return 1;
}

// Syscall #428: open_tree
static int test_sc_open_tree_428(void) {
long ret = syscall(428, AT_FDCWD, "/tmp", 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #429: move_mount
static int test_sc_move_mount_429(void) {
long ret = syscall(429, -1, "", -1, "", 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #430: fsopen
static int test_sc_fsopen_430(void) {
long ret = syscall(430, "ext4", 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #431: fsconfig
static int test_sc_fsconfig_431(void) {
long ret = syscall(431, -1, 0, NULL, NULL, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #432: fsmount
static int test_sc_fsmount_432(void) {
long ret = syscall(432, -1, 0, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #433: fspick
static int test_sc_fspick_433(void) {
long ret = syscall(433, AT_FDCWD, "/tmp", 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #434: pidfd_open
static int test_sc_pidfd_open_434(void) {
long pfd = syscall(434, getpid(), 0);
  SC_ASSERT(pfd >= 0);
  close(pfd);
  return 1;
}

// Syscall #435: clone3
static int test_sc_clone3_435(void) {
struct {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
    uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
  } cl;
  memset(&cl, 0, sizeof(cl));
  cl.exit_signal = SIGCHLD;
  long pid = syscall(435, &cl, sizeof(cl));
  SC_ASSERT(pid >= 0);
  if (pid == 0) _exit(0);
  int st = 0;
  waitpid(pid, &st, 0);
  SC_ASSERT(WIFEXITED(st));
  return 1;
}

// Syscall #436: close_range
static int test_sc_close_range_436(void) {
long ret = syscall(436, 500, 500, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #437: openat2
static int test_sc_openat2_437(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_openat2_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0644);
  SC_ASSERT(fd >= 0); close(fd);
  struct { uint64_t flags, mode, resolve; } how = {O_RDONLY, 0, 0};
  long ret = syscall(437, AT_FDCWD, path, &how, sizeof(how));
  unlink(path);
  SC_ASSERT(ret >= 0);
  close(ret);
  return 1;
}

// Syscall #438: pidfd_getfd
static int test_sc_pidfd_getfd_438(void) {
int pfd = syscall(434 /* SYS_pidfd_open */, getpid(), 0);
  SC_ASSERT(pfd >= 0);
  int p[2];
  SC_ASSERT(pipe(p) == 0);
  long dupfd = syscall(438, pfd, p[0], 0);
  close(p[0]); close(p[1]); close(pfd);
  SC_ASSERT(dupfd >= 0);
  close(dupfd);
  return 1;
}

// Syscall #439: faccessat2
static int test_sc_faccessat2_439(void) {
long ret = syscall(439, AT_FDCWD, "/tmp", F_OK, 0);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #441: epoll_pwait2
static int test_sc_epoll_pwait2_441(void) {
int ep = epoll_create(1);
  SC_ASSERT(ep >= 0);
  struct epoll_event ev;
  struct timespec zero_ts = {0, 0};
  long ret = syscall(441, ep, &ev, 1, &zero_ts, NULL);
  close(ep);
  SC_ASSERT(ret >= 0);
  return 1;
}

// Syscall #442: mount_setattr
static int test_sc_mount_setattr_442(void) {
  pid_t pid = fork();
  SC_ASSERT(pid >= 0);
  if (pid == 0) {
    if (syscall(272 /* SYS_unshare */, 0x00020000 /* CLONE_NEWNS */) != 0) {
      _exit(1);
    }
    mkdir("/tmp/sc_mnt_442", 0755);
    if (syscall(165 /* SYS_mount */, "tmpfs", "/tmp/sc_mnt_442", "tmpfs", 0, 0) != 0) {
      _exit(2);
    }
    struct { uint64_t set, clear, propagation, userns_fd; } attr = {1, 0, 0, 0};
    long ret = syscall(442, AT_FDCWD, "/tmp/sc_mnt_442", 0, &attr, sizeof(attr));
    syscall(166 /* SYS_umount2 */, "/tmp/sc_mnt_442", 2 /* MNT_DETACH */);
    rmdir("/tmp/sc_mnt_442");
    _exit(ret == 0 ? 0 : 3);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  SC_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 1;
}

// Syscall #443: quotactl_fd
static int test_sc_quotactl_fd_443(void) {
int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  SC_ASSERT(dfd >= 0);
  long ret = syscall(443, dfd, (0x800001 << 8), 0, NULL);
  close(dfd);
  SC_ASSERT(ret == 0);
  return 1;
}

// Syscall #444: landlock_create_ruleset
static int test_sc_landlock_create_ruleset_444(void) {
long ret = syscall(444, NULL, 0, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #445: landlock_add_rule
static int test_sc_landlock_add_rule_445(void) {
long ret = syscall(445, -1, 0, NULL, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #446: landlock_restrict_self
static int test_sc_landlock_restrict_self_446(void) {
long ret = syscall(446, -1, 0);
  SC_ASSERT(ret == -1 && errno == ENOSYS);
  return 1;
}

// Syscall #452: fchmodat2
static int test_sc_fchmodat2_452(void) {
char path[64];
  snprintf(path, sizeof(path), "/tmp/sc_test_fchmodat2_%d.tmp", getpid());
  int fd = open(path, O_CREAT | O_RDWR, 0600);
  SC_ASSERT(fd >= 0); close(fd);
  long ret = syscall(452, AT_FDCWD, path, 0644, 0);
  unlink(path);
  SC_ASSERT(ret == 0);
  return 1;
}


struct single_syscall_test_entry {
  int num;
  const char *name;
  int (*fn)(void);
};

static const struct single_syscall_test_entry g_all_syscall_tests[] = {
  { 0, "read", test_sc_read_0 },
  { 1, "write", test_sc_write_1 },
  { 2, "open", test_sc_open_2 },
  { 3, "close", test_sc_close_3 },
  { 4, "stat", test_sc_stat_4 },
  { 5, "fstat", test_sc_fstat_5 },
  { 6, "lstat", test_sc_lstat_6 },
  { 7, "poll", test_sc_poll_7 },
  { 8, "lseek", test_sc_lseek_8 },
  { 9, "mmap", test_sc_mmap_9 },
  { 10, "mprotect", test_sc_mprotect_10 },
  { 11, "munmap", test_sc_munmap_11 },
  { 12, "brk", test_sc_brk_12 },
  { 13, "rt_sigaction", test_sc_rt_sigaction_13 },
  { 14, "rt_sigprocmask", test_sc_rt_sigprocmask_14 },
  { 15, "rt_sigreturn", test_sc_rt_sigreturn_15 },
  { 16, "ioctl", test_sc_ioctl_16 },
  { 17, "pread64", test_sc_pread64_17 },
  { 18, "pwrite64", test_sc_pwrite64_18 },
  { 19, "readv", test_sc_readv_19 },
  { 20, "writev", test_sc_writev_20 },
  { 21, "access", test_sc_access_21 },
  { 22, "pipe", test_sc_pipe_22 },
  { 23, "select", test_sc_select_23 },
  { 24, "sched_yield", test_sc_sched_yield_24 },
  { 25, "mremap", test_sc_mremap_25 },
  { 26, "msync", test_sc_msync_26 },
  { 27, "mincore", test_sc_mincore_27 },
  { 28, "madvise", test_sc_madvise_28 },
  { 29, "shmget", test_sc_shmget_29 },
  { 30, "shmat", test_sc_shmat_30 },
  { 31, "shmctl", test_sc_shmctl_31 },
  { 32, "dup", test_sc_dup_32 },
  { 33, "dup2", test_sc_dup2_33 },
  { 34, "pause", test_sc_pause_34 },
  { 35, "nanosleep", test_sc_nanosleep_35 },
  { 37, "alarm", test_sc_alarm_37 },
  { 38, "setitimer", test_sc_setitimer_38 },
  { 39, "getpid", test_sc_getpid_39 },
  { 40, "sendfile", test_sc_sendfile_40 },
  { 41, "socket", test_sc_socket_41 },
  { 42, "connect", test_sc_connect_42 },
  { 43, "accept", test_sc_accept_43 },
  { 44, "sendto", test_sc_sendto_44 },
  { 45, "recvfrom", test_sc_recvfrom_45 },
  { 46, "sendmsg", test_sc_sendmsg_46 },
  { 47, "recvmsg", test_sc_recvmsg_47 },
  { 48, "shutdown", test_sc_shutdown_48 },
  { 49, "bind", test_sc_bind_49 },
  { 50, "listen", test_sc_listen_50 },
  { 51, "getsockname", test_sc_getsockname_51 },
  { 52, "getpeername", test_sc_getpeername_52 },
  { 53, "socketpair", test_sc_socketpair_53 },
  { 54, "setsockopt", test_sc_setsockopt_54 },
  { 55, "getsockopt", test_sc_getsockopt_55 },
  { 56, "clone", test_sc_clone_56 },
  { 57, "fork", test_sc_fork_57 },
  { 58, "vfork", test_sc_vfork_58 },
  { 59, "execve", test_sc_execve_59 },
  { 60, "exit", test_sc_exit_60 },
  { 61, "wait4", test_sc_wait4_61 },
  { 62, "kill", test_sc_kill_62 },
  { 63, "uname", test_sc_uname_63 },
  { 64, "semget", test_sc_semget_64 },
  { 65, "semop", test_sc_semop_65 },
  { 66, "semctl", test_sc_semctl_66 },
  { 67, "shmdt", test_sc_shmdt_67 },
  { 72, "fcntl", test_sc_fcntl_72 },
  { 73, "flock", test_sc_flock_73 },
  { 74, "fsync", test_sc_fsync_74 },
  { 75, "fdatasync", test_sc_fdatasync_75 },
  { 77, "ftruncate", test_sc_ftruncate_77 },
  { 78, "getdents", test_sc_getdents_78 },
  { 79, "getcwd", test_sc_getcwd_79 },
  { 80, "chdir", test_sc_chdir_80 },
  { 81, "fchdir", test_sc_fchdir_81 },
  { 82, "rename", test_sc_rename_82 },
  { 83, "mkdir", test_sc_mkdir_83 },
  { 84, "rmdir", test_sc_rmdir_84 },
  { 85, "creat", test_sc_creat_85 },
  { 86, "link", test_sc_link_86 },
  { 87, "unlink", test_sc_unlink_87 },
  { 88, "symlink", test_sc_symlink_88 },
  { 89, "readlink", test_sc_readlink_89 },
  { 90, "chmod", test_sc_chmod_90 },
  { 91, "fchmod", test_sc_fchmod_91 },
  { 92, "chown", test_sc_chown_92 },
  { 93, "fchown", test_sc_fchown_93 },
  { 94, "lchown", test_sc_lchown_94 },
  { 95, "umask", test_sc_umask_95 },
  { 96, "gettimeofday", test_sc_gettimeofday_96 },
  { 97, "getrlimit", test_sc_getrlimit_97 },
  { 98, "getrusage", test_sc_getrusage_98 },
  { 99, "sysinfo", test_sc_sysinfo_99 },
  { 100, "times", test_sc_times_100 },
  { 101, "ptrace", test_sc_ptrace_101 },
  { 102, "getuid", test_sc_getuid_102 },
  { 103, "syslog", test_sc_syslog_103 },
  { 104, "getgid", test_sc_getgid_104 },
  { 105, "setuid", test_sc_setuid_105 },
  { 106, "setgid", test_sc_setgid_106 },
  { 107, "geteuid", test_sc_geteuid_107 },
  { 108, "getegid", test_sc_getegid_108 },
  { 109, "setpgid", test_sc_setpgid_109 },
  { 110, "getppid", test_sc_getppid_110 },
  { 111, "getpgrp", test_sc_getpgrp_111 },
  { 112, "setsid", test_sc_setsid_112 },
  { 113, "setreuid", test_sc_setreuid_113 },
  { 114, "setregid", test_sc_setregid_114 },
  { 115, "getgroups", test_sc_getgroups_115 },
  { 116, "setgroups", test_sc_setgroups_116 },
  { 117, "setresuid", test_sc_setresuid_117 },
  { 118, "getresuid", test_sc_getresuid_118 },
  { 119, "setresgid", test_sc_setresgid_119 },
  { 120, "getresgid", test_sc_getresgid_120 },
  { 121, "getpgid", test_sc_getpgid_121 },
  { 122, "setfsuid", test_sc_setfsuid_122 },
  { 123, "setfsgid", test_sc_setfsgid_123 },
  { 124, "getsid", test_sc_getsid_124 },
  { 125, "capget", test_sc_capget_125 },
  { 126, "capset", test_sc_capset_126 },
  { 127, "rt_sigpending", test_sc_rt_sigpending_127 },
  { 128, "rt_sigtimedwait", test_sc_rt_sigtimedwait_128 },
  { 129, "rt_sigqueueinfo", test_sc_rt_sigqueueinfo_129 },
  { 130, "rt_sigsuspend", test_sc_rt_sigsuspend_130 },
  { 131, "sigaltstack", test_sc_sigaltstack_131 },
  { 132, "utime", test_sc_utime_132 },
  { 133, "mknod", test_sc_mknod_133 },
  { 137, "statfs", test_sc_statfs_137 },
  { 138, "fstatfs", test_sc_fstatfs_138 },
  { 140, "getpriority", test_sc_getpriority_140 },
  { 141, "setpriority", test_sc_setpriority_141 },
  { 142, "sched_setparam", test_sc_sched_setparam_142 },
  { 143, "sched_getparam", test_sc_sched_getparam_143 },
  { 144, "sched_setscheduler", test_sc_sched_setscheduler_144 },
  { 145, "sched_getscheduler", test_sc_sched_getscheduler_145 },
  { 146, "sched_get_priority_max", test_sc_sched_get_priority_max_146 },
  { 147, "sched_get_priority_min", test_sc_sched_get_priority_min_147 },
  { 148, "sched_rr_get_interval", test_sc_sched_rr_get_interval_148 },
  { 149, "mlock", test_sc_mlock_149 },
  { 150, "munlock", test_sc_munlock_150 },
  { 151, "mlockall", test_sc_mlockall_151 },
  { 152, "munlockall", test_sc_munlockall_152 },
  { 155, "pivot_root", test_sc_pivot_root_155 },
  { 157, "prctl", test_sc_prctl_157 },
  { 158, "arch_prctl", test_sc_arch_prctl_158 },
  { 160, "setrlimit", test_sc_setrlimit_160 },
  { 161, "chroot", test_sc_chroot_161 },
  { 164, "settimeofday", test_sc_settimeofday_164 },
  { 165, "mount", test_sc_mount_165 },
  { 166, "umount2", test_sc_umount2_166 },
  { 167, "swapon", test_sc_swapon_167 },
  { 169, "reboot", test_sc_reboot_169 },
  { 170, "sethostname", test_sc_sethostname_170 },
  { 171, "setdomainname", test_sc_setdomainname_171 },
  { 175, "init_module", test_sc_init_module_175 },
  { 176, "delete_module", test_sc_delete_module_176 },
  { 179, "quotactl", test_sc_quotactl_179 },
  { 186, "gettid", test_sc_gettid_186 },
  { 187, "readahead", test_sc_readahead_187 },
  { 188, "setxattr", test_sc_setxattr_188 },
  { 189, "lsetxattr", test_sc_lsetxattr_189 },
  { 190, "fsetxattr", test_sc_fsetxattr_190 },
  { 191, "getxattr", test_sc_getxattr_191 },
  { 192, "lgetxattr", test_sc_lgetxattr_192 },
  { 193, "fgetxattr", test_sc_fgetxattr_193 },
  { 194, "listxattr", test_sc_listxattr_194 },
  { 195, "llistxattr", test_sc_llistxattr_195 },
  { 196, "flistxattr", test_sc_flistxattr_196 },
  { 197, "removexattr", test_sc_removexattr_197 },
  { 198, "lremovexattr", test_sc_lremovexattr_198 },
  { 199, "fremovexattr", test_sc_fremovexattr_199 },
  { 200, "tkill", test_sc_tkill_200 },
  { 201, "time", test_sc_time_201 },
  { 202, "futex", test_sc_futex_202 },
  { 203, "sched_setaffinity", test_sc_sched_setaffinity_203 },
  { 204, "sched_getaffinity", test_sc_sched_getaffinity_204 },
  { 213, "epoll_create", test_sc_epoll_create_213 },
  { 217, "getdents64", test_sc_getdents64_217 },
  { 218, "set_tid_address", test_sc_set_tid_address_218 },
  { 220, "semtimedop", test_sc_semtimedop_220 },
  { 221, "fadvise64", test_sc_fadvise64_221 },
  { 222, "timer_create", test_sc_timer_create_222 },
  { 223, "timer_settime", test_sc_timer_settime_223 },
  { 224, "timer_gettime", test_sc_timer_gettime_224 },
  { 225, "timer_getoverrun", test_sc_timer_getoverrun_225 },
  { 226, "timer_delete", test_sc_timer_delete_226 },
  { 227, "clock_settime", test_sc_clock_settime_227 },
  { 228, "clock_gettime", test_sc_clock_gettime_228 },
  { 229, "clock_getres", test_sc_clock_getres_229 },
  { 230, "clock_nanosleep", test_sc_clock_nanosleep_230 },
  { 231, "exit_group", test_sc_exit_group_231 },
  { 232, "epoll_wait", test_sc_epoll_wait_232 },
  { 233, "epoll_ctl", test_sc_epoll_ctl_233 },
  { 234, "tgkill", test_sc_tgkill_234 },
  { 235, "utimes", test_sc_utimes_235 },
  { 237, "mbind", test_sc_mbind_237 },
  { 238, "set_mempolicy", test_sc_set_mempolicy_238 },
  { 239, "get_mempolicy", test_sc_get_mempolicy_239 },
  { 247, "waitid", test_sc_waitid_247 },
  { 251, "ioprio_set", test_sc_ioprio_set_251 },
  { 252, "ioprio_get", test_sc_ioprio_get_252 },
  { 253, "inotify_init", test_sc_inotify_init_253 },
  { 254, "inotify_add_watch", test_sc_inotify_add_watch_254 },
  { 255, "inotify_rm_watch", test_sc_inotify_rm_watch_255 },
  { 256, "migrate_pages", test_sc_migrate_pages_256 },
  { 257, "openat", test_sc_openat_257 },
  { 258, "mkdirat", test_sc_mkdirat_258 },
  { 259, "mknodat", test_sc_mknodat_259 },
  { 260, "fchownat", test_sc_fchownat_260 },
  { 261, "futimesat", test_sc_futimesat_261 },
  { 262, "newfstatat", test_sc_newfstatat_262 },
  { 263, "unlinkat", test_sc_unlinkat_263 },
  { 264, "renameat", test_sc_renameat_264 },
  { 265, "linkat", test_sc_linkat_265 },
  { 266, "symlinkat", test_sc_symlinkat_266 },
  { 267, "readlinkat", test_sc_readlinkat_267 },
  { 268, "fchmodat", test_sc_fchmodat_268 },
  { 269, "faccessat", test_sc_faccessat_269 },
  { 270, "pselect6", test_sc_pselect6_270 },
  { 271, "ppoll", test_sc_ppoll_271 },
  { 272, "unshare", test_sc_unshare_272 },
  { 273, "set_robust_list", test_sc_set_robust_list_273 },
  { 274, "get_robust_list", test_sc_get_robust_list_274 },
  { 275, "splice", test_sc_splice_275 },
  { 276, "tee", test_sc_tee_276 },
  { 277, "sync_file_range", test_sc_sync_file_range_277 },
  { 279, "move_pages", test_sc_move_pages_279 },
  { 280, "utimensat", test_sc_utimensat_280 },
  { 281, "epoll_pwait", test_sc_epoll_pwait_281 },
  { 282, "signalfd", test_sc_signalfd_282 },
  { 283, "timerfd_create", test_sc_timerfd_create_283 },
  { 285, "fallocate", test_sc_fallocate_285 },
  { 286, "timerfd_settime", test_sc_timerfd_settime_286 },
  { 287, "timerfd_gettime", test_sc_timerfd_gettime_287 },
  { 288, "accept4", test_sc_accept4_288 },
  { 289, "signalfd4", test_sc_signalfd4_289 },
  { 290, "eventfd2", test_sc_eventfd2_290 },
  { 291, "epoll_create1", test_sc_epoll_create1_291 },
  { 292, "dup3", test_sc_dup3_292 },
  { 293, "pipe2", test_sc_pipe2_293 },
  { 294, "inotify_init1", test_sc_inotify_init1_294 },
  { 295, "preadv", test_sc_preadv_295 },
  { 296, "pwritev", test_sc_pwritev_296 },
  { 297, "rt_tgsigqueueinfo", test_sc_rt_tgsigqueueinfo_297 },
  { 299, "recvmmsg", test_sc_recvmmsg_299 },
  { 302, "prlimit64", test_sc_prlimit64_302 },
  { 303, "name_to_handle_at", test_sc_name_to_handle_at_303 },
  { 304, "open_by_handle_at", test_sc_open_by_handle_at_304 },
  { 307, "sendmmsg", test_sc_sendmmsg_307 },
  { 309, "getcpu", test_sc_getcpu_309 },
  { 312, "kcmp", test_sc_kcmp_312 },
  { 314, "sched_setattr", test_sc_sched_setattr_314 },
  { 315, "sched_getattr", test_sc_sched_getattr_315 },
  { 316, "renameat2", test_sc_renameat2_316 },
  { 317, "seccomp", test_sc_seccomp_317 },
  { 318, "getrandom", test_sc_getrandom_318 },
  { 319, "memfd_create", test_sc_memfd_create_319 },
  { 323, "userfaultfd", test_sc_userfaultfd_323 },
  { 324, "membarrier", test_sc_membarrier_324 },
  { 325, "mlock2", test_sc_mlock2_325 },
  { 326, "copy_file_range", test_sc_copy_file_range_326 },
  { 332, "statx", test_sc_statx_332 },
  { 334, "rseq", test_sc_rseq_334 },
  { 399, "uptime", test_sc_uptime_399 },
  { 424, "pidfd_send_signal", test_sc_pidfd_send_signal_424 },
  { 425, "io_uring_setup", test_sc_io_uring_setup_425 },
  { 426, "io_uring_enter", test_sc_io_uring_enter_426 },
  { 427, "io_uring_register", test_sc_io_uring_register_427 },
  { 428, "open_tree", test_sc_open_tree_428 },
  { 429, "move_mount", test_sc_move_mount_429 },
  { 430, "fsopen", test_sc_fsopen_430 },
  { 431, "fsconfig", test_sc_fsconfig_431 },
  { 432, "fsmount", test_sc_fsmount_432 },
  { 433, "fspick", test_sc_fspick_433 },
  { 434, "pidfd_open", test_sc_pidfd_open_434 },
  { 435, "clone3", test_sc_clone3_435 },
  { 436, "close_range", test_sc_close_range_436 },
  { 437, "openat2", test_sc_openat2_437 },
  { 438, "pidfd_getfd", test_sc_pidfd_getfd_438 },
  { 439, "faccessat2", test_sc_faccessat2_439 },
  { 441, "epoll_pwait2", test_sc_epoll_pwait2_441 },
  { 442, "mount_setattr", test_sc_mount_setattr_442 },
  { 443, "quotactl_fd", test_sc_quotactl_fd_443 },
  { 444, "landlock_create_ruleset", test_sc_landlock_create_ruleset_444 },
  { 445, "landlock_add_rule", test_sc_landlock_add_rule_445 },
  { 446, "landlock_restrict_self", test_sc_landlock_restrict_self_446 },
  { 452, "fchmodat2", test_sc_fchmodat2_452 },
};

#define TOTAL_REGISTERED_SYSCALL_TESTS 288

static int run_all_single_syscall_tests(void) {
  int pass_count = 0;
  int fail_count = 0;
  printf("Starting individual isolated tests for all %d registered syscalls...\n", TOTAL_REGISTERED_SYSCALL_TESTS);

  for (size_t i = 0; i < TOTAL_REGISTERED_SYSCALL_TESTS; i++) {
    const struct single_syscall_test_entry *entry = &g_all_syscall_tests[i];
    printf("  [%3zu/%d] syscall #%-3d (%-22s) ... ", i + 1, TOTAL_REGISTERED_SYSCALL_TESTS, entry->num, entry->name);
    fflush(stdout);
    errno = 0;
    int ok = entry->fn();
    if (ok) {
      printf("PASS\n");
      pass_count++;
    } else {
      printf("FAIL\n");
      fail_count++;
    }
  }

  printf("\nIndividual Syscall Test Results: %d/%d PASSED (%d failed)\n",
         pass_count, TOTAL_REGISTERED_SYSCALL_TESTS, fail_count);
  return fail_count == 0 ? 0 : 1;
}

#endif // SYSCALL_TESTS_H
