#!/usr/bin/env python3
"""mem_map arena: block descriptors are adjacent, so upstream raw-pointer
walks stay inside the block.

The old chunked mem_map stored descriptors in scattered order-0 pages, so any
multi-page walk (`&head[i]`, `++p`, `p + i`) that crossed a chunk boundary
wrote into whatever physical page followed the chunk storage: PMM free-list
corruption during amdgpu init, bogus TTM pool page arrays (`LOAD_IP_FW`
failures) and a teardown KPF in amdgpu_ttm_tt_unpopulate().  The arena maps
those same order-0 chunk pages at adjacent VA slots, so upstream pointer
arithmetic is correct again — the two TTM loops and nth_page() are back to
their upstream shape, and the mem_map-specific helpers are gone.

The test slices the real helpers into a host harness whose arena is one host
allocation (contiguous VA) backed lazily one order-0 page per chunk, then
drives an order-2 block across the old chunk boundary through every writer.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/page.c').read_text()


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


# Source guards: the sparse-chunk machinery must stay gone, and the arena
# lookup plus lazy mapping must be the only path pfn_to_page() uses.
assert 'page_block_entry' not in source
assert 'mem_section' not in source
assert 'page_arena(' in source and 'asc_vmm_map_page(' in source
assert 'page_chunk_publish(' in source

# Block-metadata writers walk raw descriptors again, which the arena makes
# valid for any block (they must not reintroduce a boundary helper).
for signature in ('static void page_set_order(',
                  'static void page_set_order_plain(',
                  'void split_page('):
    body = function(source, signature)
    assert 'head +' in body and 'page_block_entry' not in body, signature

# nth_page() is the upstream pointer form again; the PFN workaround is gone.
mm = (root / 'kernel/linuxkpi/include/linux/mm.h').read_text()
assert '#define nth_page(page, n) ((page) + (n))' in mm
assert 'pfn_to_page(page_to_pfn(page)' not in mm

# TTM's pool walks are upstream again (`++p` fill, `p + i` clears) and the
# KPI-specific divergence patch for them does not exist any more.
pool = (root / 'kernel/linux/drivers/gpu/drm/ttm/ttm_pool.c').read_text()
alloc = function(pool, 'static int ttm_pool_page_allocated(')
give = function(pool, 'static void ttm_pool_type_give(')
assert '++p' in alloc and '**pages = p;' in alloc and 'nth_page(' not in alloc
assert 'p + i' in give and 'nth_page(' not in give
patch = root / 'scripts/linux/patches/0003-ttm-pool-chunked-memmap-walks.patch'
assert not patch.exists(), patch

# The vmap window's first-fit allocator must start above the arena.
vm = (root / 'kernel/linuxkpi/src/vmalloc.c').read_text()
assert 'uint64_t cursor = vmap_floor;' in vm
assert 'vmap_floor = arena_end;' in vm

code = r'''
#define _GNU_SOURCE
#include <sys/mman.h>
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
 * 64 bytes with the aligned(64) layout). */
struct page {unsigned long flags;unsigned int pfn;int _refcount,_mapcount;
             unsigned int cache_flags;struct page *compound_head;
             unsigned long compound_order;void *mapping;unsigned long index,private;};
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
#define PG_head_bit (1UL << 6)
#define PG_tail_bit (1UL << 21)
#define __SetPageTail(p)   ((p)->flags |= PG_tail_bit)
#define __ClearPageTail(p) ((p)->flags &= ~PG_tail_bit)
#define __SetPageHead(p)   ((p)->flags |= PG_head_bit)
#define __ClearPageHead(p) ((p)->flags &= ~PG_head_bit)
/* Mirrors the restored upstream mm.h definition. */
#define nth_page(page, n) ((page) + (n))
static bool page_tail(const struct page *p) {return (p->flags & PG_tail_bit)!=0;}
static struct page *compound_head(struct page *page) {
  if (page_tail(page)) return page->compound_head;
  return page;
}
static uint64_t *page_chunk_present;
static int page_chunk_lock;
static bool page_model_ready;
static unsigned long max_pfn,page_offset_base,physmem_end,vmemmap_base,linuxkpi_vmemmap_size;
static int metadata_allocs,metadata_pages_max,map_calls;
static uint64_t data_phys;
static size_t data_count;
static int data_frees,fail_alloc_after=-1,fail_map_after=-1;
#define MAX_PAGE_ORDER 10
#define __GFP_DMA32 1
#define __GFP_DIRECT_RECLAIM 2
#define __GFP_NORETRY 4
#define __GFP_COMP 8
#define __GFP_ZERO 16
#define GPU_DIAG_PAGE_ALLOC 0
#define atomic_read(p) (*(p))
typedef unsigned int gfp_t;
static void asc_pmm_drain_cached_frames(void) {}
static unsigned long linuxkpi_shrink_reclaim(gfp_t g,unsigned long n) {(void)g;(void)n;return 0;}
static unsigned long (*asc_mm_file_cache_reclaim)(unsigned long);
#define asc_pmm_get_free_pages() 0UL
static bool gpu_diag_resource_enabled(void) {return false;}
#define gpu_diag_record(...) ((void)0)
static void *page_address(struct page *p) {(void)p;abort();}

#define spin_lock_irqsave(lock,flags) do {(void)(lock);(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock,flags) do {(void)(lock);(void)(flags);} while(0)
#define atomic_set(p,n) (*(p)=(n))
static void *asc_pmm_alloc_pages(size_t pages) {
  void *mem;

  if (data_phys && pages == data_count) return (void *)(uintptr_t)data_phys;
  if (fail_alloc_after == 0) {fail_alloc_after=-1;return NULL;}
  if (fail_alloc_after > 0) fail_alloc_after--;
  metadata_allocs++;
  if (pages > 1) metadata_pages_max = 1;
  mem = aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
  assert(mem);
  memset(mem, 0, pages * PAGE_SIZE);
  return mem;
}
static void *asc_pmm_alloc_pages_range(size_t n,uint64_t lo,uint64_t hi) {
  (void)lo;(void)hi;return asc_pmm_alloc_pages(n);
}
static void asc_pmm_free_pages(void *phys,size_t count) {
  if ((uint64_t)(uintptr_t)phys == data_phys) {
    assert(count == data_count);data_frees++;return;
  }
  assert(count == 1);free(phys);
}
static uint64_t *asc_vmm_get_kernel_pml4(void) { return (uint64_t *)0x1000; }
static bool asc_vmm_map_page(uint64_t *pml4,uint64_t va,uint64_t pa,uint64_t flags) {
  (void)pml4;
  assert((va & (PAGE_SIZE-1)) == 0 && (pa & (PAGE_SIZE-1)) == 0);
  assert(flags & ASC_PAGE_PRESENT);
  assert(va >= (uint64_t)(uintptr_t)arena && va < (uint64_t)(uintptr_t)arena + ARENA_SPAN);
  if (fail_map_after == 0) {fail_map_after=-1;return false;}
  if (fail_map_after > 0) fail_map_after--;
  assert(mprotect((void *)(uintptr_t)va,PAGE_SIZE,PROT_READ|PROT_WRITE)==0);
  map_calls++; return true;
}
static uint64_t asc_pmm_get_hhdm_offset(void) {return 0;}
static uint64_t asc_pmm_get_managed_memory_end(void) {return 5ULL<<30;}
'''
for signature in ('static struct page *page_arena(',
                  'static inline bool page_chunk_mapped(',
                  'static inline void page_chunk_publish(',
                  'static struct page *page_chunk_alloc(',
                  'struct page *pfn_to_page(',
                  'static void page_set_order(',
                  'static void page_set_order_plain(',
                  'struct page *alloc_pages(',
                  'void split_page(',
                  'void linuxkpi_page_init('):
    code += function(source, signature) + '\n'
code += r'''
int main(void) {
  unsigned long span=ARENA_SPAN;
  arena=mmap(NULL,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  assert(arena!=MAP_FAILED);
  linuxkpi_page_init();
  assert(page_model_ready);

  /* Block head at chunk slot 62: an order-2 block spans slots 62,63 of one
   * chunk and slots 0,1 of the next — the old boundary. */
  unsigned long mid=((4ULL<<30)+(512ULL<<20))>>PAGE_SHIFT;
  unsigned long head_pfn=mid+CHUNK_PAGES-2;
  int before=metadata_allocs, maps=map_calls;
  struct page *head=pfn_to_page(head_pfn);
  assert(head->pfn==head_pfn);
  assert(metadata_allocs==before+1 && map_calls==maps+1);

  /* In-chunk step: plain pointer arithmetic. */
  assert(pfn_to_page(head_pfn+1)==head+1);

  /* Crossing step: one more lazy order-0 chunk, and the descriptor is still
   * adjacent — `head + 2` is the contract upstream walks rely on. */
  before=metadata_allocs; maps=map_calls;
  assert(pfn_to_page(head_pfn+2)==head+2);
  assert(metadata_allocs==before+1 && map_calls==maps+1 && metadata_pages_max==0);
  assert(pfn_to_page(head_pfn+3)==head+3 && metadata_allocs==before+1);
  for (unsigned long i=0;i<4;i++) {
    assert(pfn_to_page(head_pfn+i)==head+i);
    assert((head+i)->pfn==head_pfn+i);
  }

  /* Upstream TTM shape: fill the page array by stepping `++p`, then
   * resolve with `p + i` — both must land on the block's own descriptors. */
  struct page *filled[4], **slot=filled, *step=head;
  for (int n=4;n;--n,++slot,++step) *slot=step;
  for (unsigned long i=0;i<4;i++) {
    assert(filled[i]==pfn_to_page(head_pfn+i));
    assert(nth_page(head,i)==pfn_to_page(head_pfn+i));
    assert(nth_page(head,i)==head+i);
  }

  /* page_set_order_plain() writes every descriptor of the block (raw walk). */
  page_set_order_plain(head,2);
  for (unsigned long i=0;i<4;i++) {
    struct page *p=pfn_to_page(head_pfn+i);
    assert(p==head+i);
    assert(p->_refcount==1 && p->_mapcount==-1);
    assert(p->compound_head==p && p->compound_order==0);
    assert(!(p->flags & (PG_head_bit|PG_tail_bit)));
  }

  /* page_set_order() marks the tail pages of a compound block. */
  page_set_order(head,2);
  for (unsigned long i=1;i<4;i++) {
    struct page *t=pfn_to_page(head_pfn+i);
    assert(page_tail(t) && t->compound_head==head);
  }
  assert((head->flags & PG_head_bit) && !page_tail(head));
  assert(head->compound_order==2);

  /* split_page() clears the compound state across the boundary too. */
  split_page(head,2);
  for (unsigned long i=0;i<4;i++) {
    struct page *p=pfn_to_page(head_pfn+i);
    assert(p->compound_head==p && !page_tail(p));
    assert(p->_refcount==1 && p->_mapcount==-1);
  }
  /* Actual allocator: order 8 spans four fresh metadata chunks, exactly
   * the size from the panic. PROT_NONE catches any unprepared raw walk. */
  data_count=256;
  for (unsigned int comp=0;comp<2;comp++) {
    unsigned long first=8192+comp*1024;
    data_phys=first<<PAGE_SHIFT;
    before=map_calls;
    head=alloc_pages(comp ? __GFP_COMP : 0,8);
    assert(head==page_arena(first) && map_calls==before+4);
    for (unsigned long i=0;i<data_count;i++) {
      assert(head[i].pfn==first+i);
      assert(head[i]._refcount==(comp && i ? 0 : 1));
      assert(head[i].compound_head==(comp ? head : head+i));
    }
    split_page(head,8);
  }

  /* Fail a later metadata allocation or mapping after the first chunk has
   * succeeded. The entire data block is returned, and retry reuses the
   * published chunk without exposing partially initialized block state. */
  for (unsigned int map_failure=0;map_failure<2;map_failure++) {
    unsigned long first=12288+map_failure*1024;
    data_phys=first<<PAGE_SHIFT;
    int freed=data_frees;
    if (map_failure) fail_map_after=1; else fail_alloc_after=1;
    assert(!alloc_pages(__GFP_COMP,8));
    assert(data_frees==freed+1);
    assert(page_chunk_mapped(first/CHUNK_PAGES));
    assert(!page_chunk_mapped(first/CHUNK_PAGES+1));
    assert(page_arena(first)->_refcount==0 && page_arena(first)->flags==0);
    head=alloc_pages(__GFP_COMP,8);assert(head==page_arena(first));
    assert(head[255].pfn==first+255 && head[255].compound_head==head);
  }
  /* Reject ranges crossing max_pfn without mapping any descriptors. */
  data_phys=(max_pfn-128)<<PAGE_SHIFT;
  before=map_calls;
  int freed=data_frees;
  assert(!alloc_pages(0,8) && data_frees==freed+1 && map_calls==before);
  data_phys=4096UL<<PAGE_SHIFT;data_count=1;
  before=map_calls;
  assert(alloc_pages(0,0)==page_arena(4096) && map_calls==before+1);
  assert(metadata_pages_max==0);
  puts("PASS: mem_map arena keeps block descriptors adjacent for raw walks");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vmemmap-arena-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
