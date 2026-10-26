/* Exercise the actual RX core with host-backed PMM pages and concurrent IRQ
 * producers/workers. Build with -pthread -ffunction-sections -fdata-sections
 * and -Wl,--gc-sections, using the normal kernel include directories. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
typedef struct { atomic_uint locked; uint64_t saved_flags; } spinlock_t;
typedef struct { unsigned locked; } rawspinlock_t;
#define SPINLOCK_INIT {false}
#define RAWSPINLOCK_INIT {false}
static void spinlock_init(spinlock_t *p) { atomic_store(&p->locked, false); }
static void spinlock_acquire(spinlock_t *p) {
  while (atomic_exchange_explicit(&p->locked, true, memory_order_acquire)) {}
}
static void spinlock_release(spinlock_t *p) {
  atomic_store_explicit(&p->locked, false, memory_order_release);
}
#define sched_yield net_test_yield
#include "../../kernel/src/net/core.c"
#undef sched_yield
static size_t free_pages, live_pages;
static bool fail_alloc;
static unsigned wakes;
static struct net_device dev;
void *pmm_alloc_page(void) {
  if (fail_alloc || !free_pages) return NULL;
  void *p = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  assert(p); --free_pages; ++live_pages; return p;
}
void pmm_free_page(void *p) { assert(live_pages); --live_pages; ++free_pages; free(p); }
uint64_t pmm_get_hhdm_offset(void) { return 0; }
size_t pmm_get_free_pages(void) { return free_pages; }
uint64_t lapic_timer_get_ticks(void) { return 10000; }
void klog_puts(const char *p) { (void)p; }
void wait_queue_init(wait_queue_t *q) { memset(q, 0, sizeof(*q)); }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; __atomic_add_fetch(&wakes,1,__ATOMIC_RELAXED); }
void wait_queue_wake_one(wait_queue_t *q) { wait_queue_wake_all(q); }
static struct rx_buffer *take(void) {
  spinlock_acquire(&lock); struct rx_buffer *p = pop_locked(); spinlock_release(&lock); return p;
}
static void release(struct rx_buffer *p) {
  spinlock_acquire(&lock); free_locked(p); spinlock_release(&lock);
}
static void trim(void) {
  assert(!queue_head && !queue_tail && !queue_depth);
  while(hot_count) { pmm_free_page(hot_pool[--hot_count]); --active_packets; }
  assert(!live_pages && !active_packets);
}
static void submit_id(unsigned id) {
  unsigned char frame[NET_FRAME_MAX];
  memset(frame,(unsigned char)id,sizeof(frame)); memcpy(frame,&id,sizeof(id));
  assert(net_rx_submit_irq(NULL,frame,sizeof(frame)));
}
static unsigned packet_id(struct rx_buffer *p) {
  unsigned id; assert(p && packet_current(&p->packet));
  assert(p->packet.length == NET_FRAME_MAX);
  memcpy(&id,p->packet.data,sizeof(id));
  for(size_t i=sizeof(id);i<NET_FRAME_MAX;i++) assert(p->packet.data[i] == (unsigned char)id);
  return id;
}
#define TOTAL 100000u
static atomic_uint producers_done, received;
static atomic_uchar seen[TOTAL];
static void *producer(void *arg) {
  unsigned base=(uintptr_t)arg;
  for(unsigned i=0;i<TOTAL/2;i++) submit_id(base+i);
  atomic_fetch_add(&producers_done,1); return NULL;
}
static void *consumer(void *arg) {
  (void)arg;
  for(;;) {
    struct rx_buffer *p=take();
    if(!p) {
      if(atomic_load(&producers_done)==2) { if(!(p=take())) break; }
      else { sched_yield(); continue; }
    }
    unsigned id=packet_id(p); assert(id<TOTAL);
    assert(atomic_fetch_add(&seen[id],1)==0); atomic_fetch_add(&received,1);
    release(p);
  }
  return NULL;
}
int main(void) {
  free_pages=1u<<20; net_core_init();
  dev.registered=true; dev.generation=7; default_device=&dev;
  assert(!net_rx_submit_irq(NULL,NULL,1));
  unsigned char oversize[NET_FRAME_MAX+1]={0};
  assert(!net_rx_submit_irq(NULL,oversize,sizeof(oversize)));
  /* Exceeds both former limits, checks FIFO, payload and accounting. */
  for(unsigned i=0;i<12000;i++) submit_id(i);
  uint32_t queued,owned; net_queue_snapshot(&queued,&owned);
  assert(queued==12000 && owned==12000 && !dev.stats.rx_dropped);
  for(unsigned i=0;i<12000;i++) { struct rx_buffer *p=take(); assert(packet_id(p)==i); release(p); }
  net_queue_snapshot(&queued,&owned);
  assert(!queued && owned==NET_PACKET_HOT_CAPACITY && live_pages==owned);
  /* Reuse cached buffers and empty->nonempty transitions repeatedly. */
  for(unsigned i=0;i<10000;i++) { submit_id(i); struct rx_buffer *p=take(); assert(packet_id(p)==i); release(p); }
  submit_id(1); struct rx_buffer *p=take(); ++dev.generation;
  assert(!packet_current(&p->packet)); release(p); trim();
  /* Allocation failure does not enqueue or leak; recovery accepts traffic. */
  fail_alloc=true; assert(!net_rx_submit_irq(NULL,oversize,1));
  assert(!queue_depth && !live_pages && dev.stats.rx_dropped==1);
  fail_alloc=false; submit_id(2); p=take(); assert(packet_id(p)==2); release(p); trim();
  /* Budget follows current available memory, with no fixed packet limit. */
  free_pages=80; refresh_rx_budget();
  for(unsigned i=0;i<10;i++) submit_id(i);
  assert(!net_rx_submit_irq(NULL,oversize,1)); assert(queue_depth==10 && live_pages==10);
  free_pages+=80; /* Memory returned by another subsystem. */
  refresh_rx_budget();
  for(unsigned i=10;i<20;i++) submit_id(i);
  assert(!net_rx_submit_irq(NULL,oversize,1));
  for(unsigned i=0;i<20;i++) { p=take(); assert(packet_id(p)==i); release(p); }
  trim(); assert(free_pages==160);
  free_pages=1u<<20; refresh_rx_budget();
  pthread_t threads[4];
  assert(!pthread_create(&threads[0],NULL,producer,(void*)(uintptr_t)0));
  assert(!pthread_create(&threads[1],NULL,producer,(void*)(uintptr_t)(TOTAL/2)));
  assert(!pthread_create(&threads[2],NULL,consumer,NULL));
  assert(!pthread_create(&threads[3],NULL,consumer,NULL));
  for(unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
  assert(atomic_load(&received)==TOTAL);
  for(unsigned i=0;i<TOTAL;i++) assert(atomic_load(&seen[i])==1);
  assert(dev.stats.rx_dropped==3 && dev.stats.queue_full==3);
  assert(wakes>0); trim(); assert(free_pages==(1u<<20));
  puts("PASS: dynamic RX >12000 FIFO packets, burst reclamation, OOM/pressure recovery, generation checks, 100000 concurrent packets, no page leaks");
}
