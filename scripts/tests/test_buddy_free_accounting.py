#!/usr/bin/env python3
"""Check actual buddy split/merge accounting and pressure reads under contention."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT/'kernel/src/mm/pmm_buddy.c')
parser.add_argument('--header', type=Path, default=ROOT/'kernel/src/mm/pmm_internal.h')
parser.add_argument('--init-source', type=Path, default=ROOT/'kernel/src/mm/pmm_init.c')
args = parser.parse_args()
header = args.header.read_text()
# Use the production allocator structures and inline bitmap/conversion helpers.
structures = header[header.index('#define MAX_ORDER'):header.index('/* Global')]
helpers = header[header.index('/* Bitmap and conversion'):header.index('/* Buddy Allocator API')]
init_source = args.init_source.read_text()
seed_start = init_source.index('        int node = numa_phys_to_node(phys);')
seed = init_source[seed_start:init_source.index('        n->usable_memory +=',seed_start)]
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lib/list.h"
#define PAGE_SIZE 4096u
#define KLOG_CLR_RED ""
#define KLOG_CLR_RESET ""
typedef struct { pthread_mutex_t mutex; } spinlock_t;
static _Thread_local unsigned lock_acquires;
static unsigned long validations;
static void spinlock_init(spinlock_t *s) { assert(!pthread_mutex_init(&s->mutex,0)); }
static void spinlock_acquire(spinlock_t *s) { lock_acquires++;assert(!pthread_mutex_lock(&s->mutex)); }
static void spinlock_release(spinlock_t *s) { assert(!pthread_mutex_unlock(&s->mutex)); }
static void klog_puts(const char *s) { (void)s; }
static void klog_uint64(uint64_t n) { (void)n; }
static void klog_hex64(uint64_t n) { (void)n; }
static uint64_t rdtsc(void) { return 1234567; }
static uint8_t *pmm_bitmap;
static uint16_t *pmm_refcounts;
static uint64_t pmm_lowest_page,pmm_highest_page,pmm_page_count;
static uint64_t pmm_physical_memory_offset;
static bool pmm_is_managed(uint64_t phys) {
 return phys/PAGE_SIZE>=pmm_lowest_page && phys/PAGE_SIZE<pmm_highest_page;
}
static bool pmm_kernel_ptr_is_managed(const void *p) {
 validations++;return pmm_is_managed((uintptr_t)p);
}
'''
bitmap = r'''
void pmm_bitmap_set_range(uint8_t *bm,size_t pfn,size_t n) {
 for(size_t i=0;i<n;i++)pmm_bitmap_set(bm,pfn+i-pmm_lowest_page);
}
void pmm_bitmap_clear_range(uint8_t *bm,size_t pfn,size_t n) {
 for(size_t i=0;i<n;i++)pmm_bitmap_clear(bm,pfn+i-pmm_lowest_page);
}
'''
tests = r'''
#define PAGES 4096u
static void *frames[PAGES];
static void release(void *p,size_t order) {
 spinlock_acquire(&b_zone.lock);
 buddy_free_internal(&b_zone,(uintptr_t)p,order);
 spinlock_release(&b_zone.lock);
}
static size_t pressure_read(void) {
 unsigned before=lock_acquires;unsigned long checked=validations;
 size_t n=buddy_zone_free_pages(&b_zone);
 assert(lock_acquires==before && "pressure read must not take the allocator lock");
 assert(validations==checked && "pressure read must not walk free-list nodes");
 return n;
}
static void compare_list_at(unsigned line) {
 size_t pages=0;
 for(unsigned order=0;order<MAX_ORDER;order++) {
  struct list_head *p;list_for_each(p,&b_zone.free_list[order]) {
   assert(buddy_zone_node_valid(&b_zone,p,order));pages+=(size_t)1<<order;
  }
 }
 if(pressure_read()!=pages)fprintf(stderr,"line %u: counter=%zu list=%zu\n",line,pressure_read(),pages);
 assert(pressure_read()==pages);
}
#define compare_list() compare_list_at(__LINE__)
static unsigned done;
static void *churn(void *unused) {
 (void)unused;
 for(unsigned i=0;i<20000;i++) {
  void *p=buddy_alloc_internal(&b_zone,i%4);assert(p);release(p,i%4);
 }
 __atomic_store_n(&done,1,__ATOMIC_RELEASE);return 0;
}
int main(void) {
 void *ram=0;assert(!posix_memalign(&ram,PAGES*PAGE_SIZE,PAGES*PAGE_SIZE));
 memset(ram,0,PAGES*PAGE_SIZE);
 pmm_lowest_page=(uintptr_t)ram/PAGE_SIZE;pmm_highest_page=pmm_lowest_page+PAGES;
 pmm_page_count=PAGES;pmm_bitmap=calloc(PAGES/8,1);pmm_refcounts=calloc(PAGES,2);
 buddy_zone_init(&boot_node.zone);boot_seed((uintptr_t)ram,12);
 assert(buddy_zone_free_pages(&boot_node.zone)==PAGES && "boot seeding must initialize free-page accounting");
 memset(ram,0,PAGES*PAGE_SIZE);memset(pmm_bitmap,0,PAGES/8);
 assert(pmm_bitmap && pmm_refcounts);buddy_zone_init(&b_zone);
 assert(pressure_read()==0);release(ram,12);compare_list();
 assert(pressure_read()==PAGES);
 for(unsigned i=0;i<PAGES;i++) {
  frames[i]=buddy_alloc_internal(&b_zone,0);assert(frames[i]);
  assert(pressure_read()==PAGES-i-1);
 }
 assert(!buddy_alloc_internal(&b_zone,0));
 for(unsigned i=0;i<PAGES;i+=2)release(frames[i],0);
 assert(pressure_read()==PAGES/2);compare_list();
 // The accounting read stays independent of 2,048 fragmented free headers.
 unsigned long checked=validations;
 for(unsigned i=0;i<100000;i++)assert(pressure_read()==PAGES/2);
 assert(validations==checked);
 for(unsigned i=1;i<PAGES;i+=2)release(frames[i],0);
 compare_list();assert(pressure_read()==PAGES);
 uintptr_t lo=(uintptr_t)ram+1024*PAGE_SIZE,hi=lo+32*PAGE_SIZE;
 void *range=buddy_alloc_pages_range(&b_zone,17,lo,hi);
 assert((uintptr_t)range==lo && pressure_read()==PAGES-32);
 compare_list();release(range,5);compare_list();
 // PCP refill also splits through the locked allocator entry point.
 spinlock_acquire(&b_zone.lock);void *pcp=buddy_alloc_page_locked(&b_zone);
 spinlock_release(&b_zone.lock);assert(pcp && pressure_read()==PAGES-1);
 release(pcp,0);compare_list();
 pthread_t worker;assert(!pthread_create(&worker,0,churn,0));
 unsigned before=lock_acquires;
 while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE)) {
  size_t available=buddy_zone_free_pages(&b_zone);
  assert(available<=PAGES && available>=PAGES-8);
 }
 assert(lock_acquires==before);
 assert(!pthread_join(worker,0));compare_list();assert(pressure_read()==PAGES);
 // Recovery discards a bad list's accounted pages instead of reporting them free.
 struct buddy_block *bad=list_first_entry(&b_zone.free_list[12],struct buddy_block,node);
 bad->magic=0;assert(!buddy_alloc_internal(&b_zone,12));assert(pressure_read()==0);
 // A bad tail retains the valid prefix, including its accounting.
 memset(ram,0,PAGES*PAGE_SIZE);memset(pmm_bitmap,0,PAGES/8);
 buddy_zone_init(&b_zone);release(ram,0);release((char *)ram+2*PAGE_SIZE,0);
 bad=(struct buddy_block *)((char *)ram+2*PAGE_SIZE);bad->magic=0;
 assert(pressure_read()==2);buddy_list_validate(&b_zone,0);
 assert(pressure_read()==1);compare_list();
 free(pmm_refcounts);free(pmm_bitmap);free(ram);
 puts("PASS: buddy accounting, split/merge/range/PCP, corruption recovery and lock-free pressure reads during churn");
}
'''
source = '\n'.join(line for line in args.source.read_text().splitlines()
                   if not line.startswith('#include '))
boot = r'''
struct numa_node { struct buddy_zone zone; };
static struct numa_node boot_node;
static int numa_phys_to_node(uint64_t phys) { (void)phys;return 0; }
static struct numa_node *numa_get_node(int node) { assert(node==0);return &boot_node; }
static void boot_seed(uint64_t phys,size_t order) {
'''+seed+'}\n'
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp/'lib').mkdir()
    # Host allocations use low virtual addresses; retain the actual list
    # operations while adapting only their kernel-address guard.
    (tmp/'lib/list.h').write_text((ROOT/'kernel/src/lib/list.h').read_text().replace(
        '0xFFFF800000000000ULL','1ULL'))
    c = tmp/'test.c'
    c.write_text(prefix+structures+helpers+bitmap+source+boot+tests)
    subprocess.run(['cc','-O2','-g','-Wall','-Wextra','-Werror','-std=gnu11',
                    '-pthread','-fsanitize=address,undefined',
                    '-I',str(tmp),str(c),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
