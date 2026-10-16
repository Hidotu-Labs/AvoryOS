#!/usr/bin/env python3
"""Virtually contiguous page metadata covers RAM above PCI address holes, and
every metadata allocation stays order-0 and lazy (one present-bitmap page at
init, one chunk page per 64 descriptors first touched)."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/linuxkpi/src/page.c').read_text()
def function(signature):
    start=source.index(signature)
    brace=source.index('{',start)
    end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define klogf(...) ((void)0)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define SECTION_SHIFT 27
#define PAGES_PER_SECTION (1UL << (SECTION_SHIFT-PAGE_SHIFT))
#define SECTIONS_FOR(phys) (((phys) >> SECTION_SHIFT) + 2)
/* Must mirror the kernel descriptor size (linuxkpi/include/linux/mm_types.h,
 * 64 bytes with the aligned(64) layout): the arena stride and the chunk math
 * below are derived from it. */
struct page {unsigned long pfn;int _refcount,_mapcount;unsigned long _pad[6];};
_Static_assert(sizeof(struct page)==64,"model descriptor must match kernel");
#define CHUNK_PAGES (PAGE_SIZE / sizeof(struct page))
#define CHUNK_BYTES (CHUNK_PAGES * sizeof(struct page))
#define CHUNKS_PER_SECTION (PAGES_PER_SECTION / CHUNK_PAGES)
/* The arena is one host allocation; page.c sees it through the window macros
 * exactly like the kernel sees its reserved VA range. */
static unsigned char *arena;
#define ARENA_SPAN (SECTIONS_FOR(5ULL<<30) * (PAGES_PER_SECTION * sizeof(struct page)))
#define VMAP_WINDOW_BASE ((unsigned long)arena)
#define VMAP_WINDOW_END ((unsigned long)arena + ARENA_SPAN)
#define ASC_PAGE_PRESENT 0x001ULL
#define ASC_PAGE_RW 0x002ULL
#define ASC_PAGE_NX 0x8000000000000000ULL
static uint64_t *page_chunk_present;
static int page_chunk_lock;
static bool page_model_ready;
static unsigned long max_pfn,page_offset_base,physmem_end,vmemmap_base,linuxkpi_vmemmap_size;
static int metadata_allocs,metadata_pages_max,map_calls,freed_calls;
static bool fail_next_alloc,fail_next_map;
static uint64_t last_map_va;
#define spin_lock_irqsave(lock,flags) do {(void)(lock);(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock,flags) do {(void)(lock);(void)(flags);} while(0)
#define atomic_set(p,n) (*(p)=(n))
static void *asc_pmm_alloc_pages(size_t pages) {
  void *mem;

  metadata_allocs++;
  if (pages > 1) metadata_pages_max = 1;
  if (fail_next_alloc) { fail_next_alloc = false; return NULL; }
  mem = aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
  assert(mem);
  memset(mem, 0, pages * PAGE_SIZE);
  return mem;
}
static void asc_pmm_free_pages(void *phys,size_t count) {
  (void)count; freed_calls++; free(phys);
}
static uint64_t *asc_vmm_get_kernel_pml4(void) { return (uint64_t *)0x1000; }
static bool asc_vmm_map_page(uint64_t *pml4,uint64_t va,uint64_t pa,uint64_t flags) {
  (void)pml4;
  assert((va & (PAGE_SIZE-1)) == 0 && (pa & (PAGE_SIZE-1)) == 0);
  assert(flags & ASC_PAGE_PRESENT);
  if (fail_next_map) { fail_next_map = false; return false; }
  assert(va >= (uint64_t)(uintptr_t)arena && va < (uint64_t)(uintptr_t)arena + ARENA_SPAN);
  map_calls++; last_map_va = va; return true;
}
static uint64_t asc_pmm_get_hhdm_offset(void) {return 0;}
/* 4 GiB of RAM with a PCI hole: upper RAM ends at physical address 5 GiB. */
static uint64_t asc_pmm_get_managed_memory_end(void) {return 5ULL<<30;}
'''
for signature in ('static struct page *page_arena(',
                  'static inline bool page_chunk_mapped(',
                  'static inline void page_chunk_publish(',
                  'static struct page *page_chunk_alloc(',
                  'struct page *pfn_to_page(',
                  'void linuxkpi_page_init('):
    code+=function(signature)+'\n'
code+=r'''
int main(void) {
  unsigned long span=ARENA_SPAN;
  arena=aligned_alloc(PAGE_SIZE,span); assert(arena);
  memset(arena,0,span);
  linuxkpi_page_init();
  assert(page_model_ready && max_pfn==((5ULL<<30)>>PAGE_SHIFT));
  assert(vmemmap_base==(unsigned long)arena);
  assert(linuxkpi_vmemmap_size==span);
  /* The only eager metadata is the present bitmap: one order-0 page covers
   * 8 GiB of descriptor chunks, so a 4 GiB machine pays exactly one. */
  assert(metadata_allocs==1 && metadata_pages_max==0);
  assert(max_pfn*sizeof(struct page)<=span);
  /* Section-local index 0 of the 4 GiB..4.5 GiB range sits above the hole. */
  unsigned long mid=((4ULL<<30)+(512ULL<<20))>>PAGE_SHIFT;
  unsigned long high=mid+200;
  assert(!page_chunk_mapped(mid/CHUNK_PAGES));
  assert(!page_chunk_mapped(high/CHUNK_PAGES));
  int before=metadata_allocs, maps=map_calls;
  struct page *p=pfn_to_page(high);
  assert(p && p->pfn==high);
  /* Exactly one order-0 chunk page, mapped at the chunk's arena slot. */
  assert(metadata_allocs==before+1 && metadata_pages_max==0);
  assert(map_calls==maps+1);
  assert(last_map_va==(uint64_t)arena+(uint64_t)(high/CHUNK_PAGES)*PAGE_SIZE);
  assert(page_chunk_mapped(high/CHUNK_PAGES));
  /* Same chunk: no new metadata (the lazy per-chunk contract). */
  before=metadata_allocs; maps=map_calls;
  struct page *q=pfn_to_page(high+1);
  assert(q==p+1 && q->pfn==high+1);
  assert(metadata_allocs==before && map_calls==maps);
  /* Contiguity: adjacent PFNs are adjacent arena descriptors. */
  assert(pfn_to_page(high+2)==(struct page *)((char *)arena+(uint64_t)(high+2)*sizeof(struct page)));
  /* A different chunk costs exactly one more order-0 page ... */
  assert(!page_chunk_mapped(mid/CHUNK_PAGES));
  assert(pfn_to_page(mid)->pfn==mid && metadata_allocs==before+1);
  assert(metadata_pages_max==0 && map_calls==maps+1);
  assert(page_chunk_mapped(mid/CHUNK_PAGES));
  /* Bounds: last managed page maps; the first page above it and a far page
   * above it are rejected without allocating or mapping anything. */
  assert(pfn_to_page(max_pfn-1)->pfn==max_pfn-1);
  before=metadata_allocs; maps=map_calls;
  assert(!pfn_to_page(max_pfn));
  assert(!pfn_to_page(max_pfn+4096));
  assert(metadata_allocs==before && map_calls==maps && metadata_pages_max==0);
  /* A failed chunk allocation maps nothing and stays retryable. */
  unsigned long fresh=mid+4096;
  fail_next_alloc=true;
  before=metadata_allocs; maps=map_calls;
  assert(!pfn_to_page(fresh));
  assert(metadata_allocs==before+1 && map_calls==maps);
  assert(!page_chunk_mapped(fresh/CHUNK_PAGES));
  assert(pfn_to_page(fresh)->pfn==fresh && page_chunk_mapped(fresh/CHUNK_PAGES));
  /* A failed mapping frees the chunk page and stays retryable. */
  unsigned long fresh2=fresh+4096;
  fail_next_map=true;
  int freed_before=freed_calls;
  assert(!pfn_to_page(fresh2));
  assert(freed_calls==freed_before+1);
  assert(!page_chunk_mapped(fresh2/CHUNK_PAGES));
  assert(pfn_to_page(fresh2)->pfn==fresh2);
  assert(metadata_pages_max==0);
  puts("PASS: arena metadata covers high RAM, is order-0 and lazy per chunk");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-pfn-bound-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
