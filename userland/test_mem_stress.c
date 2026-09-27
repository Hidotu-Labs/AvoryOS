#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <grp.h>
#include <sys/fsuid.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/xattr.h>
#include <sys/sem.h>
#include <sys/ipc.h>

#define DEBUGLOG(...) printf(__VA_ARGS__)

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif
#ifndef SYS_uptime
#define SYS_uptime 399
#endif

#define NUM_ITERATIONS 100
#define NUM_BLOCKS 128
#define BLOCK_SIZE 4096

// --- Clone & Futex Definitions ---
#define CLONE_STRESS_THREADS 4
#define CLONE_STRESS_ITERATIONS 10
#define THREAD_STACK_SIZE (64 * 1024)

#ifndef CLONE_VM
#define CLONE_VM 0x00000100
#endif
#ifndef CLONE_FS
#define CLONE_FS 0x00000200
#endif
#ifndef CLONE_FILES
#define CLONE_FILES 0x00000400
#endif
#ifndef CLONE_SIGHAND
#define CLONE_SIGHAND 0x00000800
#endif
#ifndef CLONE_PTRACE
#define CLONE_PTRACE 0x00002000
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK 0x00004000
#endif
#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD 0x00010000
#endif
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_SYSVSEM
#define CLONE_SYSVSEM 0x00040000
#endif
#ifndef CLONE_SETTLS
#define CLONE_SETTLS 0x00080000
#endif
#ifndef CLONE_PARENT_SETTID
#define CLONE_PARENT_SETTID 0x00100000
#endif
#ifndef CLONE_CHILD_CLEARTID
#define CLONE_CHILD_CLEARTID 0x00200000
#endif
#ifndef CLONE_DETACHED
#define CLONE_DETACHED 0x00400000
#endif
#ifndef CLONE_UNTRACED
#define CLONE_UNTRACED 0x00800000
#endif
#ifndef CLONE_CHILD_SETTID
#define CLONE_CHILD_SETTID 0x01000000
#endif

static long raw_clone(unsigned long flags, void *child_stack, int *ptid,
                      int *ctid, unsigned long newtls) {
  long ret;
  __asm__ volatile("movq %2, %%rdi\n"
                   "movq %3, %%rsi\n"
                   "movq %4, %%rdx\n"
                   "movq %5, %%r10\n"
                   "movq %6, %%r8\n"
                   "movq $56, %%rax\n"
                   "syscall\n"
                   "movq %%rax, %0\n"
                   : "=r"(ret)
                   : "r"(flags), "r"(flags), "r"(child_stack), "r"(ptid),
                     "r"(ctid), "r"(newtls)
                   : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11",
                     "memory");
  return ret;
}

static long raw_futex(uint32_t *uaddr, int op, uint32_t val,
                      const struct timespec *timeout, uint32_t *uaddr2,
                      uint32_t val3) {
  long ret;
  __asm__ volatile("movq %1, %%rdi\n"
                   "movq %2, %%rsi\n"
                   "movq %3, %%rdx\n"
                   "movq %4, %%r10\n"
                   "movq %5, %%r8\n"
                   "movq %6, %%r9\n"
                   "movq $202, %%rax\n"
                   "syscall\n"
                   "movq %%rax, %0\n"
                   : "=r"(ret)
                   : "r"(uaddr), "r"((long)op), "r"((long)val), "r"(timeout),
                     "r"(uaddr2), "r"((long)val3)
                   : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "rcx",
                     "r11", "memory");
  return ret;
}

typedef struct {
  _Atomic uint32_t val; // 0 = unlocked, 1 = locked, 2 = locked with waiters
} mutex_t;

void mutex_lock(mutex_t *m) {
  uint32_t expected = 0;
  if (atomic_compare_exchange_strong(&m->val, &expected, 1))
    return;
  if (expected != 2)
    expected = atomic_exchange(&m->val, 2);
  while (expected != 0) {
    raw_futex((uint32_t *)&m->val, FUTEX_WAIT, 2, NULL, NULL, 0);
    expected = atomic_exchange(&m->val, 2);
  }
}

void mutex_unlock(mutex_t *m) {
  if (atomic_fetch_sub(&m->val, 1) != 1) {
    atomic_store(&m->val, 0);
    raw_futex((uint32_t *)&m->val, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
}

long get_free_mem_kb() {
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd == -1) {
    perror("open /proc/meminfo");
    return -1;
  }

  char buf[1024];
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';

  char *p = strstr(buf, "MemFree:");
  if (!p)
    return -1;
  p += strlen("MemFree:");
  while (*p == ' ')
    p++;

  return atol(p);
}

/*
 * The kernel intentionally retains empty VMA and kmalloc slab pages for
 * reuse. Prime the peak allocation shapes exercised by the first two tests
 * before taking the baseline, so bounded cache growth is not reported as a
 * leak. A genuine per-iteration leak will still reduce MemFree during the
 * measured stress loops.
 */
static int prime_mmap_vfs_caches(void) {
  void *blocks[NUM_BLOCKS];
  int mapped = 0;

  for (; mapped < NUM_BLOCKS; mapped++) {
    blocks[mapped] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blocks[mapped] == MAP_FAILED)
      break;
    memset(blocks[mapped], 0, BLOCK_SIZE);
  }
  for (int i = 0; i < mapped; i++)
    munmap(blocks[i], BLOCK_SIZE);
  if (mapped != NUM_BLOCKS)
    return -1;

  const char *path = "/tmp/.mem_stress_warmup";
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0)
    return -1;
  char byte = 0;
  int ok = write(fd, &byte, 1) == 1;
  close(fd);
  if (unlink(path) < 0)
    ok = 0;
  return ok ? 0 : -1;
}

static int prime_wait4_caches(void) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
    _exit(0);

  int status;
  pid_t result = waitpid(pid, &status, WNOHANG);
  if (result == 0)
    result = waitpid(pid, &status, 0);
  return result == pid ? 0 : -1;
}

static void run_registered_syscall_coverage(int iterations);

static int prime_registered_syscall_caches(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);

  int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sfd < 0)
    return -1;
  close(sfd);

  int pidfd = syscall(SYS_pidfd_open, getpid(), 0);
  if (pidfd < 0)
    return -1;
  close(pidfd);

  run_registered_syscall_coverage(1);
  return 0;
}

void test_mmap_stress() {
  DEBUGLOG("Starting MMAP/MUNMAP stress test...\n");
  void *blocks[NUM_BLOCKS];

  for (int i = 0; i < NUM_ITERATIONS; i++) {
    for (int j = 0; j < NUM_BLOCKS; j++) {
      blocks[j] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (blocks[j] == MAP_FAILED) {
        fprintf(stderr, "mmap failed at iteration %d, block %d: %s\n", i, j,
                strerror(errno));
        exit(1);
      }
      memset(blocks[j], 0xAA, BLOCK_SIZE);
    }

    for (int j = 0; j < NUM_BLOCKS; j++) {
      if (munmap(blocks[j], BLOCK_SIZE) == -1) {
        fprintf(stderr, "munmap failed at iteration %d, block %d: %s\n", i, j,
                strerror(errno));
        exit(1);
      }
    }

    if (i % 25 == 0) {
      DEBUGLOG("MMAP iteration %d complete\n", i);
    }
  }
  DEBUGLOG("MMAP/MUNMAP stress test PASSED\n");
}

void test_vfs_stress() {
  DEBUGLOG("Starting VFS stress test...\n");
  char filename[64];
  char buffer[1024];
  memset(buffer, 'X', sizeof(buffer));

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    snprintf(filename, sizeof(filename), "/tmp/stress_%d.tmp", i);
    int fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd == -1) {
      fprintf(stderr, "open failed at iteration %d: %s\n", i, strerror(errno));
      exit(1);
    }
    write(fd, buffer, sizeof(buffer));
    close(fd);
    unlink(filename);

    if (i % 50 == 0) {
      DEBUGLOG("VFS iteration %d complete\n", i);
    }
  }
  DEBUGLOG("VFS stress test PASSED\n");
}

void test_vfs_open_close_stress() {
  DEBUGLOG("Starting VFS open/close stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "VFS open failed at iteration %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    char byte;
    read(fd, &byte, 1);
    close(fd);
    if (i % 250 == 0)
      DEBUGLOG("VFS open/close iteration %d complete\n", i);
  }
  DEBUGLOG("VFS open/close stress test PASSED\n");
}

static int epoll_lifecycle_once(void) {
  int fds[2];
  int epfd = epoll_create1(0);
  if (epfd < 0)
    return -1;
  if (pipe(fds) < 0) {
    close(epfd);
    return -1;
  }

  struct epoll_event event = {.events = EPOLLIN, .data.fd = fds[0]};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &event) < 0) {
    close(fds[0]);
    close(fds[1]);
    close(epfd);
    return -1;
  }

  write(fds[1], "E", 1);
  struct epoll_event ready;
  epoll_wait(epfd, &ready, 1, 0);
  close(fds[0]);
  close(fds[1]);
  close(epfd);
  return 0;
}

void test_epoll_stress() {
  DEBUGLOG("Starting EPOLL stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    if (epoll_lifecycle_once() < 0) {
      fprintf(stderr, "epoll lifecycle failed at iteration %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (i % 100 == 0)
      DEBUGLOG("EPOLL iteration %d complete\n", i);
  }
  DEBUGLOG("EPOLL stress test PASSED\n");
}

void test_poll_stress() {
  DEBUGLOG("Starting POLL stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fds[2];
    if (pipe(fds) < 0) {
      fprintf(stderr, "poll pipe failed: %s\n", strerror(errno));
      exit(1);
    }
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    poll(&pfd, 1, 0);
    write(fds[1], "P", 1);
    poll(&pfd, 1, 0);
    close(fds[0]);
    close(fds[1]);
    if (i % 100 == 0)
      DEBUGLOG("POLL iteration %d complete\n", i);
  }
  DEBUGLOG("POLL stress test PASSED\n");
}

void test_ppoll_stress() {
  DEBUGLOG("Starting PPOLL stress test...\n");
  const struct timespec timeout = {0, 0};
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fds[2];
    if (pipe(fds) < 0) {
      fprintf(stderr, "ppoll pipe failed: %s\n", strerror(errno));
      exit(1);
    }
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    ppoll(&pfd, 1, &timeout, NULL);
    write(fds[1], "Q", 1);
    ppoll(&pfd, 1, &timeout, NULL);
    close(fds[0]);
    close(fds[1]);
    if (i % 100 == 0)
      DEBUGLOG("PPOLL iteration %d complete\n", i);
  }
  DEBUGLOG("PPOLL stress test PASSED\n");
}

void test_pipe_stress() {
  DEBUGLOG("Starting PIPE stress test...\n");
  int pipefds[2];
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    if (pipe(pipefds) == -1) {
      fprintf(stderr, "pipe failed at iteration %d: %s\n", i, strerror(errno));
      exit(1);
    }
    close(pipefds[0]);
    close(pipefds[1]);
    if (i % 100 == 0) {
      DEBUGLOG("PIPE iteration %d complete\n", i);
    }
  }
  DEBUGLOG("PIPE stress test PASSED\n");
}

