#!/usr/bin/env python3
"""New native GEM storage must be black before it is published or mapped."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/drivers/gpu/drm/drm_gem.c').read_text()
start = source.index('struct drm_gem_object *ascentdrm_gem_object_create(')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct drm_device { int lock, gem_objects; unsigned next_gem_handle; };
struct drm_gem_object {
 struct drm_device *dev;
 size_t size;
 uint64_t phys_addr;
 void *virt_addr;
 int cache_mode, refcount, list;
 unsigned handle;
};
#define DRM_GEM_CACHE_WB 0
static unsigned char storage[8192];
static int fail_alloc, published;
static void *kmalloc(size_t n) { return malloc(n); }
static void kfree(void *p) { free(p); }
static void *pmm_alloc_blocks(unsigned pages) {
 assert(pages == 2); memset(storage, 0xa5, sizeof(storage));
 return fail_alloc ? NULL : storage;
}
static uint64_t pmm_get_hhdm_offset(void) { return 0; }
static void spinlock_acquire(int *p) { assert(!*p); *p=1; }
static void spinlock_release(int *p) { assert(*p); *p=0; }
static void list_add_tail(int *entry, int *head) {
 (void)entry; (void)head;
 for (unsigned i=0; i<sizeof(storage); i++) assert(storage[i] == 0);
 published++;
}
'''
code += function
code += r'''
int main(void) {
 struct drm_device dev = {.next_gem_handle=1};
 /* Includes the page padding exposed through mmap, not just visible pixels. */
 struct drm_gem_object *obj = ascentdrm_gem_object_create(&dev, 4097);
 assert(obj && obj->size==8192 && published==1 && obj->handle==1);
 for (unsigned i=0; i<8192; i++) assert(((unsigned char *)obj->virt_addr)[i]==0);
 free(obj);
 fail_alloc=1;
 assert(!ascentdrm_gem_object_create(&dev, 4097));
 assert(published==1);
 puts("PASS: recycled GEM pages and mapping padding cleared before publication; failed allocations unpublished");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-gem-init-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
