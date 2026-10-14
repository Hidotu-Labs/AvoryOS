#!/usr/bin/env python3
"""GPU-free regression: owned wrapper plus imported doorbell/free BO functions.

TTM/platform dependencies are modeled; manager destruction rejects a live BO.
The negative control restores the former order and must fail. No hardware I/O.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AMD = ROOT / 'kernel/linux/drivers/gpu/drm/amd/amdgpu'


def function(path, name):
    source = path.read_text()
    match = re.search(r'^(?:void|int)\s+' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
typedef uint64_t u64;
struct amdgpu_device;
struct ttm_device { struct amdgpu_device *owner; bool manager_alive; unsigned live; };
struct amdgpu_bo { struct { struct ttm_device *bdev; } tbo; bool pinned, mapped, reserved; unsigned priority; };
struct amdgpu_device { struct { bool initialized; struct ttm_device bdev; } mman;
 struct { struct amdgpu_bo *kernel_doorbells; void *cpu_addr; } doorbell; bool in_suspend; };
#define WARN_ON(x) assert(!(x))
#define likely(x) (x)
static unsigned releases, moves, teardown_calls;
static struct amdgpu_device *amdgpu_ttm_adev(struct ttm_device *d) { return d->owner; }
static int amdgpu_bo_reserve(struct amdgpu_bo *b, bool no_intr) {
 assert(no_intr && b->tbo.bdev->manager_alive && !b->reserved); b->reserved=true; return 0;
}
static void amdgpu_bo_kunmap(struct amdgpu_bo *b) { assert(b->reserved && b->mapped); b->mapped=false; }
static void amdgpu_bo_unpin(struct amdgpu_bo *b) { assert(b->reserved && b->pinned); b->pinned=false; }
static void amdgpu_bo_unreserve(struct amdgpu_bo *b) {
 /* Real unreserve moves the unpinned resource to its manager's priority LRU. */
 assert(b->reserved && !b->pinned && b->priority==2 && b->tbo.bdev->manager_alive);
 moves++; b->reserved=false;
}
static void amdgpu_bo_unref(struct amdgpu_bo **b) {
 assert(!(*b)->reserved && !(*b)->mapped && !(*b)->pinned);
 (*b)->tbo.bdev->live--; releases++; free(*b); *b=NULL;
}
void linuxkpi_original_amdgpu_ttm_fini(struct amdgpu_device *a) {
 teardown_calls++;
 if (!a->mman.initialized) return;
 /* Manager fini cannot evict the still pinned kernel doorbell allocation. */
 assert(!a->mman.bdev.live && !a->doorbell.kernel_doorbells && !a->doorbell.cpu_addr);
 a->mman.bdev.manager_alive=false; a->mman.initialized=false;
}
static void setup(struct amdgpu_device *a, bool initialized, bool doorbell) {
 *a=(struct amdgpu_device){0}; a->mman.initialized=initialized;
 a->mman.bdev.owner=a; a->mman.bdev.manager_alive=initialized;
 if (doorbell) {
  assert(initialized); struct amdgpu_bo *b=calloc(1,sizeof(*b)); assert(b);
  b->tbo.bdev=&a->mman.bdev; b->priority=2; b->pinned=b->mapped=true;
  a->doorbell.kernel_doorbells=b; a->doorbell.cpu_addr=b; a->mman.bdev.live=1;
 }
}
'''
MAIN = r'''
int main(void) {
 struct amdgpu_device a,b;
 /* Failure before TTM init, before doorbell allocation, and after allocation;
  * the latter also models PSP failure and normal device release. */
 for (unsigned stage=0; stage<3; stage++) {
  setup(&a,stage>0,stage==2); setup(&b,true,true);
  unsigned before=releases;
  amdgpu_ttm_fini(&a);
  assert(releases==before+(stage==2) && !a.mman.initialized);
  assert(b.mman.bdev.manager_alive && b.doorbell.kernel_doorbells);
  amdgpu_doorbell_fini(&a); amdgpu_ttm_fini(&a);
  assert(releases==before+(stage==2));
  amdgpu_ttm_fini(&b); amdgpu_doorbell_fini(&b);
 }
 /* MMIO disconnect can free the BO before GMC software teardown. */
 setup(&a,true,true); amdgpu_doorbell_fini(&a);
 assert(a.mman.bdev.manager_alive && !a.doorbell.kernel_doorbells && !a.doorbell.cpu_addr);
 amdgpu_ttm_fini(&a); amdgpu_doorbell_fini(&a);
 assert(releases==5 && moves==5 && teardown_calls==10);
 puts("PASS: kernel doorbell release precedes manager destruction; partial-init, repeated/early cleanup and device isolation");
}
'''

with tempfile.TemporaryDirectory(prefix='amdgpu-doorbell-') as tmp:
    p = Path(tmp)
    for header in ('amdgpu.h', 'amdgpu_doorbell.h', 'amdgpu_ttm.h'):
        (p / header).write_text('')
    wrapper = ROOT / 'kernel/linuxkpi/src/amdgpu_teardown.c'
    imported = function(AMD / 'amdgpu_object.c', 'amdgpu_bo_free_kernel') + '\n'
    imported += function(AMD / 'amdgpu_doorbell_mgr.c', 'amdgpu_doorbell_fini') + '\n'
    source = SHIM + imported + wrapper.read_text() + MAIN
    flags = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
             '-DAVORYOS_AMDGPU_MEMORY_POLICY', '-I' + tmp]
    (p / 'test.c').write_text(source)
    subprocess.run(flags + ['-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True, timeout=10)
    (p / 'old.c').write_text(source.replace('if (adev->mman.initialized)\n    amdgpu_doorbell_fini(adev);', '/* former order */'))
    subprocess.run(flags + [str(p / 'old.c'), '-o', str(p / 'old')], check=True)
    result = subprocess.run([str(p / 'old')], stderr=subprocess.PIPE, timeout=10)
    assert result.returncode != 0 and b'!a->mman.bdev.live' in result.stderr
    subprocess.run(['cc', '-nostdinc', '-c', str(wrapper), '-o', str(p / 'disabled.o')], check=True)
    print('PASS: former-order negative control fails on live doorbell BO; AMD-disabled compilation')