void test_socket_stress() {
  DEBUGLOG("Starting SOCKET stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s != -1) {
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "stress_sock_%d",
               i);
      bind(s, (struct sockaddr *)&addr, sizeof(addr));
      close(s);
    }
    if (i % 50 == 0) {
      DEBUGLOG("SOCKET iteration %d complete\n", i);
    }
  }
  DEBUGLOG("SOCKET stress test PASSED\n");
}

// Full Unix stream: bind/listen/connect/accept/send/recv/close lifecycle.
// Uses fork so the parent acts as server and the child as client, then
// both sides close their FDs and the parent reaps the child.
void test_unix_stream_stress() {
  DEBUGLOG("Starting UNIX STREAM connect/accept/send/recv stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS; i++) {
    // Build a unique abstract socket name per iteration.
    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1,
             "stream_stress_%d", i);
    socklen_t addrlen = sizeof(srv_addr);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
      fprintf(stderr, "unix_stream socket() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (bind(srv, (struct sockaddr *)&srv_addr, addrlen) < 0) {
      fprintf(stderr, "unix_stream bind() failed at iter %d: %s\n", i,
              strerror(errno));
      close(srv);
      exit(1);
    }
    if (listen(srv, 1) < 0) {
      fprintf(stderr, "unix_stream listen() failed at iter %d: %s\n", i,
              strerror(errno));
      close(srv);
      exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
      // Child: connect, send, recv, close
      close(srv);
      int cli = socket(AF_UNIX, SOCK_STREAM, 0);
      if (cli < 0)
        exit(1);
      if (connect(cli, (struct sockaddr *)&srv_addr, addrlen) < 0)
        exit(1);
      const char msg[] = "ping";
      if (send(cli, msg, sizeof(msg), 0) < 0)
        exit(1);
      char buf[8];
      if (recv(cli, buf, sizeof(buf), 0) < 0)
        exit(1);
      close(cli);
      exit(0);
    } else if (pid > 0) {
      // Parent: accept, recv, send, close
      int cli = accept(srv, NULL, NULL);
      if (cli >= 0) {
        char buf[8];
        recv(cli, buf, sizeof(buf), 0);
        const char reply[] = "pong";
        send(cli, reply, sizeof(reply), 0);
        close(cli);
      }
      close(srv);
      int status;
      waitpid(pid, &status, 0);
    } else {
      close(srv);
    }

    if (i % 25 == 0)
      DEBUGLOG("UNIX STREAM iteration %d complete\n", i);
  }
  DEBUGLOG("UNIX STREAM connect/accept/send/recv stress test PASSED\n");
}

// socketpair + send/recv + close — no forking required, exercises the paired
// socket send/recv path and verifies both FDs are released cleanly.
void test_socketpair_stress() {
  DEBUGLOG("Starting SOCKETPAIR send/recv stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
      fprintf(stderr, "socketpair() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    const char msg[] = "hello";
    char buf[8];

    if (send(sv[0], msg, sizeof(msg), 0) < 0) {
      close(sv[0]);
      close(sv[1]);
      fprintf(stderr, "socketpair send() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (recv(sv[1], buf, sizeof(buf), 0) < 0) {
      close(sv[0]);
      close(sv[1]);
      fprintf(stderr, "socketpair recv() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    close(sv[0]);
    close(sv[1]);

    if (i % 100 == 0)
      DEBUGLOG("SOCKETPAIR iteration %d complete\n", i);
  }
  DEBUGLOG("SOCKETPAIR send/recv stress test PASSED\n");
}

// Unix DGRAM: bind server, sendto from unbound client, recvfrom on server.
// Exercises the datagram path and verifies no FD or buffer leaks.
void test_unix_dgram_stress() {
  DEBUGLOG("Starting UNIX DGRAM sendto/recvfrom stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1,
             "dgram_stress_%d", i);
    socklen_t addrlen = sizeof(srv_addr);

    int srv = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (srv < 0) {
      fprintf(stderr, "unix_dgram server socket() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (bind(srv, (struct sockaddr *)&srv_addr, addrlen) < 0) {
      fprintf(stderr, "unix_dgram bind() failed at iter %d: %s\n", i,
              strerror(errno));
      close(srv);
      exit(1);
    }

    int cli = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (cli < 0) {
      close(srv);
      fprintf(stderr, "unix_dgram client socket() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    const char msg[] = "dgram";
    ssize_t sent =
        sendto(cli, msg, sizeof(msg), 0, (struct sockaddr *)&srv_addr, addrlen);
    if (sent < 0) {
      // Some kernels may not support unbound dgram send; tolerate ENOTCONN
      if (errno != ENOTCONN && errno != EOPNOTSUPP) {
        close(cli);
        close(srv);
        fprintf(stderr, "unix_dgram sendto() failed at iter %d: %s\n", i,
                strerror(errno));
        exit(1);
      }
    } else {
      char buf[16];
      struct sockaddr_un from;
      socklen_t fromlen = sizeof(from);
      recvfrom(srv, buf, sizeof(buf), MSG_DONTWAIT,
               (struct sockaddr *)&from, &fromlen);
    }

    close(cli);
    close(srv);

    if (i % 50 == 0)
      DEBUGLOG("UNIX DGRAM iteration %d complete\n", i);
  }
  DEBUGLOG("UNIX DGRAM sendto/recvfrom stress test PASSED\n");
}

// getsockopt / setsockopt / getsockname / getpeername lifecycle stress.
// Verifies that socket option paths don't leak resources.
void test_socket_opts_stress() {
  DEBUGLOG("Starting SOCKET opts/name stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
      fprintf(stderr, "socket_opts socketpair() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    // setsockopt: SO_SNDBUF / SO_RCVBUF
    int bufsize = 65536;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    // getsockopt: SO_TYPE, SO_SNDBUF, SO_RCVBUF, SO_ERROR
    int val = 0;
    socklen_t optlen = sizeof(val);
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &val, &optlen);
    optlen = sizeof(val);
    getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &val, &optlen);
    optlen = sizeof(val);
    getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &val, &optlen);
    optlen = sizeof(val);
    getsockopt(sv[0], SOL_SOCKET, SO_ERROR, &val, &optlen);

    // getsockname / getpeername
    struct sockaddr_un name;
    socklen_t namelen = sizeof(name);
    getsockname(sv[0], (struct sockaddr *)&name, &namelen);
    namelen = sizeof(name);
    getpeername(sv[0], (struct sockaddr *)&name, &namelen);

    close(sv[0]);
    close(sv[1]);

    if (i % 100 == 0)
      DEBUGLOG("SOCKET OPTS iteration %d complete\n", i);
  }
  DEBUGLOG("SOCKET opts/name stress test PASSED\n");
}

// shutdown() on both ends of a socketpair; verifies the shutdown path
// doesn't leak the socket or its send/receive buffers.
void test_shutdown_stress() {
  DEBUGLOG("Starting SHUTDOWN stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
      fprintf(stderr, "shutdown socketpair() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    // Write a byte so there's data in flight when we shut down
    send(sv[0], "x", 1, 0);

    // SHUT_WR on the writer side, SHUT_RD on the reader side
    shutdown(sv[0], SHUT_WR);
    shutdown(sv[1], SHUT_RD);

    // Drain any buffered data
    char buf[4];
    recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);

    // SHUT_RDWR on both
    shutdown(sv[0], SHUT_RDWR);
    shutdown(sv[1], SHUT_RDWR);

    close(sv[0]);
    close(sv[1]);

    if (i % 100 == 0)
      DEBUGLOG("SHUTDOWN iteration %d complete\n", i);
  }
  DEBUGLOG("SHUTDOWN stress test PASSED\n");
}

// accept4() with SOCK_CLOEXEC / SOCK_NONBLOCK flags stress.
// Confirms the extended accept path allocates and frees FDs cleanly.
void test_accept4_stress() {
  DEBUGLOG("Starting ACCEPT4 stress test...\n");

  for (int i = 0; i < NUM_ITERATIONS; i++) {
    struct sockaddr_un srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    snprintf(srv_addr.sun_path + 1, sizeof(srv_addr.sun_path) - 1,
             "accept4_stress_%d", i);
    socklen_t addrlen = sizeof(srv_addr);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
      fprintf(stderr, "accept4 socket() failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (bind(srv, (struct sockaddr *)&srv_addr, addrlen) < 0 ||
        listen(srv, 1) < 0) {
      close(srv);
      fprintf(stderr, "accept4 bind/listen failed at iter %d: %s\n", i,
              strerror(errno));
      exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
      close(srv);
      int cli = socket(AF_UNIX, SOCK_STREAM, 0);
      if (cli >= 0) {
        connect(cli, (struct sockaddr *)&srv_addr, addrlen);
        close(cli);
      }
      exit(0);
    } else if (pid > 0) {
      // accept4 with SOCK_CLOEXEC | SOCK_NONBLOCK
      int cli = accept4(srv, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (cli >= 0)
        close(cli);
      close(srv);
      int status;
      waitpid(pid, &status, 0);
    } else {
      close(srv);
    }

    if (i % 25 == 0)
      DEBUGLOG("ACCEPT4 iteration %d complete\n", i);
  }
  DEBUGLOG("ACCEPT4 stress test PASSED\n");
}

void test_fork_stress() {
  DEBUGLOG("Starting FORK/EXIT stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      void *p = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p != MAP_FAILED)
        memset(p, 0xBB, 1024 * 1024);
      int fd = open("/tmp/child_stress", O_CREAT | O_RDWR, 0666);
      if (fd != -1)
        write(fd, "child", 5);
      exit(0);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
      unlink("/tmp/child_stress");
    }
    if (i % 25 == 0) {
      DEBUGLOG("FORK iteration %d complete\n", i);
    }
  }
  DEBUGLOG("FORK/EXIT stress test PASSED\n");
}

static mutex_t stress_mutex = {0};
static _Atomic int threads_running = 0;

int stress_thread_entry(void *arg) {
  (void)arg;
  for (int i = 0; i < 100; i++) {
    mutex_lock(&stress_mutex);
    // Do some dummy work
    volatile int x = 0;
    for (int j = 0; j < 100; j++)
      x++;
    mutex_unlock(&stress_mutex);
    if (i % 10 == 0)
      sched_yield();
  }
  atomic_fetch_sub(&threads_running, 1);
  syscall(SYS_exit, 0);
  return 0;
}

void test_clone_futex_stress() {
  DEBUGLOG("Starting CLONE/FUTEX stress test...\n");
  uint8_t *stacks[CLONE_STRESS_THREADS];
  _Atomic int child_tids[CLONE_STRESS_THREADS];
  for (int i = 0; i < CLONE_STRESS_THREADS; i++) {
    stacks[i] = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                        CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_SETTID |
                        CLONE_CHILD_CLEARTID;

  for (int i = 0; i < CLONE_STRESS_ITERATIONS; i++) {
    atomic_store(&threads_running, CLONE_STRESS_THREADS);
    for (int j = 0; j < CLONE_STRESS_THREADS; j++) {
      atomic_store(&child_tids[j], 0);
      void *stack_top = stacks[j] + THREAD_STACK_SIZE;
      long tid = raw_clone(flags, stack_top, NULL,
                           (int *)&child_tids[j], 0);
      if (tid == 0) {
        stress_thread_entry(NULL);
      }
    }

    while (atomic_load(&threads_running) > 0) {
      usleep(1000);
    }

    // A worker decrements threads_running before entering SYS_exit. Join on
    // clear-child-TID before reusing its user stack.
    for (int j = 0; j < CLONE_STRESS_THREADS; j++) {
      int tid;
      while ((tid = atomic_load(&child_tids[j])) != 0) {
        raw_futex((uint32_t *)&child_tids[j], FUTEX_WAIT, (uint32_t)tid,
                  NULL, NULL, 0);
      }
    }

    // Give the scheduler context that resumed us a chance to consume the
    // detached-task reap queue before the next iteration.
    sched_yield();

    if (i % 2 == 0) {
      DEBUGLOG("CLONE iteration %d complete\n", i);
    }
  }

  // clear-child-TID and reap-queue publication happen on different CPUs.
  // Allow a few complete scheduling cycles before taking the PMM sample.
  for (int i = 0; i < 8; i++) {
    sched_yield();
    usleep(1000);
  }

  for (int i = 0; i < CLONE_STRESS_THREADS; i++) {
    munmap(stacks[i], THREAD_STACK_SIZE);
  }
  DEBUGLOG("CLONE/FUTEX stress test PASSED\n");
}

void test_vfs_error_stress() {
  DEBUGLOG("Starting VFS Error stress test (failed opens/stats)...\n");
  char filename[64];
  struct stat st;
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    snprintf(filename, sizeof(filename), "/nonexistent_%d", i);
    int fd = open(filename, O_RDONLY);
    if (fd != -1)
      close(fd);
    stat(filename, &st);
  }
  DEBUGLOG("VFS Error stress test PASSED\n");
}

void test_mmap_fixed_stress() {
  DEBUGLOG("Starting MMAP FIXED stress test...\n");
  size_t size = 64 * 1024;
  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (addr == MAP_FAILED) {
    perror("mmap initial");
    return;
  }

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    void *new_addr = mmap(addr, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (new_addr == MAP_FAILED) {
      fprintf(stderr, "mmap FIXED failed at iteration %d: %s\n", i,
              strerror(errno));
      break;
    }
    memset(new_addr, i, size);
  }
  munmap(addr, size);
  DEBUGLOG("MMAP FIXED stress test PASSED\n");
}

void test_readv_stress() {
  DEBUGLOG("Starting READV stress test...\n");
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd == -1)
    return;

  char buf1[64], buf2[64];
  struct iovec iov[2];
  iov[0].iov_base = buf1;
  iov[0].iov_len = sizeof(buf1);
  iov[1].iov_base = buf2;
  iov[1].iov_len = sizeof(buf2);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    lseek(fd, 0, SEEK_SET);
    if (readv(fd, iov, 2) == -1) {
      // Some systems don't support readv on procfs, but we test the syscall
      // entry/exit
    }
  }
  close(fd);
  DEBUGLOG("READV stress test PASSED\n");
}

void test_exec_stress() {
  DEBUGLOG("Starting EXECVE stress test...\n");
  // Use hello.elf if it exists, otherwise skip
  char *argv[] = {"/userland/hello.elf", NULL};
  char *envp[] = {NULL};

  if (access(argv[0], X_OK) != 0) {
    DEBUGLOG("Skipping EXECVE stress test (hello.elf not found or not "
             "executable)\n");
    return;
  }

  for (int i = 0; i < 20; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      // Close stdout/stderr to avoid spam
      int nullfd = open("/dev/null", O_WRONLY);
      if (nullfd != -1) {
        dup2(nullfd, 1);
        dup2(nullfd, 2);
        close(nullfd);
      }
      execve(argv[0], argv, envp);
      exit(0);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
    if (i % 5 == 0)
      DEBUGLOG("EXEC iteration %d complete\n", i);
  }
  DEBUGLOG("EXECVE stress test PASSED\n");
}


// ---- dup / dup2 / fcntl stress ----
void test_dup_fcntl_stress() {
  DEBUGLOG("Starting DUP/DUP2/FCNTL stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    int fds[2];
    if (pipe(fds) < 0) { fprintf(stderr, "dup pipe: %s\n", strerror(errno)); exit(1); }

    // dup
    int d = dup(fds[0]);
    if (d < 0) { fprintf(stderr, "dup failed: %s\n", strerror(errno)); exit(1); }

    // dup2 to a specific slot
    int slot = d + 1;
    int d2 = dup2(fds[1], slot);
    if (d2 < 0) { close(d); close(fds[0]); close(fds[1]); i++; continue; }

    // fcntl F_GETFL / F_SETFL O_NONBLOCK
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fl & ~O_NONBLOCK);

    // fcntl F_GETFD / F_SETFD FD_CLOEXEC
    int fd_flags = fcntl(d, F_GETFD, 0);
    fcntl(d, F_SETFD, fd_flags | FD_CLOEXEC);
    fcntl(d, F_SETFD, fd_flags & ~FD_CLOEXEC);

    // fcntl F_DUPFD
    int d3 = fcntl(fds[0], F_DUPFD, 0);
    if (d3 >= 0) close(d3);

    close(d); close(d2); close(fds[0]); close(fds[1]);
    if (i % 100 == 0) DEBUGLOG("DUP/FCNTL iteration %d complete\n", i);
  }
  DEBUGLOG("DUP/DUP2/FCNTL stress test PASSED\n");
}

// ---- pread64 / pwrite64 stress ----
void test_pread_pwrite_stress() {
  DEBUGLOG("Starting PREAD64/PWRITE64 stress test...\n");
  char path[] = "/tmp/preadwrite_stress.tmp";
  char wbuf[512];
  char rbuf[512];
  memset(wbuf, 0x5A, sizeof(wbuf));

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) { fprintf(stderr, "pread/pwrite open: %s\n", strerror(errno)); return; }

  for (int i = 0; i < NUM_ITERATIONS * 4; i++) {
    off_t off = (off_t)(i % 8) * sizeof(wbuf);
    ssize_t w = pwrite(fd, wbuf, sizeof(wbuf), off);
    if (w < 0) { fprintf(stderr, "pwrite64 failed: %s\n", strerror(errno)); break; }
    ssize_t r = pread(fd, rbuf, sizeof(rbuf), off);
    if (r < 0) { fprintf(stderr, "pread64 failed: %s\n", strerror(errno)); break; }
    if (i % 100 == 0) DEBUGLOG("PREAD/PWRITE iteration %d complete\n", i);
  }
  close(fd);
  unlink(path);
  DEBUGLOG("PREAD64/PWRITE64 stress test PASSED\n");
}

