#!/usr/bin/env python3
"""Release-path ownership test for ramfs_free_file_data().

Slices the real function out of kernel/src/fs/ramfs.c and drives it with a
modelled PMM so the two release paths are asserted directly:
  - every page still owned (ref >= 1) -> one bulk pmm_free_pages() call,
  - any page already released (ref == 0) -> per-page pmm_decref() that skips
    the zero-ref page and never bulk-frees the rounded block.  That second
    path is the fix for "[RAMFS] backing ownership lost before free" turning
    into a second buddy release of frames another owner already returned.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / 'kernel/src/fs/ramfs.c').read_text()
start = src.index('void ramfs_free_file_data(')
end = src.index('\n}\n', start) + 3
function = ('static uint32_t ramfs_bad_free_reports;\n\n' + src[start:end])

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  uint32_t capacity;
  uint8_t data_is_pmm;
} ramfs_file_t;

struct thread { uint32_t tid; char comm[16]; };
static struct thread boot_thread = { .tid = 7, .comm = "boot" };
static struct thread *sched_get_current(void) { return &boot_thread; }

#define NPAGES 64
static const uint64_t hhdm = 0xFFFF800000000000ULL;
static uint16_t refs[NPAGES];
static unsigned freed[NPAGES];      /* ref reached zero via either path */
static unsigned decref_calls[NPAGES];
static unsigned bulk_free_calls;
static unsigned bulk_pages_touched[NPAGES];
static unsigned kfree_calls;
static unsigned ownership_lost_reports;

size_t pmm_get_hhdm_offset(void) { return (size_t)hhdm; }
uint16_t pmm_get_ref(void *ptr) {
  uint64_t pfn = (uint64_t)ptr / 4096;
  assert(pfn < NPAGES);
  return refs[pfn];
}
void pmm_decref(void *ptr) {
  uint64_t pfn = (uint64_t)ptr / 4096;
  assert(pfn < NPAGES);
  decref_calls[pfn]++;
  if (refs[pfn] == 0)
    return;                      /* mirrors pmm_put_ref refusing a zero ref */
  if (--refs[pfn] == 0)
    freed[pfn]++;
}
void pmm_free_pages(void *ptr, size_t count) {
  bulk_free_calls++;
  uint64_t pfn = (uint64_t)ptr / 4096;
  for (size_t i = 0; i < count; i++) {
    assert(pfn + i < NPAGES);
    bulk_pages_touched[pfn + i]++;
    if (refs[pfn + i] == 0)
      continue;
    if (--refs[pfn + i] == 0)
      freed[pfn + i]++;
  }
}
void kfree(void *ptr) { (void)ptr; kfree_calls++; }
static unsigned klog_puts_calls;
void klog_puts(const char *s) {
  klog_puts_calls++;
  if (s && strstr(s, "ownership lost"))
    ownership_lost_reports++;
}
void klog_hex64(uint64_t n) { (void)n; }
void klog_uint64(uint64_t n) { (void)n; }
void klog_ramfs_free(void *ptr, uint64_t capacity, bool is_pmm, uint64_t pages) {
  (void)ptr; (void)capacity; (void)is_pmm; (void)pages;
}

'''

tests = r'''
static ramfs_file_t make_file(unsigned first_page, uint32_t capacity,
                              unsigned refs_in[], size_t n) {
  ramfs_file_t f;
  memset(&f, 0, sizeof(f));
  for (size_t i = 0; i < n; i++)
    refs[first_page + i] = (uint16_t)refs_in[i];
  f.data = (uint8_t *)((uint64_t)((first_page * 4096)) + hhdm);
  f.capacity = capacity;
  f.data_is_pmm = 1;
  return f;
}

int main(void) {
  /* 1. All pages still owned: one bulk release, one decref per page. */
  {
    unsigned owned[] = { 1, 1, 2 };
    ramfs_file_t f = make_file(8, 3 * 4096, owned, 3);
    ramfs_free_file_data(&f);
    assert(bulk_free_calls == 1);
    assert(bulk_pages_touched[8] == 1 && bulk_pages_touched[9] == 1 &&
           bulk_pages_touched[10] == 1);
    assert(freed[8] == 1 && freed[9] == 1 && freed[10] == 0);
    assert(refs[10] == 1);          /* extra owner keeps its reference */
    assert(decref_calls[8] == 0 && decref_calls[10] == 0);
    assert(f.data == NULL && f.capacity == 0 && f.data_is_pmm == 0);
    assert(ownership_lost_reports == 0);
  }

  /* 2. One page already returned to the PMM (ref 0): report, skip it, and
   *    release the remaining pages per page - never bulk-free the block. */
  {
    unsigned mixed[] = { 1, 0, 1, 2 };
    unsigned bulk_before = bulk_free_calls;
    ramfs_file_t f = make_file(16, 4 * 4096, mixed, 4);
    ramfs_free_file_data(&f);
    assert(bulk_free_calls == bulk_before);      /* no bulk release */
    assert(decref_calls[16] == 1 && freed[16] == 1);
    assert(decref_calls[17] == 0);               /* zero-ref page skipped */
    assert(decref_calls[18] == 1 && freed[18] == 1);
    assert(decref_calls[19] == 1 && freed[19] == 0 && refs[19] == 1);
    assert(ownership_lost_reports == 1);
    assert(f.data == NULL && f.capacity == 0 && f.data_is_pmm == 0);
  }

  /* 3. A stale duplicate release (the observed double-close shape) must not
   *    touch any zero-ref page a second time. */
  {
    unsigned owned[] = { 1, 1 };
    ramfs_file_t f = make_file(24, 2 * 4096, owned, 2);
    ramfs_file_t stale = f;
    ramfs_free_file_data(&f);
    unsigned freed_before[2] = { freed[24], freed[25] };
    unsigned decref_before[2] = { decref_calls[24], decref_calls[25] };
    ramfs_free_file_data(&stale);
    assert(freed[24] == freed_before[0] && freed[25] == freed_before[1]);
    assert(decref_calls[24] == decref_before[0] &&
           decref_calls[25] == decref_before[1]);
    assert(ownership_lost_reports == 3);   /* two zero-ref pages reported */
  }

  /* 4. kmalloc-backed buffers keep the plain kfree path. */
  {
    unsigned kfree_before = kfree_calls;
    unsigned bulk_before = bulk_free_calls;
    ramfs_file_t f;
    memset(&f, 0, sizeof(f));
    static uint8_t heap_buf[128];
    f.data = heap_buf;
    f.capacity = sizeof(heap_buf);
    f.data_is_pmm = 0;
    ramfs_free_file_data(&f);
    assert(kfree_calls == kfree_before + 1);
    assert(bulk_free_calls == bulk_before);
    assert(f.data == NULL && f.capacity == 0 && f.data_is_pmm == 0);
  }

  puts("PASS: ramfs release keeps bulk free only while every frame is owned; "
       "zero-ref frames are reported and skipped on the per-page path");
  return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + function + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
