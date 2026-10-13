#!/usr/bin/env python3
"""Sparse page metadata covers RAM above PCI address holes."""
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
#include <stdio.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define SECTION_SHIFT 27
#define PAGES_PER_SECTION (1UL << (SECTION_SHIFT-PAGE_SHIFT))
#define SECTIONS_FOR(phys) (((phys) >> SECTION_SHIFT) + 2)
struct page {unsigned long pfn;int _refcount,_mapcount;};
static struct page **mem_section;
static unsigned long mem_section_count,max_pfn,page_offset_base,physmem_end;
static bool page_model_ready;
static int page_section_lock;
#define spin_lock_irqsave(lock,flags) do {(void)(lock);(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock,flags) do {(void)(lock);(void)(flags);} while(0)
#define atomic_set(p,n) (*(p)=(n))
static void *asc_pmm_alloc_pages(size_t pages) {return calloc(pages,PAGE_SIZE);}
static uint64_t asc_pmm_get_hhdm_offset(void) {return 0;}
/* 4 GiB of RAM with a PCI hole: upper RAM ends at physical address 5 GiB. */
static uint64_t asc_pmm_get_managed_memory_end(void) {return 5ULL<<30;}
'''
for signature in ('static struct page *page_section_alloc(',
                  'struct page *pfn_to_page(',
                  'void linuxkpi_page_init('):
    code+=function(signature)+'\n'
code+=r'''
int main(void) {
 linuxkpi_page_init();
 assert(page_model_ready && max_pfn==((5ULL<<30)>>PAGE_SHIFT));
 /* This allocation is valid RAM despite exceeding the total number of bytes. */
 unsigned long high=((4ULL<<30)+(512ULL<<20))>>PAGE_SHIFT;
 struct page *p=pfn_to_page(high);
 assert(p && p->pfn==high);
 assert(pfn_to_page(max_pfn-1)->pfn==max_pfn-1);
 assert(!pfn_to_page(max_pfn));
 for(unsigned long i=0;i<mem_section_count;i++)free(mem_section[i]);
 free(mem_section);
 puts("PASS: sparse PFN metadata covers high RAM across PCI holes and rejects addresses above managed RAM");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-pfn-bound-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