// ---- writev stress ----
void test_writev_stress() {
  DEBUGLOG("Starting WRITEV stress test...\n");
  int fds[2];
  if (pipe(fds) < 0) { perror("writev pipe"); return; }

  char a[32], b[32], c[32];
  memset(a, 'A', sizeof(a)); memset(b, 'B', sizeof(b)); memset(c, 'C', sizeof(c));
  struct iovec iov[3];
  iov[0].iov_base = a; iov[0].iov_len = sizeof(a);
  iov[1].iov_base = b; iov[1].iov_len = sizeof(b);
  iov[2].iov_base = c; iov[2].iov_len = sizeof(c);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    ssize_t w = writev(fds[1], iov, 3);
    if (w < 0) { fprintf(stderr, "writev failed: %s\n", strerror(errno)); break; }
    char drain[96];
    read(fds[0], drain, (size_t)w);
    if (i % 100 == 0) DEBUGLOG("WRITEV iteration %d complete\n", i);
  }
  close(fds[0]); close(fds[1]);
  DEBUGLOG("WRITEV stress test PASSED\n");
}

// ---- sendfile stress ----
void test_sendfile_stress() {
  DEBUGLOG("Starting SENDFILE stress test...\n");
  char src_path[] = "/tmp/sendfile_src.tmp";
  char dst_path[] = "/tmp/sendfile_dst.tmp";
  char data[1024];
  memset(data, 0xCC, sizeof(data));

  int src = open(src_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (src < 0) { perror("sendfile src open"); return; }
  write(src, data, sizeof(data));

  int dst = open(dst_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (dst < 0) { perror("sendfile dst open"); close(src); return; }

  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    off_t off = 0;
    lseek(dst, 0, SEEK_SET);
    ssize_t sent = sendfile(dst, src, &off, sizeof(data));
    if (sent < 0) { fprintf(stderr, "sendfile failed: %s\n", strerror(errno)); break; }
    if (i % 75 == 0) DEBUGLOG("SENDFILE iteration %d complete\n", i);
  }
  close(src); close(dst);
  unlink(src_path); unlink(dst_path);
  DEBUGLOG("SENDFILE stress test PASSED\n");
}

// ---- ftruncate / fallocate / fsync / fstat stress ----
void test_ftruncate_fsync_stress() {
  DEBUGLOG("Starting FTRUNCATE/FALLOCATE/FSYNC/FSTAT stress test...\n");
  char path[] = "/tmp/trunc_stress.tmp";

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) { fprintf(stderr, "trunc open failed: %s\n", strerror(errno)); exit(1); }

    // grow via write then truncate down
    char buf[4096] = {0};
    write(fd, buf, sizeof(buf));
    ftruncate(fd, 1024);
    ftruncate(fd, 0);

    // fallocate to pre-reserve space
    fallocate(fd, 0, 0, 2048);

    // fsync (no-op on our kernel but exercises the syscall path)
    fsync(fd);

    // fstat
    struct stat st;
    fstat(fd, &st);

    close(fd);
    unlink(path);
    if (i % 50 == 0) DEBUGLOG("FTRUNCATE/FSYNC iteration %d complete\n", i);
  }
  DEBUGLOG("FTRUNCATE/FALLOCATE/FSYNC/FSTAT stress test PASSED\n");
}

// ---- lseek stress (SEEK_SET / SEEK_CUR / SEEK_END) ----
void test_lseek_stress() {
  DEBUGLOG("Starting LSEEK stress test...\n");
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd < 0) { perror("lseek open"); return; }

  char buf[16];
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, sizeof(buf));
    lseek(fd, -4, SEEK_CUR);
    lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (i % 250 == 0) DEBUGLOG("LSEEK iteration %d complete\n", i);
  }
  close(fd);
  DEBUGLOG("LSEEK stress test PASSED\n");
}

// ---- stat / lstat / fstat / newfstatat / statx stress ----
void test_stat_variants_stress() {
  DEBUGLOG("Starting STAT variants stress test...\n");
  struct stat st;
  struct statx stx;
  char path[] = "/tmp/stat_stress.tmp";

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd >= 0) { write(fd, "x", 1); }

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    stat(path, &st);
    lstat(path, &st);
    if (fd >= 0) fstat(fd, &st);
    fstatat(AT_FDCWD, path, &st, 0);
    fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW);
    statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &stx);
    if (i % 100 == 0) DEBUGLOG("STAT variants iteration %d complete\n", i);
  }
  if (fd >= 0) { close(fd); unlink(path); }
  DEBUGLOG("STAT variants stress test PASSED\n");
}

