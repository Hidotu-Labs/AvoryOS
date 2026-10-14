#!/usr/bin/env python3
"""Exercise TTM cache transitions and Linux mmap protections in native code."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2]
x=(root/'kernel/linuxkpi/src/x86_stubs.c').read_text()
x=x[x.index('unsigned long cachemode2protval('):x.index('/* Strong override')]
m=(root/'kernel/linuxkpi/src/mmap.c').read_text()
m=m[m.index('static unsigned long kpi_vma_pte_flags('):m.index('\nvoid zap_vma_ptes(')]
if len(sys.argv)>1:
 x=Path(sys.argv[1]).read_text()
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~4095UL)
#define ASC_PAGE_PRESENT 1UL
#define ASC_PAGE_RW 2UL
#define ASC_PAGE_USER 4UL
#define ASC_PAGE_PWT 8UL
#define ASC_PAGE_PCD 16UL
#define ASC_PAGE_PAT 128UL
#define ASC_PAGE_NX (1UL<<63)
#define _PAGE_PCD ASC_PAGE_PCD
#define _PAGE_PWT ASC_PAGE_PWT
#define GFP_KERNEL 0
#define VM_WRITE 1
#define VM_EXEC 2
#define VM_IO 4
#define VM_FAULT_SIGBUS 2
#define VM_FAULT_NOPAGE 0
enum {GPU_DIAG_CACHE=4,GPU_DIAG_MAP=5};
static int irqs_disabled(void){return 0;}
static void gpu_diag_cache_check(const void *o,uint64_t p,uint64_t f){(void)o;(void)p;(void)f;}
static void gpu_diag_record(unsigned int k,const void *o,uint64_t a,uint64_t b,uint64_t c,uint64_t d,long r){(void)k;(void)o;(void)a;(void)b;(void)c;(void)d;(void)r;}
#define kmalloc_array(n,s,f) calloc(n,s)
#define kfree free
typedef struct {unsigned long val;} pgprot_t;
#define pgprot_val(p) ((p).val)
#define __pgprot(v) ((pgprot_t){v})
enum page_cache_mode {_PAGE_CACHE_MODE_UC,_PAGE_CACHE_MODE_UC_MINUS,_PAGE_CACHE_MODE_WC,_PAGE_CACHE_MODE_WP,_PAGE_CACHE_MODE_WB};
struct page {unsigned long pfn;unsigned int cache_flags;};
struct vm_area_struct {unsigned long vm_start,vm_end,vm_flags;pgprot_t vm_page_prot;};
struct vm_fault {struct vm_area_struct *vma;unsigned long address;};
typedef int vm_fault_t;
static unsigned char ram[3*PAGE_SIZE] __attribute__((aligned(4096)));
static struct page pages[3];
static unsigned long base_pfn;
static unsigned long direct_flags[3],user_flags[3],mapped_phys[3];
static int fail_page=-1, map_calls, refs;
static struct page *pfn_to_page(unsigned long pfn){return pfn>=base_pfn&&pfn<base_pfn+3?&pages[pfn-base_pfn]:NULL;}
static unsigned long page_to_pfn(struct page *p){return p->pfn;}
static uint64_t page_to_phys(struct page *p){return p->pfn<<PAGE_SHIFT;}
static bool asc_pmm_is_managed(uint64_t phys){return phys>=(uintptr_t)ram&&phys<(uintptr_t)ram+sizeof(ram);}
static uint64_t asc_pmm_get_hhdm_offset(void){return 0;}
static __UINT64_TYPE__ *asc_vmm_get_kernel_pml4(void){return (__UINT64_TYPE__ *)1;}
static __UINT64_TYPE__ *asc_vmm_get_active_pml4(void){return (__UINT64_TYPE__ *)2;}
static bool asc_vmm_map_page(__UINT64_TYPE__ *root,uint64_t va,uint64_t pa,uint64_t flags){
 map_calls++;
 if(root==(__UINT64_TYPE__ *)1){int i=(pa-(uintptr_t)ram)/PAGE_SIZE;if(i==fail_page){fail_page=-1;return false;}assert(i>=0&&i<3);direct_flags[i]=flags;}
 else{int i=(va-0x10000)/PAGE_SIZE;assert(i>=0&&i<3);user_flags[i]=flags;mapped_phys[i]=pa;}
 return true;
}
static void asc_invlpg(uint64_t va){(void)va;}
static void get_page(struct page *p){(void)p;refs++;}
'''
post=r'''
int main(void){
 base_pfn=(uintptr_t)ram>>PAGE_SHIFT;
 for(int i=0;i<3;i++)pages[i].pfn=base_pfn+i;
 struct page *array[]={&pages[0],&pages[1],&pages[2]};
 unsigned long cache_mask=ASC_PAGE_PAT|ASC_PAGE_PWT|ASC_PAGE_PCD,wc=cache_mask;
 assert(cachemode2protval(_PAGE_CACHE_MODE_WC)==wc);
 assert((pgprot_val(pgprot_writecombine(__pgprot(ASC_PAGE_USER|ASC_PAGE_RW)))&cache_mask)==wc);
 assert(!set_pages_array_wc(array,3));
 for(int i=0;i<3;i++){assert(pages[i].cache_flags==wc);assert((direct_flags[i]&cache_mask)==wc);}
 struct vm_area_struct v={.vm_start=0x10000,.vm_end=0x13000,.vm_flags=VM_IO|VM_WRITE,.vm_page_prot=__pgprot(0)};
 /* VM_IO is also used for GTT. Match the physical page, not the flag. */
 assert(!remap_pfn_range(&v,v.vm_start,base_pfn,3*PAGE_SIZE,__pgprot(0)));
 for(int i=0;i<3;i++){assert((user_flags[i]&cache_mask)==wc);assert(mapped_phys[i]==page_to_phys(array[i]));}
 assert(!set_pages_array_uc(array,3));
 for(int i=0;i<3;i++)assert(pages[i].cache_flags==(ASC_PAGE_PCD|ASC_PAGE_PWT));
 assert(!set_pages_wb(pages,3));
 for(int i=0;i<3;i++)assert(!pages[i].cache_flags);
 assert(!vm_insert_page(&v,v.vm_start,&pages[0]));assert(!(user_flags[0]&cache_mask)&&refs==1);
 assert(vmf_insert_pfn_prot(&v,v.vm_start,base_pfn,__pgprot(wc))==VM_FAULT_NOPAGE);
 assert(!(user_flags[0]&cache_mask));
 /* BAR mappings retain WC; registers with no explicit type are true UC. */
 assert(vmf_insert_pfn_prot(&v,v.vm_start,0xdead,__pgprot(wc))==VM_FAULT_NOPAGE);
 assert((user_flags[0]&cache_mask)==wc);
 assert(vmf_insert_pfn_prot(&v,v.vm_start,0xdead,__pgprot(0))==VM_FAULT_NOPAGE);
 assert((user_flags[0]&cache_mask)==(ASC_PAGE_PCD|ASC_PAGE_PWT));
 /* A failed transition must restore the preceding pages and their aliases. */
 fail_page=1;assert(set_pages_array_wc(array,3)==-ENOMEM);
 for(int i=0;i<3;i++){assert(!pages[i].cache_flags);assert(!(direct_flags[i]&cache_mask));}
 int before=map_calls;struct page *bad[]={pages,NULL};assert(set_pages_array_wc(bad,2)==-EINVAL);assert(map_calls==before);
 assert(set_pages_array_wc(NULL,0)==0);assert(set_pages_array_wc(NULL,1)==-EINVAL);
 assert(set_pages_array_wc(array,-1)==-EINVAL);
 puts("PASS: TTM WC/UC/WB, RAM/BAR mmap aliases and failed-transition rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-cache-') as d:
 src=Path(d)/'test.c';exe=Path(d)/'test';src.write_text(pre+x+m+post)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
