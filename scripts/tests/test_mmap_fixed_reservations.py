#!/usr/bin/env python3
"""Run sys_mmap against a locked interval fixture, including Wine reservations."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/syscalls/sys_mm.c').read_text()
mmap = source[source.index('uint64_t sys_mmap('):source.index('\n// sys_munmap')]
region = source[source.index('static bool mm_compat32('):source.index('// INTERNAL HELPERS')]
constants = source[source.index('// Linux mmap constants'):source.index('// Helpers')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel/src/mm/vma.h"
#define USER_ADDR_MAX 0x7fffffffffffULL
#define PAGE_SIZE 4096ULL
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MAX_FDS 8
#define FS_TYPE_MASK 15
#define FS_FILE 1
typedef struct node {
  uint64_t (*mmap)(struct node *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  int flags;
  bool mmap_shared_only;
} vfs_node_t;
struct mm { int lock; struct vma_list vmas; uint64_t mmap_next_addr; bool compat32; };
struct thread { struct mm *mm; vfs_node_t *fds[MAX_FDS]; };
static unsigned fd_pins;
static vfs_node_t *fd_node_get(struct thread *t,uint64_t fd){
 if(!t || fd>=MAX_FDS || !t->fds[fd] || t->fds[fd]==(vfs_node_t*)-1)return NULL;
 fd_pins++;return t->fds[fd];
}
static void fd_node_put(vfs_node_t **n){if(*n){assert(fd_pins);fd_pins--;}}
static struct mm memory;
static struct thread thread = { .mm = &memory };
static struct vma regions[32];
static int count, teardown_calls, fail_add;
static void spinlock_acquire(int *lock) { assert(!*lock); *lock = 1; }
static void spinlock_release(int *lock) { assert(*lock); *lock = 0; }
static struct thread *sched_get_current(void) { return &thread; }
''' + region + r'''
static uint64_t *vmm_get_active_pml4(void) { return NULL; }
static bool is_user_pointer(uint64_t addr) { return addr <= USER_ADDR_MAX; }
static void teardown_range(uint64_t *pml4, struct thread *t, uint64_t start,
                           uint64_t len, bool locked, const char *why) {
  (void)pml4; (void)start; (void)len; (void)why;
  assert(locked && t->mm->lock); teardown_calls++;
}
static void vmm_drain_deferred_work(void) { assert(!memory.lock); }

struct vma *vma_find_overlap(struct vma_list *v, uint64_t start, uint64_t end) {
  (void)v; assert(memory.lock);
  for (int i = 0; i < count; i++)
    if (regions[i].start < end && regions[i].end > start) return &regions[i];
  return NULL;
}
bool vma_remove(struct vma_list *v, uint64_t start, uint64_t end) {
  (void)v; assert(memory.lock); bool removed = false;
  for (int i = 0; i < count; i++) {
    if (regions[i].start >= end || regions[i].end <= start) continue;
    // Tests replace whole intervals; the real VMA tree handles splits.
    assert(start <= regions[i].start && end >= regions[i].end);
    regions[i--] = regions[--count]; removed = true;
  }
  return removed;
}
int vma_add(struct vma_list *v, uint64_t start, uint64_t end, uint64_t prot,
            uint64_t flags, int fd, uint64_t off, void *node, uint64_t size) {
  (void)off; (void)node; (void)size; assert(memory.lock);
  if (fail_add || vma_find_overlap(v, start, end)) return -1;
  assert(count < 32);
  regions[count++] = (struct vma){.start=start,.end=end,.prot=prot,.flags=flags,.fd=fd};
  return 0;
}
uint64_t vma_find_gap(struct vma_list *v, uint64_t len, uint64_t base, uint64_t limit) {
  assert(memory.lock);
  while (base < limit && len <= limit - base) {
    struct vma *hit = vma_find_overlap(v, base, base + len);
    if (!hit) return base;
    base = hit->end;
  }
  return 0;
}
void vma_attach_linux(struct vma_list *v, uint64_t start, void *wrapper) {
  (void)v; (void)start; (void)wrapper;
}
static uint64_t mm_alloc_mmap_region(uint64_t length) {
  (void)length; return 0;
}
'''
tmpfs_source = Path(os.environ.get('TMPFS_SOURCE', ROOT / 'kernel/src/fs/tmpfs.c')).read_text()
start = tmpfs_source.index('        n->mmap     = tmpfs_mmap;')
end = tmpfs_source.index('    } else if', start)
tmpfs_setup = tmpfs_source[start:end]
tests = r'''
static uint64_t anon(uint64_t addr, uint64_t length, uint64_t flags) {
  uint64_t result = sys_mmap(addr, length, 0, flags|MAP_PRIVATE|MAP_ANONYMOUS,
                              UINT32_MAX, 0);
  assert(!memory.lock && !fd_pins); return result;
}
static void reset(void) {
  memset(&memory, 0, sizeof(memory)); count = teardown_calls = fail_add = 0;
  memory.mmap_next_addr = MMAP_REGION_BASE;
}
static unsigned handler_calls;
static uint64_t handler_error;
static uint64_t shared_handler(vfs_node_t *node, uint64_t addr, uint64_t len,
                               uint64_t prot, uint64_t flags, uint64_t off) {
  (void)node; (void)len; (void)prot; (void)off;
  assert((flags & 3) != MAP_PRIVATE); handler_calls++; return handler_error ? handler_error : addr;
}
static void make_tmpfs_fixture(vfs_node_t *n) {
  memset(n, 0, sizeof(*n)); n->flags=FS_FILE;
#define tmpfs_mmap shared_handler
''' + tmpfs_setup + r'''
#undef tmpfs_mmap
}
int main(void) {
  reset();
  vfs_node_t tmp; make_tmpfs_fixture(&tmp); thread.fds[3]=&tmp;
  /* Firefox's failed read-only XPI-size request must bypass the shared mapper. */
  uint64_t xpi=sys_mmap(0,0x46f474,1,MAP_PRIVATE,3,0);
  assert(xpi < (uint64_t)-4095 && !handler_calls && count==1);
  assert(regions[0].flags & MAP_PAGECACHE);
  assert(regions[0].end-regions[0].start==PAGE_ALIGN_UP(0x46f474));
  /* Writable private mappings use the same cache/CoW path; shared mappings
   * retain direct tmpfs frame ownership and its existing driver handler. */
  assert(sys_mmap(0,4096,3,MAP_PRIVATE,3,0)<(uint64_t)-4095 && !handler_calls);
  assert(regions[1].flags & MAP_PAGECACHE);
  assert(sys_mmap(0,4096,3,MAP_SHARED,3,0)<(uint64_t)-4095 && handler_calls==1);
  assert(!(regions[2].flags & MAP_PAGECACHE));
  puts("PASS: tmpfs private read-only/writable mappings use file cache; shared mappings retain driver ownership");
  handler_calls=0;
  reset(); memory.compat32=true; memory.mmap_next_addr=MMAP_REGION_BASE;
  uint64_t low=anon(0,4096,0);assert(low==0x40000000ULL);
  assert(anon(0xbffff000,8192,MAP_FIXED)==E_NOMEM);
  assert(anon(0xc0000000,4096,MAP_FIXED)==E_NOMEM);
  assert(anon(0x20000000,4096,MAP_FIXED)==0x20000000);
  reset();
  vfs_node_t memfd = {.mmap=shared_handler, .flags=FS_FILE,
                      .mmap_shared_only=true};
  thread.fds[3] = &memfd;
  uint64_t private_addr = sys_mmap(0,4096,3,MAP_PRIVATE,3,0);
  assert(private_addr == MMAP_REGION_BASE && handler_calls == 0);
  assert(regions[0].flags & MAP_PAGECACHE);
  uint64_t shared_addr = sys_mmap(0,4096,3,MAP_SHARED,3,0);
  assert(shared_addr == private_addr+4096 && handler_calls == 1);
  assert(!(regions[1].flags & MAP_PAGECACHE));
  assert(sys_mmap(0,4096,3,3 /* MAP_SHARED_VALIDATE */,3,0)
         == shared_addr+4096 && handler_calls == 2);
  int mapped_before = count;
  handler_error = (uint64_t)-22;
  assert(sys_mmap(0,4096,3,MAP_SHARED,3,1) == (uint64_t)-22);
  assert(count == mapped_before);
  handler_error = (uint64_t)-12;
  assert(sys_mmap(0,4096,3,MAP_SHARED,3,0) == (uint64_t)-12);
  assert(count == mapped_before);
  handler_error = 0;
  thread.fds[3] = NULL;
  reset();
  uint64_t initial = anon(0, 0x369000, 0);
  assert(initial == MMAP_REGION_BASE);
  uint64_t cursor = memory.mmap_next_addr;
  // Wine's top-of-address-space reservations precede ordinary bookkeeping.
  assert(anon(0x7ffff0000000ULL, 0xe000000, MAP_FIXED_NOREPLACE|0x4000)
         == 0x7ffff0000000ULL);
  assert(memory.mmap_next_addr == cursor);
  assert(anon(0x7ff000000000ULL, 0x600000000ULL, MAP_FIXED_NOREPLACE|0x4000)
         == 0x7ff000000000ULL);
  assert(memory.mmap_next_addr == cursor);
  int before = count;
  // Unsupported host-address-space probe: no VMA or cursor side effects.
  assert(anon(0x8000000000000000ULL, 4096, MAP_FIXED_NOREPLACE) == E_NOMEM);
  assert(count == before && memory.mmap_next_addr == cursor);
  assert(anon(0, 0x100000, 0) == cursor); // Wine's view_block_start allocation.
  assert(anon(0, 0x100000, 0) == cursor + 0x100000);
  // A fixed mapping inside the arena must be skipped by the next gap search.
  reset(); cursor = MMAP_REGION_BASE;
  assert(anon(cursor, 4096, MAP_FIXED_NOREPLACE) == cursor);
  assert(memory.mmap_next_addr == cursor);
  assert(anon(0, 4096, 0) == cursor + 4096);
  int teardown_before = teardown_calls;
  before = count;
  assert(anon(cursor, 4096, MAP_FIXED_NOREPLACE) == E_EXIST);
  assert(count == before && teardown_calls == teardown_before);
  uint64_t after = memory.mmap_next_addr;
  assert(anon(cursor, 4096, MAP_FIXED) == cursor);
  assert(count == before && memory.mmap_next_addr == after);
  assert(anon(cursor + 1, 4096, MAP_FIXED) == E_INVAL);
  assert(anon(USER_ADDR_MAX & ~(PAGE_SIZE-1), 8192, MAP_FIXED) == E_NOMEM);
  assert(anon(0x1000, UINT64_MAX, MAP_FIXED) == E_NOMEM);
  assert(anon(0, 0, 0) == E_INVAL);
  fail_add = 1;
  assert(anon(0x10000, 4096, MAP_FIXED_NOREPLACE) == E_NOMEM);
  assert(memory.mmap_next_addr == after);
  puts("PASS: Wine high reservations preserve allocation space, fixed overlap/replacement, atomic VMA installation, bounds and failure paths");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + constants + mmap + tests)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(ROOT),
                    str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