// ---- getdents64 stress ----
void test_getdents64_stress() {
  DEBUGLOG("Starting GETDENTS64 stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (fd < 0) fd = open("/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) continue;
    char buf[1024];
    long n;
    while ((n = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0) {}
    close(fd);
    if (i % 75 == 0) DEBUGLOG("GETDENTS64 iteration %d complete\n", i);
  }
  DEBUGLOG("GETDENTS64 stress test PASSED\n");
}

// ---- statfs / fstatfs stress ----
void test_statfs_stress() {
  DEBUGLOG("Starting STATFS/FSTATFS stress test...\n");
  struct statfs sf;
  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    statfs("/", &sf);
    statfs("/tmp", &sf);
    statfs("/proc", &sf);
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) { fstatfs(fd, &sf); close(fd); }
    if (i % 100 == 0) DEBUGLOG("STATFS iteration %d complete\n", i);
  }
  DEBUGLOG("STATFS/FSTATFS stress test PASSED\n");
}

// ---- mkdir / rmdir / rename / symlink / readlink / chmod / chown stress ----
void test_fs_ops_stress() {
  DEBUGLOG("Starting FS OPS (mkdir/rmdir/rename/symlink/readlink) stress test...\n");
  char dir[64], dir2[64], sym[64], linkbuf[128];

  for (int i = 0; i < NUM_ITERATIONS; i++) {
    snprintf(dir,  sizeof(dir),  "/tmp/fsops_dir_%d",  i);
    snprintf(dir2, sizeof(dir2), "/tmp/fsops_dir2_%d", i);
    snprintf(sym,  sizeof(sym),  "/tmp/fsops_sym_%d",  i);

    mkdir(dir, 0755);
    rename(dir, dir2);
    rmdir(dir2);

    // create a file, symlink to it, readlink, unlink both
    char file[64], file2[64];
    snprintf(file,  sizeof(file),  "/tmp/fsops_f_%d.tmp", i);
    snprintf(file2, sizeof(file2), "/tmp/fsops_f2_%d.tmp", i);
    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
      write(fd, "data", 4);
      close(fd);
      chmod(file, 0600);
      chown(file, 0, 0);
      symlink(file, sym);
      readlink(sym, linkbuf, sizeof(linkbuf) - 1);
      // link (hard link)
      link(file, file2);
      unlink(file2);
      unlink(sym);
      unlink(file);
    }
    if (i % 25 == 0) DEBUGLOG("FS OPS iteration %d complete\n", i);
  }
  DEBUGLOG("FS OPS stress test PASSED\n");
}

// ---- access / faccessat stress ----
void test_access_stress() {
  DEBUGLOG("Starting ACCESS/FACCESSAT stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    access("/proc/meminfo", R_OK);
    access("/tmp",          F_OK | X_OK);
    access("/nonexistent_access_test", F_OK);  // expected ENOENT
    faccessat(AT_FDCWD, "/proc/meminfo", R_OK, 0);
    faccessat(AT_FDCWD, "/nonexistent",  F_OK, 0);
    if (i % 100 == 0) DEBUGLOG("ACCESS iteration %d complete\n", i);
  }
  DEBUGLOG("ACCESS/FACCESSAT stress test PASSED\n");
}

// ---- getcwd / chdir / fchdir stress ----
void test_getcwd_chdir_stress() {
  DEBUGLOG("Starting GETCWD/CHDIR/FCHDIR stress test...\n");
  char cwd[256];
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    getcwd(cwd, sizeof(cwd));
    chdir("/tmp");
    chdir("/");
    chdir("/proc");
    int fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { fchdir(fd); close(fd); }
    chdir("/");
    if (i % 75 == 0) DEBUGLOG("GETCWD/CHDIR iteration %d complete\n", i);
  }
  DEBUGLOG("GETCWD/CHDIR/FCHDIR stress test PASSED\n");
}

// ---- mprotect stress (R/W/RX cycling) ----
void test_mprotect_stress() {
  DEBUGLOG("Starting MPROTECT stress test...\n");
  size_t sz = 4 * 4096;
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mprotect mmap"); exit(1); }
    memset(p, i & 0xFF, sz);
    mprotect(p, sz, PROT_READ);
    mprotect(p, sz, PROT_READ | PROT_WRITE);
    mprotect(p, sz, PROT_NONE);
    mprotect(p, sz, PROT_READ | PROT_WRITE | PROT_EXEC);
    munmap(p, sz);
    if (i % 50 == 0) DEBUGLOG("MPROTECT iteration %d complete\n", i);
  }
  DEBUGLOG("MPROTECT stress test PASSED\n");
}

// ---- mremap stress ----
void test_mremap_stress() {
  DEBUGLOG("Starting MREMAP stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS; i++) {
    size_t old_sz = 2 * 4096;
    void *p = mmap(NULL, old_sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mremap mmap"); exit(1); }
    memset(p, 0xAB, old_sz);

    // Grow with MREMAP_MAYMOVE
    size_t new_sz = 8 * 4096;
    void *q = mremap(p, old_sz, new_sz, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
      munmap(p, old_sz);
      if (i % 25 == 0) DEBUGLOG("MREMAP iteration %d skipped\n", i);
      continue;
    }
    memset((char *)q + old_sz, 0xCD, new_sz - old_sz);

    // Shrink in place
    void *r = mremap(q, new_sz, old_sz, 0);
    if (r != MAP_FAILED) munmap(r, old_sz);
    else munmap(q, new_sz);

    if (i % 25 == 0) DEBUGLOG("MREMAP iteration %d complete\n", i);
  }
  DEBUGLOG("MREMAP stress test PASSED\n");
}

// ---- brk stress ----
void test_brk_stress() {
  DEBUGLOG("Starting BRK stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    // brk(0) returns current break
    void *base = sbrk(0);
    if (base == (void *)-1) continue;

    // extend by 4 pages
    void *grown = sbrk(4 * 4096);
    if (grown == (void *)-1) continue;

    // touch the new memory
    memset(grown, i & 0xFF, 4 * 4096);

    // shrink back
    sbrk(-(4 * 4096));

    if (i % 50 == 0) DEBUGLOG("BRK iteration %d complete\n", i);
  }
  DEBUGLOG("BRK stress test PASSED\n");
}

// ---- madvise stress ----
void test_madvise_stress() {
  DEBUGLOG("Starting MADVISE stress test...\n");
  size_t sz = 16 * 4096;
  void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("madvise mmap"); return; }
  memset(p, 0, sz);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    madvise(p, sz, MADV_NORMAL);
    madvise(p, sz, MADV_SEQUENTIAL);
    madvise(p, sz, MADV_RANDOM);
    madvise(p, sz, MADV_WILLNEED);
    madvise(p, sz, MADV_DONTNEED);
    if (i % 100 == 0) DEBUGLOG("MADVISE iteration %d complete\n", i);
  }
  munmap(p, sz);
  DEBUGLOG("MADVISE stress test PASSED\n");
}

// ---- pipe2 stress ----
void test_pipe2_stress() {
  DEBUGLOG("Starting PIPE2 stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    int fds[2];
    if (pipe2(fds, O_NONBLOCK) < 0) {
      fprintf(stderr, "pipe2 O_NONBLOCK failed: %s\n", strerror(errno)); exit(1);
    }
    write(fds[1], "X", 1);
    char c;
    read(fds[0], &c, 1);
    close(fds[0]); close(fds[1]);

    if (pipe2(fds, O_CLOEXEC) < 0) {
      fprintf(stderr, "pipe2 O_CLOEXEC failed: %s\n", strerror(errno)); exit(1);
    }
    close(fds[0]); close(fds[1]);

    if (i % 100 == 0) DEBUGLOG("PIPE2 iteration %d complete\n", i);
  }
  DEBUGLOG("PIPE2 stress test PASSED\n");
}

// ---- memfd_create + ftruncate + mmap stress ----
void test_memfd_stress() {
  DEBUGLOG("Starting MEMFD_CREATE stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int fd = memfd_create("stress_memfd", 0);
    if (fd < 0) {
      if (i == 0) DEBUGLOG("Skipping MEMFD (not implemented)\n");
      break;
    }
    size_t sz = 4 * 4096;
    ftruncate(fd, (off_t)sz);

    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
      memset(p, i & 0xFF, sz);
      munmap(p, sz);
    }
    close(fd);
    if (i % 50 == 0) DEBUGLOG("MEMFD iteration %d complete\n", i);
  }
  DEBUGLOG("MEMFD_CREATE stress test PASSED\n");
}

// ---- timerfd lifecycle stress ----
void test_timerfd_stress() {
  DEBUGLOG("Starting TIMERFD stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
      if (i == 0) DEBUGLOG("Skipping TIMERFD (not implemented)\n");
      break;
    }
    struct itimerspec its = {0};
    its.it_value.tv_sec  = 0;
    its.it_value.tv_nsec = 1;  // 1 ns — fires essentially immediately
    timerfd_settime(fd, 0, &its, NULL);

    struct itimerspec cur = {0};
    timerfd_gettime(fd, &cur);

    // Disarm
    struct itimerspec disarm = {0};
    timerfd_settime(fd, 0, &disarm, NULL);

    close(fd);
    if (i % 75 == 0) DEBUGLOG("TIMERFD iteration %d complete\n", i);
  }
  DEBUGLOG("TIMERFD stress test PASSED\n");
}

// ---- eventfd stress ----
void test_eventfd_stress() {
  DEBUGLOG("Starting EVENTFD stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
      if (i == 0) DEBUGLOG("Skipping EVENTFD (not implemented)\n");
      break;
    }
    uint64_t val = 42;
    write(fd, &val, sizeof(val));
    uint64_t rval = 0;
    read(fd, &rval, sizeof(rval));
    if (rval != 42) {
      fprintf(stderr, "eventfd read mismatch: expected 42 got %lu\n", rval);
    }
    close(fd);
    if (i % 100 == 0) DEBUGLOG("EVENTFD iteration %d complete\n", i);
  }
  DEBUGLOG("EVENTFD stress test PASSED\n");
}

// ---- inotify lifecycle stress ----
void test_inotify_stress() {
  DEBUGLOG("Starting INOTIFY stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
      if (i == 0) DEBUGLOG("Skipping INOTIFY (not implemented)\n");
      break;
    }
    // Add a watch on /tmp
    int wd = inotify_add_watch(fd, "/tmp", IN_CREATE | IN_DELETE);
    // Create a file to trigger the watch
    char path[64];
    snprintf(path, sizeof(path), "/tmp/inotify_stress_%d.tmp", i);
    int tfd = open(path, O_CREAT | O_WRONLY, 0644);
    if (tfd >= 0) close(tfd);

    // Drain events (non-blocking)
    char evbuf[256];
    read(fd, evbuf, sizeof(evbuf));

    if (wd >= 0) inotify_rm_watch(fd, wd);
    unlink(path);
    close(fd);
    if (i % 50 == 0) DEBUGLOG("INOTIFY iteration %d complete\n", i);
  }
  DEBUGLOG("INOTIFY stress test PASSED\n");
}

