#!/usr/bin/env python3
"""Actual dentry-wide and clean-page reclaim with reference/lock fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    s=Path(path).read_text();start=s.index(signature);end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]+'\n'


code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define FS_FILE 1
#define FS_PAGE_CACHE 128
#define MAP_PAGECACHE (1ULL<<34)
#define FS_TYPE_MASK 15
#define VFS_DENTRY_WAYS 4
#define VFS_DENTRY_BUCKETS 4
#define VFS_DENTRY_BUCKET_MASK 3
#define VFS_PAGE_MAGIC 123
#define CACHE_BATCH 64u
struct page {uint32_t magic,refs;bool dirty,loading,writeback;uint64_t frame_phys,last_used;};
typedef struct page vfs_page_t;
struct tree {vfs_page_t *p[4];};
typedef struct {unsigned flags;int refs,pages_lock;struct tree pages;} vfs_node_t;
typedef struct {struct {bool valid;vfs_node_t *child;} entries[VFS_DENTRY_WAYS];int lock;} vfs_dentry_cache_bucket_t;
static vfs_dentry_cache_bucket_t vfs_dentry_cache[VFS_DENTRY_BUCKETS];
static unsigned phys_refs[32],released,closes,visits,recursive_target;
static uint32_t vfs_cached_pages;
struct vma {struct vma *left,*right;vfs_node_t *file_node;uint64_t flags;};
static bool vfs_node_is_alive(vfs_node_t *n){return n && n->refs>0;}
size_t vfs_cache_reclaim(vfs_node_t *,size_t);
static void vfs_cache_clear_unused(vfs_node_t *n){vfs_cache_reclaim(n,4);}
static size_t vfs_dentry_reclaim_pages(size_t);
static bool spinlock_try_acquire(int *p){if(*p)return false;*p=1;return true;}
static void spinlock_release(int *p){assert(*p);*p=0;}
static bool vfs_node_tryref(vfs_node_t *p){if(!p->refs)return false;p->refs++;return true;}
static void vfs_close(vfs_node_t *p){assert(p->refs>1);p->refs--;closes++;}
static bool page_value_valid(vfs_page_t *p){return p && p->magic==VFS_PAGE_MAGIC;}
static void cache_report_invalid_value(void *p){(void)p;assert(0);}
static unsigned vfs_page_ref_count(vfs_page_t *p){return p->refs;}
static unsigned pmm_get_ref(void *p){return phys_refs[(uintptr_t)p];}
static void asc_radix_tree_for_each(struct tree *t,bool(*fn)(uint64_t,void *,void *),void *ctx){
 visits++;
 if(recursive_target){recursive_target=0;assert(!vfs_dentry_reclaim_pages(1));}
 for(unsigned i=0;i<4;i++)if(t->p[i] && !fn(i,t->p[i],ctx))break;
}
static void *asc_radix_tree_delete(struct tree *t,uint64_t k){void *p=t->p[k];t->p[k]=NULL;return p;}
static vfs_page_t *cache_delete_locked(vfs_node_t *n,uint64_t k,bool *r){
 assert(n->pages_lock);
 for(unsigned i=0;i<VFS_DENTRY_BUCKETS;i++)assert(!vfs_dentry_cache[i].lock);
 vfs_page_t *p=asc_radix_tree_delete(&n->pages,k);*r=true;if(p)vfs_cached_pages--;return p;
}
static void cache_page_release(vfs_page_t *p){assert(phys_refs[p->frame_phys]==1 && !p->refs && !p->dirty && !p->loading && !p->writeback);phys_refs[p->frame_phys]=0;p->magic=0;released++;}
'''
path='kernel/src/fs/page_cache.c'
code+=extract(path,'struct reclaim_batch {')+';\n'
code+=extract(path,'static bool collect_reclaimable(')
code+=extract(path,'size_t vfs_cache_reclaim(')
code+=extract('kernel/src/fs/vfs.c','size_t vfs_dentry_reclaim_pages(')
code+=extract(os.environ.get('VMM_SOURCE','kernel/src/mm/vmm_free.c'),'static void reclaim_unmapped_file_cache(')
code+=r'''
int main(void){
 vfs_node_t n[3]={{.flags=FS_FILE,.refs=1},{.flags=FS_FILE,.refs=1},{.flags=FS_FILE,.refs=1}};
 vfs_page_t p[6]={0};
 for(unsigned i=0;i<6;i++){p[i].magic=VFS_PAGE_MAGIC;p[i].frame_phys=i+1;phys_refs[i+1]=1;}
 n[0].pages.p[0]=&p[0];n[0].pages.p[1]=&p[1];
 n[1].pages.p[0]=&p[2];n[1].pages.p[1]=&p[3];n[1].pages.p[2]=&p[4];n[1].pages.p[3]=&p[5];
 p[2].dirty=true;p[3].loading=true;p[4].writeback=true;phys_refs[6]=2;vfs_cached_pages=6;
 vfs_dentry_cache[0].entries[0]=(typeof(vfs_dentry_cache[0].entries[0])){true,&n[0]};
 vfs_dentry_cache[0].entries[1]=(typeof(vfs_dentry_cache[0].entries[0])){true,&n[0]};
 vfs_dentry_cache[0].entries[2]=(typeof(vfs_dentry_cache[0].entries[0])){true,&n[1]};
 vfs_dentry_cache[1].entries[0]=(typeof(vfs_dentry_cache[0].entries[0])){true,&n[2]};
 assert(!vfs_dentry_reclaim_pages(0));assert(!closes);
 recursive_target=1;
 assert(vfs_dentry_reclaim_pages(1)==1 && released==1 && closes==2);
 assert(n[0].refs==1 && n[1].refs==1 && vfs_cached_pages==5);
 /* A caller holding this page lock must skip, rather than self-deadlock. */
 n[0].pages_lock=1;
 assert(!vfs_dentry_reclaim_pages(20) && n[0].pages_lock==1);
 n[0].pages_lock=0;
 /* A contended bucket is skipped without touching any protected child. */
 vfs_dentry_cache[0].lock=1;
 assert(!vfs_dentry_reclaim_pages(20));vfs_dentry_cache[0].lock=0;
 assert(vfs_dentry_reclaim_pages(20)==1 && released==2 && vfs_cached_pages==4);
 for(unsigned i=2;i<6;i++)assert(p[i].magic==VFS_PAGE_MAGIC && phys_refs[i+1]);
 for(unsigned i=0;i<3;i++)assert(n[i].refs==1 && !n[i].pages_lock);
 assert(visits<100);
 /* After process PTE teardown, tmpfs private copies must not be retained by
  * the persistent filesystem node merely because FS_PAGE_CACHE is absent. */
 p[0]=(vfs_page_t){.magic=VFS_PAGE_MAGIC,.frame_phys=1};phys_refs[1]=1;
 n[0].pages.p[0]=&p[0];vfs_cached_pages++;
 struct vma mapping={.file_node=&n[0],.flags=MAP_PAGECACHE};
 reclaim_unmapped_file_cache(&mapping);
 assert(!n[0].pages.p[0] && !phys_refs[1] && released==3);

 puts("PASS: cross-process clean caches reclaimed; dirty/loading/writeback/mapped pages retained; contended locks and recursion skipped; every pin released");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
