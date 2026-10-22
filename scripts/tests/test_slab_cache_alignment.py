#!/usr/bin/env python3
"""Execute native slab allocation/free; old code fails requested alignment."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root/'kernel/src/mm/slab_cache.h').read_text()
current = (root/'kernel/src/mm/slab_cache.c').read_text()
# Recreate the former first-object calculation without depending on Git HEAD.
old = current.replace('(uint8_t *)s + cache->object_offset',
                      '(uint8_t *)s + sizeof(struct slab_page)')
old = old.replace('page_base + cache->object_offset',
                  'page_base + sizeof(struct slab_page)')
assert old != current
shim = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static unsigned pages,locked;
static void spinlock_acquire(spinlock_t *l) { (void)l;assert(!locked++); }
static void spinlock_release(spinlock_t *l) { (void)l;assert(locked--==1); }
static uint64_t pmm_get_hhdm_offset(void) { return 0; }
static void *pmm_alloc_page(void) { void *p=NULL;assert(!posix_memalign(&p,4096,4096));pages++;return p; }
static void pmm_free_page(void *p) { assert(pages);pages--;free(p); }
static uint64_t *vmm_get_active_pml4(void) { return NULL; }
static uint64_t vmm_virt_to_phys(uint64_t *pml4,uint64_t address) { (void)pml4;return address; }
static void klog_puts(const char *text) { (void)text; }
static void klog_uint64(uint64_t value) { (void)value; }
'''
main = r'''
int main(void) {
 slab_cache_init();
 for(size_t alignment=8;alignment<=2048;alignment*=2) {
  kmem_cache_t *cache=kmem_cache_create("alignment",37,alignment,NULL,NULL);assert(cache);
  unsigned count=cache->objs_per_slab*3+1;
  void **objects=calloc(count,sizeof(void *));assert(objects);
  for(unsigned i=0;i<count;i++) {
   objects[i]=kmem_cache_alloc(cache);assert(objects[i]);
   assert(!((uintptr_t)objects[i]&(alignment-1)) && "requested slab alignment violated");
   assert(((uintptr_t)objects[i]&4095)+37<=4096);
   for(unsigned j=0;j<i;j++)assert(objects[i]!=objects[j]);
   memset(objects[i],(unsigned char)i,37);
  }
  assert(cache->active_objects==count);
  for(unsigned i=0;i<count;i++) {
   for(unsigned j=0;j<37;j++)assert(((unsigned char *)objects[i])[j]==(unsigned char)i);
   kmem_cache_free(cache,objects[i]);
  }
  assert(!cache->active_objects);
  for(unsigned i=0;i<count;i++) { objects[i]=kmem_cache_alloc(cache);assert(objects[i]); }
  for(unsigned i=0;i<count;i++)kmem_cache_free(cache,objects[i]);
  kmem_cache_shrink(cache);assert(!cache->total_slabs && !pages);
  kmem_cache_destroy(cache);free(objects);
 }
 assert(!kmem_cache_create("bad-align",32,3,NULL,NULL));
 assert(!kmem_cache_create("too-aligned",32,8192,NULL,NULL));
 assert(!kmem_cache_create("overflow",SIZE_MAX,256,NULL,NULL));
 assert(!kmem_cache_create("no-space",4096,256,NULL,NULL));
 assert(!pages && !locked);
 puts("PASS: native slab alignment 8..2048, metadata/capacity, free/reuse/shrink and invalid sizes");
}
'''
with tempfile.TemporaryDirectory(prefix='slab-alignment-') as tmp:
    for name, source in [('old',old),('fixed',current)]:
        path=Path(tmp)/name
        source=re.sub(r'^#include.*$','',source,flags=re.M)
        path.with_suffix('.c').write_text(shim+header+source+main)
        subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined',
                        str(path.with_suffix('.c')),'-o',str(path)],check=True)
        result=subprocess.run([str(path)],capture_output=True,text=True,timeout=15)
        if name=='old':
            assert result.returncode!=0 and 'requested slab alignment violated' in result.stderr,result.stderr
            print('PASS: original slab first-object misalignment fails the same regression')
        else:
            assert result.returncode==0,result.stdout+result.stderr
            print(result.stdout.strip())
