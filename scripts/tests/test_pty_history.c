/* Native PTY ABI and real glibc Bash/Readline regression, run by
 * test_pty_history_boot.py in a private guest. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Do not use libc's termios: legacy Linux ioctl payloads have 19 cc bytes. */
struct wire_termios {
  uint32_t iflag, oflag, cflag, lflag;
  unsigned char line, cc[19];
};
struct wire_termios2 {
  struct wire_termios base;
  uint32_t ispeed, ospeed;
};
#define GET2 0x802c542aUL
#define SET2 0x402c542bUL
#define SETW2 0x402c542cUL
#define SETF2 0x402c542dUL
_Static_assert(sizeof(struct wire_termios) == 36, "legacy Linux ABI");
_Static_assert(sizeof(struct wire_termios2) == 44, "extended Linux ABI");
static int failures;
#define CHECK(ok, what) do { if (!(ok)) { \
  printf("FAIL: %s (line %d, errno %d)\n", what, __LINE__, errno); failures++; \
} } while (0)

static int pair(int *slave) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  int unlock = 0;
  if (master < 0 || ioctl(master, TIOCSPTLCK, &unlock)) return -1;
  char *name = ptsname(master);
  *slave = name ? open(name, O_RDWR | O_NOCTTY) : -1;
  if (*slave < 0) { close(master); return -1; }
  return master;
}

static void abi_test(void) {
  int s, other_s;
  int m = pair(&s), other_m = pair(&other_s);
  CHECK(m >= 0 && other_m >= 0, "open two independent PTYs");
  if (m < 0 || other_m < 0) return;
  CHECK(!fcntl(s, F_SETFL, O_NONBLOCK), "nonblocking ABI probe reads");
  struct wire_termios2 original, other;
  CHECK(!ioctl(s, GET2, &original), "TCGETS2 slave");
  CHECK(!ioctl(other_m, GET2, &other), "TCGETS2 second master");
  unsigned long setters[] = {SET2, SETW2, SETF2, TCSETS, TCSETSW, TCSETSF};
  for (int endpoint = 0; endpoint < 2; endpoint++) {
    int fd = endpoint ? s : m, peer = endpoint ? m : s;
    for (unsigned i = 0; i < sizeof(setters)/sizeof(*setters); i++) {
      struct wire_termios2 value = original;
      value.base.lflag &= ~(ICANON | ECHO);
      value.base.iflag &= ~ICRNL;
      value.base.cc[VMIN] = 1;
      value.base.cc[VTIME] = 0;
      value.ispeed = value.ospeed = 38400;
      CHECK(!ioctl(fd, setters[i], &value), "set attributes on either endpoint");
      struct { struct wire_termios value; uint32_t guard; } legacy;
      memset(&legacy, 0xa5, sizeof(legacy));
      CHECK(!ioctl(peer, TCGETS, &legacy.value), "read legacy attributes on peer");
      CHECK(!memcmp(&legacy.value, &value.base, sizeof(value.base)),
            "extended and legacy ioctls share PTY state");
      CHECK(legacy.guard == 0xa5a5a5a5, "TCGETS writes exactly 36 bytes");
      struct { struct wire_termios2 value; uint32_t guard; } extended;
      memset(&extended, 0xa5, sizeof(extended));
      CHECK(!ioctl(peer, GET2, &extended.value), "read extended attributes on peer");
      CHECK(!memcmp(&extended.value.base, &value.base, sizeof(value.base)),
            "TCGETS2 reports this pair's flags");
      CHECK(extended.value.ispeed == 38400 && extended.value.ospeed == 38400,
            "extended speeds survive roundtrip and legacy setters");
      CHECK(extended.guard == 0xa5a5a5a5, "TCGETS2 writes exactly 44 bytes");
      struct wire_termios2 unchanged;
      CHECK(!ioctl(other_s, GET2, &unchanged) &&
            !memcmp(&unchanged, &other, sizeof(other)), "other PTY is isolated");
      CHECK(write(m, "\033[A", 3) == 3, "write history key");
      struct pollfd p = {s, POLLIN, 0}; char bytes[3];
      CHECK(poll(&p, 1, 1000) == 1 && read(s, bytes, 3) == 3 &&
            !memcmp(bytes, "\033[A", 3), "raw key reaches slave before Enter");
      p.fd = m;
      CHECK(poll(&p, 1, 20) == 0, "raw key is not echoed to terminal");
    }
    errno = 0;
    CHECK(ioctl(fd, GET2, NULL) == -1 && errno == EFAULT, "GET2 null pointer rejected");
    errno = 0;
    CHECK(ioctl(fd, SET2, NULL) == -1 && errno == EFAULT, "SET2 null pointer rejected");
  }
  close(s); close(m); close(other_s); close(other_m);
  puts("PTY_HISTORY_BOOT: ABI matrix finished");
}

