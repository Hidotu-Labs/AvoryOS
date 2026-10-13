#!/usr/bin/env python3
"""Real native empty-table reclamation must retain shared kernel root tables."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/mm/vmm_map.c').read_text()
f=s[s.index('void vmm_free_empty_tables('):s.index('void vmm_unmap_page(')]
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_FLAG_PRESENT 1ULL
#define PAGE_FLAG_PS 128ULL
#define PAGE_MASK (~4095ULL)
#define PHYS_TO_VIRT(p) ((uintptr_t)(p))
static uint64_t *kernel;
static void *freed[1024];static unsigned nfreed,flushes;
static uint64_t *vmm_get_kernel_pml4(void){return kernel;}
static void vmm_dbg_out(const char *s){(void)s;}
static void vmm_dbg_hex(uint64_t n){(void)n;}
static bool vmm_dbg_range_has_lapic(uint64_t va,uint64_t size){(void)va;(void)size;return false;}
static void tlb_flush_deferred(uint64_t va,uint64_t cr3){(void)va;(void)cr3;flushes++;}
static void vmm_defer_table_free(void *p){assert(nfreed<1024);freed[nfreed++]=p;memset(p,0xee,4096);}
static uint64_t *table(void){void *p=aligned_alloc(4096,4096);assert(p);memset(p,0,4096);return p;}
'''
post=r'''
int main(void){
 kernel=table();uint64_t *shared=table();kernel[384]=(uintptr_t)shared|1;
 uint64_t *process[2]={table(),table()};process[0][384]=process[1][384]=kernel[384];
 for(unsigned cycle=0;cycle<100;cycle++){
  uint64_t *pd=table(),*pt=table();shared[0]=(uintptr_t)pd|1;pd[0]=(uintptr_t)pt|1;
  pt[0]=(0x12345000+cycle*4096)|1;
  for(unsigned p=0;p<2;p++){
   assert(process[p][384]==kernel[384]);
   uint64_t *view=(void*)(uintptr_t)(process[p][384]&PAGE_MASK);
   assert(view==shared && view[0]==((uintptr_t)pd|1));
  }
  pt[0]=0;unsigned before=nfreed;
  vmm_free_empty_tables(kernel,0xffffc00000000000ULL);
  assert(nfreed==before+2 && !shared[0]);
  assert(kernel[384]==((uintptr_t)shared|1));
  assert(process[0][384]==kernel[384] && process[1][384]==kernel[384]);
  assert(freed[before]==pt && freed[before+1]==pd);free(pt);free(pd);
 }
 /* User roots have no shared top-level identity and remain reclaimable. */
 uint64_t *user=table(),*pdpt=table(),*pd=table(),*pt=table();
 user[0]=(uintptr_t)pdpt|1;pdpt[0]=(uintptr_t)pd|1;pd[0]=(uintptr_t)pt|1;
 unsigned before=nfreed;vmm_free_empty_tables(user,0);
 assert(!user[0] && nfreed==before+3);free(pt);free(pd);free(pdpt);free(user);
 /* Reject reclaim through a process's shallow kernel entry. */
 before=nfreed;vmm_free_empty_tables(process[0],0xffffc00000000000ULL);assert(nfreed==before);
 free(process[0]);free(process[1]);free(shared);free(kernel);
 assert(flushes==203);
 puts("PASS: 100 VMAP map/unmap cycles retain the shared kernel PDPT in two process roots, reclaim child tables, and preserve full user-table reclamation");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-shared-vmap-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+f+post)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 old=f.replace('pdpt_empty && pml4_index < 256','pdpt_empty');assert old!=f
 (p/'test.c').write_text(pre+old+post)
 subprocess.run(['cc','-O2',str(p/'test.c'),'-o',str(p/'old')],check=True)
 result=subprocess.run([str(p/'old')],capture_output=True)
 assert result.returncode!=0,'shared-root PDPT recycling passed'
 print('PASS: old kernel PDPT reclamation fails on the first VMAP teardown')
# Exercise the actual LinuxKPI pre-population entry point as well: kernel
# roots must be writable before cloning, without a live zero-page alias.
v=(root/'kernel/linuxkpi/src/vmalloc.c').read_text()
init=v[v.index('void linuxkpi_vmalloc_init(void) {'):]
pre_init=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define KPI_VMAP_BASE 0xffffc00000000000ULL
#define KPI_VMAP_END 0xffffe00000000000ULL
#define KPI_VMAP_PREPOP_STRIDE 0x8000000000ULL
#define KPI_VMAP_PREPOP_ENTRIES 64
#define ASC_PAGE_PRESENT 1ULL
#define ASC_PAGE_RW 2ULL
#define ASC_PAGE_NX (1ULL<<63)
static bool vmap_ready;
static uint64_t roots[512];static unsigned maps,unmaps;static bool leaf[512];
static uint64_t asc_pmm_get_zero_page_phys(void){return 0x1234000;}
static uint64_t *asc_vmm_get_kernel_pml4(void){return roots;}
static bool asc_vmm_map_page(uint64_t *p,uint64_t va,uint64_t pa,uint64_t flags){
 assert(p==roots && pa==0x1234000 && flags==(ASC_PAGE_PRESENT|ASC_PAGE_RW|ASC_PAGE_NX));
 unsigned i=(va>>39)&511;assert(i>=384 && i<448 && !leaf[i]);roots[i]=3;leaf[i]=true;maps++;return true;
}
static void asc_vmm_unmap_page(uint64_t *p,uint64_t va){
 assert(p==roots);unsigned i=(va>>39)&511;assert(leaf[i] && roots[i]==3);leaf[i]=false;unmaps++;
}
'''
post_init=r'''
int main(void){
 linuxkpi_vmalloc_init();assert(vmap_ready && maps==64 && unmaps==64);
 for(unsigned i=384;i<448;i++)assert(roots[i]==3 && !leaf[i]);
 linuxkpi_vmalloc_init();assert(maps==64 && unmaps==64);
 puts("PASS: actual VMAP initialization prepares 64 writable shared roots without retaining zero-page aliases");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vmap-init-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre_init+init+post_init)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
