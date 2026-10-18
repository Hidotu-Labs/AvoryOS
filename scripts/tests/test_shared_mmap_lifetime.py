#!/usr/bin/env python3
"""Shared tmpfs/memfd mappings must own one PMM reference per PTE.

A MAP_SHARED view of a tmpfs or memfd file installs the filesystem's frames
straight into user PTEs.  Without a reference of its own, the mapping could
outlive the backing: tmpfs releases frames on ftruncate shrink, and memfd
storage is reallocated (old block released) when an mmap needs more capacity
or a write grows the file.  The PMM then re-issued the frame while the stale
PTEs stayed writable, so writes through the old mapping corrupted whatever
the frame had been reallocated for.

The probe fills each mapping with 0x11, forces the backing release, then
claims a large anonymous buffer filled with 0x5A and reads the old mapping
back.  A 0x5A byte proves the frame was recycled while the mapping was live.
The probe never writes through a possibly-recycled PTE, so a failing run
corrupts nothing.
"""
from boot_probe import run_probe

INIT = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif

#define MAP_PGSZ 4096
#define MAP_NPAGES 64
#define MAP_MARK 0x5A
#define MAP_FILL 0x11

static int map_failures;

static void *map_claim(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return NULL;
  memset(p, MAP_MARK, bytes);
  return p;
}

static void map_check(const char *name, unsigned char *m, size_t len) {
  int hits = 0;
  for (size_t i = 0; i < len; i++)
    if (m[i] == MAP_MARK)
      hits++;
  printf("SHARED_MMAP_LIFETIME: %s recycled=%d/%zu\n", name, hits, len);
  if (hits)
    map_failures++;
}

/* memfd shared mapping; a later mmap that needs more capacity reallocates
 * the storage block and releases the old frames. */
static void map_memfd_realloc(void) {
  int fd = (int)syscall(SYS_memfd_create, "lifetime_realloc", 0);
  if (fd < 0) {
    printf("SHARED_MMAP_LIFETIME: memfd_create errno=%d\n", errno);
    map_failures++;
    return;
  }
  if (ftruncate(fd, MAP_NPAGES * MAP_PGSZ) != 0) {
    printf("SHARED_MMAP_LIFETIME: ftruncate errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  unsigned char *m1 = mmap(NULL, MAP_NPAGES * MAP_PGSZ, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
  if (m1 == MAP_FAILED) {
    printf("SHARED_MMAP_LIFETIME: m1 errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  memset(m1, MAP_FILL, MAP_NPAGES * MAP_PGSZ);

  size_t big = (size_t)(MAP_NPAGES + 64) * MAP_PGSZ;
  unsigned char *m2 = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m2 == MAP_FAILED) {
    printf("SHARED_MMAP_LIFETIME: m2 errno=%d\n", errno);
    map_failures++;
  } else {
    memset(m2, MAP_FILL, big);
  }

  if (!map_claim(big * 4)) {
    printf("SHARED_MMAP_LIFETIME: claim1 errno=%d\n", errno);
    map_failures++;
  }
  map_check("memfd_realloc", m1, MAP_NPAGES * MAP_PGSZ);
  close(fd);
}

/* memfd shared mapping; a write past the capacity reallocates the storage
 * block and releases the old frames. */
static void map_memfd_write_grow(void) {
  int fd = (int)syscall(SYS_memfd_create, "lifetime_grow", 0);
  if (fd < 0) {
    printf("SHARED_MMAP_LIFETIME: memfd2_create errno=%d\n", errno);
    map_failures++;
    return;
  }
  if (ftruncate(fd, MAP_NPAGES * MAP_PGSZ) != 0) {
    printf("SHARED_MMAP_LIFETIME: memfd2 ftruncate errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  unsigned char *m1 = mmap(NULL, MAP_NPAGES * MAP_PGSZ, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
  if (m1 == MAP_FAILED) {
    printf("SHARED_MMAP_LIFETIME: memfd2 m1 errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  memset(m1, MAP_FILL, MAP_NPAGES * MAP_PGSZ);

  static unsigned char blob[64 * MAP_PGSZ];
  memset(blob, MAP_FILL, sizeof(blob));
  ssize_t w = pwrite(fd, blob, sizeof(blob), MAP_NPAGES * MAP_PGSZ);
  if (w != (ssize_t)sizeof(blob)) {
    printf("SHARED_MMAP_LIFETIME: memfd2 pwrite=%zd errno=%d\n", w, errno);
    map_failures++;
  }

  if (!map_claim((size_t)(MAP_NPAGES + 64) * MAP_PGSZ * 4)) {
    printf("SHARED_MMAP_LIFETIME: claim2 errno=%d\n", errno);
    map_failures++;
  }
  map_check("memfd_write_grow", m1, MAP_NPAGES * MAP_PGSZ);
  close(fd);
}

/* tmpfs shared mapping; ftruncate to zero releases the frames in place. */
static void map_tmpfs_truncate(void) {
  const char *path = "/tmp/lifetime_probe";
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0) {
    printf("SHARED_MMAP_LIFETIME: open %s errno=%d\n", path, errno);
    map_failures++;
    return;
  }
  if (ftruncate(fd, MAP_NPAGES * MAP_PGSZ) != 0) {
    printf("SHARED_MMAP_LIFETIME: tmpfs ftruncate errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  unsigned char *m1 = mmap(NULL, MAP_NPAGES * MAP_PGSZ, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
  if (m1 == MAP_FAILED) {
    printf("SHARED_MMAP_LIFETIME: tmpfs m1 errno=%d\n", errno);
    map_failures++;
    close(fd);
    return;
  }
  memset(m1, MAP_FILL, MAP_NPAGES * MAP_PGSZ);

  if (ftruncate(fd, 0) != 0) {
    printf("SHARED_MMAP_LIFETIME: tmpfs shrink errno=%d\n", errno);
    map_failures++;
  }
  if (!map_claim((size_t)MAP_NPAGES * MAP_PGSZ * 8)) {
    printf("SHARED_MMAP_LIFETIME: claim3 errno=%d\n", errno);
    map_failures++;
  }
  map_check("tmpfs_truncate", m1, MAP_NPAGES * MAP_PGSZ);
  close(fd);
  unlink(path);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("SHARED_MMAP_LIFETIME: START\n");

  map_memfd_realloc();
  map_memfd_write_grow();
  map_tmpfs_truncate();

  if (map_failures) {
    printf("SHARED_MMAP_LIFETIME: FAIL failures=%d\n", map_failures);
  } else {
    printf("SHARED_MMAP_LIFETIME: PASS\n");
  }
  fflush(stdout);
  sync();
  reboot(RB_POWER_OFF);
  for (;;)
    pause();
}
'''

if __name__ == '__main__':
    run_probe('shared-mmap-lifetime', INIT,
              'SHARED_MMAP_LIFETIME: PASS')
