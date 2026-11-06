/* Desktop latency workloads. GPL-2.0. */
#define _GNU_SOURCE
#include "desktop.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int bench_errors;
uint64_t bench_now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void bench_fail(const char *name) {
  printf("RESULT name=%s status=ERROR errno=%d message=%s\n", name, errno,
         strerror(errno));
  bench_errors++;
}
void bench_skip(const char *name, const char *reason) {
  printf("RESULT name=%s status=SKIP reason=%s\n", name, reason);
}
static int compare(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
static size_t percentile(size_t n, unsigned p) {
  return (n * p + 99) / 100 - 1;
}
void bench_stats(const char *name, uint64_t *v, size_t n) {
  if (!n) {
    bench_skip(name, "no_samples");
    return;
  }
  long double sum = 0;
  size_t over16 = 0, over50 = 0, over100 = 0;
  for (size_t i = 0; i < n; i++) {
    sum += v[i];
    over16 += v[i] > 16666667;
    over50 += v[i] > 50000000;
    over100 += v[i] > 100000000;
    if (getenv("AVORY_BENCH_RAW"))
      printf("SAMPLE name=%s index=%zu ns=%" PRIu64 "\n", name, i, v[i]);
  }
  qsort(v, n, sizeof(*v), compare);
  printf(
      "LATENCY name=%s unit=us count=%zu min=%.3f mean=%.3Lf p50=%.3f p90=%.3f "
      "p95=%.3f p99=%.3f max=%.3f over_16ms=%zu over_50ms=%zu over_100ms=%zu\n",
      name, n, v[0] / 1e3, sum / n / 1e3L, v[percentile(n, 50)] / 1e3,
      v[percentile(n, 90)] / 1e3, v[percentile(n, 95)] / 1e3,
      v[percentile(n, 99)] / 1e3, v[n - 1] / 1e3, over16, over50, over100);
}
int bench_transfer(int fd, void *buf, size_t bytes, int writing) {
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = writing ? write(fd, (char *)buf + done, bytes - done)
                        : read(fd, (char *)buf + done, bytes - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      if (!n)
        errno = EPIPE;
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}
int bench_wait(int pid) {
  int status;
  pid_t p;
  do {
    p = waitpid(pid, &status, 0);
  } while (p < 0 && errno == EINTR);
  if (p != pid) {
    printf("PROCESS expected_pid=%d wait_result=%d errno=%d\n", pid, p, errno);
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    printf("PROCESS pid=%d exit_status=0x%x\n", pid, status);
    errno = ECHILD;
    return -1;
  }
  return 0;
}
static void ipc(unsigned samples, int sockets, int loaded) {
  int to[2], from[2];
  if (sockets) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, to)) {
      bench_fail("socketpair");
      return;
    }
    from[0] = to[0];
    from[1] = to[1];
  } else if (pipe(to)) {
    bench_fail("pipe");
    return;
  } else if (pipe(from)) {
    close(to[0]);
    close(to[1]);
    bench_fail("pipe");
    return;
  }
  fflush(NULL);
  pid_t child = fork();
  if (child < 0) {
    bench_fail("ipc_fork");
    goto close_fds;
  }
  if (!child) {
    int input = sockets ? to[1] : to[0];
    close(sockets ? to[0] : to[1]);
    if (!sockets)
      close(from[0]);
    uint64_t payload[8];
    for (unsigned i = 0; i < samples + 1; i++) {
      if (bench_transfer(input, payload, sizeof(payload), 0) || payload[0] != i)
        _exit(2);
      payload[1] ^= 0xa5a5;
      if (bench_transfer(from[1], payload, sizeof(payload), 1))
        _exit(3);
    }
    _exit(0);
  }
  close(to[sockets ? 1 : 0]);
  if (!sockets)
    close(from[1]);
  int output = sockets ? to[0] : to[1];
  uint64_t *times = calloc(samples, sizeof(*times));
  if (!times) {
    kill(child, SIGKILL);
    bench_wait(child);
    bench_fail("ipc_alloc");
    goto parent_close;
  }
  int failed = 0;
  for (unsigned i = 0; i < samples + 1; i++) {
    uint64_t payload[8] = {i, 0x12345678, 2, 3, 4, 5, 6, 7},
             start = bench_now();
    if (bench_transfer(output, payload, sizeof(payload), 1) ||
        bench_transfer(from[0], payload, sizeof(payload), 0) ||
        payload[0] != i || payload[1] != (0x12345678 ^ 0xa5a5) ||
        payload[7] != 7) {
      failed = 1;
      break;
    }
    if (i)
      times[i - 1] = bench_now() - start;
  }
  if (failed)
    kill(child, SIGKILL);
  if (bench_wait(child) || failed)
    bench_fail("ipc_roundtrip");
  else
    bench_stats(sockets ? (loaded ? "unix_socket_64B_roundtrip_loaded"
                                  : "unix_socket_64B_roundtrip_idle")
                        : (loaded ? "pipe_64B_roundtrip_loaded"
                                  : "pipe_64B_roundtrip_idle"),
                times, samples);
  free(times);
