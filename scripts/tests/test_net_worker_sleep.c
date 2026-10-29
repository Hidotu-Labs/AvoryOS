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
#include "hal/hal.h"
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
#include <setjmp.h>
static bool irq_enabled=true, inject_packet, early_wake, timer_case;
static unsigned registrations, removals, yields, handled, timer_ticks;
static struct thread current;
static jmp_buf finished;
static void *entry_allocation;
static wait_queue_entry_t *registered_entry;
static struct net_device dev;
hal_irq_state_t hal_irq_save(void){bool previous=irq_enabled;irq_enabled=false;return previous;}
void hal_irq_restore(hal_irq_state_t flags){
  if(flags&&(current.state==THREAD_BLOCKED||current.state==THREAD_SLEEPING))assert(current.wakeup_ticks>10000);
  irq_enabled=flags;
}
uint64_t lapic_timer_get_ticks(void){
  /* Model a timer interrupt at this call, the old timer's vulnerable point. */
  if(irq_enabled&&(current.state==THREAD_BLOCKED||current.state==THREAD_SLEEPING))assert(current.wakeup_ticks>10000);
  return 10000;
}
struct thread *sched_get_current(void){return &current;}
void *kmalloc(size_t size){assert(!entry_allocation);return entry_allocation=calloc(1,size);}
void *pmm_alloc_page(void){return aligned_alloc(PAGE_SIZE,PAGE_SIZE);}
void pmm_free_page(void *page){free(page);}
uint64_t pmm_get_hhdm_offset(void){return 0;}
size_t pmm_get_free_pages(void){return 1u<<20;}
void klog_puts(const char *s){(void)s;}
void wait_queue_init(wait_queue_t *q){memset(q,0,sizeof(*q));}
void wait_queue_wake_all(wait_queue_t *q){(void)q;current.state=THREAD_RUNNING;}
void wait_queue_wake_one(wait_queue_t *q){wait_queue_wake_all(q);}
void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *entry){
  (void)q;assert(!irq_enabled);assert(current.wakeup_ticks==10050);assert(current.state==THREAD_BLOCKED);
  if(++registrations==2){assert(removals==1);longjmp(finished,1);}
  assert(!registered_entry);registered_entry=entry;
  if(inject_packet){unsigned char frame[64]={0};assert(net_rx_submit_irq(NULL,frame,sizeof(frame)));}
  if(early_wake)current.state=THREAD_RUNNING;
}
void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *entry){
  (void)q;assert(!irq_enabled);assert(current.state==THREAD_RUNNING);assert(!current.wakeup_ticks);assert(registered_entry==entry);registered_entry=NULL;removals++;
}
void net_test_yield(void){
  assert(irq_enabled);assert(current.wakeup_ticks>10000);
  if(timer_case){assert(current.state==THREAD_SLEEPING);assert(current.wakeup_ticks==10010);}
  else{assert(registered_entry);assert(current.state==THREAD_BLOCKED||early_wake);}
  yields++;current.state=THREAD_RUNNING;
}
void tcp_timer_tick(uint64_t now){
  assert(now==10000&&irq_enabled&&current.state==THREAD_RUNNING&&!current.wakeup_ticks);
  if(++timer_ticks==2)longjmp(finished,1);
}
static void handle(struct net_packet *packet){
  assert(packet->length==64);assert(current.state==THREAD_RUNNING&&!current.wakeup_ticks);assert(irq_enabled);handled++;
}
static void test_rx(bool inject,bool wake){
  memset(&current,0,sizeof(current));current.state=THREAD_RUNNING;irq_enabled=true;registrations=removals=yields=handled=0;inject_packet=inject;early_wake=wake;registered_entry=NULL;
  if(!setjmp(finished))net_worker();
  assert(registrations==2&&removals==1);assert(yields==(inject?0:1));assert(handled==(inject?1:0));
  free(entry_allocation);entry_allocation=NULL;irq_enabled=true;current.state=THREAD_RUNNING;current.wakeup_ticks=0;
}
int main(int argc, char **argv){
  (void)argv;
  net_core_init();dev.registered=true;dev.generation=1;default_device=&dev;rx_handler=handle;
  if(argc==1){test_rx(false,false);test_rx(false,true);test_rx(true,false);}
  else current.state=THREAD_RUNNING;
  timer_case=true;yields=0;if(!setjmp(finished))net_timer_worker();assert(timer_ticks==2&&yields==1);
  while(hot_count)pmm_free_page(hot_pool[--hot_count]);
  puts("PASS: RX registration, arrival/wake races, cleanup and TCP timer deadline publication");
}
