#!/usr/bin/env python3
"""Run real LinuxKPI queue/worker code against concurrent requeue and flush."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/workqueue.c').read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
code = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
struct list_head { struct list_head *next,*prev; };
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
#define list_for_each_entry(p,h,m) for(p=list_first_entry(h,__typeof__(*p),m); &(p)->m!=(h); p=container_of(p->m.next,__typeof__(*p),m))
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add_tail(struct list_head *p,struct list_head *h) { p->next=h;p->prev=h->prev;h->prev->next=p;h->prev=p; }
static void list_del_init(struct list_head *p) { p->prev->next=p->next;p->next->prev=p->prev;INIT_LIST_HEAD(p); }
#define LIST_HEAD(n) struct list_head n; INIT_LIST_HEAD(&n)
static void list_splice_init(struct list_head *h,struct list_head *dest) { if(list_empty(h))return; dest->next=h->next;dest->prev=h->prev;h->next->prev=dest;h->prev->next=dest;INIT_LIST_HEAD(h); }
typedef pthread_mutex_t spinlock_t;
#define spin_lock pthread_mutex_lock
#define spin_unlock pthread_mutex_unlock
#define spin_lock_irqsave(l,f) do { (f)=0;spin_lock(l); }while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f);spin_unlock(l); }while(0)
#define kfree(p) ((void)(p))
#define msecs_to_jiffies(n) (n)
#define KPI_SERVICE_IDLE_MS 1000
#define msleep(n) usleep((n)*1000)
#define wait_event_timeout(w,c,t) ({ int n=0; while(!(c)&&n++<(t)*20)usleep(50); 1; })
#define wait_event(w,c) do { while(!(c))usleep(50); }while(0)
struct wait_queue_head { int unused; };
static void __kpi_wake_up(struct wait_queue_head *w,int a,int b) { (void)w;(void)a;(void)b; }
struct task_struct { int unused; };
static atomic_bool stop;
static bool kthread_should_stop(void) { return atomic_load(&stop); }
struct work_struct { struct list_head entry;void (*func)(struct work_struct*);volatile int pending,running;struct workqueue_struct *wq; };
struct workqueue_struct { char *name;struct list_head pending;spinlock_t lock;struct wait_queue_head more_work,flush_wait;unsigned active,gen;int max_workers;unsigned nr_workers;struct task_struct *workers[4]; };
struct wq_worker { struct workqueue_struct *wq;int idx; };
static void wq_ensure_worker(struct workqueue_struct *q) { (void)q; }
'''
code += '\n'.join(function(s) for s in ('static int worker_fn(', 'static bool __queue_work(', 'bool flush_work(', 'bool cancel_work_sync('))
code += r'''
static struct workqueue_struct q;
static struct work_struct work;
static atomic_int calls,inside;
static atomic_bool release_first,flush_done;
static void callback(struct work_struct *w) {
 (void)w;assert(atomic_fetch_add(&inside,1)==0);
 int n=atomic_fetch_add(&calls,1);
 if(n==0)while(!atomic_load(&release_first))usleep(50);
 assert(atomic_fetch_sub(&inside,1)==1);
}
static void *worker(void *a) { worker_fn(a);return NULL; }
static void *flusher(void *a) { (void)a;flush_work(&work);atomic_store(&flush_done,true);return NULL; }
int main(void) {
 INIT_LIST_HEAD(&q.pending);pthread_mutex_init(&q.lock,NULL);
 INIT_LIST_HEAD(&work.entry);work.func=callback;
 struct wq_worker wa[2]={{&q,0},{&q,1}};
 struct task_struct tasks[2];q.workers[0]=&tasks[0];q.workers[1]=&tasks[1];q.nr_workers=2;
 pthread_t threads[2],flush;
 assert(__queue_work(&q,&work));
 for(int i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,worker,&wa[i]));
 while(!atomic_load(&calls))usleep(50);
 assert(!work.pending && work.running);
 assert(__queue_work(&q,&work));assert(!__queue_work(&q,&work));
 assert(!pthread_create(&flush,NULL,flusher,NULL));
 usleep(20000);assert(atomic_load(&calls)==1 && !atomic_load(&flush_done));
 atomic_store(&release_first,true);pthread_join(flush,NULL);
 assert(atomic_load(&calls)==2 && !work.pending && !work.running && q.active==0);
 atomic_store(&stop,true);
 for(int i=0;i<2;i++)pthread_join(threads[i],NULL);
 assert(list_empty(&q.pending));
 /* Cancellation removes a queued invocation, even while another runs. */
 work.running=1;assert(__queue_work(&q,&work));work.running=0;
 assert(cancel_work_sync(&work));assert(!work.pending && !q.active && list_empty(&q.pending));
 puts("PASS: running work requeues once, never runs concurrently, flush waits and cancellation drains");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-kpi-workqueue-') as tmp:
    p = Path(tmp)
    (p/'check.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True,timeout=10)
