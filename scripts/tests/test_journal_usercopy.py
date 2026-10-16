#!/usr/bin/env python3
"""Actual page-cache update and VFS write: fault-time reclaim and error lengths."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    source = Path(path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
typedef struct {bool held;} spinlock_t;
typedef struct {uintptr_t frame_phys;bool loading,uptodate,evicted;unsigned last_used;} vfs_page_t;
typedef struct vfs_node {
 spinlock_t pages_lock;unsigned pages,flags,mask;
 uint32_t (*write)(struct vfs_node *,uint32_t,uint32_t,uint8_t *);
} vfs_node_t;
static vfs_page_t page[2];
static unsigned char frames[2][PAGE_SIZE], input[2*PAGE_SIZE];
static vfs_node_t node;
static bool user,fail_alloc,fail_copy;
static unsigned copies,invalidated,allocations;
static void spinlock_acquire(spinlock_t *l){assert(!l->held);l->held=true;}
static void spinlock_release(spinlock_t *l){assert(l->held);l->held=false;}
static bool is_user_ptr(uint64_t p){return user && p>=(uintptr_t)input && p<(uintptr_t)input+sizeof(input);}
static void *kmalloc(size_t n){assert(!node.pages_lock.held);void *p=fail_alloc?NULL:malloc(n);if(p)allocations++;return p;}
static void kfree(void *p){if(p)allocations--;free(p);}
static uint64_t cache_key(uint32_t offset){return offset/PAGE_SIZE;}
static void *asc_radix_tree_lookup(void *tree,uint64_t key){(void)tree;assert(node.pages_lock.held && key<2);return page[key].evicted?NULL:&page[key];}
static bool page_value_valid(vfs_page_t *p){return p==&page[0] || p==&page[1];}
static bool vfs_page_is_evicted(vfs_page_t *p){return p->evicted;}
static void cache_drop_invalid_locked(vfs_node_t *n,uint32_t o,vfs_page_t *p){(void)n;(void)o;(void)p;assert(0);}
static vfs_page_t *cache_delete_locked(vfs_node_t *n,uint64_t key,bool *release){assert(n->pages_lock.held && key<2);page[key].evicted=true;*release=false;return &page[key];}
static void cache_page_release(vfs_page_t *p){(void)p;}
static void cache_remove_range(vfs_node_t *n,uint64_t first,uint64_t last,bool unused){(void)unused;assert(n->pages_lock.held);for(unsigned i=first;i<=last;i++)page[i].evicted=true;}
static unsigned cache_stamp(void){return 42;}
static void vfs_cache_invalidate_range(vfs_node_t *n,uint32_t o,uint32_t z){
 spinlock_acquire(&n->pages_lock);cache_remove_range(n,cache_key(o),cache_key(o+z-1),false);spinlock_release(&n->pages_lock);invalidated++;
}
static unsigned long copy_from_user(void *dst,const void *src,unsigned long n){
 /* Simulate reclaim of the same file on a user fault. This must not recurse
  * into a pages_lock held by the cache update. */
 spinlock_acquire(&node.pages_lock);spinlock_release(&node.pages_lock);copies++;
 if(fail_copy)return n;
 memcpy(dst,src,n);return 0;
}
'''
source = prefix + extract(os.environ.get('PAGE_CACHE_SOURCE', 'kernel/src/fs/page_cache.c'),
                          'void vfs_cache_update_or_invalidate(')
source += r'''
static void reset(void){
 assert(!allocations && !node.pages_lock.held);
 memset(&node,0,sizeof(node));memset(page,0,sizeof(page));memset(frames,0xa5,sizeof(frames));
 for(unsigned i=0;i<2;i++){page[i].frame_phys=(uintptr_t)frames[i];page[i].uptodate=true;}
 memset(input,0x5a,sizeof(input));user=false;fail_alloc=fail_copy=false;copies=invalidated=0;
}
int main(void){
 reset();user=true;vfs_cache_update_or_invalidate(&node,17,PAGE_SIZE+19,input);
 assert(copies==2 && !allocations && !node.pages_lock.held);
 assert(frames[0][16]==0xa5 && frames[0][17]==0x5a && frames[1][35]==0x5a && frames[1][36]==0xa5);
 reset();user=true;fail_copy=true;vfs_cache_update_or_invalidate(&node,0,13,input);
 assert(page[0].evicted && invalidated==1 && frames[0][0]==0xa5 && !allocations);
 reset();user=true;fail_alloc=true;vfs_cache_update_or_invalidate(&node,0,sizeof(input),input);
 assert(page[0].evicted && page[1].evicted && !copies && !allocations);
 reset();page[0].loading=true;vfs_cache_update_or_invalidate(&node,0,13,input);assert(page[0].evicted);
 reset();vfs_cache_update_or_invalidate(&node,0,13,input);assert(!copies && frames[0][12]==0x5a && frames[0][13]==0xa5);
 puts("PASS cache updates allow fault-time reclaim, retain partial-page suffixes, invalidate on copy/allocation failure and retire in-flight fills");
}
'''
vfs_prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define FS_TYPE_MASK 7
#define FS_FILE 1
#define FS_DIRECTORY 2
#define FS_PAGE_CACHE 8
#define FS_DENTRY_NOCACHE 16
struct thread{unsigned euid;};
typedef struct vfs_node{unsigned flags,mask;bool mmap_shared_only;char name[16];uint32_t (*write)(struct vfs_node*,uint32_t,uint32_t,uint8_t*);}vfs_node_t;
static struct thread current;
static unsigned updates;
static uint32_t answer;
static struct thread *sched_get_current(void){return &current;}
static void vfs_chmod(vfs_node_t *n,unsigned m){(void)n;(void)m;}
static void sysdiag_record_missing_fs_node(const char *n,struct thread *t,int e,const char *o){(void)n;(void)t;(void)e;(void)o;}
static void vfs_cache_update_or_invalidate(vfs_node_t *n,uint32_t o,uint32_t z,uint8_t *b){(void)n;(void)o;(void)b;assert(z==13 || z==100);updates++;}
static uint32_t write_result(vfs_node_t *n,uint32_t o,uint32_t z,uint8_t *b){(void)n;(void)o;(void)z;(void)b;return answer;}
'''
vfs = vfs_prefix + extract(os.environ.get('VFS_SOURCE', 'kernel/src/fs/vfs.c'), 'uint32_t vfs_write(')
vfs += r'''
int main(void){
 vfs_node_t node={.flags=FS_FILE|FS_PAGE_CACHE,.write=write_result};uint8_t buf[100];
 int errors[]={5,12,14,28};for(unsigned i=0;i<4;i++){answer=(uint32_t)-errors[i];assert(vfs_write(&node,0,100,buf)==answer && !updates);}
 answer=0;assert(vfs_write(&node,0,100,buf)==0 && !updates);
 answer=13;assert(vfs_write(&node,0,100,buf)==13 && updates==1);
 answer=100;assert(vfs_write(&node,0,100,buf)==100 && updates==2);
 puts("PASS write errors never become unsigned multi-gigabyte page-cache updates");
}
'''
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    for name, code in [('cache', source), ('vfs', vfs)]:
        (root / f'{name}.c').write_text(code)
        subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                        str(root / f'{name}.c'), '-o', str(root / name)], check=True)
        subprocess.run([str(root / name)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
