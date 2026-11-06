#define _GNU_SOURCE
#include "desktop.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

enum { FILE_BYTES = 32 * 1024 * 1024, BLOCK = 4096, CHUNK = 65536 };
static unsigned char expected[CHUNK];
static int io_at(int fd, void *buf, size_t size, off_t off, int writing) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = writing
                    ? pwrite(fd, (char *)buf + done, size - done, off + done)
                    : pread(fd, (char *)buf + done, size - done, off + done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      if (!n)
        errno = EIO;
      return -1;
    }
    done += n;
  }
  return 0;
}
static int validate(const unsigned char *buf, size_t n) {
  if (memcmp(buf, expected, n)) {
    errno = EIO;
    return -1;
  }
  return 0;
}
/* A positive control matters: AvoryOS currently implements fadvise as a no-op
 * and mincore as PTE presence. That combination must never earn a cold score.
 */
static int evict_verified(int fd, int memory_fs) {
  if (memory_fs) {
    bench_skip("storage_cold", "memory_filesystem");
    return 0;
  }
  void *map = mmap(NULL, FILE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
  unsigned char vec[FILE_BYTES / BLOCK];
  if (map == MAP_FAILED) {
    bench_skip("storage_cold", "residency_mapping_unavailable");
    return 0;
  }
  int ok = !mincore(map, FILE_BYTES, vec);
  size_t warm = 0;
  if (ok)
    for (size_t i = 0; i < sizeof(vec); i++)
      warm += vec[i] & 1;
  if (!ok || warm != sizeof(vec)) {
    munmap(map, FILE_BYTES);
    bench_skip("storage_cold", "warm_pagecache_positive_control_failed");
    return 0;
  }
  int advice = posix_fadvise(fd, 0, FILE_BYTES, POSIX_FADV_DONTNEED);
  size_t resident = 0;
  if (!advice && !mincore(map, FILE_BYTES, vec))
    for (size_t i = 0; i < sizeof(vec); i++)
      resident += vec[i] & 1;
  else
    resident = sizeof(vec);
  munmap(map, FILE_BYTES);
  printf("STORAGE cache_probe warm_pages=%zu after_evict_pages=%zu "
         "advice_error=%d\n",
         warm, resident, advice);
  if (resident) {
    bench_skip("storage_cold", "cache_eviction_not_verified");
    return 0;
  }
  return 1;
}
static int reads(int fd, unsigned n, int cold, int memory_fs) {
  unsigned char buf[CHUNK];
  uint64_t start = bench_now();
  for (off_t off = 0; off < FILE_BYTES; off += CHUNK)
    if (io_at(fd, buf, CHUNK, off, 0) || validate(buf, CHUNK))
      return -1;
  uint64_t dt = bench_now() - start;
  printf("THROUGHPUT name=storage_%s_sequential_read MiB_s=%.3f bytes=%d "
         "cache=%s\n",
         cold ? "cold" : "warm", 32.0 * 1e9 / (dt ? dt : 1), FILE_BYTES,
         cold ? "verified_evicted" : "buffered_warm");
  /* Sequential read above warmed the file; evict again for random reads. */
  if (cold && !evict_verified(fd, memory_fs))
    return 0;
  unsigned order[FILE_BYTES / BLOCK];
  for (unsigned i = 0; i < FILE_BYTES / BLOCK; i++)
    order[i] = i;
  uint32_t rng = 0x12345678;
  for (unsigned i = FILE_BYTES / BLOCK - 1; i; i--) {
    rng = rng * 1664525 + 1013904223;
    unsigned j = rng % (i + 1), tmp = order[i];
    order[i] = order[j];
    order[j] = tmp;
  }
  if (n > FILE_BYTES / BLOCK)
    n = FILE_BYTES / BLOCK;
  uint64_t *times = calloc(n, sizeof(*times));
  if (!times)
    return -1;
  for (unsigned i = 0; i < n; i++) {
    start = bench_now();
    int rc = io_at(fd, buf, BLOCK, (off_t)order[i] * BLOCK, 0);
    times[i] = bench_now() - start;
    /* Pattern restarts per 64 KiB chunk. */
    size_t within = (order[i] * BLOCK) % CHUNK;
    if (!rc && memcmp(buf, expected + within, BLOCK)) {
      errno = EIO;
      rc = -1;
    }
    if (rc) {
      free(times);
      return -1;
    }
  }
  bench_stats(cold ? "storage_cold_random_4KiB" : "storage_warm_random_4KiB",
              times, n);
  free(times);
  return 0;
}
static int traversal(int dfd, unsigned n) {
  /* Use an actual nested tree; enumerate it and resolve complete paths. */
  for (unsigned d = 0; d < 8; d++) {
    char dirname[32];
    snprintf(dirname, sizeof(dirname), "tree-%u", d);
    if (mkdirat(dfd, dirname, 0700))
      return -1;
    for (unsigned f = 0; f < 16; f++) {
      char path[64];
      snprintf(path, sizeof(path), "tree-%u/file-%u", d, f);
      int fd = openat(dfd, path, O_CREAT | O_EXCL | O_WRONLY, 0600);
      if (fd < 0)
        return -1;
      close(fd);
    }
  }
  uint64_t *times = calloc(n, sizeof(*times));
  if (!times)
    return -1;
  for (unsigned i = 0; i < n; i++) {
    uint64_t start = bench_now();
    for (unsigned d = 0; d < 8; d++) {
      char path[32];
      snprintf(path, sizeof(path), "tree-%u", d);
      int fd = openat(dfd, path, O_RDONLY | O_DIRECTORY);
      if (fd < 0) {
        free(times);
        return -1;
      }
      DIR *dir = fdopendir(fd);
      if (!dir) {
        close(fd);
        free(times);
        return -1;
      }
      unsigned found = 0;
      struct dirent *e;
      errno = 0;
      while ((e = readdir(dir))) {
        if (e->d_name[0] == '.')
          continue;
        struct stat st;
        if (fstatat(dirfd(dir), e->d_name, &st, 0) || !S_ISREG(st.st_mode)) {
          closedir(dir);
          free(times);
          return -1;
        }
        found++;
      }
      int err = errno;
      closedir(dir);
      if (err || found != 16) {
        free(times);
        errno = err ? err : EIO;
        return -1;
      }
    }
    times[i] = bench_now() - start;
  }
  bench_stats("storage_walk_8dirs_128files_warm", times, n);
  free(times);
  return 0;
}
static void writeback_load(int fd, unsigned samples) {
  int ready[2];
  if (pipe(ready)) {
    bench_fail("storage_load_pipe");
    return;
  }
  fflush(NULL);
  pid_t child = fork();
  if (child < 0) {
    close(ready[0]);
    close(ready[1]);
    bench_fail("storage_load_fork");
    return;
  }
  if (!child) {
    close(ready[0]);
    unsigned char block[BLOCK] = {0};
    unsigned iteration = 0;
    for (;;) {
      if (io_at(fd, block, sizeof(block),
                (off_t)(iteration % (FILE_BYTES / BLOCK)) * BLOCK, 1) ||
          fdatasync(fd))
        _exit(2);
      if (!iteration) {
        char ok = 1;
        if (bench_transfer(ready[1], &ok, 1, 1))
          _exit(3);
        close(ready[1]);
      }
      iteration++;
    }
  }
  close(ready[1]);
  char ok = 0;
  int bad = bench_transfer(ready[0], &ok, 1, 0);
  close(ready[0]);
  if (bad || ok != 1)
    bench_fail("storage_load_ready");
  else
    bench_wakeups(samples, "wake_storage_writeback_load");
  kill(child, SIGTERM);
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  if (result != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
    errno = EIO;
    bench_fail("storage_load_worker");
  }
}
int bench_storage(unsigned samples, const char *base) {
  char dir[PATH_MAX];
  if (snprintf(dir, sizeof(dir), "%s/avory-desktop-io-XXXXXX", base) >=
      (int)sizeof(dir)) {
    errno = ENAMETOOLONG;
    bench_fail("storage_path");
    return -1;
  }
  if (!mkdtemp(dir)) {
    bench_fail("storage_mkdtemp");
    return -1;
  }
  int dfd = open(dir, O_RDONLY | O_DIRECTORY), fd = -1;
  if (dfd < 0) {
    bench_fail("storage_dir");
    rmdir(dir);
    return -1;
  }
  struct statfs fs;
  int memory_fs = 0;
  if (!fstatfs(dfd, &fs)) {
    memory_fs = (unsigned long)fs.f_type == 0x01021994UL ||
                (unsigned long)fs.f_type == 0x858458f6UL;
    printf("STORAGE path=%s filesystem_magic=0x%lx medium=%s bytes=%d\n", dir,
           (unsigned long)fs.f_type,
           memory_fs ? "memory" : "filesystem_requires_mount_record",
           FILE_BYTES);
  } else {
    bench_skip("storage_cold", "filesystem_identity_unavailable");
    memory_fs = 1;
  }
  fd = openat(dfd, "data", O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    bench_fail("storage_create");
    goto cleanup;
  }
  unsigned char buf[CHUNK];
  for (unsigned i = 0; i < CHUNK; i++)
    expected[i] = buf[i] = i % 251;
  uint64_t start = bench_now();
  for (off_t off = 0; off < FILE_BYTES; off += CHUNK)
    if (io_at(fd, buf, CHUNK, off, 1)) {
      bench_fail("storage_write");
      goto cleanup;
    }
  uint64_t written = bench_now(), sync_start = written;
  if (fdatasync(fd)) {
    bench_fail("storage_fdatasync");
    goto cleanup;
  }
  uint64_t synced = bench_now(), flush = synced - sync_start;
  printf("THROUGHPUT name=storage_buffered_write MiB_s=%.3f bytes=%d\n",
         32.0 * 1e9 / (written - start ? written - start : 1), FILE_BYTES);
  printf("THROUGHPUT name=storage_write_and_fdatasync MiB_s=%.3f bytes=%d "
         "physical_durability=not_powerfail_verified\n",
         32.0 * 1e9 / (synced - start ? synced - start : 1), FILE_BYTES);
  bench_stats("storage_writeback_fdatasync_32MiB", &flush, 1);
  if (reads(fd, samples, 0, memory_fs)) {
    bench_fail("storage_warm_read");
    goto cleanup;
  }
  /* Warm reads above are also the residency positive control. */
  if (evict_verified(fd, memory_fs) && reads(fd, samples, 1, memory_fs))
    bench_fail("storage_cold_read");
  if (traversal(dfd, samples))
    bench_fail("storage_traversal");
  uint64_t *times = calloc(samples, sizeof(*times));
  if (!times) {
    bench_fail("storage_samples");
    goto cleanup;
  }
  for (unsigned i = 0; i < samples; i++) {
    start = bench_now();
    if (io_at(fd, buf, BLOCK, (off_t)(i % (FILE_BYTES / BLOCK)) * BLOCK, 1) ||
        fsync(fd)) {
      bench_fail("storage_4KiB_fsync");
      free(times);
      goto cleanup;
    }
    times[i] = bench_now() - start;
  }
  bench_stats("storage_4KiB_write_fsync", times, samples);
  puts("STORAGE synchronous_write_method=pwrite_then_fsync "
       "flag_only_sync_not_scored");
  start = bench_now();
  if (fsync(dfd))
    bench_skip("storage_directory_fsync", "not_supported");
  else {
    uint64_t dt = bench_now() - start;
    bench_stats("storage_directory_fsync", &dt, 1);
  }
  free(times);
  writeback_load(fd, samples);
cleanup:
  if (fd >= 0)
    close(fd);
  unlinkat(dfd, "data", 0);
  for (unsigned d = 0; d < 8; d++) {
    char path[64];
    for (unsigned f = 0; f < 16; f++) {
      snprintf(path, sizeof(path), "tree-%u/file-%u", d, f);
      unlinkat(dfd, path, 0);
    }
    snprintf(path, sizeof(path), "tree-%u", d);
    unlinkat(dfd, path, AT_REMOVEDIR);
  }
  close(dfd);
  if (rmdir(dir))
    bench_fail("storage_cleanup");
  return 0;
}
