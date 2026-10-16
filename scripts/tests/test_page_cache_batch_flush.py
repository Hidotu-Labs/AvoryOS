#!/usr/bin/env python3
"""Batched dirty writeback and reclaim: coalescing, clamping, failures, races."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract(path, signature):
    s=Path(path).read_text();start=s.index(signature);end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]+'\n'


path=os.environ.get('PAGE_CACHE_SOURCE','kernel/src/fs/page_cache.c')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096
#define VFS_PAGE_MAGIC 123
/* Mirror the kernel batching bounds; the extracted code consumes them. */
#define CACHE_BATCH 64u
#define CACHE_SYNC_RUN_PAGES 32u
#define TREE_SLOTS 160
#define MAX_CALLS 16
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
struct page {uint32_t magic,refs;bool dirty,loading,writeback;uint32_t offset;uint64_t frame_phys,dirty_seq,last_used;bool evicted;};
typedef struct page vfs_page_t;
struct tree {vfs_page_t *p[TREE_SLOTS];};
typedef struct vfs_node {unsigned flags;int refs;int pages_lock;struct tree pages;uint32_t length;uint32_t (*write)(struct vfs_node *,uint32_t,uint32_t,uint8_t *);} vfs_node_t;
static uint8_t frames[TREE_SLOTS][PAGE_SIZE] __attribute__((aligned(4096)));
static unsigned frame_refs[TREE_SLOTS];
static vfs_page_t pages[TREE_SLOTS];
static vfs_page_t *tree_slots[TREE_SLOTS];
static uint32_t vfs_cached_pages;
static unsigned walks,invalid_reports,allocs,frees,freed;
static bool fail_allocs;
static unsigned write_calls;
static uint32_t call_offset[MAX_CALLS],call_size[MAX_CALLS];
static uint8_t *call_data[MAX_CALLS];
static int fail_call=-1,redirty_call=-1;
static uint32_t fail_return;
static vfs_page_t *redirty_page;
static vfs_node_t node;
static uint32_t node_write(vfs_node_t *n,uint32_t offset,uint32_t size,uint8_t *buffer){
 (void)n;
 unsigned call=write_calls++;
 assert(call<MAX_CALLS);
 call_offset[call]=offset;call_size[call]=size;
 call_data[call]=malloc(size?size:1);memcpy(call_data[call],buffer,size);
 /* Simulate a thread re-dirtying a page while the write is in flight. */
 if(redirty_page&&(int)call==redirty_call){redirty_page->dirty=true;redirty_page->dirty_seq++;}
 if((int)call==fail_call)return fail_return;
 return size;
}
static bool page_value_valid(vfs_page_t *p){uintptr_t v=(uintptr_t)p-(uintptr_t)pages;return v<sizeof(pages)&&p->magic==VFS_PAGE_MAGIC;}
static void cache_report_invalid_value(void *p){(void)p;invalid_reports++;}
static void cache_page_release(vfs_page_t *p){assert(p->evicted&&!p->refs&&!p->dirty&&!p->loading&&!p->writeback);p->magic=0;freed++;}
static unsigned vfs_page_ref_count(vfs_page_t *p){return p->refs;}
static unsigned pmm_get_ref(void *p){for(unsigned i=0;i<TREE_SLOTS;i++)if((void *)frames[i]==p)return frame_refs[i];return 1;}
static void vfs_page_get(vfs_page_t *p){p->refs++;}
static void vfs_cache_put(vfs_node_t *n,vfs_page_t *p){(void)n;assert(p->refs);p->refs--;}
static void spinlock_acquire(int *l){assert(!*l);*l=1;}
static void spinlock_release(int *l){assert(*l);*l=0;}
static bool spinlock_try_acquire(int *l){if(*l)return false;*l=1;return true;}
static void *pmm_alloc_pages(size_t count){allocs++;if(fail_allocs)return NULL;return malloc(count*PAGE_SIZE);}
static void pmm_free_pages(void *p,size_t count){(void)count;frees++;free(p);}
static void *asc_radix_tree_delete(struct tree *t,uint64_t k){(void)t;void *p=k<TREE_SLOTS?tree_slots[k]:NULL;if(k<TREE_SLOTS)tree_slots[k]=NULL;return p;}
static bool asc_radix_tree_for_each(struct tree *t,bool(*fn)(uint64_t,void *,void *),void *ctx){
 (void)t;walks++;
 for(uint64_t k=0;k<TREE_SLOTS;k++)if(tree_slots[k]&&!fn(k,tree_slots[k],ctx))return false;
 return true;
}
static vfs_page_t *cache_delete_locked(vfs_node_t *n,uint64_t key,bool *releasable){
 assert(n->pages_lock);
 vfs_page_t *p=key<TREE_SLOTS?tree_slots[key]:NULL;
 if(p){tree_slots[key]=NULL;p->evicted=true;vfs_cached_pages--;}
 *releasable=p&&!p->refs;
 return p;
}
static void reset_state(void){
 for(unsigned i=0;i<MAX_CALLS;i++){free(call_data[i]);call_data[i]=NULL;}
 memset(tree_slots,0,sizeof(tree_slots));
 memset(pages,0,sizeof(pages));
 memset(&node,0,sizeof(node));
 node.write=node_write;
 write_calls=0;walks=0;invalid_reports=0;allocs=0;frees=0;freed=0;
 fail_allocs=false;fail_call=-1;fail_return=0;redirty_call=-1;redirty_page=NULL;
 for(unsigned i=0;i<TREE_SLOTS;i++)frame_refs[i]=1;
 vfs_cached_pages=0;
}
static vfs_page_t *add_page(unsigned index,uint32_t offset,bool dirty){
 vfs_page_t *p=&pages[index];
 p->magic=VFS_PAGE_MAGIC;p->frame_phys=(uint64_t)(uintptr_t)frames[index];
 memset(frames[index],index&0xFF,PAGE_SIZE);
 p->offset=offset;p->dirty=dirty;p->dirty_seq=1;
 tree_slots[offset/PAGE_SIZE]=p;
 vfs_cached_pages++;
 return p;
}
/* Every byte of one write call must match the frame it was gathered from. */
static void check_data(unsigned call,unsigned first_page){
 unsigned full=call_size[call]/PAGE_SIZE,tail=call_size[call]%PAGE_SIZE;
 for(unsigned k=0;k<full;k++)
  for(unsigned b=0;b<PAGE_SIZE;b++)
   assert(call_data[call][k*PAGE_SIZE+b]==((first_page+k)&0xFF));
 for(unsigned b=0;b<tail;b++)
  assert(call_data[call][full*PAGE_SIZE+b]==((first_page+full)&0xFF));
}
'''
code+=extract(path,'struct dirty_batch {')+';\n'
code+=extract(path,'static bool collect_dirty(')
code+=extract(path,'static uint32_t cache_page_writable(')
code+=extract(path,'static void cache_sync_run_finish(')
code+=extract(path,'static bool cache_sync_batch(')
code+=extract(path,'void vfs_cache_sync(')
code+=extract(path,'struct reclaim_batch {')+';\n'
code+=extract(path,'static bool collect_reclaimable(')
code+=extract(path,'size_t vfs_cache_reclaim(')
code+=r'''
int main(void){
 /* 64 adjacent dirty pages: one walk and two 32-page coalesced calls, where
  * the per-page loop would have made 64 walk restarts and 64 write calls. */
 reset_state();
 node.length=64u*PAGE_SIZE;
 for(unsigned i=0;i<64;i++)add_page(i,i*PAGE_SIZE,true);
 vfs_cache_sync(&node);
 assert(write_calls==2&&walks<=3);
 assert(call_offset[0]==0&&call_size[0]==32u*PAGE_SIZE);
 assert(call_offset[1]==32u*PAGE_SIZE&&call_size[1]==32u*PAGE_SIZE);
 check_data(0,0);check_data(1,32);
 assert(allocs==2&&frees==2);
 for(unsigned i=0;i<64;i++)assert(!pages[i].dirty&&!pages[i].writeback&&!pages[i].refs);

 /* Gaps split the batch into runs; each run is one call. */
 reset_state();
 node.length=16u*PAGE_SIZE;
 add_page(0,0,true);add_page(1,PAGE_SIZE,true);
 add_page(2,3u*PAGE_SIZE,true);
 add_page(3,5u*PAGE_SIZE,true);add_page(4,6u*PAGE_SIZE,true);add_page(5,7u*PAGE_SIZE,true);
 vfs_cache_sync(&node);
 assert(write_calls==3&&walks==2);
 assert(call_offset[0]==0&&call_size[0]==2u*PAGE_SIZE);
 assert(call_offset[1]==3u*PAGE_SIZE&&call_size[1]==PAGE_SIZE);
 assert(call_offset[2]==5u*PAGE_SIZE&&call_size[2]==3u*PAGE_SIZE);
 check_data(0,0);check_data(1,2);check_data(2,3);
 for(unsigned i=0;i<6;i++)assert(!pages[i].dirty);

 /* EOF clamps the coalesced call; the tail page is partial, pages wholly
  * past EOF are retired without I/O. */
 reset_state();
 node.length=3u*PAGE_SIZE+1000;
 for(unsigned i=0;i<5;i++)add_page(i,i*PAGE_SIZE,true);
 vfs_cache_sync(&node);
 assert(write_calls==1);
 assert(call_offset[0]==0&&call_size[0]==3u*PAGE_SIZE+1000);
 check_data(0,0);
 for(unsigned i=0;i<5;i++)assert(!pages[i].dirty&&!pages[i].writeback);

 /* A failed call stops the sync: earlier pages are clean, the failed call's
  * pages and everything after stay dirty but out of writeback. */
 reset_state();
 node.length=64u*PAGE_SIZE;
 for(unsigned i=0;i<64;i++)add_page(i,i*PAGE_SIZE,true);
 fail_call=1;fail_return=0;
 vfs_cache_sync(&node);
 assert(write_calls==2);
 for(unsigned i=0;i<32;i++)assert(!pages[i].dirty);
 for(unsigned i=32;i<64;i++)assert(pages[i].dirty&&!pages[i].writeback);
 for(unsigned i=0;i<64;i++)assert(!pages[i].refs);

 /* A partial call cleans only the covered prefix of its run. */
 reset_state();
 node.length=64u*PAGE_SIZE;
 for(unsigned i=0;i<64;i++)add_page(i,i*PAGE_SIZE,true);
 fail_call=1;fail_return=16u*PAGE_SIZE;
 vfs_cache_sync(&node);
 assert(write_calls==2);
 for(unsigned i=0;i<48;i++)assert(!pages[i].dirty);
 for(unsigned i=48;i<64;i++)assert(pages[i].dirty&&!pages[i].writeback);

 /* A page re-dirtied while its call is in flight stays dirty and is written
  * again, alone, before the sync returns. */
 reset_state();
 node.length=4u*PAGE_SIZE;
 for(unsigned i=0;i<3;i++)add_page(i,i*PAGE_SIZE,true);
 redirty_call=0;redirty_page=&pages[1];
 vfs_cache_sync(&node);
 assert(write_calls==2);
 assert(call_size[0]==3u*PAGE_SIZE&&call_size[1]==PAGE_SIZE);
 assert(pages[1].dirty_seq==2);
 for(unsigned i=0;i<3;i++)assert(!pages[i].dirty&&!pages[i].writeback);

 /* No contiguous staging: per-page writes, never worse than before. */
 reset_state();
 node.length=4u*PAGE_SIZE;
 for(unsigned i=0;i<4;i++)add_page(i,i*PAGE_SIZE,true);
 fail_allocs=true;
 vfs_cache_sync(&node);
 assert(write_calls==4&&allocs==1&&frees==0);
 for(unsigned i=0;i<4;i++)
  assert(call_offset[i]==i*PAGE_SIZE&&call_size[i]==PAGE_SIZE&&!pages[i].dirty&&!pages[i].refs);

 /* Reclaim deletes a whole batch per walk instead of one page per walk. */
 reset_state();
 for(unsigned i=0;i<100;i++)add_page(i,i*PAGE_SIZE,i==5);
 assert(vfs_cache_reclaim(&node,50)==50);
 assert(walks==1&&freed==50&&vfs_cached_pages==50);
 for(unsigned i=0;i<51;i++)if(i!=5)assert(!tree_slots[i]);
 assert(tree_slots[5]&&tree_slots[51]);
 assert(vfs_cache_reclaim(&node,100)==49);
 assert(freed==99&&vfs_cached_pages==1&&tree_slots[5]);
 for(unsigned i=0;i<100;i++)assert(!pages[i].refs);

 /* Dirty, loading, writeback, mapped and reader-held pages are skipped. */
 reset_state();
 add_page(0,0,false);
 add_page(1,PAGE_SIZE,false);pages[1].loading=true;
 add_page(2,2u*PAGE_SIZE,false);pages[2].writeback=true;
 add_page(3,3u*PAGE_SIZE,false);frame_refs[3]=2;
 add_page(4,4u*PAGE_SIZE,false);pages[4].refs=1;
 assert(vfs_cache_reclaim(&node,10)==1);
 assert(freed==1&&!tree_slots[0]&&tree_slots[1]&&tree_slots[2]&&tree_slots[3]&&tree_slots[4]);

 /* Invalid leaves are unlinked in the same walk that flushes dirty pages. */
 reset_state();
 add_page(0,0,true);
 tree_slots[2]=(vfs_page_t *)(uintptr_t)0xdead;vfs_cached_pages++;
 node.length=PAGE_SIZE;
 vfs_cache_sync(&node);
 assert(invalid_reports==1&&!tree_slots[2]);
 assert(vfs_cached_pages==1&&write_calls==1);
 assert(tree_slots[0]&&!pages[0].dirty&&!pages[0].writeback);

 puts("PASS: dirty runs coalesced and 32-page chunked; EOF clamped; failures stop with exact dirty/writeback state; re-dirtied pages retried; per-page fallback; reclaim and invalid sweeps batched");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
