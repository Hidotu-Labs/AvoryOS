/* test_heap_corrupt - hammer the allocation pattern that broke avory-installer
 * ("double free or corruption (out)" after copying ~56k files) and verify
 * every byte the program owns.
 *
 *   - calloc()/free() of tables doubling up to 16 MiB (mmap/munmap), checked
 *     for zero on arrival: glibc never clears mmapped calloc memory;
 *   - a pointer array grown with realloc() in 4096-entry steps (mremap);
 *   - many small strdup()s;
 *   - fork() children that scribble on their CoW copy and _exit();
 *   - optional sysfs uevent writes ("change"), which run the kernel's deep,
 *     recursive sysfs_find_devpath() on this process's kernel stack.
 *
 * Any mismatch is printed with its address and value; a value in the kernel
 * half (0xffff8...) means a kernel write landed in a user page.
 *
 * Usage: test_heap_corrupt [iterations] [--uevent]
 * Compare runs with KSM on and off: echo 0 > /proc/sys/vm/ksm_run
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TABLE_MAX_BYTES (16u << 20)
#define ARRAY_STEP 4096
#define ARRAY_MAX (ARRAY_STEP * 16) /* 512 KiB of pointers */

static unsigned long failures;

static void report(const char *what, const void *addr, uint64_t got,
                   uint64_t want) {
  failures++;
  if (failures > 32)
    return;
  printf("FAIL %s at %p: got %#llx want %#llx%s\n", what, addr,
         (unsigned long long)got, (unsigned long long)want,
         got >= 0xffff800000000000ULL ? "  <-- KERNEL POINTER in user memory"
                                      : "");
  fflush(stdout);
}

static uint64_t tag(uint64_t seed, size_t i) {
  return (seed * 0x9E3779B97F4A7C15ULL) ^ (i * 0xBF58476D1CE4E5B9ULL) ^ 1;
}

static void check_words(const char *what, const uint64_t *w, size_t n,
                        uint64_t seed, bool zero) {
  for (size_t i = 0; i < n; i++) {
    uint64_t want = zero ? 0 : tag(seed, i);
    if (w[i] != want) {
      report(what, &w[i], w[i], want);
      return;
    }
  }
}

static void uevent_poke(void) {
  DIR *d = opendir("/sys/class/block");
  if (!d)
    return;
  struct dirent *e;
  char path[512];
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
      continue;
    (void)!write(fd, "change", 6);
    close(fd);
  }
  closedir(d);
}

/* A forked child writes over its copy of everything; the parent re-verifies
 * afterwards, so a CoW break that wrote the shared frame shows up there. */
static void fork_scribble(uint64_t *a, size_t an, char **strs, size_t sn) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return;
  }
  if (pid == 0) {
    for (size_t i = 0; i < an; i++)
      a[i] = ~a[i];
    for (size_t i = 0; i < sn; i++)
      if (strs[i])
        memset(strs[i], 'X', strlen(strs[i]));
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
}

int main(int argc, char **argv) {
  int iterations = 50;
  bool uevents = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--uevent"))
      uevents = true;
    else
      iterations = atoi(argv[i]);
  }

  char ksm[8] = "?";
  int kfd = open("/proc/sys/vm/ksm_run", O_RDONLY);
  if (kfd >= 0) {
    ssize_t n = read(kfd, ksm, sizeof(ksm) - 1);
    ksm[n > 0 ? n : 0] = 0;
    ksm[strcspn(ksm, "\n")] = 0;
    close(kfd);
  }
  printf("test_heap_corrupt: %d iterations, ksm_run=%s, uevents=%s\n",
         iterations, ksm, uevents ? "on" : "off");
  fflush(stdout);

  for (int it = 0; it < iterations; it++) {
    uint64_t seed = (uint64_t)it + 1;

    /* 1. Tables doubling to 16 MiB: zero on arrival, pattern survives. */
    for (size_t bytes = 64 << 10; bytes <= TABLE_MAX_BYTES; bytes <<= 1) {
      size_t n = bytes / sizeof(uint64_t);
      uint64_t *t = calloc(n, sizeof(uint64_t));
      if (!t) {
        printf("calloc(%zu) failed\n", bytes);
        return 2;
      }
      check_words("calloc not zero", t, n, 0, true);
      for (size_t i = 0; i < n; i++)
        t[i] = tag(seed + bytes, i);
      if (uevents)
        uevent_poke();
      check_words("table", t, n, seed + bytes, false);
      free(t);
    }

    /* 2. realloc-grown pointer array of strdups, as in packages.c. */
    char **strs = NULL;
    size_t count = 0, cap = 0;
    char buf[96];
    for (size_t i = 0; i < ARRAY_MAX; i++) {
      if (count == cap) {
        cap += ARRAY_STEP;
        char **grown = realloc(strs, cap * sizeof(*strs));
        if (!grown) {
          printf("realloc(%zu) failed\n", cap * sizeof(*strs));
          return 2;
        }
        strs = grown;
        for (size_t j = 0; j < count; j++) {
          snprintf(buf, sizeof(buf), "usr/share/pkg/%zu/file-%llx", j,
                   (unsigned long long)tag(seed, j));
          if (!strs[j] || strcmp(strs[j], buf)) {
            report("string after realloc", &strs[j], (uint64_t)(uintptr_t)strs[j], 0);
            break;
          }
        }
      }
      snprintf(buf, sizeof(buf), "usr/share/pkg/%zu/file-%llx", i,
               (unsigned long long)tag(seed, i));
      strs[count++] = strdup(buf);
    }

    /* 3. fork with everything live, then verify the parent's copy. */
    uint64_t *probe = malloc(1 << 20);
    size_t pn = (1 << 20) / sizeof(uint64_t);
    for (size_t i = 0; i < pn; i++)
      probe[i] = tag(seed * 7, i);
    fork_scribble(probe, pn, strs, count);
    check_words("probe after fork", probe, pn, seed * 7, false);
    for (size_t j = 0; j < count; j++) {
      snprintf(buf, sizeof(buf), "usr/share/pkg/%zu/file-%llx", j,
               (unsigned long long)tag(seed, j));
      if (strcmp(strs[j], buf)) {
        report("string after fork", strs[j], (uint64_t)(unsigned char)strs[j][0],
               (uint64_t)(unsigned char)buf[0]);
        break;
      }
    }

    /* 4. Tear it all down, as the installer's free pass did. */
    free(probe);
    for (size_t j = 0; j < count; j++)
      free(strs[j]);
    free(strs);

    if ((it + 1) % 10 == 0 || failures) {
      printf("iteration %d/%d: %lu failures\n", it + 1, iterations, failures);
      fflush(stdout);
    }
  }

  printf("test_heap_corrupt: %s (%lu failures)\n", failures ? "FAIL" : "PASS",
         failures);
  return failures ? 1 : 0;
}
