#!/usr/bin/env python3
"""Userspace errno for RAM-backed resize failures under memory pressure.

Slices the real ramfs/tmpfs resize backends plus vfs_truncate()/
vfs_fallocate()/sys_ftruncate()/sys_fallocate() and asserts that an
allocation failure surfaces as -ENOMEM (-12).  The 2026-10-16 OOM capture
reported "ftruncate(#77) returned ... (EPERM)" while free_pages was zero:
the backends returned -1 for every failure and sys_ftruncate collapsed even
distinguishable errors to -1.  A parseable regression proof can run this
same test against patched sources via the *_SRC environment variables.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def source(env, default):
    return Path(os.environ.get(env, ROOT / default)).read_text()


def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


ramfs_src = source('AVORY_RAMFS_SRC', 'kernel/src/fs/ramfs.c')
tmpfs_src = source('AVORY_TMPFS_SRC', 'kernel/src/fs/tmpfs.c')
vfs_src = source('AVORY_VFS_SRC', 'kernel/src/fs/vfs.c')
sys_fd_src = source('AVORY_SYS_FD_SRC', 'kernel/src/syscalls/sys_fd.c')

slices = [
    'static uint32_t ramfs_bad_free_reports;\n',
    extract(ramfs_src, 'void ramfs_free_file_data('),
    extract(ramfs_src, 'static uint8_t *ramfs_alloc_buffer('),
    extract(ramfs_src, 'int ramfs_truncate(vfs_node_t *node, uint32_t new_len) {'),
    extract(ramfs_src, 'int ramfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {'),
    extract(tmpfs_src, 'static int tmpfs_truncate(vfs_node_t *node, uint32_t new_len) {'),
    extract(tmpfs_src, 'static int tmpfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {'),
    extract(vfs_src, 'int vfs_truncate(vfs_node_t *node, uint32_t size) {'),
    extract(vfs_src, 'int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {'),
    extract(sys_fd_src, 'static uint64_t sys_ftruncate(uint64_t fd, uint64_t length,'),
    extract(sys_fd_src, 'static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,'),
]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FS_FILE 0x01
#define FS_TYPE_MASK 0x0F
#define FS_PAGE_CACHE 0x80
#define MAX_FDS 1024
#define FD_IN_RANGE(t,fd) ((t) && (uint64_t)(fd) < MAX_FDS)
#define TMPFS_PAGE_BATCH 64
#define PAGE_SIZE 4096U
#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

typedef struct vfs_node vfs_node_t;
struct vfs_node {
  void *device;
  uint32_t flags;
  uint32_t length;
  bool mmap_shared_only;
  char name[128];
  int (*truncate)(vfs_node_t *, uint32_t);
  int (*fallocate)(vfs_node_t *, int, uint32_t, uint32_t);
};

typedef struct {
  uint8_t *data;
  uint32_t capacity;
  uint8_t data_is_pmm;
} ramfs_file_t;

typedef struct radix_tree { int unused; } radix_tree_t;
typedef struct { int locked; } spinlock_t;
typedef struct tmpfs_sb { uint64_t max_bytes; } tmpfs_sb_t;
typedef struct tmpfs_page { uint32_t page_index; uint64_t phys; } tmpfs_page_t;
typedef struct {
  radix_tree_t pages;
  uint32_t num_pages;
  spinlock_t lock;
  tmpfs_sb_t *sb;
} tmpfs_file_t;

typedef struct fd_table { spinlock_t lock; vfs_node_t *fds[MAX_FDS]; } fd_table_t;
struct thread { uint32_t tid; char comm[16]; fd_table_t *files; };
static struct thread boot_thread = { .tid = 7, .comm = "boot" };
static struct thread *sched_get_current(void) { return &boot_thread; }

#define spinlock_acquire(l) ((void)(l))
#define spinlock_release(l) ((void)(l))

#define NPAGES 256
static uint8_t pmpool[NPAGES * PAGE_SIZE] __attribute__((aligned(4096)));
static uint8_t kheap[1 << 20];
static size_t pmoff, kheap_off;
static uint16_t refs[NPAGES];
static bool pmm_fail, kmalloc_fail;
static unsigned alloc_calls, free_calls, free_page_calls;

size_t pmm_get_hhdm_offset(void) { return 0; }

static uint64_t pfn_of(const void *ptr) {
  uintptr_t v = (uintptr_t)ptr;
  assert(v >= (uintptr_t)pmpool && v < (uintptr_t)pmpool + sizeof(pmpool));
  return (v - (uintptr_t)pmpool) / PAGE_SIZE;
}

void *pmm_alloc_pages(size_t count) {
  alloc_calls++;
  size_t bytes = count * PAGE_SIZE;
  if (pmm_fail || pmoff + bytes > sizeof(pmpool))
    return NULL;
  void *p = pmpool + pmoff;
  pmoff += bytes;
  for (size_t i = 0; i < count; i++)
    refs[pfn_of((uint8_t *)p + i * PAGE_SIZE)] = 1;
  return p;
}

uint16_t pmm_get_ref(void *ptr) { return refs[pfn_of(ptr)]; }

void pmm_decref(void *ptr) {
  uint64_t pfn = pfn_of(ptr);
  if (refs[pfn] == 0)
    return;
  refs[pfn]--;
}

void pmm_free_pages(void *ptr, size_t count) {
  free_calls++;
  uint64_t pfn = pfn_of(ptr);
  for (size_t i = 0; i < count; i++) {
    free_page_calls++;
    if (refs[pfn + i])
      refs[pfn + i]--;
  }
}

void *kmalloc(size_t size) {
  if (kmalloc_fail)
    return NULL;
  size = (size + 7) & ~(size_t)7;
  if (kheap_off + size > sizeof(kheap))
    return NULL;
  void *p = kheap + kheap_off;
  kheap_off += size;
  return p;
}
void kfree(void *ptr) { (void)ptr; }

void klog_puts(const char *s) { (void)s; }
void klog_hex64(uint64_t n) { (void)n; }
void klog_uint64(uint64_t n) { (void)n; }
void klog_ramfs_free(void *p, uint64_t cap, bool pmm, uint64_t pages) {
  (void)p; (void)cap; (void)pmm; (void)pages;
}
static void klogf(const char *fmt, ...) { (void)fmt; }
#define klog_debugf(...) ((void)0)

static unsigned close_calls;
static bool vfs_mount_readonly(vfs_node_t *node) { (void)node; return false; }
static void vfs_close(vfs_node_t *node) { (void)node; close_calls++; }
static void vfs_cache_clear(vfs_node_t *node) { (void)node; }

static vfs_node_t *pinned_node;
static vfs_node_t *fd_pin_node(struct thread *t, uint64_t fd,
                               uint64_t *offset, uint64_t *flags) {
  (void)t; (void)fd; (void)offset; (void)flags;
  return pinned_node;
}

/* tmpfs page-tree model: allocation is driven by tmpfs_alloc_ok. */
static bool tmpfs_alloc_ok = true;
static unsigned tmpfs_alloc_calls;
static tmpfs_page_t tmpfs_page;
static tmpfs_page_t *tmpfs_get_or_alloc_page(tmpfs_file_t *file, uint32_t index) {
  (void)file;
  tmpfs_alloc_calls++;
  if (!tmpfs_alloc_ok)
    return NULL;
  tmpfs_page.page_index = index;
  tmpfs_page.phys = 0x100000;
  return &tmpfs_page;
}
static tmpfs_page_t *tmpfs_get_page(tmpfs_file_t *file, uint32_t index) {
  (void)file; (void)index;
  return NULL;
}
static uint32_t tmpfs_take_pages_locked(tmpfs_file_t *file, uint32_t from,
                                        tmpfs_page_t **batch, uint32_t max) {
  (void)file; (void)from; (void)batch; (void)max;
  return 0;
}
static void tmpfs_release_pages(tmpfs_page_t **pages, uint32_t n) {
  (void)pages; (void)n;
}
static void tmpfs_uncharge_bytes(tmpfs_sb_t *sb, uint64_t bytes) {
  (void)sb; (void)bytes;
}
static void tmpfs_free_all_pages(tmpfs_file_t *file) { (void)file; }

'''

tests = r'''
static void reset(void) {
  memset(refs, 0, sizeof(refs));
  pmoff = kheap_off = 0;
  pmm_fail = kmalloc_fail = false;
  alloc_calls = free_calls = free_page_calls = 0;
  close_calls = tmpfs_alloc_calls = 0;
  tmpfs_alloc_ok = true;
  pinned_node = NULL;
  boot_thread.files = NULL;
}

static vfs_node_t memfd_node(ramfs_file_t *file, bool is_pmm) {
  vfs_node_t node;
  memset(&node, 0, sizeof(node));
  file->data = NULL;
  file->capacity = 0;
  file->data_is_pmm = is_pmm ? 1 : 0;
  node.device = file;
  node.flags = FS_FILE;
  node.truncate = ramfs_truncate;
  node.fallocate = ramfs_fallocate;
  return node;
}

int main(void) {
  /* 1. ramfs growth with PMM and kmalloc both exhausted: ENOMEM, not EPERM. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    pmm_fail = kmalloc_fail = true;
    int rc = ramfs_truncate(&node, 8192);
    assert(rc == -12);
    assert(node.length == 0);
    assert(alloc_calls == 1);
  }

  /* 2. Same failure through ftruncate(2): the syscall passes -ENOMEM. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    pmm_fail = kmalloc_fail = true;
    pinned_node = &node;
    uint64_t rc = sys_ftruncate(3, 8192, 0, 0, 0, 0);
    assert(rc == (uint64_t)-12);
    assert(close_calls == 1);
  }

  /* 3. Successful growth still returns 0 and owns live references. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    pinned_node = &node;
    uint64_t rc = sys_ftruncate(3, 8192, 0, 0, 0, 0);
    assert(rc == 0);
    assert(node.length == 8192 && file.capacity == 8192 && file.data_is_pmm);
    assert(pmm_get_ref(file.data) == 1 && pmm_get_ref(file.data + 4096) == 1);
    ramfs_free_file_data(&file);
  }

  /* 4. Shrinking inside capacity never allocates and stays successful. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    uint64_t phys = (uint64_t)pmm_alloc_pages(1);
    file.data = (uint8_t *)phys;
    file.capacity = 4096;
    node.length = 4096;
    pmm_fail = kmalloc_fail = true;
    int rc = ramfs_truncate(&node, 100);
    assert(rc == 0 && node.length == 100);
    assert(alloc_calls == 1);   /* only the test's own allocation */
    ramfs_free_file_data(&file);
  }

  /* 5. fallocate growth reports ENOMEM; ftruncate's old -1 collapse used to
   *    surface as EOPNOTSUPP for fallocate and EPERM for ftruncate. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    pmm_fail = kmalloc_fail = true;
    int rc = ramfs_fallocate(&node, 0, 0, 16384);
    assert(rc == -12);
    pinned_node = &node;
    uint64_t src = sys_fallocate(4, 0, 0, 16384, 0, 0);
    assert(src == (uint64_t)-12);
  }

  /* 6. A node with no fallocate handler still reports EOPNOTSUPP. */
  {
    reset();
    ramfs_file_t file;
    vfs_node_t node = memfd_node(&file, true);
    node.fallocate = NULL;
    pinned_node = &node;
    uint64_t rc = sys_fallocate(4, 0, 0, 16384, 0, 0);
    assert(rc == (uint64_t)-95);
  }

  /* 7. tmpfs growth under exhaustion: ENOMEM, not EPERM. */
  {
    reset();
    tmpfs_file_t file;
    memset(&file, 0, sizeof(file));
    vfs_node_t node;
    memset(&node, 0, sizeof(node));
    node.device = &file;
    node.flags = FS_FILE;
    node.truncate = tmpfs_truncate;
    node.fallocate = tmpfs_fallocate;
    tmpfs_alloc_ok = false;
    int rc = tmpfs_truncate(&node, 12288);
    assert(rc == 0 && node.length == 12288);
    assert(tmpfs_alloc_calls == 0);   /* sparse growth never allocates */
    rc = tmpfs_fallocate(&node, 0x01, 0, 12288);
    assert(rc == -12);
  }

  /* 8. tmpfs shrink still succeeds without allocating. */
  {
    reset();
    tmpfs_file_t file;
    memset(&file, 0, sizeof(file));
    vfs_node_t node;
    memset(&node, 0, sizeof(node));
    node.device = &file;
    node.flags = FS_FILE;
    node.truncate = tmpfs_truncate;
    node.length = 4096;
    tmpfs_alloc_ok = false;
    int rc = tmpfs_truncate(&node, 100);
    assert(rc == 0 && node.length == 100);
    assert(tmpfs_alloc_calls == 0);
  }

  /* 9. A node without a truncate handler keeps the legacy -EPERM mapping. */
  {
    reset();
    vfs_node_t node;
    memset(&node, 0, sizeof(node));
    node.device = (void *)1;
    node.flags = FS_FILE;
    node.truncate = NULL;
    pinned_node = &node;
    uint64_t rc = sys_ftruncate(3, 8192, 0, 0, 0, 0);
    assert(rc == (uint64_t)-1);
  }

  puts("PASS: RAM-backed resize failures report -ENOMEM through ftruncate/"
       "fallocate; unsupported operations keep their documented errnos");
  return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + '\n'.join(slices) + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