// ---- getrandom stress ----
void test_getrandom_stress() {
  DEBUGLOG("Starting GETRANDOM stress test...\n");
  uint8_t buf[64];
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    ssize_t n = getrandom(buf, sizeof(buf), 0);
    if (n < 0) { fprintf(stderr, "getrandom failed: %s\n", strerror(errno)); exit(1); }
    if (i % 250 == 0) DEBUGLOG("GETRANDOM iteration %d complete\n", i);
  }
  DEBUGLOG("GETRANDOM stress test PASSED\n");
}

// ---- clock_gettime / gettimeofday / nanosleep stress ----
void test_clock_stress() {
  DEBUGLOG("Starting CLOCK/TIME stress test...\n");
  struct timespec ts;
  struct timeval  tv;
  struct timespec sleep_ts = {0, 0}; // zero sleep = yield

  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_REALTIME,  &ts);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    gettimeofday(&tv, NULL);
    nanosleep(&sleep_ts, NULL);
    if (i % 250 == 0) DEBUGLOG("CLOCK iteration %d complete\n", i);
  }
  DEBUGLOG("CLOCK/TIME stress test PASSED\n");
}

// ---- uname / sysinfo / getrlimit / prlimit stress ----
void test_sysinfo_stress() {
  DEBUGLOG("Starting UNAME/SYSINFO/GETRLIMIT stress test...\n");
  struct utsname uts;
  struct sysinfo si;
  struct rlimit rl;

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    uname(&uts);
    sysinfo(&si);
    getrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_STACK,  &rl);
    getrlimit(RLIMIT_AS,     &rl);

    // prlimit: query self
    struct rlimit new_rl = {1024, 1024};
    struct rlimit old_rl;
    prlimit(0, RLIMIT_NOFILE, &new_rl, &old_rl);
    prlimit(0, RLIMIT_NOFILE, &old_rl, NULL); // restore

    if (i % 100 == 0) DEBUGLOG("SYSINFO iteration %d complete\n", i);
  }
  DEBUGLOG("UNAME/SYSINFO/GETRLIMIT stress test PASSED\n");
}

// ---- getpid / gettid / getppid / getpgid / getpgrp / getsid stress ----
void test_pid_query_stress() {
  DEBUGLOG("Starting PID/TID/PGID query stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 20; i++) {
    pid_t pid  = getpid();
    pid_t tid  = syscall(SYS_gettid);
    pid_t ppid = getppid();
    pid_t pgid = getpgid(0);
    pid_t pgrp = getpgrp();
    pid_t sid  = getsid(0);
    (void)pid; (void)tid; (void)ppid; (void)pgid; (void)pgrp; (void)sid;
    if (i % 500 == 0) DEBUGLOG("PID QUERY iteration %d complete\n", i);
  }
  DEBUGLOG("PID/TID/PGID query stress test PASSED\n");
}

// ---- uid/gid getters/setters stress ----
void test_uid_gid_stress() {
  DEBUGLOG("Starting UID/GID stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;

    getuid(); geteuid();
    getgid(); getegid();
    getresuid(&ruid, &euid, &suid);
    getresgid(&rgid, &egid, &sgid);

    // round-trip set/get (we run as root so this succeeds)
    setuid(0); setgid(0);
    setresuid(0, 0, 0);
    setresgid(0, 0, 0);

    if (i % 250 == 0) DEBUGLOG("UID/GID iteration %d complete\n", i);
  }
  DEBUGLOG("UID/GID stress test PASSED\n");
}

// ---- prctl (PR_SET_NAME / PR_GET_NAME) stress ----
void test_prctl_stress() {
  DEBUGLOG("Starting PRCTL stress test...\n");
  char name[16];
  char orig[16];
  prctl(PR_GET_NAME, orig, 0, 0, 0);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    char setname[16];
    snprintf(setname, sizeof(setname), "stress_%d", i % 9999);
    prctl(PR_SET_NAME, setname, 0, 0, 0);
    prctl(PR_GET_NAME, name, 0, 0, 0);
    if (i % 100 == 0) DEBUGLOG("PRCTL iteration %d complete\n", i);
  }
  prctl(PR_SET_NAME, orig, 0, 0, 0);
  DEBUGLOG("PRCTL stress test PASSED\n");
}

// ---- sched_yield / sched_getscheduler / sched_setscheduler /
//      sched_getparam / sched_setparam /
//      sched_getaffinity / sched_setaffinity stress ----
void test_sched_stress() {
  DEBUGLOG("Starting SCHED stress test...\n");
  struct sched_param sp = {0};
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(0, &mask);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    sched_yield();
    sched_getscheduler(0);
    sched_setscheduler(0, SCHED_OTHER, &sp);
    sched_getparam(0, &sp);
    sched_setparam(0, &sp);
    sched_getaffinity(0, sizeof(mask), &mask);
    sched_setaffinity(0, sizeof(mask), &mask);
    sched_get_priority_max(SCHED_OTHER);
    sched_get_priority_min(SCHED_OTHER);
    if (i % 100 == 0) DEBUGLOG("SCHED iteration %d complete\n", i);
  }
  DEBUGLOG("SCHED stress test PASSED\n");
}

// ---- setpgid / setsid / setpriority / getpriority stress ----
void test_process_attrs_stress() {
  DEBUGLOG("Starting PROCESS ATTRS stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    // Fork so we can call setsid/setpgid safely without affecting the harness
    pid_t pid = fork();
    if (pid == 0) {
      setpgid(0, 0);
      // setsid would fail if we are a process group leader, so only try
      // from a process that just called setpgid(0,0) to get its own group
      // (only succeeds if tid != pgid after setpgid, but try anyway)
      setsid();
      setpriority(PRIO_PROCESS, 0, 0);
      getpriority(PRIO_PROCESS, 0);
      exit(0);
    } else if (pid > 0) {
      int st;
      waitpid(pid, &st, 0);
    }
    if (i % 25 == 0) DEBUGLOG("PROCESS ATTRS iteration %d complete\n", i);
  }
  DEBUGLOG("PROCESS ATTRS stress test PASSED\n");
}

// ---- setitimer / alarm stress ----
void test_itimer_stress() {
  DEBUGLOG("Starting SETITIMER/ALARM stress test...\n");
  struct itimerval itv, old;

  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    // Set a 100 ms timer and immediately disarm it
    itv.it_value.tv_sec  = 0;
    itv.it_value.tv_usec = 100000;
    itv.it_interval.tv_sec  = 0;
    itv.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &itv, &old);

    // Disarm immediately
    itv.it_value.tv_sec  = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &itv, &old);

    // alarm(0) cancels any pending alarm
    alarm(0);

    if (i % 75 == 0) DEBUGLOG("ITIMER iteration %d complete\n", i);
  }
  DEBUGLOG("SETITIMER/ALARM stress test PASSED\n");
}

// ---- mlock / munlock stress ----
void test_mlock_stress() {
  DEBUGLOG("Starting MLOCK stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    size_t sz = 4 * 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mlock mmap"); exit(1); }
    memset(p, 0, sz);

    int r = mlock(p, sz);
    if (r == 0) munlock(p, sz);   // only unlockif lock succeeded

    munmap(p, sz);
    if (i % 50 == 0) DEBUGLOG("MLOCK iteration %d complete\n", i);
  }
  DEBUGLOG("MLOCK stress test PASSED\n");
}

// ---- utimensat / futimesat / utimes stress ----
void test_utimes_stress() {
  DEBUGLOG("Starting UTIMES/UTIMENSAT stress test...\n");
  char path[] = "/tmp/utimes_stress.tmp";
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0) { write(fd, "t", 1); close(fd); }

  struct timespec ts[2] = {{0, 0}, {0, 0}};
  struct timeval  tv[2] = {{0, 0}, {0, 0}};

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    ts[0].tv_sec = ts[1].tv_sec = (time_t)i;
    utimensat(AT_FDCWD, path, ts, 0);
    utimensat(AT_FDCWD, path, NULL, 0); // update to now

    tv[0].tv_sec = tv[1].tv_sec = (time_t)i;
    utimes(path, tv);

    if (i % 100 == 0) DEBUGLOG("UTIMES iteration %d complete\n", i);
  }
  unlink(path);
  DEBUGLOG("UTIMES/UTIMENSAT stress test PASSED\n");
}

// ---- umask stress ----
void test_umask_stress() {
  DEBUGLOG("Starting UMASK stress test...\n");
  mode_t saved = umask(0);
  for (int i = 0; i < NUM_ITERATIONS * 20; i++) {
    umask(0022);
    umask(0077);
    umask(0);
    if (i % 500 == 0) DEBUGLOG("UMASK iteration %d complete\n", i);
  }
  umask(saved);
  DEBUGLOG("UMASK stress test PASSED\n");
}

// ---- openat / mkdirat / unlinkat / fchmodat / fchownat stress ----
void test_at_variants_stress() {
  DEBUGLOG("Starting *AT variant syscall stress test...\n");
  int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  if (dirfd < 0) { perror("at_variants open /tmp"); return; }

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    char name[64], subdir[64];
    snprintf(name,   sizeof(name),   "at_stress_%d.tmp", i);
    snprintf(subdir, sizeof(subdir), "at_stress_dir_%d", i);

    // openat + close
    int fd = openat(dirfd, name, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
      write(fd, "hello", 5);
      // fchmodat
      fchmodat(dirfd, name, 0600, 0);
      // fchownat
      fchownat(dirfd, name, 0, 0, 0);
      close(fd);
      // unlinkat (file)
      unlinkat(dirfd, name, 0);
    }

    // mkdirat + unlinkat (dir)
    mkdirat(dirfd, subdir, 0755);
    unlinkat(dirfd, subdir, AT_REMOVEDIR);

    if (i % 50 == 0) DEBUGLOG("AT VARIANTS iteration %d complete\n", i);
  }
  close(dirfd);
  DEBUGLOG("*AT variant syscall stress test PASSED\n");
}

// ---- vfork + execve stress (already tested separately, combine here) ----
void test_vfork_stress() {
  DEBUGLOG("Starting VFORK stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS / 2; i++) {
    pid_t pid = vfork();
    if (pid == 0) {
      // Child: do as little as possible before exit (vfork shares address space)
      _exit(0);
    } else if (pid > 0) {
      int st;
      waitpid(pid, &st, 0);
    }
    if (i % 10 == 0) DEBUGLOG("VFORK iteration %d complete\n", i);
  }
  DEBUGLOG("VFORK stress test PASSED\n");
}

// ---- kill (self SIGCONT, ignored signals) stress ----
void test_kill_stress() {
  DEBUGLOG("Starting KILL stress test...\n");
  // Explicitly ignore every signal we're going to send to ourselves.
  // SIGCONT's POSIX default is "resume", but the kernel currently falls
  // back to terminate for unhandled signals — set SIG_IGN to be safe.
  struct sigaction sa = {0};
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  struct sigaction old_usr1, old_usr2, old_cont;
  sigaction(SIGUSR1, &sa, &old_usr1);
  sigaction(SIGUSR2, &sa, &old_usr2);
  sigaction(SIGCONT, &sa, &old_cont);

  pid_t self = getpid();
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    kill(self, SIGUSR1);
    kill(self, SIGUSR2);
    kill(self, SIGCONT);
    // kill(self, 0) is a simple existence check — never delivers a signal
    kill(self, 0);
    if (i % 250 == 0) DEBUGLOG("KILL iteration %d complete\n", i);
  }
  // Restore original handlers
  sigaction(SIGUSR1, &old_usr1, NULL);
  sigaction(SIGUSR2, &old_usr2, NULL);
  sigaction(SIGCONT, &old_cont, NULL);
  DEBUGLOG("KILL stress test PASSED\n");
}

