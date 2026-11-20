/*
 * test_syscall_memory.c - Per-syscall process memory footprint benchmark.
 *
 * Reuses the 288 registered syscall cases from syscall_tests.h. Each sample
 * runs in a fresh child process so one case's mappings, credentials, or other
 * process state do not affect the next. The reported RSS/VM deltas are
 * post-call snapshots of the test process; very short-lived allocations and
 * kernel-only object memory are not visible in these per-process counters.
 *
 * Usage: test_syscall_memory [samples_per_syscall] (default 1, maximum 100)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "syscall_tests.h"

#define DEFAULT_SAMPLES 1
#define MAX_SAMPLES 100
#define STATUS_BUFFER_SIZE 4096

typedef struct {
  int64_t rss_kib;
  int64_t vm_kib;
  int64_t kheap_bytes;
  int64_t pmm_bytes;
} memory_snapshot_t;

typedef struct {
  memory_snapshot_t before;
  memory_snapshot_t after;
  int test_ok;
  int snapshot_ok;
} worker_result_t;

typedef struct {
  int64_t rss_delta_sum;
  int64_t rss_delta_max;
  int64_t vm_delta_sum;
  int64_t vm_delta_max;
  int64_t kheap_delta_sum;
  int64_t kheap_delta_max;
  int64_t pmm_delta_sum;
  int64_t pmm_delta_max;
  unsigned samples;
  unsigned pass_count;
  unsigned error_count;
} syscall_memory_result_t;

static int parse_status_value(const char *buffer, const char *key,
                              int64_t *value_out) {
  size_t key_len = strlen(key);
  const char *line = buffer;

  while (*line) {
    if (!strncmp(line, key, key_len)) {
      char *end = NULL;
      long long value = strtoll(line + key_len, &end, 10);
      if (end == line + key_len)
        return 0;
      *value_out = (int64_t)value;
      return 1;
    }
    const char *next = strchr(line, '\n');
    if (!next)
      break;
    line = next + 1;
  }
  return 0;
}

static int read_memory_snapshot(memory_snapshot_t *snapshot) {
  char buffer[STATUS_BUFFER_SIZE];
  int fd = open("/proc/self/status", O_RDONLY);
  if (fd < 0)
    return 0;

  ssize_t used = 0;
  while ((size_t)used < sizeof(buffer) - 1) {
    ssize_t n = read(fd, buffer + used, sizeof(buffer) - 1 - (size_t)used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    used += n;
  }
  close(fd);
  buffer[used] = '\0';

  snapshot->rss_kib = -1;
  snapshot->vm_kib = -1;
  snapshot->kheap_bytes = 0;
  snapshot->pmm_bytes = 0;
  int ok = parse_status_value(buffer, "VmRSS:", &snapshot->rss_kib) &&
           parse_status_value(buffer, "VmSize:", &snapshot->vm_kib);
  (void)parse_status_value(buffer, "KHeapAlloc:", &snapshot->kheap_bytes);
  (void)parse_status_value(buffer, "PmmAlloc:", &snapshot->pmm_bytes);
  return ok;
}

static int invoke_registered_case(const struct single_syscall_test_entry *entry) {
  /* Keep these system-wide operations harmless while still measuring their
   * syscall dispatch and validation paths. The shared registry's mount case
   * requests a real tmpfs mount, and its setdomainname case changes UTS state. */
  if (entry->num == 165) { /* mount: missing source + invalid fs, no mount */
    long ret = syscall(165, NULL, "/tmp", "avory_bench_invalid_fs", 0, NULL);
    return ret < 0;
  }
  if (entry->num == 171) { /* setdomainname: invalid pointer, no state change */
    long ret = syscall(171, NULL, 5);
    return ret < 0;
  }
  return entry->fn();
}

static int write_result(int fd, const worker_result_t *result) {
  const char *p = (const char *)result;
  size_t remaining = sizeof(*result);
  while (remaining) {
    ssize_t n = write(fd, p, remaining);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += n;
    remaining -= (size_t)n;
  }
  return 1;
}

static int read_result(int fd, worker_result_t *result) {
  char *p = (char *)result;
  size_t remaining = sizeof(*result);
  while (remaining) {
    ssize_t n = read(fd, p, remaining);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += n;
    remaining -= (size_t)n;
  }
  return 1;
}