static void collect(int fd, char *out, size_t size) {
  size_t used = 0;
  struct pollfd p = {fd, POLLIN, 0};
  while (used + 1 < size && poll(&p, 1, 600) > 0) {
    ssize_t n = read(fd, out + used, size - used - 1);
    if (n <= 0) break;
    used += (size_t)n;
  }
  out[used] = 0;
}

static void bash_test(int colored) {
  int s, m = pair(&s);
  CHECK(m >= 0, "open Bash PTY"); if (m < 0) return;
  struct winsize ws = {24, 132, 0, 0};
  CHECK(!ioctl(m, TIOCSWINSZ, &ws), "set terminal dimensions");
  pid_t pid = fork();
  if (pid == 0) {
    close(m);
    if (setsid() < 0 || ioctl(s, TIOCSCTTY, 0) < 0) _exit(120);
    dup2(s, 0); dup2(s, 1); dup2(s, 2); if (s > 2) close(s);
    setenv("TERM", "xterm-256color", 1);
    setenv("HOME", "/root", 1);
    setenv("HISTFILE", "/dev/null", 1);
    setenv("PS1", colored ? "\\[\033[32m\\]PTY> \\[\033[0m\\]" : "PTY> ", 1);
    execl("/usr/bin/bash", "bash", "--noprofile", "--norc", "-i", NULL);
    _exit(127);
  }
  close(s); CHECK(pid > 0, "spawn Bash"); if (pid < 0) {close(m); return;}
  char out[8192];
  collect(m, out, sizeof(out));
  CHECK(strstr(out, "PTY> ") != NULL, "Bash prompt appears");
  struct wire_termios state;
  CHECK(!ioctl(m, TCGETS, &state) && !(state.lflag & (ICANON | ECHO)),
        "glibc Readline disables canonical mode and echo on the PTY");
  const char *command = "echo HISTORY_EXECUTED\r";
  CHECK(write(m, command, strlen(command)) == (ssize_t)strlen(command), "send command");
  collect(m, out, sizeof(out));
  CHECK(strstr(out, "\rHISTORY_EXECUTED\r\n") != NULL, "initial command executes");
  CHECK(write(m, "\033[A", 3) == 3, "send Up");
  collect(m, out, sizeof(out));
  CHECK(strstr(out, "echo HISTORY_EXECUTED") != NULL, "Up displays history before Enter");
  CHECK(strstr(out, "\033[A") == NULL, "Up is not echoed as cursor movement");
  CHECK(write(m, "\033[B", 3) == 3, "send Down");
  collect(m, out, sizeof(out));
  CHECK(*out && !strstr(out, "\033[B"), "Down redraws the empty command");
  ws.ws_col = 80;
  CHECK(!ioctl(m, TIOCSWINSZ, &ws), "resize terminal");
  collect(m, out, sizeof(out));
  CHECK(write(m, "\033[A", 3) == 3, "recall after resize");
  collect(m, out, sizeof(out));
  CHECK(strstr(out, "echo HISTORY_EXECUTED") != NULL, "history redraws after resize");
  CHECK(write(m, "\r", 1) == 1, "execute recalled command");
  collect(m, out, sizeof(out));
  CHECK(strstr(out, "HISTORY_EXECUTED\r\n") != NULL, "recalled command executes");
  CHECK(write(m, "exit\r", 5) == 5, "exit Bash");
  collect(m, out, sizeof(out));
  int status = -1;
  pid_t waited;
  do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
  printf("PTY_HISTORY_BOOT: Bash pid=%d waited=%d status=%d\n", pid, waited, status);
  CHECK(waited == pid && status == 0, "Bash exits cleanly");
  close(m);
  printf("PTY_HISTORY_BOOT: %s prompt finished\n", colored ? "colored" : "plain");
}

static void child_exited(int sig) { (void)sig; }

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  puts("PTY_HISTORY_BOOT: START");
  /* A real handler keeps PID 1's children waitable in this kernel. */
  signal(SIGCHLD, child_exited);
  abi_test(); bash_test(0); bash_test(1);
  printf("PTY_HISTORY_BOOT: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
  fflush(NULL);
  reboot(RB_POWER_OFF);
  return failures ? 1 : 0;
}
