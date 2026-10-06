// AvoryOS - child state notification: SIGCHLD to the parent, and pidfds
//
// Two independent ways exist to learn that a child process changed state, and
// both are load-bearing for the ported desktop.
//
// 1. The exit signal.  Qt's forkfd helper (src/3rdparty/forkfd) has two
//    implementations.  With a pidfd it polls the descriptor; otherwise it forks
//    and the *parent* blocks on a pipe that only a SIGCHLD handler ever writes
//    to after reaping.  QProcess asks for the pipe implementation for every
//    spawn that installs a childProcessModifier, which is every KPtyProcess -
//    so every Konsole session.  Konsole's shell child runs the libutempter
//    helper through a nested QProcess inside KPty::login(), and that wait has no
//    timeout at all: with no SIGCHLD the child sat there until konsole gave up
//    with "Could not start program '/bin/bash'", having never called execve().
//
// 2. clone(CLONE_PIDFD).  Legacy clone() has no pidfd field and reports the
//    descriptor through the parent_tid argument.  When that slot was left
//    untouched, callers kept an uninitialised stack value and used it as a file
//    descriptor: waitid(P_PIDFD, ...) failed, and a value that happened to fall
//    in the descriptor range parked the caller in poll() forever.
//
// A process that never installed a SIGCHLD handler must not be woken either:
// Linux discards ignored signals before they reach the pending mask, and here a
// pending signal also makes a blocking poll() return EINTR.  The last two checks
// pin that down.
//
// 3. pipe2(O_CLOEXEC).  QProcess::waitForStarted() only polls the read end of a
//    pipe for POLLIN and counts on the end-of-file that exec produces when the
//    write end is close-on-exec.  While pipe2() ignored its flags the exec'd
//    shell kept that descriptor alive, the wait ran out its 30 seconds, and
//    Konsole printed "Could not start program '/bin/bash'" over a bash that had
//    started and was waiting at a prompt.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef WEXITED
#define WEXITED 4
#endif

static int failures = 0;

