#!/usr/bin/env python3
"""Fault-inject actual file reads and cache fills; never publish failed I/O."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def extract(path, signature):
    source = Path(path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BS 1024u
#define PAGE_SIZE 4096u
#define MAX_READAHEAD_PAGES 64u
#define READAHEAD_SCRATCH_CHUNK 32u
#define page_copy(d,s) memcpy((d),(s),PAGE_SIZE)
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
typedef struct device device;
struct device { int (*read_sectors)(device *, uint64_t, uint32_t, void *); };
typedef struct { uint32_t i_size,i_blocks,i_flags; } ext2_inode_t;
typedef struct { uint32_t block_size; uint64_t blocks_count; device *dev; } ext2_mount_t;
typedef struct node {
 void *device; uint32_t inode,length; char name[32];
 uint32_t (*read)(struct node *,uint32_t,uint32_t,uint8_t *);
} vfs_node_t;
typedef struct { uint64_t frame_phys; } vfs_page_t;
static bool user, fail_alloc, fail_inode, fail_map, hole, vfs_readahead_force_scratch;
static uint32_t fail_block, copy_limit = UINT32_MAX;
static int backend_error;
static void *kmalloc(size_t n) { if(fail_alloc)return NULL; void *p=malloc(n); if(p)memset(p,0xcc,n);return p; }
static void kfree(void *p) { free(p); }
static void *pmm_alloc_pages(uint32_t n) { return malloc(n*PAGE_SIZE); }
static void pmm_free_pages(void *p,uint32_t n) { (void)n;free(p); }
#define klogf(...) ((void)0)
#define klog_puts(...) ((void)0)
static bool is_user_ptr(uint64_t p) { (void)p;return user; }
static unsigned long copy_to_user(void *d,const void *s,unsigned long n) {
 unsigned long c=n<copy_limit?n:copy_limit;memcpy(d,s,c);return n-c;
}
static unsigned long clear_user(void *d,unsigned long n) {
 unsigned long c=n<copy_limit?n:copy_limit;memset(d,0,c);return n-c;
}
static int ext2_read_inode(ext2_mount_t *m,unsigned i,ext2_inode_t *out) {
 (void)m;(void)i;if(fail_inode)return -1;*out=(ext2_inode_t){.i_size=4*BS};return 0;
}
static int ext2_get_block_num_checked(ext2_mount_t *m,ext2_inode_t *i,uint32_t l,uint64_t *b) {
 (void)m;(void)i;if(fail_map)return -1;*b=hole?0:10+l;return 0;
}
static uint64_t ext2_get_block_num(ext2_mount_t *m,ext2_inode_t *i,uint32_t l) {
 uint64_t b=0;ext2_get_block_num_checked(m,i,l,&b);return b;
}
static bool ext4_inode_has_extents(ext2_inode_t *i) { (void)i;return true; }
static uint64_t ext4_get_extent_run(ext2_mount_t *m,ext2_inode_t *i,uint32_t l,uint32_t *r) {
 (void)m;(void)i;*r=4-l;return hole?0:10+l;
}
static int ext2_read_block(ext2_mount_t *m,uint64_t b,void *d) {
 (void)m;if(b==fail_block)return -1;memset(d,(int)b,BS);return 0;
}
static int sector_read(device *d,uint64_t lba,uint32_t sectors,void *out) {
 (void)d;for(uint32_t n=0;n<sectors/2;n++){
  uint64_t b=lba/2+n;if(b==fail_block)return -1;memset((char*)out+n*BS,(int)b,BS);
 }return 0;
}
'''
code += extract(os.environ.get('EXT2_READ_SOURCE', root/'kernel/src/fs/ext2/ext2_file.c'), 'uint32_t ext2_read_impl(')
code += extract(os.environ.get('EXT4_READ_SOURCE', root/'kernel/src/fs/ext4/ext4_mount.c'), 'uint32_t ext4_read_impl(')
cache = os.environ.get('CACHE_SOURCE', root/'kernel/src/fs/page_cache.c')
for sig in ['static void vfs_readahead_warn_short(', 'static uint32_t vfs_cache_fill_pages_individually(', 'static bool vfs_cache_frames_contiguous(', 'static uint32_t vfs_cache_fill_pages(']:
    code += extract(cache, sig)
code += r'''
static uint32_t backend(vfs_node_t *n,uint32_t o,uint32_t s,uint8_t *d) {
 (void)n;(void)o;if(backend_error)return (uint32_t)backend_error;memset(d,0x5a,s);return s;
}
int main(void) {
 device dev={sector_read};ext2_mount_t m={BS,100,&dev};
 vfs_node_t n={.device=&m,.length=4*BS};uint8_t out[4*PAGE_SIZE];
 uint32_t (*readers[])(vfs_node_t *,uint32_t,uint32_t,uint8_t *)={ext2_read_impl,ext4_read_impl};
 for(unsigned r=0;r<2;r++)for(unsigned u=0;u<2;u++){
  user=u;fail_block=0;memset(out,0xee,sizeof(out));
  assert(readers[r](&n,0,4*BS,out)==4*BS);
  for(unsigned i=0;i<4*BS;i++)assert(out[i]==10+i/BS);
  fail_inode=true;assert((int32_t)readers[r](&n,0,BS,out)==-5);fail_inode=false;
  fail_block=10;assert((int32_t)readers[r](&n,1,BS,out)==-5);
  assert((int32_t)readers[r](&n,0,BS,out)==-5);
  fail_block=11;memset(out,0xee,sizeof(out));
  assert(readers[r](&n,1,2*BS,out)==BS-1);
  for(unsigned i=0;i<BS-1;i++)assert(out[i]==10);
  assert(out[BS-1]==0xee); /* failed second block never copies stale first block */
  fail_block=0;hole=true;copy_limit=0;
  if(user)assert((int32_t)readers[r](&n,0,BS,out)==-14);
  copy_limit=17;if(user)assert(readers[r](&n,0,BS,out)==17);
  copy_limit=UINT32_MAX;hole=false;
 }
 user=false;fail_map=true;assert((int32_t)ext2_read_impl(&n,0,BS,out)==-5);fail_map=false;
 /* Exact counts are required for all four read-ahead fill routes. */
 uint8_t *frames=malloc(4*PAGE_SIZE);assert(frames);
 vfs_page_t entries[2]={{(uintptr_t)frames},{(uintptr_t)(frames+PAGE_SIZE)}};
 vfs_page_t *pages[]={&entries[0],&entries[1]};n.read=backend;n.length=2*PAGE_SIZE;
 for(unsigned mode=0;mode<3;mode++){
  vfs_readahead_force_scratch=mode==1;
  entries[1].frame_phys=(uintptr_t)(frames+(mode==2?2:1)*PAGE_SIZE);
  for(unsigned e=0;e<3;e++){
   backend_error=e==0?-5:e==1?-12:-14;
   assert(vfs_cache_fill_pages(&n,0,pages,2)==0);
   assert(vfs_cache_fill_pages(&n,0,pages,1)==0);
   assert(vfs_cache_fill_pages_individually(&n,0,pages,0,2)==0);
  }
  backend_error=0;assert(vfs_cache_fill_pages(&n,0,pages,2)==2);
 }
 free(frames);puts("PASS: ext2/ext4 failed reads, short copies, and cache error rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-read-errors-') as tmp:
    src=Path(tmp)/'test.c';src.write_text(code)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