// ---- rt_sigaction / rt_sigprocmask stress ----
void test_signal_mask_stress() {
  DEBUGLOG("Starting SIGACTION/SIGPROCMASK stress test...\n");
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  sigaddset(&set, SIGUSR2);

  struct sigaction sa = {0}, old_sa;
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    sigaction(SIGUSR1, &sa, &old_sa);
    sigaction(SIGUSR1, &old_sa, NULL); // restore

    sigprocmask(SIG_BLOCK,   &set, &oldset);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    sigprocmask(SIG_SETMASK, &oldset, NULL); // restore

    if (i % 100 == 0) DEBUGLOG("SIGMASK iteration %d complete\n", i);
  }
  DEBUGLOG("SIGACTION/SIGPROCMASK stress test PASSED\n");
}

// ---- wait4 with WNOHANG on no-child: ECHILD stress ----
void test_wait4_stress() {
  DEBUGLOG("Starting WAIT4 stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      // child: just exit immediately
      _exit(i & 0xFF);
    } else if (pid > 0) {
      int st;
      // Try WNOHANG first (may return 0 if child not dead yet)
      pid_t r = waitpid(pid, &st, WNOHANG);
      if (r == 0) {
        // child still running; blocking wait
        waitpid(pid, &st, 0);
      }
    }
    if (i % 50 == 0) DEBUGLOG("WAIT4 iteration %d complete\n", i);
  }
  DEBUGLOG("WAIT4 stress test PASSED\n");
}

// ---- xattr stress ----
void test_xattr_stress(void) {
  DEBUGLOG("Starting XATTR stress test...\n");
  char path[] = "/tmp/xattr_stress.tmp";
  char sympath[] = "/tmp/xattr_symlink.tmp";
  char listbuf[256];
  char valbuf[64];

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) {
    perror("xattr open");
    return;
  }
  write(fd, "test", 4);
  symlink(path, sympath);

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    // setxattr, lsetxattr, fsetxattr
    setxattr(path, "user.stress", "val1", 4, 0);
    fsetxattr(fd, "user.stress2", "val2", 4, 0);
    lsetxattr(sympath, "user.stress3", "val3", 4, 0);

    // getxattr, lgetxattr, fgetxattr
    getxattr(path, "user.stress", valbuf, sizeof(valbuf));
    fgetxattr(fd, "user.stress2", valbuf, sizeof(valbuf));
    lgetxattr(sympath, "user.stress3", valbuf, sizeof(valbuf));

    // listxattr, llistxattr, flistxattr
    listxattr(path, listbuf, sizeof(listbuf));
    flistxattr(fd, listbuf, sizeof(listbuf));
    llistxattr(sympath, listbuf, sizeof(listbuf));

    // removexattr, lremovexattr, fremovexattr
    removexattr(path, "user.stress");
    fremovexattr(fd, "user.stress2");
    lremovexattr(sympath, "user.stress3");

    if (i % 50 == 0)
      DEBUGLOG("XATTR iteration %d complete\n", i);
  }

  close(fd);
  unlink(sympath);
  unlink(path);
  DEBUGLOG("XATTR stress test PASSED\n");
}

// ---- sysv semaphores stress ----
void test_sysv_sem_stress(void) {
  DEBUGLOG("Starting SYSV SEMAPHORE stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) {
      if (i == 0) DEBUGLOG("Skipping SYSV SEM (not implemented)\n");
      break;
    }

    // semctl SETVAL
    union semun {
      int val;
      struct semid_ds *buf;
      unsigned short *array;
    } arg;
    arg.val = 1;
    semctl(semid, 0, SETVAL, arg);

    // semop (decrement, then increment)
    struct sembuf sb_down = {0, -1, IPC_NOWAIT};
    struct sembuf sb_up = {0, 1, 0};
    semop(semid, &sb_down, 1);
    semop(semid, &sb_up, 1);

    // semtimedop (non-blocking 0 timeout)
    struct timespec ts = {0, 0};
    syscall(SYS_semtimedop, semid, &sb_down, 1, &ts);
    syscall(SYS_semtimedop, semid, &sb_up, 1, &ts);

    // semctl GETVAL
    semctl(semid, 0, GETVAL, 0);

    // semctl IPC_RMID
    semctl(semid, 0, IPC_RMID, 0);

    if (i % 50 == 0)
      DEBUGLOG("SYSV SEM iteration %d complete\n", i);
  }
  DEBUGLOG("SYSV SEMAPHORE stress test PASSED\n");
}

// ---- splice / copy_file_range stress ----
void test_splice_copy_range_stress(void) {
  DEBUGLOG("Starting SPLICE / COPY_FILE_RANGE stress test...\n");
  char in_path[] = "/tmp/cfr_in.tmp";
  char out_path[] = "/tmp/cfr_out.tmp";
  char splice_path[] = "/tmp/splice_out.tmp";

  int pfd[2];
  if (pipe(pfd) < 0) {
    perror("splice pipe");
    return;
  }

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    // 1. Test splice: pipe -> file
    write(pfd[1], "splice_data", 11);
    int sfd = open(splice_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (sfd >= 0) {
      loff_t off_out = 0;
      splice(pfd[0], NULL, sfd, &off_out, 11, SPLICE_F_NONBLOCK);
      close(sfd);
      unlink(splice_path);
    }

    // 2. Test copy_file_range: file -> file
    int ifd = open(in_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
    int ofd = open(out_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (ifd >= 0 && ofd >= 0) {
      write(ifd, "copy_file_range_data", 20);
      loff_t off_in = 0;
      loff_t off_out = 0;
      copy_file_range(ifd, &off_in, ofd, &off_out, 20, 0);
    }
    if (ifd >= 0) { close(ifd); unlink(in_path); }
    if (ofd >= 0) { close(ofd); unlink(out_path); }

    if (i % 50 == 0)
      DEBUGLOG("SPLICE/CFR iteration %d complete\n", i);
  }

  close(pfd[0]);
  close(pfd[1]);
  DEBUGLOG("SPLICE / COPY_FILE_RANGE stress test PASSED\n");
}

// ---- sendmmsg / recvmmsg stress ----
void test_msg_vector_stress(void) {
  DEBUGLOG("Starting SENDMMSG / RECVMMSG stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
      break;

    char buf1[] = "hello";
    char buf2[] = "world";
    struct iovec iov[2];
    iov[0].iov_base = buf1; iov[0].iov_len = sizeof(buf1);
    iov[1].iov_base = buf2; iov[1].iov_len = sizeof(buf2);

    struct mmsghdr msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].msg_hdr.msg_iov = &iov[0];
    msgs[0].msg_hdr.msg_iovlen = 1;
    msgs[1].msg_hdr.msg_iov = &iov[1];
    msgs[1].msg_hdr.msg_iovlen = 1;

    sendmmsg(sv[0], msgs, 2, 0);

    char rbuf1[16];
    char rbuf2[16];
    struct iovec riov[2];
    riov[0].iov_base = rbuf1; riov[0].iov_len = sizeof(rbuf1);
    riov[1].iov_base = rbuf2; riov[1].iov_len = sizeof(rbuf2);

    struct mmsghdr rmsgs[2];
    memset(rmsgs, 0, sizeof(rmsgs));
    rmsgs[0].msg_hdr.msg_iov = &riov[0];
    rmsgs[0].msg_hdr.msg_iovlen = 1;
    rmsgs[1].msg_hdr.msg_iov = &riov[1];
    rmsgs[1].msg_hdr.msg_iovlen = 1;

    struct timespec timeout = {0, 0};
    recvmmsg(sv[1], rmsgs, 2, MSG_DONTWAIT, &timeout);

    close(sv[0]);
    close(sv[1]);

    if (i % 50 == 0)
      DEBUGLOG("SENDMMSG/RECVMMSG iteration %d complete\n", i);
  }
  DEBUGLOG("SENDMMSG / RECVMMSG stress test PASSED\n");
}

// ---- msync / mincore / mlockall / mlock2 / mbind / set_mempolicy / get_mempolicy stress ----
void test_mem_policy_stress(void) {
  DEBUGLOG("Starting MEMORY POLICY & EXTENDED MM stress test...\n");
  size_t sz = 4 * 4096;
  void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mem_policy mmap");
    return;
  }
  memset(p, 0xEE, sz);

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    // msync
    msync(p, sz, MS_ASYNC);
    msync(p, sz, MS_SYNC);

    // mincore
    unsigned char vec[4] = {0};
    mincore(p, sz, vec);

    // mlockall / munlockall
    mlockall(MCL_CURRENT);
    munlockall();

    // mlock2
    syscall(SYS_mlock2, p, sz, 0);
    munlock(p, sz);

    // mbind / set_mempolicy / get_mempolicy
    int mode = 0;
    unsigned long nodemask = 0;
    syscall(SYS_get_mempolicy, &mode, &nodemask, sizeof(nodemask) * 8, 0, 0);
    syscall(SYS_set_mempolicy, 0 /* MPOL_DEFAULT */, NULL, 0);
    syscall(SYS_mbind, p, sz, 0 /* MPOL_DEFAULT */, NULL, 0, 0);

    if (i % 50 == 0)
      DEBUGLOG("MEM POLICY iteration %d complete\n", i);
  }

  munmap(p, sz);
  DEBUGLOG("MEMORY POLICY & EXTENDED MM stress test PASSED\n");
}

// ---- pause / rt_sigsuspend / rt_sigtimedwait / signalfd4 / rt_sigreturn stress ----
static volatile sig_atomic_t g_ext_sig_handled = 0;
static void ext_sig_handler(int sig) {
  (void)sig;
  g_ext_sig_handled = 1;
}

void test_extended_signals_stress(void) {
  DEBUGLOG("Starting EXTENDED SIGNALS stress test...\n");

  // 1. rt_sigtimedwait & signalfd4
  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, SIGUSR2);
  struct timespec zero_ts = {0, 0};
  siginfo_t sinfo;

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    syscall(SYS_rt_sigtimedwait, &waitmask, &sinfo, &zero_ts, 8 /* sizeof(sigset_t) */);

    int sfd = syscall(SYS_signalfd4, -1, &waitmask, 8, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd >= 0) close(sfd);

    if (i % 50 == 0)
      DEBUGLOG("SIGTIMEDWAIT/SIGNALFD4 iteration %d complete\n", i);
  }

  // 2. pause, rt_sigsuspend, and rt_sigreturn via handler dispatch in child
  for (int i = 0; i < NUM_ITERATIONS / 5; i++) {
    pid_t cpid = fork();
    if (cpid == 0) {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = ext_sig_handler;
      sigemptyset(&sa.sa_mask);
      sigaction(SIGUSR1, &sa, NULL);

      // Child sets alarm so it never hangs if signal is lost
      alarm(2);

      // Parent sends SIGUSR1; pause() catches it and returns -1 with EINTR
      // The return from ext_sig_handler automatically invokes SYS_rt_sigreturn!
      pause();

      // Next, test sigsuspend
      sigset_t suspend_mask;
      sigemptyset(&suspend_mask);
      sigsuspend(&suspend_mask);

      _exit(0);
    } else if (cpid > 0) {
      usleep(2000);
      kill(cpid, SIGUSR1);
      usleep(2000);
      kill(cpid, SIGUSR1);
      int status;
      waitpid(cpid, &status, 0);
    }
  }

  DEBUGLOG("EXTENDED SIGNALS stress test PASSED\n");
}