parent_close:
  close(output);
  if (!sockets)
    close(from[0]);
  return;
close_fds:
  close(to[0]);
  close(to[1]);
  if (!sockets) {
    close(from[0]);
    close(from[1]);
  }
}
void bench_wakeups(unsigned samples, const char *label) {
  uint64_t *times = calloc(samples, sizeof(*times));
  if (!times) {
    bench_fail("wake_alloc");
    return;
  }
  const uint64_t period = 5000000;
  uint64_t deadline = bench_now() + period;
  unsigned missed = 0;
  for (unsigned i = 0; i < samples; i++) {
    uint64_t t = bench_now();
    while (t < deadline) {
      uint64_t left = deadline - t;
      struct timespec nap = {left / 1000000000, left % 1000000000};
      if (nanosleep(&nap, NULL) && errno != EINTR) {
        bench_fail("nanosleep");
        free(times);
        return;
      }
      t = bench_now();
    }
    times[i] = t - deadline;
    uint64_t skipped = times[i] / period;
    missed += (unsigned)skipped;
    deadline += (skipped + 1) * period;
  }
  printf("SCHED name=%s period_us=5000 missed_periods=%u\n", label, missed);
  bench_stats(label, times, samples);
  free(times);
}
int bench_scheduling(unsigned samples, unsigned workers) {
  cpu_set_t saved, set;
  int affinity = 0;
  if (!sched_getaffinity(0, sizeof(saved), &saved)) {
    CPU_ZERO(&set);
    for (unsigned i = 0; i < CPU_SETSIZE; i++)
      if (CPU_ISSET(i, &saved)) {
        CPU_SET(i, &set);
        break;
      }
    affinity = !sched_setaffinity(0, sizeof(set), &set);
  }
  printf("SCHED affinity=%s workers=%u "
         "workload=integer_and_8MiB_memory_per_worker\n",
         affinity ? "one_shared_cpu" : "unavailable", workers);
  bench_wakeups(samples, "wake_idle");
  ipc(samples, 0, 0);
  ipc(samples, 1, 0);
  pid_t children[16];
  unsigned started = 0;
  for (unsigned i = 0; i < workers; i++) {
    int ready[2];
    if (pipe(ready)) {
      bench_fail("load_pipe");
      break;
    }
    fflush(NULL);
    pid_t p = fork();
    if (p < 0) {
      close(ready[0]);
      close(ready[1]);
      bench_fail("load_fork");
      break;
    }
    if (!p) {
      close(ready[0]);
      size_t size = 8 * 1024 * 1024;
      volatile uint64_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (data == MAP_FAILED)
        _exit(2);
      for (size_t j = 0; j < size / 8; j++)
        data[j] = j;
      char ok = 1;
      if (bench_transfer(ready[1], &ok, 1, 1))
        _exit(3);
      close(ready[1]);
      uint64_t state = i + 1;
      for (;;) {
        state = state * 6364136223846793005ULL + 1;
        data[(state >> 16) % (size / 8)] ^= state;
      }
    }
    close(ready[1]);
    children[started++] = p;
    char ok;
    int bad = bench_transfer(ready[0], &ok, 1, 0);
    close(ready[0]);
    if (bad) {
      bench_fail("load_ready");
      break;
    }
  }
  if (started == workers) {
    bench_wakeups(samples, "wake_cpu_memory_load");
    puts("SCHED phase=loaded_ipc");
    ipc(samples, 0, 1);
    ipc(samples, 1, 1);
  }
  for (unsigned i = 0; i < started; i++)
    kill(children[i], SIGTERM);
  for (unsigned i = 0; i < started; i++) {
    int status;
    while (waitpid(children[i], &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (affinity && sched_setaffinity(0, sizeof(saved), &saved))
    bench_fail("restore_affinity");
  return 0;
}
static unsigned number(const char *s, unsigned max) {
  char *end;
  errno = 0;
  unsigned long n = strtoul(s, &end, 10);
  if (errno || !*s || *end || n < 1 || n > max) {
    fprintf(stderr, "Expected 1..%u: %s\n", max, s);
    exit(2);
  }
  return n;
}
int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc == 3 && !strcmp(argv[1], "--app-child"))
    return bench_app_child(number(argv[2], INT_MAX));
  const char *mode = argc > 1 ? argv[1] : "all";
  if (!strcmp(mode, "--help")) {
    puts("bench_desktop all|scheduling|storage|apps|graphics [samples=120] "
         "[workdir=/tmp] [workers=2]\n"
         "bench_desktop browser [runs=3] [workdir=/tmp]\nbench_desktop command "
         "RUNS -- PROGRAM [ARGS...]\n"
         "AVORY_BENCH_RAW=1 emits each sample. Browser and compatibility "
         "workloads use private profiles.");
    return 0;
  }
  int all = !strcmp(mode, "all");
  if (!all && strcmp(mode, "scheduling") && strcmp(mode, "storage") &&
      strcmp(mode, "apps") && strcmp(mode, "graphics") &&
      strcmp(mode, "browser") && strcmp(mode, "command")) {
    fprintf(stderr, "Unknown desktop mode: %s\n", mode);
    return 2;
  }
  unsigned n = argc > 2                   ? number(argv[2], 10000)
               : !strcmp(mode, "browser") ? 3
                                          : 120;
  const char *base = argc > 3 ? argv[3] : "/tmp";
  if (!strcmp(mode, "command")) {
    if (argc < 5 || strcmp(argv[3], "--")) {
      fprintf(stderr, "command RUNS -- PROGRAM [ARGS...]\n");
      return 2;
    }
    bench_command(n, argv + 4);
    return bench_errors ? 1 : 0;
  }
  if (argc > 5) {
    fprintf(stderr, "Too many arguments\n");
    return 2;
  }
  unsigned workers = argc > 4 ? number(argv[4], 16) : 2;
  struct utsname u;
  if (!uname(&u))
    printf("ENV suite=desktop_v1 os=%s release=%s machine=%s cpus=%ld "
           "samples=%u workdir=%s display=%s\n",
           u.sysname, u.release, u.machine, sysconf(_SC_NPROCESSORS_ONLN), n,
           base, getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
  char self[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (len < 0 || (size_t)len >= sizeof(self) - 1) {
    bench_fail("self_path");
    return 1;
  }
  self[len] = 0;
  if (all || !strcmp(mode, "scheduling"))
    bench_scheduling(n, workers);
  if (all || !strcmp(mode, "storage"))
    bench_storage(n, base);
  if (all || !strcmp(mode, "apps"))
    bench_apps(n, self);
  if (all || !strcmp(mode, "graphics"))
    bench_graphics(n);
  if (all || !strcmp(mode, "browser"))
    bench_browser(all ? 3 : n, base);
  printf("SUITE name=desktop_v1 status=%s errors=%d\n",
         bench_errors ? "ERROR" : "COMPLETE", bench_errors);
  return bench_errors ? 1 : 0;
}