static void warmup_libc_helpers(void) {
  /* Fault in common libc helpers (syscall(), errno, snprintf(), malloc()/free())
   * inside the child before taking the `before` snapshot. fork_can_refault()
   * drops reconstructible file-backed pages (such as libc.so.6) on fork(), so
   * warming up in the parent does not keep them mapped in the child. */
  volatile long pid = syscall(39); /* getpid */
  errno = 0;
  volatile int err = errno;
  char buf[64];
  volatile int n = snprintf(buf, sizeof(buf), "%ld %d", pid, err);
  (void)n;
  volatile char ch = buf[0];
  (void)ch;
  void * volatile p = malloc(64);
  if (p) {
    memset(p, 0, 64);
    free(p);
  }
}

static int run_sample(const struct single_syscall_test_entry *entry,
                      worker_result_t *result) {
  int pipefd[2];
  if (pipe(pipefd) < 0)
    return 0;

  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
  }

  if (pid == 0) {
    close(pipefd[0]);
    warmup_libc_helpers();
    worker_result_t child_result;
    memset(&child_result, 0, sizeof(child_result));
    /* Warm up read_memory_snapshot() (open/read/close/strlen/strncmp/strtoll/strchr)
     * so parse_status_value() does not fault libc pages after `before` is read. */
    memory_snapshot_t warmup_snap;
    (void)read_memory_snapshot(&warmup_snap);
    child_result.snapshot_ok = read_memory_snapshot(&child_result.before);
    child_result.test_ok = invoke_registered_case(entry);
    child_result.snapshot_ok =
        read_memory_snapshot(&child_result.after) && child_result.snapshot_ok;
    int wrote = write_result(pipefd[1], &child_result);
    close(pipefd[1]);
    _exit(wrote && child_result.test_ok ? 0 : 1);
  }

  close(pipefd[1]);
  int received = read_result(pipefd[0], result);
  close(pipefd[0]);

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0 || !received || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return 0;
  return 1;
}

static unsigned parse_samples(int argc, char **argv) {
  if (argc < 2)
    return DEFAULT_SAMPLES;
  if (argc > 2) {
    fprintf(stderr, "Usage: %s [samples_per_syscall]\n", argv[0]);
    exit(2);
  }

  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(argv[1], &end, 10);
  if (errno || !argv[1][0] || !end || *end || value < 1 ||
      value > MAX_SAMPLES) {
    fprintf(stderr, "Expected samples in range 1..%u: %s\n", MAX_SAMPLES,
            argv[1]);
    exit(2);
  }
  return (unsigned)value;
}

static void add_sample(syscall_memory_result_t *result,
                       const worker_result_t *sample, int worker_ok) {
  if (worker_ok)
    result->pass_count++;
  else
    result->error_count++;

  if (!sample->snapshot_ok)
    return;

  int64_t rss_delta = sample->after.rss_kib - sample->before.rss_kib;
  int64_t vm_delta = sample->after.vm_kib - sample->before.vm_kib;
  int64_t kheap_delta = sample->after.kheap_bytes - sample->before.kheap_bytes;
  int64_t pmm_delta = sample->after.pmm_bytes - sample->before.pmm_bytes;
  result->rss_delta_sum += rss_delta;
  result->vm_delta_sum += vm_delta;
  result->kheap_delta_sum += kheap_delta;
  result->pmm_delta_sum += pmm_delta;
  if (result->samples == 0 || rss_delta > result->rss_delta_max)
    result->rss_delta_max = rss_delta;
  if (result->samples == 0 || vm_delta > result->vm_delta_max)
    result->vm_delta_max = vm_delta;
  if (result->samples == 0 || kheap_delta > result->kheap_delta_max)
    result->kheap_delta_max = kheap_delta;
  if (result->samples == 0 || pmm_delta > result->pmm_delta_max)
    result->pmm_delta_max = pmm_delta;
  result->samples++;
}

static void print_delta(char *buffer, size_t size, int64_t sum,
                        int64_t maximum, unsigned samples, const char *unit) {
  if (!samples) {
    snprintf(buffer, size, "N/A");
    return;
  }
  snprintf(buffer, size, "%.1f/%" PRId64 " %s",
           (double)sum / (double)samples, maximum, unit);
}