static void check(int ok, const char *what) {
  printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

/* ------------------------------------------------------------------------ */
/* 1. the forkfd "death pipe" pattern: a handler reaps, the pipe wakes poll  */
/* ------------------------------------------------------------------------ */

static int notify_pipe[2];

struct reap_note {
  int pid;
  int status;
};

static void on_sigchld(int sig, siginfo_t *info, void *uctx) {
  (void)sig;
  (void)info;
  (void)uctx;
  struct reap_note note = {0, 0};
  siginfo_t si;
  memset(&si, 0, sizeof(si));
  while (waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == 0 && si.si_pid > 0) {
    note.pid = si.si_pid;
    note.status = si.si_status;
    ssize_t written = write(notify_pipe[1], &note, sizeof(note));
    (void)written;
  }
}

static void test_sigchld_death_pipe(void) {
  printf("== SIGCHLD drives the forkfd death pipe\n");
  if (pipe(notify_pipe) != 0) {
    check(0, "pipe()");
    return;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_sigchld;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  check(sigaction(SIGCHLD, &sa, NULL) == 0, "sigaction(SIGCHLD, SA_SIGINFO)");

  pid_t child = fork();
  if (child == 0) {
    _exit(37);
  }
  check(child > 0, "fork()");
  if (child <= 0)
    return;

  /* No wait4() here on purpose: the handler is the only thing allowed to reap,
     exactly as in QProcess::waitForFinished(). */
  struct pollfd pfd = {.fd = notify_pipe[0], .events = POLLIN, .revents = 0};
  int ready;
  do {
    ready = poll(&pfd, 1, 5000);
  } while (ready < 0 && errno == EINTR);
  check(ready == 1, "poll(death pipe) wakes when the child exits");
  if (ready != 1) {
    close(notify_pipe[0]);
    close(notify_pipe[1]);
    return;
  }

  struct reap_note note = {0, 0};
  ssize_t got = read(notify_pipe[0], &note, sizeof(note));
  check(got == (ssize_t)sizeof(note), "handler reported the child through the pipe");
  check(note.pid == (int)child, "handler reaped the expected pid");
  check(note.status == 37, "exit status survives the handler");

  close(notify_pipe[0]);
  close(notify_pipe[1]);

  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &dfl, NULL);
}

/* ------------------------------------------------------------------------ */
/* 2. clone(CLONE_PIDFD) reports a descriptor through parent_tid             */
/* ------------------------------------------------------------------------ */

static void test_clone_pidfd(void) {
  printf("== clone(CLONE_PIDFD) returns a usable pidfd\n");

  int pidfd = -1;
  long pid = syscall(SYS_clone, CLONE_PIDFD | SIGCHLD, 0, &pidfd, 0, 0);
  if (pid == 0) {
    /* Child: exactly as forkfd does, exit without touching stdio. */
    _exit(9);
  }
  check(pid > 0, "clone(CLONE_PIDFD | SIGCHLD)");
  if (pid <= 0)
    return;

  check(pidfd >= 0, "pidfd written through the parent_tid slot");
  if (pidfd < 0)
    return;
  check(fcntl(pidfd, F_GETFD) != -1, "the reported descriptor exists");

  struct pollfd pfd = {.fd = pidfd, .events = POLLIN | POLLPRI, .revents = 0};
  check(poll(&pfd, 1, 5000) == 1, "poll(pidfd) reports the exit");

  siginfo_t si;
  memset(&si, 0, sizeof(si));
  int r = waitid(P_PIDFD, pidfd, &si, WEXITED);
  check(r == 0, "waitid(P_PIDFD) reaps the child");
  check(si.si_pid == (pid_t)pid, "pidfd names the cloned child");
  check(si.si_status == 9, "exit status reported through the pidfd");
  close(pidfd);
}

/* ------------------------------------------------------------------------ */
/* 3. only pidfds are accepted by waitid(P_PIDFD)                            */
/* ------------------------------------------------------------------------ */

static void test_pidfd_only(void) {
  printf("== waitid(P_PIDFD) rejects anything that is not a pidfd\n");

  siginfo_t si;
  memset(&si, 0, sizeof(si));
  /* Qt probes for pidfd support this exact way and needs EBADF, not EINVAL, or
     it silently falls back to the pipe implementation. */
  errno = 0;
  int r = waitid(P_PIDFD, INT32_MAX, &si, WEXITED | WNOHANG);
  check(r == -1 && errno == EBADF, "a descriptor number out of range gives EBADF");

  int fd = open("/etc/hostname", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    check(0, "open(/etc/hostname)");
    return;
  }
  /* A plain file carries no pid, so treating it as one would make the caller
     wait on an arbitrary process. */
  errno = 0;
  r = waitid(P_PIDFD, fd, &si, WEXITED | WNOHANG);
  check(r == -1 && errno == EBADF, "a regular file gives EBADF, not a foreign wait");
  close(fd);
}

/* A desktop parent can observe exit through a pidfd before it calls waitid.
 * SIG_DFL must retain the status even when no wait syscall is blocked yet.
 * The old process-name heuristic discarded children of these names. */
static void test_desktop_delayed_wait(void) {
  printf("== desktop process names preserve delayed child waits\n");
  char original_name[16] = {0};
  if (prctl(PR_GET_NAME, original_name) != 0) {
    check(0, "read process name");
    return;
  }
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  check(sigaction(SIGCHLD, &dfl, NULL) == 0, "default SIGCHLD for delayed wait");
  const char *names[] = {"sddm", "dbus-daemon", "plasmashell", "startplasma-x11"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    check(prctl(PR_SET_NAME, names[i]) == 0, names[i]);
    for (int round = 0; round < 8; round++) {
      int pidfd = -1;
      long child = syscall(SYS_clone, CLONE_PIDFD | SIGCHLD, 0, &pidfd, 0, 0);
      if (child == 0)
        _exit(37);
      if (child < 0 || pidfd < 0) {
        check(0, "create desktop child with pidfd");
        if (child > 0)
          waitpid((pid_t)child, NULL, 0);
        break;
      }
      struct pollfd pfd = {.fd = pidfd, .events = POLLIN};
      int ready = poll(&pfd, 1, 3000);
      check(ready == 1, "pidfd reports desktop child exit before wait");
      /* Let any asynchronous reaper run, while no wait syscall is active. */
      usleep(20000);
      siginfo_t si;
      memset(&si, 0, sizeof(si));
      int result = waitid(P_PIDFD, pidfd, &si, WEXITED | WNOHANG | WNOWAIT);
      check(result == 0 && si.si_pid == child && si.si_status == 37,
            "delayed waitid(WNOWAIT) retains exit status");
      int status = 0;
      pid_t reaped = waitpid((pid_t)child, &status, ready == 1 ? WNOHANG : 0);
      check(reaped == child && WIFEXITED(status) && WEXITSTATUS(status) == 37,
            "waitpid consumes retained desktop child status");
      close(pidfd);
    }
  }
  check(prctl(PR_SET_NAME, original_name) == 0, "restore process name");
}

static void test_blocked_sigchld_signalfd(void) {
  printf("== blocked default SIGCHLD remains readable through signalfd\n");
  sigset_t mask, saved_mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &mask, &saved_mask) != 0) {
    check(0, "block SIGCHLD for signalfd");
    return;
  }
  int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  check(fd >= 0, "create SIGCHLD signalfd");
  if (fd >= 0) {
    pid_t child = fork();
    if (child == 0)
      _exit(29);
    check(child > 0, "fork signalfd child");
    if (child > 0) {
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      int ready = poll(&pfd, 1, 3000);
      check(ready == 1, "blocked SIGCHLD wakes signalfd poll");
      struct signalfd_siginfo info = {0};
      ssize_t bytes = read(fd, &info, sizeof(info));
      check(bytes == sizeof(info) && info.ssi_signo == SIGCHLD &&
                info.ssi_pid == (uint32_t)child,
            "signalfd identifies the exited child");
      int status = 0;
      check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 29,
            "signalfd notification preserves waitable status");
    }
    close(fd);
  }
  check(sigprocmask(SIG_SETMASK, &saved_mask, NULL) == 0,
        "restore signal mask after signalfd");
}

