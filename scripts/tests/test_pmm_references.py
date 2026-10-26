#!/usr/bin/env python3
"""Exercise actual PMM reference and PCP code with controlled physical frames."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
s = (ROOT / 'kernel/src/mm/pmm.c').read_text()
ref = s[s.index('static volatile uint64_t pmm_ref_errors;'):s.index('static volatile uint64_t buddy_mark_used_skips;')]
free = s[s.index('__attribute__((optimize("O3"))) void pmm_free_pages('):]
pcp = '\n'.join(line for line in (ROOT / 'kernel/src/mm/pmm_pcp.c').read_text().splitlines() if not line.startswith('#include'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#define PAGE_SIZE 4096
#define MAX_ORDER 20
#define MAX_CPUS 2
#define MAX_NUMA_NODES 2
#define NUMA_NO_NODE -1
#define PCP_CAPACITY 128
#define PCP_BATCH 32
#define KLOG_CLR_RED ""
#define KLOG_CLR_RESET ""
typedef struct { volatile uint32_t locked; pthread_mutex_t mtx; } spinlock_t;
static void spinlock_init(spinlock_t *l) { l->locked = 0; pthread_mutex_init(&l->mtx, NULL); }
static void spinlock_acquire(spinlock_t *l) { pthread_mutex_lock(&l->mtx); l->locked = 1; }
static void spinlock_release(spinlock_t *l) { l->locked = 0; pthread_mutex_unlock(&l->mtx); }
static bool spinlock_is_locked(spinlock_t *l) { return __atomic_load_n(&l->locked, __ATOMIC_RELAXED) != 0; }
struct buddy_zone { spinlock_t lock; };
struct pcp_cache { void *pages[PCP_CAPACITY]; uint32_t count; volatile uint32_t active; spinlock_t lock; };
static bool pmm_sites_enabled __attribute__((unused)) = false;
static void pmm_site_record_free(unsigned c, void *caller) __attribute__((unused));
static void pmm_site_record_free(unsigned c, void *caller) { (void)c; (void)caller; }
static void pmm_site_record_alloc(unsigned c, void *caller) __attribute__((unused));
static void pmm_site_record_alloc(unsigned c, void *caller) { (void)c; (void)caller; }
struct numa_node { struct buddy_zone zone; int fallback_order[2]; bool online; };
struct cpu_info { unsigned cpu_id; };
static struct cpu_info cpu;
static struct numa_node node;
static uint64_t pmm_zero_page_phys, pmm_lowest_page=1, pmm_highest_page=257;
static uint16_t refs[256], *pmm_refcounts=refs;
static uint8_t bitmap[32], *pmm_bitmap=bitmap;
static unsigned next_frame=1, frees[256];
static void klog_puts(const char *s) { (void)s; }
static void klog_hex64(uint64_t n) { (void)n; }
static void klog_uint64(uint64_t n) { (void)n; }
static bool pmm_bitmap_test(const uint8_t *bm, uint64_t bit) { return bm[bit/8] & (1u<<(bit%8)); }
static void setbit(unsigned pfn, bool value) {
 unsigned bit=pfn-1; if(value) bitmap[bit/8]|=1u<<(bit%8); else bitmap[bit/8]&=~(1u<<(bit%8));
}
static bool pmm_is_managed(uint64_t p) { return p/PAGE_SIZE>=1 && p/PAGE_SIZE<257; }
static size_t pmm_get_order(size_t count) { size_t order=0; while ((1ull<<order)<count) order++; return order; }
typedef unsigned hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void) { return 0; }
static void hal_irq_restore(hal_irq_state_t f) { (void)f; }
static struct cpu_info *cpu_get_current(void) { return &cpu; }
static int numa_cpu_to_node(unsigned id) { (void)id; return 0; }
static int numa_phys_to_node(uint64_t p) { (void)p; return 0; }
static struct numa_node *numa_get_node(int id) { (void)id; return &node; }
static void *buddy_alloc_page_locked(struct buddy_zone *z) {
 (void)z; if (next_frame>256) return NULL;
 unsigned pfn=next_frame++;refs[pfn-1]=1;setbit(pfn,true);return (void *)(uint64_t)(pfn*PAGE_SIZE);
}
static void buddy_free_internal(struct buddy_zone *z,uint64_t phys,size_t order) {
 (void)z;
 for (unsigned i=0;i<(1u<<order);i++) {
  unsigned pfn=phys/PAGE_SIZE+i;
  assert(refs[pfn-1]==0);assert(pmm_bitmap_test(bitmap,pfn-1));
  setbit(pfn,false);frees[pfn-1]++;
 }
}
/* pmm_pcp.c slice needs these before their definitions. */
void pmm_zero_pool_drain_all(void);
size_t pmm_zero_pool_total_cached(void);
static size_t pmm_get_hhdm_offset(void) { return 0; }
static void page_zero(void *p) { memset(p,0,PAGE_SIZE); }
static void pmm_pcp_drain_all(void);
'''
# Functions are nonstatic in the actual implementation.
prefix=prefix.replace('static void pmm_pcp_drain_all(void);','void pmm_pcp_drain_all(void);')
tests = r'''
static void *put(void *ptr) { pmm_decref(ptr); return NULL; }
static void own(unsigned pfn,unsigned ref) { refs[pfn-1]=ref;setbit(pfn,true); }
static void own_free(unsigned pfn,unsigned ref) { refs[pfn-1]=ref;setbit(pfn,false); }
int main(void) {
 spinlock_init(&node.zone.lock);node.online=true;
 node.fallback_order[0]=0;node.fallback_order[1]=-1;
 pmm_pcp_init();
 void *p=pmm_pcp_alloc_page();assert(p && pmm_get_ref(p)==1);
 for(unsigned i=1;i<next_frame;i++)
  if(i!=(uintptr_t)p/PAGE_SIZE) assert(refs[i-1]==0);
 pmm_decref(p);pmm_pcp_drain_all();
 for(unsigned i=1;i<next_frame;i++) assert(refs[i-1]==0 && frees[i-1]==1);
 // Mixed CoW ownership: releasing the allocation must not strand last refs.
 for(unsigned i=64;i<68;i++) own(i,i%2?2:1);
 pmm_free_pages((void *)(64*PAGE_SIZE),4);
 for(unsigned i=64;i<68;i++) {
  assert(refs[i-1]==(i%2?1:0));
  assert(frees[i-1]==(i%2?0:1));
 }
 // Repeated release of a zero-ref frame must never underflow or free twice.
 pmm_decref((void *)(64*PAGE_SIZE));assert(refs[63]==0 && frees[63]==1);
 pmm_incref((void *)(64*PAGE_SIZE));assert(refs[63]==0);
 // An already-released constituent must not abort release of later pages.
 own(72,1);own(73,0);own(74,1);own(75,2);
 pmm_free_pages((void *)(72*PAGE_SIZE),4);
 assert(refs[71]==0 && refs[72]==0 && refs[73]==0 && refs[74]==1);
 assert(frees[71]==1 && frees[73]==1 && frees[74]==0);
 // Refcount/bitmap disagreement (frame released through another path but a
 // stale owner still holds a count): put_ref reaching zero must not link the
 // frame into the buddy lists again. The model's buddy_free_internal rejects
 // a bitmap-clear free, so the old unconditional free aborts here.
 own(96,1);own_free(97,1);
 pmm_free_pages((void *)(96*PAGE_SIZE),2);
 assert(refs[95]==0 && frees[95]==1);
 assert(refs[96]==0 && frees[96]==0);
 own_free(98,1);
 pmm_free_pages((void *)(98*PAGE_SIZE),1);
 assert(refs[97]==0 && frees[97]==0);
 own(80,1);pmm_free_pages((void *)(80*PAGE_SIZE+1),4);assert(refs[79]==1);
 pmm_free_pages((void *)(255*PAGE_SIZE),4);assert(refs[79]==1);
 own(90,UINT16_MAX);pmm_decref((void *)(90*PAGE_SIZE));
 pmm_incref((void *)(90*PAGE_SIZE));assert(refs[89]==UINT16_MAX);
 own(91,UINT16_MAX-1);pmm_incref((void *)(91*PAGE_SIZE));assert(refs[90]==UINT16_MAX-1);
 // Two legitimate owners release concurrently: exactly one final put.
 for(unsigned i=0;i<100;i++) {
  own(100,2);unsigned before=frees[99];pthread_t a,b;
  pthread_create(&a,NULL,put,(void *)(100*PAGE_SIZE));
  pthread_create(&b,NULL,put,(void *)(100*PAGE_SIZE));
  pthread_join(a,NULL);pthread_join(b,NULL);pmm_pcp_drain_all();
  assert(refs[99]==0 && frees[99]==before+1);
 }
 puts("PASS: mixed/shared releases, zero-ref retain/release, bounds, pin/overflow, PCP refill/drain and concurrent final put");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)
    (path/'test.c').write_text(prefix+pcp+ref+free+tests)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
