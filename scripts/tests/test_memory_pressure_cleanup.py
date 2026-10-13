#!/usr/bin/env python3
"""Run real NVMe/reclaim cleanup functions with deterministic failure fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def extract(path, signature):
    source = path.read_text()
    start = source.index(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096
#define FS_PAGE_CACHE 128
#define MAP_PAGECACHE (1ULL<<34)
#define klogf(...) ((void)0)
struct nvme_controller {bool dma_quiesced;char *name;};
struct nvme_namespace {int unused;};
static int pages_live, io_result, io_calls, copy_in_error, copy_out_error;
static int fail_alloc_once, reclaim_attempts, drain_attempts;
static bool may_block=true;
static bool nvme_may_block(void){return may_block;}
static size_t mm_file_cache_reclaim(size_t);
static void pmm_pcp_drain_all(void){drain_attempts++;}
static void *last_buffer;
static void *pmm_alloc_pages(size_t pages) {
  if(fail_alloc_once){fail_alloc_once--;return NULL;}
  void *p=calloc(pages,PAGE_SIZE);assert(p);last_buffer=p;pages_live+=pages;return p;
}
static void pmm_free_pages(void *p,size_t pages) {pages_live-=pages;free(p);}
static uint64_t pmm_get_hhdm_offset(void) {return 0;}
static unsigned long copy_from_user(void *dst,const void *src,size_t n) {
  if(copy_in_error)return n;memcpy(dst,src,n);return 0;
}
static unsigned long copy_to_user(void *dst,const void *src,size_t n) {
  if(copy_out_error)return n;memcpy(dst,src,n);return 0;
}
static int nvme_io_direct(struct nvme_controller *c,struct nvme_namespace *ns,
 uint64_t slba,uint32_t nlb,uint64_t va,bool write,bool fua) {
 (void)c;(void)ns;(void)slba;(void)nlb;(void)fua;io_calls++;
 if(!io_result && !write)memset((void *)va,0x5a,1024);return io_result;
}
typedef struct {unsigned flags;int refs,id;} vfs_node_t;
struct vma {struct vma *left,*right;void *file_node;uint64_t flags;};
struct mm {int lock;struct {struct vma *root;} vmas;};
struct thread {struct mm *mm;};
static struct mm memory;
static struct thread current={.mm=&memory};
static int reclaimed_calls, close_calls;
static size_t vfs_dentry_reclaim_pages(size_t target){(void)target;reclaim_attempts++;return 0;}
static struct thread *sched_get_current(void) {return &current;}
static bool spinlock_try_acquire(int *p) {if(*p)return false;*p=1;return true;}
static void spinlock_release(int *p) {assert(*p);*p=0;}
static bool vfs_node_is_alive(vfs_node_t *p) {return p!=NULL;}
static bool vfs_node_tryref(vfs_node_t *p) {p->refs++;return true;}
static void vfs_close(vfs_node_t *p) {assert(p->refs>1);p->refs--;close_calls++;}
static size_t vfs_cache_reclaim(vfs_node_t *p,size_t target) {
 assert(!memory.lock);assert(p->refs==2);reclaimed_calls++;
 return p->id==1?target:0;
}
'''
main = r'''
int main(void) {
 struct nvme_controller ctrl={0};struct nvme_namespace ns={0};unsigned char data[1024]={0};
 /* Repeated completed error statuses must not consume physical pages. */
 io_result=-1;
 for(int i=0;i<1000;i++) {
  assert(nvme_io_bounce(&ctrl,&ns,0,2,data,true,false,1024,false)==-1);
  assert(pages_live==0);
 }
 io_result=0;io_calls=0;copy_in_error=1;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,true,true,1024,false)==-1);
 assert(!io_calls && !pages_live);copy_in_error=0;copy_out_error=1;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,true,1024,false)==-1);
 assert(io_calls==1 && !pages_live);copy_out_error=0;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,true,1024,false)==0);
 assert(data[0]==0x5a && data[1023]==0x5a && !pages_live);
 /* An unquiesced transport failure must retain DMA memory, unlike ordinary
    errors. The fixture releases it only after simulating device shutdown. */
 io_result=-2;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,false,1024,false)==-2);
 assert(pages_live==1);ctrl.dma_quiesced=true;pmm_free_pages(last_buffer,1);
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,false,1024,false)==-2);
 assert(!pages_live);
 io_result=0;fail_alloc_once=1;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,false,1024,false)==0);
 assert(reclaim_attempts==1 && drain_attempts==1 && !pages_live);
 may_block=false;fail_alloc_once=1;
 assert(nvme_io_bounce(&ctrl,&ns,0,2,data,false,false,1024,false)==-1);
 assert(reclaim_attempts==1 && drain_attempts==1 && !pages_live);may_block=true;
 puts("PASS: bounce cleanup and reclaim/retry under pressure; masked IRQ contexts skip reclaim; unsafe DMA pages stay pinned");
 vfs_node_t files[3]={{.flags=FS_PAGE_CACHE,.refs=1,.id=1},
 {.flags=FS_PAGE_CACHE,.refs=1,.id=2},{.flags=FS_PAGE_CACHE,.refs=1,.id=3}};
 struct vma v[4]={{.file_node=&files[0]}, {.file_node=&files[1]},
 {.file_node=&files[1]}, {.file_node=&files[2]}};
 for(int i=0;i<3;i++)v[i].right=&v[i+1];memory.vmas.root=&v[0];
 assert(mm_file_cache_reclaim(1)==1);
 assert(reclaimed_calls==1 && close_calls==3);
 for(int i=0;i<3;i++)assert(files[i].refs==1);
 /* Tmpfs generic private mappings participate despite lacking FS_PAGE_CACHE. */
 files[0].flags=0;v[0].flags=MAP_PAGECACHE;
 assert(mm_file_cache_reclaim(1)==1 && close_calls==6);
 files[0].flags=FS_PAGE_CACHE;
 memory.lock=1;assert(mm_file_cache_reclaim(1)==0);
 assert(close_calls==6);memory.lock=0;
 assert(mm_file_cache_reclaim(0)==0 && close_calls==6);
 puts("PASS: early reclaim target releases every unique file pin; tmpfs private caches included; contended/zero-target calls leave no pins");
}
'''
nvme = Path(os.environ.get('NVME_SOURCE', ROOT/'kernel/src/drivers/storage/nvme.c'))
cache = Path(os.environ.get('CACHE_SOURCE', ROOT/'kernel/src/fs/page_cache.c'))
code = prefix + extract(nvme, 'static int nvme_io_bounce(')
code += extract(cache, 'size_t mm_file_cache_reclaim(') + main
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