/* ------------------------------------------------------------------------ */
/* 4. nobody asked, nobody is woken                                          */
/* ------------------------------------------------------------------------ */

static void test_no_spurious_wakeup(void) {
  printf("== an ignored SIGCHLD is not queued\n");

  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &dfl, NULL);

  int sync_pipe[2];
  if (pipe(sync_pipe) != 0) {
    check(0, "pipe()");
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    ssize_t ignored = write(sync_pipe[1], "x", 1);
    (void)ignored;
    close(sync_pipe[0]);
    close(sync_pipe[1]);
    _exit(0);
  }
  check(child > 0, "fork()");
  if (child <= 0) {
    close(sync_pipe[0]);
    close(sync_pipe[1]);
    return;
  }
  char c = 0;
  ssize_t ignored = read(sync_pipe[0], &c, 1); /* child is on its way out */
  (void)ignored;
  close(sync_pipe[0]);
  close(sync_pipe[1]);

  /* An unrelated, never-written pipe: poll() can only report EINTR if a signal
     is pending for us, which a default-disposition SIGCHLD must never be. */
  int quiet_pipe[2];
  if (pipe(quiet_pipe) != 0) {
    check(0, "pipe()");
    return;
  }
  struct pollfd pfd = {.fd = quiet_pipe[0], .events = POLLIN, .revents = 0};
  int intr = 0, timed_out = 0;
  for (int i = 0; i < 40; i++) { /* ~2s, long enough for the exit to land */
    errno = 0;
    int r = poll(&pfd, 1, 50);
    if (r == -1 && errno == EINTR) {
      intr++;
      break;
    }
    if (r == 0)
      timed_out++;
  }
  check(intr == 0 && timed_out > 0, "no EINTR from a child exit nobody handles");
  close(quiet_pipe[0]);
  close(quiet_pipe[1]);

  int status = 0;
  waitpid(child, &status, __WALL); /* reclaim, may already be gone */
}

/* ------------------------------------------------------------------------ */
/* 5. exec must drop the write end of a pipe made with pipe2(O_CLOEXEC)      */
/* ------------------------------------------------------------------------ */

/* This is how QProcess learns the child got through execve(): the parent only
 * polls the read end of a pipe for POLLIN and waits for the end-of-file that
 * exec is supposed to produce by closing the write end.  pipe2()'s O_CLOEXEC
 * used to be ignored, so the freshly exec'd shell kept the write end open, no
 * end-of-file ever arrived, waitForStarted() ran out its 30 seconds, and
 * Konsole reported "Could not start program '/bin/bash'" above a bash that was
 * sitting at a prompt the whole time. */
static void test_exec_closes_started_pipe(void) {
  printf("== exec drops the write end of a pipe2(O_CLOEXEC) pair\n");

  int raw[2];
  if (pipe(raw) == 0) {
    /* Must stay inheritable, or shells lose the pipes they mean to pass on. */
    check((fcntl(raw[1], F_GETFD) & FD_CLOEXEC) == 0, "pipe() keeps FD_CLOEXEC clear");
    close(raw[0]);
    close(raw[1]);
  } else {
    check(0, "pipe()");
  }

  int fds[2];
  check(pipe2(fds, O_CLOEXEC) == 0, "pipe2(O_CLOEXEC)");
  check((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0, "read end is marked close-on-exec");
  check((fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0, "write end is marked close-on-exec");

  pid_t child = fork();
  if (child == 0) {
    close(fds[0]);
    /* Stands in for the shell: it outlives the wait window, so an end-of-file
       can only come from exec closing the descriptor, not from the child dying. */
    execl("/bin/sh", "sh", "-c", "sleep 5", (char *)NULL);
    _exit(127);
  }
  check(child > 0, "fork()");
  if (child <= 0)
    return;
  close(fds[1]); /* the parent never writes; only exec can close the far end */

  struct pollfd pfd = {.fd = fds[0], .events = POLLIN, .revents = 0};
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int ready = poll(&pfd, 1, 3000); /* QProcess::waitForStarted() uses 30000 */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long waited_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                   (t1.tv_nsec - t0.tv_nsec) / 1000000;

  check(ready == 1, "poll(read end, POLLIN) wakes when the child execs");
  check(ready == 1 && waited_ms < 2500, "the end-of-file comes from exec, not from exit");
  char byte = 0;
  check(ready == 1 && read(fds[0], &byte, 1) == 0, "read() reports end of file");
  close(fds[0]);

  int status = 0;
  kill(child, SIGKILL);
  waitpid(child, &status, __WALL);
}

int main(void) {
  printf("--- test_child_notify ---\n");
  test_no_spurious_wakeup();
  test_sigchld_death_pipe();
  test_clone_pidfd();
  test_pidfd_only();
  test_desktop_delayed_wait();
  test_blocked_sigchld_signalfd();
  test_exec_closes_started_pipe();
  printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