// ---- remaining registered syscall families ----
// These complement the focused stress tests above. Privileged calls are made
// with invalid authorization arguments, so the handler is exercised without
// changing machine state.
static void run_registered_syscall_coverage(int iterations) {
  DEBUGLOG("Starting registered-syscall coverage stress test...\n");

  for (int i = 0; i < iterations; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/syscall_coverage_%d.tmp", i);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) continue;
    write(fd, "coverage", 8);

    // Descriptor and legacy I/O variants.
    ioctl(fd, 0, NULL);                 // expected ENOTTY
    flock(fd, LOCK_EX | LOCK_NB);
    flock(fd, LOCK_UN);
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_NORMAL);
    syscall(SYS_faccessat2, AT_FDCWD, path, F_OK, 0);

    // fchmod, fchown, lchown
    fchmod(fd, 0644);
    fchown(fd, 0, 0);
    lchown(path, 0, 0);

    int doomed = dup(fd);
    if (doomed >= 0)
      syscall(SYS_close_range, (unsigned)doomed, (unsigned)doomed, 0);

    // Old and new stat/directory ABIs.
    struct statx sx;
    memset(&sx, 0, sizeof(sx));
    statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &sx);
    int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
      char dents[1024];
      syscall(SYS_getdents, dfd, dents, sizeof(dents));
      close(dfd);
    }

    // select/pselect and the original epoll ABI, all nonblocking.
    struct timeval zero_tv = {0, 0};
    struct timespec zero_ts = {0, 0};
    select(0, NULL, NULL, NULL, &zero_tv);
    pselect(0, NULL, NULL, NULL, &zero_ts, NULL);
    int ep = epoll_create(1);
    if (ep >= 0) {
      struct epoll_event event;
      epoll_pwait(ep, &event, 1, 0, NULL);
      close(ep);
    }

    // clock_getres and clock_nanosleep have distinct syscall entries.
    struct timespec resolution;
    clock_getres(CLOCK_MONOTONIC, &resolution);
    clock_nanosleep(CLOCK_MONOTONIC, 0, &zero_ts, NULL);

    // getrusage
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    // syslog query
    syscall(SYS_syslog, 10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);

    // setreuid, setregid
    setreuid(0, 0);
    setregid(0, 0);

    // capget, capset
    struct { uint32_t version; int pid; } cap_hdr = { 0x20080522, 0 };
    struct { uint32_t eff, perm, inh; } cap_data[2] = {{0, 0, 0}, {0, 0, 0}};
    syscall(SYS_capget, &cap_hdr, cap_data);
    syscall(SYS_capset, &cap_hdr, cap_data);

    // mknod (FIFO)
    char fifo_path[64];
    snprintf(fifo_path, sizeof(fifo_path), "/tmp/fifo_cov_%d.tmp", i);
    mknod(fifo_path, S_IFIFO | 0644, 0);
    unlink(fifo_path);

    // umount2
    syscall(SYS_umount2, "/nonexistent_mount_test", 1 /* MNT_FORCE */);

    // sethostname
    sethostname("avory", 5);

    // waitid (non-blocking WNOHANG)
    siginfo_t wi_info;
    memset(&wi_info, 0, sizeof(wi_info));
    syscall(SYS_waitid, P_ALL, 0, &wi_info, WEXITED | WNOHANG, NULL);

    // inotify_init
    int infd = syscall(SYS_inotify_init);
    if (infd >= 0) close(infd);

    // renameat, linkat, symlinkat, readlinkat, renameat2, fchmodat2
    char at_path1[64], at_path2[64], at_sym[64], at_rbuf[64];
    snprintf(at_path1, sizeof(at_path1), "/tmp/at_cov1_%d.tmp", i);
    snprintf(at_path2, sizeof(at_path2), "/tmp/at_cov2_%d.tmp", i);
    snprintf(at_sym, sizeof(at_sym), "/tmp/at_sym_%d.tmp", i);
    int at_fd = open(at_path1, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (at_fd >= 0) {
      write(at_fd, "at", 2);
      close(at_fd);
      linkat(AT_FDCWD, at_path1, AT_FDCWD, at_path2, 0);
      unlinkat(AT_FDCWD, at_path2, 0);
      symlinkat(at_path1, AT_FDCWD, at_sym);
      readlinkat(AT_FDCWD, at_sym, at_rbuf, sizeof(at_rbuf));
      unlink(at_sym);
      syscall(SYS_renameat2, AT_FDCWD, at_path1, AT_FDCWD, at_path2, 0);
      syscall(SYS_fchmodat2, AT_FDCWD, at_path2, 0600, 0);
      renameat(AT_FDCWD, at_path2, AT_FDCWD, at_path1);
      unlink(at_path1);
    }

    // unshare
    syscall(SYS_unshare, 0);

    // get_robust_list
    void *rob_head = NULL;
    size_t rob_len = 0;
    syscall(SYS_get_robust_list, 0, &rob_head, &rob_len);

    // name_to_handle_at, open_by_handle_at
    struct file_handle fh;
    int mount_id = 0;
    syscall(SYS_name_to_handle_at, AT_FDCWD, path, &fh, &mount_id, 0);
    syscall(SYS_open_by_handle_at, AT_FDCWD, &fh, O_RDONLY);

    // getcpu
    unsigned cpu_num = 0, node_num = 0;
    syscall(SYS_getcpu, &cpu_num, &node_num, NULL);

    // kcmp
    syscall(SYS_kcmp, getpid(), getpid(), 1 /* KCMP_VM */, 0, 0);

    // seccomp
    syscall(SYS_seccomp, 0 /* SECCOMP_SET_MODE_STRICT */, 0, NULL);

    // clone3 (invalid args test returns -EINVAL cleanly)
    syscall(SYS_clone3, NULL, 0);

    close(fd);
    unlink(path);
  }

  // SysV shared-memory allocation/attach/detach/removal lifecycle.
  for (int i = 0; i < iterations; i++) {
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) break;
    void *p = shmat(id, NULL, 0);
    if (p != (void *)-1) {
      memset(p, i, 4096);
      shmdt(p);
    }
    shmctl(id, IPC_RMID, NULL);
  }

  // signalfd, sigaltstack, tkill and tgkill.
  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);
  for (int i = 0; i < iterations; i++) {
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd >= 0) {
      syscall(SYS_tkill, syscall(SYS_gettid), 0);
      syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), 0);
      close(sfd);
    }
  }
  sigprocmask(SIG_SETMASK, &oldmask, NULL);

  void *alt_mem = malloc(SIGSTKSZ);
  if (alt_mem) {
    stack_t old_ss, ss = {.ss_sp = alt_mem, .ss_size = SIGSTKSZ, .ss_flags = 0};
    if (sigaltstack(&ss, &old_ss) == 0) sigaltstack(&old_ss, NULL);
    free(alt_mem);
  }

  // Socket message and address-query entries.
  for (int i = 0; i < iterations; i++) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) break;
    char byte = 'x', out = 0, control[32];
    struct iovec tx = {.iov_base = &byte, .iov_len = 1};
    struct iovec rx = {.iov_base = &out, .iov_len = 1};
    struct msghdr msg = {.msg_iov = &tx, .msg_iovlen = 1};
    sendmsg(sv[0], &msg, 0);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &rx; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    recvmsg(sv[1], &msg, 0);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    getsockname(sv[0], (struct sockaddr *)&addr, &len);
    len = sizeof(addr);
    getpeername(sv[0], (struct sockaddr *)&addr, &len);
    close(sv[0]); close(sv[1]);
  }

  // Process-local query/setter entries. Preserve every observable value.
  gid_t groups[32];
  int ngroups = getgroups(32, groups);
  if (ngroups >= 0) setgroups((size_t)ngroups, groups);
  setfsuid(geteuid());
  setfsgid(getegid());
  syscall(SYS_membarrier, 0, 0);
  uint64_t fsbase = 0;
  syscall(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &fsbase);
  syscall(399 /* AscentOS SYS_UPTIME */);
  int pidfd = syscall(SYS_pidfd_open, getpid(), 0);
  if (pidfd >= 0) close(pidfd);

  // Thread-runtime bookkeeping calls are isolated because set_tid_address and
  // robust-list state are intentionally persistent until thread exit.
  pid_t child = fork();
  if (child == 0) {
    int clear_tid = 0;
    syscall(SYS_set_tid_address, &clear_tid);
    syscall(SYS_set_robust_list, NULL, 0);
    syscall(SYS_rseq, NULL, 0, 0, 0);
    syscall(SYS_exit, 0);
    __builtin_unreachable();
  }
  if (child > 0) waitpid(child, NULL, 0);
  child = fork();
  if (child == 0) {
    syscall(SYS_exit_group, 0);
    __builtin_unreachable();
  }
  if (child > 0) waitpid(child, NULL, 0);

  // Exercise privileged dispatch paths without authorizing an operation.
  syscall(SYS_mount, NULL, NULL, NULL, 0, NULL);
  syscall(SYS_reboot, 0, 0, 0, NULL);

  DEBUGLOG("Registered-syscall coverage stress test PASSED\n");
}

void test_registered_syscall_coverage(void) {
  run_registered_syscall_coverage(NUM_ITERATIONS);
}

