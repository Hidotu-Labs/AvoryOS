#!/usr/bin/env python3
"""The native huge-page split must report failures to teardown callers."""
from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/mm/vmm_map.c').read_text();s=s[s.index('bool vmm_split_huge_page('):]
pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define PAGE_MASK (~4095ULL)
#define PAGE_FLAG_PRESENT 1ULL
#define PAGE_FLAG_PS 128ULL
#define PHYS_TO_VIRT(x) ((uintptr_t)(x))
#define USER_SPACE_LIMIT 0x7fffffffffffULL
static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[512] __attribute__((aligned(4096)));
static uint64_t pt[512] __attribute__((aligned(4096)));
static bool fail;static unsigned flushes;
static void vmm_lock_acquire(void){}
static void vmm_lock_release(void){}
static void tlb_shootdown_context_for(uint64_t p){(void)p;flushes++;}
static void tlb_shootdown_all(void){flushes++;}
static uint64_t *get_next_level(uint64_t *p,size_t i,bool a,unsigned l){
 (void)a;(void)l;if(fail)return NULL;p[i]=(uintptr_t)pt|1;return pt;
}
'''
post=r'''
int main(void){
 assert(vmm_split_huge_page(root,0));root[0]=(uintptr_t)pdpt|1;
 pdpt[0]=129;assert(!vmm_split_huge_page(root,0));
 pdpt[0]=(uintptr_t)pd|1;pd[0]=129;fail=true;
 assert(!vmm_split_huge_page(root,0));assert(!flushes);
 fail=false;assert(vmm_split_huge_page(root,0));assert(flushes==1);
 assert(vmm_split_huge_page(root,0));assert(flushes==1);
 puts("PASS: actual huge-page split reports allocation failure and unsupported 1 GiB leaf; successful split returns true and flushes once");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-vmm-split-') as d:
 p=Path(d);(p/'test.c').write_text(pre+s+post)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
