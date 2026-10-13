#!/usr/bin/env python3
"""Exercise real LinuxKPI VMAP teardown, failure unwinding and BAR offsets."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'kernel/linuxkpi/src/vmalloc.c').read_text()
s = s[s.index('#define KPI_VMAP_BASE'):s.index('struct page *vmalloc_to_page')]
full = (root / 'kernel/linuxkpi/src/vmalloc.c').read_text()
s += full[full.index('#define KPI_IOREMAP_TRACE'):full.index('/* ── memremap')]
pre = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define GFP_KERNEL 0
#define __GFP_ZERO 1
#define ASC_PAGE_PRESENT 1
#define ASC_PAGE_RW 2
#define ASC_PAGE_NX 4
#define ASC_PAGE_PCD 8
#define ASC_PAGE_PWT 16
#define ASC_PAGE_PAT 32
#define ZERO_SIZE_PTR ((void*)16)
#define ZERO_OR_NULL_PTR(p) ((uintptr_t)(p)<=16)
#define __iomem
#define DEFINE_SPINLOCK(n) int n
#define spin_lock_irqsave(lock,flags) ((void)(lock),(flags)=0)
#define spin_unlock_irqrestore(lock,flags) ((void)(lock),(void)(flags))
typedef uint64_t resource_size_t;
typedef unsigned long pgprot_t;
typedef int gfp_t;
struct page { uint64_t phys; };
static void *kzalloc(size_t n,int f){(void)f;return calloc(1,n);}
static void *kcalloc(size_t n,size_t z,int f){(void)f;return calloc(n,z);}
static void kfree(void *p){free(p);}
static uint64_t page_to_phys(struct page *p){return p->phys;}
static struct page *alloc_page(int f){(void)f;struct page *p=malloc(sizeof(*p));p->phys=0x100000;return p;}
static void __free_page(struct page *p){free(p);}
static unsigned long linuxkpi_page_cache_flags(uint64_t p){(void)p;return 0;}
static uint64_t *asc_vmm_get_kernel_pml4(void){return NULL;}
static void asc_vmm_free_empty_tables(uint64_t *p,uint64_t a){(void)p;(void)a;}
static void asc_invlpg(uint64_t a){(void)a;}
static bool is_vmalloc_addr(const void *p){return (uintptr_t)p>=0xffffc00000000000ULL;}
static bool mapped[64];static unsigned attempts,fail_at;
static bool inject;static void *replacement;
static void *ioremap(resource_size_t,unsigned long);
static bool asc_vmm_map_page(uint64_t *p,uint64_t va,uint64_t pa,unsigned long flags){
 (void)p;(void)pa;(void)flags;unsigned i=(va-0xffffc00000000000ULL)/4096;
 assert(i<64);attempts++;if(fail_at && attempts==fail_at)return false;mapped[i]=true;return true;
}
static void asc_vmm_unmap_page(uint64_t *p,uint64_t va){
 (void)p;unsigned i=(va-0xffffc00000000000ULL)/4096;assert(i<64);
 if(inject){inject=false;replacement=ioremap(0x900000,4096);assert(replacement);}
 mapped[i]=false;
}
'''
post = r'''
int main(void){
 /* A teardown must not expose its VA to an allocation racing its PTE zaps. */
 void *a=ioremap(0x200000,8192);assert(a);inject=true;iounmap(a);
 assert(replacement!=a && mapped[((uintptr_t)replacement-KPI_VMAP_BASE)/4096]);
 iounmap(replacement);
 struct page page[2]={{0x300000},{0x301000}};struct page *pages[2]={page,page+1};
 a=vmap(pages,2,0,0);assert(a);inject=true;vfree(a);
 assert(replacement!=a && mapped[((uintptr_t)replacement-KPI_VMAP_BASE)/4096]);
 iounmap(replacement);
 /* A 4 KiB BAR range at offset 1 touches two pages, not one. */
 a=ioremap(0x400001,4096);assert(a);void *b=ioremap(0x500000,4096);assert(b);
 assert((uintptr_t)b>=((uintptr_t)a&~4095ULL)+8192);iounmap(a);iounmap(b);
 /* A partially failed mapping returns NULL and restores all reservations. */
 fail_at=attempts+2;assert(!vmap(pages,2,0,0));fail_at=0;
 for(unsigned i=0;i<64;i++)assert(!mapped[i]);
 fail_at=attempts+2;assert(!ioremap(0x600000,8192));fail_at=0;
 for(unsigned i=0;i<64;i++)assert(!mapped[i]);
 fail_at=attempts+2;assert(!vmalloc(8192));fail_at=0;
 for(unsigned i=0;i<64;i++)assert(!mapped[i]);
 assert(vmap_head.next==&vmap_head);
 assert(!vmalloc(~0UL) && !ioremap(0x600001,~0UL));
 puts("PASS: real VMAP and I/O teardown retain reservations through concurrent allocation; offsets cannot overlap; page-table failure unwinds");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vmap-life-') as d:
 p=Path(d); (p/'test.c').write_text(pre+s+post)
 flags=['cc','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined']
 subprocess.run(flags+[str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 # Reintroduce the original early-hole publication, so the regression must fail.
 mutant=s.replace('area = vmap_area_at(va);','area = vmap_area_at(va);\n  if (area) vmap_area_remove_locked(area);')
 assert mutant!=s
 (p/'test.c').write_text(pre+mutant+post)
 subprocess.run(flags+[str(p/'test.c'),'-o',str(p/'old')],check=True)
 r=subprocess.run([str(p/'old')],capture_output=True)
 assert r.returncode!=0,'old early-hole publication unexpectedly passed'
 print('PASS: original I/O teardown race fails the same regression')