void check_leak(const char *test_name, long *last_mem) {
  long current_mem = get_free_mem_kb();
  if (current_mem == -1)
    return;

  long diff = *last_mem - current_mem;
  if (diff > 0) {
    printf("  [!] %s: LEAKED %ld kB\n", test_name, diff);
  } else if (diff < 0) {
    printf("  [ok] %s: Reclaimed %ld kB\n", test_name, -diff);
  } else {
    printf("  [ok] %s: No leak\n", test_name);
  }
  *last_mem = current_mem;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("=== Userland Leak Detection Stress Test ===\n");

  // Prime the kernel slab caches used by epoll, watched VFS nodes and pipes.
  // Empty slab pages are intentionally retained for reuse, so measuring the
  // first-ever lifecycle would mislabel bounded cache growth as a leak.
  if (epoll_lifecycle_once() < 0) {
    printf("CRITICAL: epoll allocator warm-up failed: %s\n", strerror(errno));
    return 1;
  }
  if (prime_wait4_caches() < 0) {
    printf("CRITICAL: wait4 allocator warm-up failed: %s\n", strerror(errno));
    return 1;
  }
  if (prime_registered_syscall_caches() < 0) {
    printf("CRITICAL: syscall descriptor cache warm-up failed: %s\n",
           strerror(errno));
    return 1;
  }
  /* Run this last: the other warm-ups can change the process VMA layout and
   * make the first measured mmap require another page-table/slab page. */
  if (prime_mmap_vfs_caches() < 0) {
    printf("CRITICAL: mmap/VFS allocator warm-up failed: %s\n",
           strerror(errno));
    return 1;
  }

  long initial_mem = get_free_mem_kb();
  if (initial_mem == -1) {
    printf("CRITICAL: Could not read /proc/meminfo. Make sure procfs is "
           "mounted.\n");
    return 1;
  }
  printf("Initial Free Memory: %ld kB\n", initial_mem);
  long current_mem = initial_mem;

  printf("\n--- Running MMAP Stress ---\n");
  test_mmap_stress();
  check_leak("MMAP Stress", &current_mem);

  printf("\n--- Running VFS Stress ---\n");
  test_vfs_stress();
  check_leak("VFS Stress", &current_mem);

  printf("\n--- Running VFS Open/Close Stress ---\n");
  test_vfs_open_close_stress();
  check_leak("VFS Open/Close Stress", &current_mem);

  printf("\n--- Running EPOLL Stress ---\n");
  test_epoll_stress();
  check_leak("EPOLL Stress", &current_mem);

  printf("\n--- Running POLL Stress ---\n");
  test_poll_stress();
  check_leak("POLL Stress", &current_mem);

  printf("\n--- Running PPOLL Stress ---\n");
  test_ppoll_stress();
  check_leak("PPOLL Stress", &current_mem);

  printf("\n--- Running PIPE Stress ---\n");
  test_pipe_stress();
  check_leak("PIPE Stress", &current_mem);

  printf("\n--- Running SOCKET Stress ---\n");
  test_socket_stress();
  check_leak("SOCKET Stress", &current_mem);

  printf("\n--- Running UNIX STREAM Stress ---\n");
  test_unix_stream_stress();
  check_leak("UNIX STREAM Stress", &current_mem);

  printf("\n--- Running SOCKETPAIR Stress ---\n");
  test_socketpair_stress();
  check_leak("SOCKETPAIR Stress", &current_mem);

  printf("\n--- Running UNIX DGRAM Stress ---\n");
  test_unix_dgram_stress();
  check_leak("UNIX DGRAM Stress", &current_mem);

  printf("\n--- Running SOCKET OPTS Stress ---\n");
  test_socket_opts_stress();
  check_leak("SOCKET OPTS Stress", &current_mem);

  printf("\n--- Running SHUTDOWN Stress ---\n");
  test_shutdown_stress();
  check_leak("SHUTDOWN Stress", &current_mem);

  printf("\n--- Running ACCEPT4 Stress ---\n");
  test_accept4_stress();
  check_leak("ACCEPT4 Stress", &current_mem);

  printf("\n--- Running FORK Stress ---\n");
  test_fork_stress();
  check_leak("FORK Stress", &current_mem);

  printf("\n--- Running CLONE/FUTEX Stress ---\n");
  test_clone_futex_stress();
  check_leak("CLONE/FUTEX Stress", &current_mem);

  printf("\n--- Running VFS Error Stress ---\n");
  test_vfs_error_stress();
  check_leak("VFS Error Stress", &current_mem);

  printf("\n--- Running MMAP FIXED Stress ---\n");
  test_mmap_fixed_stress();
  check_leak("MMAP FIXED Stress", &current_mem);

  printf("\n--- Running READV Stress ---\n");
  test_readv_stress();
  check_leak("READV Stress", &current_mem);

  printf("\n--- Running EXECVE Stress ---\n");
  test_exec_stress();
  check_leak("EXECVE Stress", &current_mem);

  printf("\n--- Running DUP/DUP2/FCNTL Stress ---\n");
  test_dup_fcntl_stress();
  check_leak("DUP/DUP2/FCNTL Stress", &current_mem);

  printf("\n--- Running PREAD64/PWRITE64 Stress ---\n");
  test_pread_pwrite_stress();
  check_leak("PREAD64/PWRITE64 Stress", &current_mem);

  printf("\n--- Running WRITEV Stress ---\n");
  test_writev_stress();
  check_leak("WRITEV Stress", &current_mem);

  printf("\n--- Running SENDFILE Stress ---\n");
  test_sendfile_stress();
  check_leak("SENDFILE Stress", &current_mem);

  printf("\n--- Running FTRUNCATE/FALLOCATE/FSYNC/FSTAT Stress ---\n");
  test_ftruncate_fsync_stress();
  check_leak("FTRUNCATE/FALLOCATE/FSYNC Stress", &current_mem);

  printf("\n--- Running LSEEK Stress ---\n");
  test_lseek_stress();
  check_leak("LSEEK Stress", &current_mem);

  printf("\n--- Running STAT Variants Stress ---\n");
  test_stat_variants_stress();
  check_leak("STAT Variants Stress", &current_mem);

  printf("\n--- Running GETDENTS64 Stress ---\n");
  test_getdents64_stress();
  check_leak("GETDENTS64 Stress", &current_mem);

  printf("\n--- Running STATFS/FSTATFS Stress ---\n");
  test_statfs_stress();
  check_leak("STATFS/FSTATFS Stress", &current_mem);

  printf("\n--- Running FS OPS Stress ---\n");
  test_fs_ops_stress();
  check_leak("FS OPS Stress", &current_mem);

  printf("\n--- Running ACCESS/FACCESSAT Stress ---\n");
  test_access_stress();
  check_leak("ACCESS/FACCESSAT Stress", &current_mem);

  printf("\n--- Running GETCWD/CHDIR/FCHDIR Stress ---\n");
  test_getcwd_chdir_stress();
  check_leak("GETCWD/CHDIR Stress", &current_mem);

  printf("\n--- Running MPROTECT Stress ---\n");
  test_mprotect_stress();
  check_leak("MPROTECT Stress", &current_mem);

  printf("\n--- Running MREMAP Stress ---\n");
  test_mremap_stress();
  check_leak("MREMAP Stress", &current_mem);

  printf("\n--- Running BRK Stress ---\n");
  test_brk_stress();
  check_leak("BRK Stress", &current_mem);

  printf("\n--- Running MADVISE Stress ---\n");
  test_madvise_stress();
  check_leak("MADVISE Stress", &current_mem);

  printf("\n--- Running PIPE2 Stress ---\n");
  test_pipe2_stress();
  check_leak("PIPE2 Stress", &current_mem);

  printf("\n--- Running MEMFD_CREATE Stress ---\n");
  test_memfd_stress();
  check_leak("MEMFD_CREATE Stress", &current_mem);

  printf("\n--- Running TIMERFD Stress ---\n");
  test_timerfd_stress();
  check_leak("TIMERFD Stress", &current_mem);

  printf("\n--- Running EVENTFD Stress ---\n");
  test_eventfd_stress();
  check_leak("EVENTFD Stress", &current_mem);

  printf("\n--- Running INOTIFY Stress ---\n");
  test_inotify_stress();
  check_leak("INOTIFY Stress", &current_mem);

  printf("\n--- Running GETRANDOM Stress ---\n");
  test_getrandom_stress();
  check_leak("GETRANDOM Stress", &current_mem);

  printf("\n--- Running CLOCK/TIME Stress ---\n");
  test_clock_stress();
  check_leak("CLOCK/TIME Stress", &current_mem);

  printf("\n--- Running UNAME/SYSINFO/GETRLIMIT Stress ---\n");
  test_sysinfo_stress();
  check_leak("SYSINFO/GETRLIMIT Stress", &current_mem);

  printf("\n--- Running PID/TID/PGID Query Stress ---\n");
  test_pid_query_stress();
  check_leak("PID Query Stress", &current_mem);

  printf("\n--- Running UID/GID Stress ---\n");
  test_uid_gid_stress();
  check_leak("UID/GID Stress", &current_mem);

  printf("\n--- Running PRCTL Stress ---\n");
  test_prctl_stress();
  check_leak("PRCTL Stress", &current_mem);

  printf("\n--- Running SCHED Stress ---\n");
  test_sched_stress();
  check_leak("SCHED Stress", &current_mem);

  printf("\n--- Running PROCESS ATTRS Stress ---\n");
  test_process_attrs_stress();
  check_leak("PROCESS ATTRS Stress", &current_mem);

  printf("\n--- Running SETITIMER/ALARM Stress ---\n");
  test_itimer_stress();
  check_leak("SETITIMER/ALARM Stress", &current_mem);

  printf("\n--- Running MLOCK Stress ---\n");
  test_mlock_stress();
  check_leak("MLOCK Stress", &current_mem);

  printf("\n--- Running UTIMES/UTIMENSAT Stress ---\n");
  test_utimes_stress();
  check_leak("UTIMES/UTIMENSAT Stress", &current_mem);

  printf("\n--- Running UMASK Stress ---\n");
  test_umask_stress();
  check_leak("UMASK Stress", &current_mem);

  printf("\n--- Running *AT Variants Stress ---\n");
  test_at_variants_stress();
  check_leak("AT Variants Stress", &current_mem);

  printf("\n--- Running VFORK Stress ---\n");
  test_vfork_stress();
  check_leak("VFORK Stress", &current_mem);

  printf("\n--- Running KILL Stress ---\n");
  test_kill_stress();
  check_leak("KILL Stress", &current_mem);

  printf("\n--- Running SIGACTION/SIGPROCMASK Stress ---\n");
  test_signal_mask_stress();
  check_leak("SIGACTION/SIGPROCMASK Stress", &current_mem);

  printf("\n--- Running WAIT4 Stress ---\n");
  test_wait4_stress();
  check_leak("WAIT4 Stress", &current_mem);

  printf("\n--- Running XATTR Stress ---\n");
  test_xattr_stress();
  check_leak("XATTR Stress", &current_mem);

  printf("\n--- Running SYSV SEMAPHORE Stress ---\n");
  test_sysv_sem_stress();
  check_leak("SYSV Semaphore Stress", &current_mem);

  printf("\n--- Running SPLICE / COPY_FILE_RANGE Stress ---\n");
  test_splice_copy_range_stress();
  check_leak("SPLICE/CFR Stress", &current_mem);

  printf("\n--- Running SENDMMSG / RECVMMSG Stress ---\n");
  test_msg_vector_stress();
  check_leak("SENDMMSG/RECVMMSG Stress", &current_mem);

  printf("\n--- Running MEMORY POLICY & EXTENDED MM Stress ---\n");
  test_mem_policy_stress();
  check_leak("Memory Policy Stress", &current_mem);

  printf("\n--- Running EXTENDED SIGNALS Stress ---\n");
  test_extended_signals_stress();
  check_leak("Extended Signals Stress", &current_mem);

  printf("\n--- Running REGISTERED SYSCALL COVERAGE Stress ---\n");
  test_registered_syscall_coverage();
  check_leak("Registered Syscall Coverage Stress", &current_mem);

  printf("Verifying memory levels...\n");
  long final_mem = get_free_mem_kb();
  printf("Final Free Memory:   %ld kB\n", final_mem);

  long total_diff = initial_mem - final_mem;
  if (total_diff > 0) {
    printf("FAIL: Total Memory leak: %ld kB\n", total_diff);
    return 1;
  } else if (total_diff < 0) {
    printf("PASS: Memory levels improved. Gained %ld kB.\n", -total_diff);
  } else {
    printf("PASS: No memory leaks detected.\n");
  }

  printf("ALL TESTS COMPLETED!\n");
  return 0;
}
