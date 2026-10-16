#!/usr/bin/env python3
"""Exercise the actual FIFO handoff against concurrent thread cleanup."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('WAIT_QUEUE_SOURCE',
              root / 'kernel/src/sched/wait.c')).read_text()
start = source.index('bool wait_queue_handoff(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
enum { THREAD_BLOCKED, THREAD_READY, THREAD_DEAD, THREAD_ZOMBIE };
struct thread { unsigned tid; int state; uint64_t wakeup_ticks; };
typedef struct wait_queue_entry {
 struct thread *thread;
 struct wait_queue *wq;
 struct wait_queue_entry *next;
} wait_queue_entry_t;
typedef struct wait_queue {
 pthread_mutex_t lock;
 wait_queue_entry_t *head;
} wait_queue_t;
static _Thread_local bool irq_enabled = true, queue_held;
typedef bool hal_irq_state_t;
static bool hal_irq_save(void) { bool old=irq_enabled;irq_enabled=false;return old; }
static void hal_irq_restore(bool old) { irq_enabled=old; }
#define LOCKDIAG_WQ_LOCK(q) do { pthread_mutex_lock(&(q)->lock);queue_held=true; } while (0)
#define LOCKDIAG_WQ_UNLOCK(q) do { queue_held=false;pthread_mutex_unlock(&(q)->lock); } while (0)
static bool wq_entry_ptr_sane(const void *p) { return p != NULL; }
static wait_queue_t *queue, *cleanup_queue;
static wait_queue_entry_t *selected;
static pthread_t cleaner;
static atomic_bool observed, reaped;
static unsigned published, wakes, granted;
static bool race_cleanup;
static void *cleanup(void *arg) {
 (void)arg;
 /* Match cleanup's back-pointer check: NULL would let it bypass exclusion. */
 cleanup_queue=selected->wq;
 atomic_store(&observed,true);
 if(cleanup_queue) pthread_mutex_lock(&cleanup_queue->lock);
 atomic_store(&reaped,true);
 if(cleanup_queue) pthread_mutex_unlock(&cleanup_queue->lock);
 return NULL;
}
static void publish(struct thread *t,void *arg) {
 assert(arg==queue && queue_held && !irq_enabled);
 assert(t->wakeup_ticks==5000 && selected->wq==queue);
 published++;granted=t->tid;
 if(race_cleanup) {
  assert(!pthread_create(&cleaner,NULL,cleanup,NULL));
  while(!atomic_load(&observed)) sched_yield();
  assert(cleanup_queue==queue && !atomic_load(&reaped));
 }
}
static void sched_wakeup(struct thread *t) {
 assert(queue_held && !irq_enabled && granted==t->tid);
 assert(selected->wq==queue && !atomic_load(&reaped));
 assert(t->wakeup_ticks==0);
 t->state=THREAD_READY;wakes++;
}
'''
code += source[start:end]
code += r'''
int main(void) {
 wait_queue_t q={.lock=PTHREAD_MUTEX_INITIALIZER};queue=&q;
 struct thread dead={.tid=1,.state=THREAD_DEAD};
 struct thread a={.tid=2,.state=THREAD_BLOCKED,.wakeup_ticks=5000};
 struct thread b={.tid=3,.state=THREAD_BLOCKED,.wakeup_ticks=5000};
 wait_queue_entry_t eb={.thread=&b,.wq=&q};
 wait_queue_entry_t ea={.thread=&a,.wq=&q,.next=&eb};
 wait_queue_entry_t ed={.thread=&dead,.wq=&q,.next=&ea};q.head=&ed;
 selected=&ea;race_cleanup=true;
 assert(wait_queue_handoff(&q,publish,&q));
 pthread_join(cleaner,NULL);
 assert(atomic_load(&reaped) && a.state==THREAD_READY && ea.wq==NULL);
 assert(q.head==&ed && ed.next==&eb && ea.next==NULL);
 assert(published==1 && wakes==1 && irq_enabled);
 atomic_store(&reaped,false);race_cleanup=false;selected=&eb;irq_enabled=false;
 assert(wait_queue_handoff(&q,publish,&q));
 assert(!irq_enabled && published==2 && wakes==2 && b.state==THREAD_READY);
 assert(eb.wq==NULL && q.head==&ed && ed.next==NULL);
 assert(!wait_queue_handoff(&q,publish,&q));
 assert(!wait_queue_handoff(NULL,publish,&q));
 assert(!wait_queue_handoff(&q,NULL,&q));
 assert(published==2 && wakes==2);
 puts("PASS actual FIFO handoff: publication and wake exclude reaping, preserve deadline until publication and restore IRQ state");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-handoff-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
