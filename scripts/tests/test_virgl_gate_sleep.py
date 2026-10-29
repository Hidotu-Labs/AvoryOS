#!/usr/bin/env python3
"""Actual VirGL gate: parked contenders, release/publication races and contention."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('VIRGL_SOURCE', root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu_virgl.c')).read_text()
if 'static int ioctl_gate_acquire(' in source:
    code = source[source.index('static wait_queue_t ioctl_wait'):source.index('static int wait_ioctl(')]
else:
    # The previous implementation's actual acquisition/release statements.
    body = source[source.index('int virtio_gpu_virgl_ioctl('):]
    begin = body.index('    while (__atomic_exchange_n(')
    acquire = body[begin:body.index('    int ret', begin)]
    release = body[body.index('    __atomic_store_n('):body.index('    return ret;')]
    code = 'static int ioctl_gate_acquire(void){\n'+acquire+'return 0;}\nstatic void ioctl_gate_release(void){\n'+release+'}\n'
prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define sched_yield guest_sched_yield
#define SPINLOCK_INIT 0
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define THREAD_READY 3
typedef uint64_t hal_irq_state_t;
struct thread {unsigned state;uint64_t wakeup_ticks;bool pending;pthread_mutex_t lock;pthread_cond_t cv;};
typedef struct entry {struct thread *thread;struct entry *next;bool linked;} wait_queue_entry_t;
typedef struct {int lock;wait_queue_entry_t *head;} wait_queue_t;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct thread self;
static _Thread_local bool irq=true;
static unsigned ioctl_owner,sleeps,entries;
static bool release_on_add,signal_on_add,spurious;
static struct thread *sched_get_current(void){return &self;}
static bool thread_has_pending_signal(struct thread *t){return t->pending;}
static uint64_t lapic_timer_get_ticks(void){return 10;}
static hal_irq_state_t hal_irq_save(void){bool old=irq;irq=false;return old;}
static void hal_irq_restore(uint64_t f){irq=f;}
static void wait_queue_wake_one(wait_queue_t *q){
 pthread_mutex_lock(&queue_lock);wait_queue_entry_t *e=q->head;
 if(e){q->head=e->next;e->linked=false;pthread_mutex_lock(&e->thread->lock);
 __atomic_store_n(&e->thread->state,THREAD_READY,__ATOMIC_RELEASE);pthread_cond_signal(&e->thread->cv);pthread_mutex_unlock(&e->thread->lock);}
 pthread_mutex_unlock(&queue_lock);
}
static void wait_queue_add_tail(wait_queue_t *q,wait_queue_entry_t *e){
 assert(!irq&&self.state==THREAD_BLOCKED&&self.wakeup_ticks==1010);
 pthread_mutex_lock(&queue_lock);wait_queue_entry_t **p=&q->head;while(*p)p=&(*p)->next;*p=e;e->linked=true;
 __atomic_add_fetch(&entries,1,__ATOMIC_RELAXED);pthread_mutex_unlock(&queue_lock);
 if(signal_on_add){signal_on_add=false;self.pending=true;}
 if(release_on_add){release_on_add=false;__atomic_store_n(&ioctl_owner,0,__ATOMIC_RELEASE);wait_queue_wake_one(q);}
}
static void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e){
 assert(!irq);pthread_mutex_lock(&queue_lock);wait_queue_entry_t **p=&q->head;
 while(*p&&*p!=e)p=&(*p)->next;if(*p)*p=e->next;e->linked=false;pthread_mutex_unlock(&queue_lock);
}
static void sched_yield(void){
 /* A runnable retry is exactly the old CPU-wasting path. */
 assert(irq&&self.state!=THREAD_RUNNING&&self.wakeup_ticks==1010);
 __atomic_add_fetch(&sleeps,1,__ATOMIC_RELAXED);
 if(spurious){spurious=false;self.state=THREAD_RUNNING;return;}
 pthread_mutex_lock(&self.lock);
 while(__atomic_load_n(&self.state,__ATOMIC_ACQUIRE)==THREAD_BLOCKED)pthread_cond_wait(&self.cv,&self.lock);
 pthread_mutex_unlock(&self.lock);
}
static void initialize(void){self=(struct thread){.state=THREAD_RUNNING};pthread_mutex_init(&self.lock,NULL);pthread_cond_init(&self.cv,NULL);irq=true;}
'''
test = r'''
static unsigned inside,completed;
static void *contender(void *unused){(void)unused;initialize();ioctl_gate_acquire();assert(irq&&!self.wakeup_ticks&&self.state==THREAD_RUNNING);
 assert(__atomic_add_fetch(&inside,1,__ATOMIC_SEQ_CST)==1);usleep(1000);assert(__atomic_sub_fetch(&inside,1,__ATOMIC_SEQ_CST)==0);
 ioctl_gate_release();__atomic_add_fetch(&completed,1,__ATOMIC_RELEASE);return NULL;}
int main(void){initialize();ioctl_gate_acquire();assert(ioctl_owner&&irq&&!entries);ioctl_gate_release();
 /* Release before the post-registration claim: must not sleep or lose wake. */
 ioctl_owner=1;release_on_add=true;ioctl_gate_acquire();assert(ioctl_owner&&irq&&!self.wakeup_ticks&&!sleeps);ioctl_gate_release();
 ioctl_owner=1;self.pending=true;assert(ioctl_gate_acquire()==-4&&ioctl_owner&&irq&&!self.wakeup_ticks);self.pending=false;ioctl_gate_release();
 ioctl_owner=1;signal_on_add=true;assert(ioctl_gate_acquire()==-4&&ioctl_owner&&irq&&!self.wakeup_ticks&&self.state==THREAD_RUNNING);self.pending=false;ioctl_gate_release();
 /* An early/spurious wake must register a fresh entry and park again. */
 ioctl_gate_acquire();spurious=true;unsigned early_entries=entries+2;pthread_t early;assert(!pthread_create(&early,NULL,contender,NULL));
 for(unsigned tries=0;__atomic_load_n(&entries,__ATOMIC_ACQUIRE)<early_entries&&tries<1000;tries++)usleep(1000);
 assert(entries>=early_entries&&!completed);ioctl_gate_release();assert(!pthread_join(early,NULL));assert(completed==1);completed=0;
 /* Eight clients must sleep while the owner waits for its host fence. */
 ioctl_gate_acquire();pthread_t clients[8];unsigned target_entries=entries+8;
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&clients[i],NULL,contender,NULL));
 for(unsigned tries=0;__atomic_load_n(&entries,__ATOMIC_ACQUIRE)<target_entries&&tries<1000;tries++)usleep(1000);
 usleep(20000);assert(!completed);assert(sleeps>=8);ioctl_gate_release();
 for(unsigned i=0;i<8;i++)assert(!pthread_join(clients[i],NULL));
 assert(completed==8&&!ioctl_owner&&irq);
 puts("PASS: real VirGL gate parks contending clients, retains exclusive ownership and cannot miss a pre-registration unlock");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp)
    (out/'gate.c').write_text(prefix+code+test)
    subprocess.run(['cc', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', str(out/'gate.c'), '-o', str(out/'gate')], check=True)
    subprocess.run([str(out/'gate')], check=True, timeout=15, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