static void print_report(const syscall_memory_result_t *results,
                         unsigned samples_per_syscall) {
  unsigned pass_total = 0;
  unsigned error_total = 0;
  unsigned measured_total = 0;

  printf("\n========================================================================================================================\n");
  printf("                 AVORYOS SYSCALL MEMORY BENCHMARK (%u SAMPLE(S) EACH)\n",
         samples_per_syscall);
  printf("========================================================================================================================\n");
  printf("%-5s | %-24s | %-9s | %-9s | %-18s | %-18s | %-18s | %-18s\n",
         "No.", "Syscall", "Pass/Err", "Measured", "RSS avg/max Δ",
         "VM avg/max Δ", "KHeap avg/max", "PMM avg/max");
  printf("------+--------------------------+-----------+-----------+--------------------+--------------------+--------------------+--------------------\n");

  for (size_t i = 0; i < TOTAL_REGISTERED_SYSCALL_TESTS; i++) {
    const struct single_syscall_test_entry *entry = &g_all_syscall_tests[i];
    const syscall_memory_result_t *r = &results[i];
    char rss[40], vm[40], kheap[40], pmm[40], pass[24];
    print_delta(rss, sizeof(rss), r->rss_delta_sum, r->rss_delta_max,
                r->samples, "KiB");
    print_delta(vm, sizeof(vm), r->vm_delta_sum, r->vm_delta_max,
                r->samples, "KiB");
    print_delta(kheap, sizeof(kheap), r->kheap_delta_sum, r->kheap_delta_max,
                r->samples, "B");
    print_delta(pmm, sizeof(pmm), r->pmm_delta_sum, r->pmm_delta_max,
                r->samples, "B");
    snprintf(pass, sizeof(pass), "%u/%u", r->pass_count, r->error_count);
    printf("%-5d | %-24s | %-9s | %-9u | %-18s | %-18s | %-18s | %-18s\n",
           entry->num, entry->name, pass, r->samples, rss, vm, kheap, pmm);
    pass_total += r->pass_count;
    error_total += r->error_count;
    measured_total += r->samples;
  }

  printf("================================================================================\n");
  printf("SUMMARY: %d registered syscalls, %u samples each; %u measured, %u passed, %u failed\n",
         TOTAL_REGISTERED_SYSCALL_TESTS, samples_per_syscall, measured_total,
         pass_total, error_total);
  printf("Memory columns show per-process VmRSS/VmSize change after each test case;\n");
  printf("short-lived allocations and kernel-only object memory are not included.\n");
  printf("mount and setdomainname use no-effect validation inputs.\n");
  printf("================================================================================\n\n");
}

int main(int argc, char **argv) {
  /* The included header's standalone runner is only used by test_mem_stress. */
  (void)&run_all_single_syscall_tests;
  unsigned samples_per_syscall = parse_samples(argc, argv);
  size_t entry_count = sizeof(g_all_syscall_tests) / sizeof(g_all_syscall_tests[0]);
  if (entry_count != TOTAL_REGISTERED_SYSCALL_TESTS) {
    fprintf(stderr, "Syscall registry mismatch: table has %zu entries, expected %d\n",
            entry_count, TOTAL_REGISTERED_SYSCALL_TESTS);
    return 2;
  }

  syscall_memory_result_t results[TOTAL_REGISTERED_SYSCALL_TESTS];
  memset(results, 0, sizeof(results));

  printf("Starting syscall memory benchmark for %d registered syscalls (%u sample(s) each)...\n",
         TOTAL_REGISTERED_SYSCALL_TESTS, samples_per_syscall);
  printf("Each sample runs in a separate child process.\n");
  fflush(stdout);

  for (size_t i = 0; i < entry_count; i++) {
    for (unsigned sample_index = 0; sample_index < samples_per_syscall;
         sample_index++) {
      worker_result_t sample;
      memset(&sample, 0, sizeof(sample));
      int worker_ok = run_sample(&g_all_syscall_tests[i], &sample);
      add_sample(&results[i], &sample, worker_ok);
    }
  }

  print_report(results, samples_per_syscall);
  return 0;
}
