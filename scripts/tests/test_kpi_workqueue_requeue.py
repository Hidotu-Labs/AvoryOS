#!/usr/bin/env python3
"""Run real LinuxKPI queue/worker code against concurrent requeue and flush."""
from pathlib import Path
import os
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
#include <string.h>
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
#define kfree free
#define KPI_WQ_MAX_WORKERS 4
#define msecs_to_jiffies(n) (n)
#define KPI_SERVICE_IDLE_MS 1000
#define msleep(n) usleep((n)*1000)
#define wait_event_timeout(w,c,t) ({ int n=0; while(!(c)&&n++<(t)*20)usleep(50); 1; })
#define wait_event(w,c) do { while(!(c))usleep(50); }while(0)
struct wait_queue_head { int unused; };
static void __kpi_wake_up(struct wait_queue_head *w,int a,int b) { (void)w;(void)a;(void)b; }
struct task_struct { pthread_t thread; };
static atomic_bool stop;
static bool kthread_should_stop(void) { return atomic_load(&stop); }
static struct task_struct *linuxkpi_task_get(struct task_struct *t){return t;}
static int kthread_stop(struct task_struct *t){atomic_store(&stop,true);assert(!pthread_join(t->thread,NULL));return 0;}
struct work_struct { struct list_head entry;void (*func)(struct work_struct*);volatile int pending,running;struct workqueue_struct *wq; };
struct workqueue_struct { char *name;struct list_head pending;spinlock_t lock;struct wait_queue_head more_work,flush_wait;unsigned active,gen;int max_workers;unsigned nr_workers;struct task_struct *workers[4];struct work_struct *current_work[4]; };
struct wq_worker { struct workqueue_struct *wq;int idx; };
struct timer_list {int active;};
static unsigned long jiffies;
static int mod_timer(struct timer_list *t,unsigned long when){(void)when;t->active=1;return 0;}
struct delayed_work {struct work_struct work;struct timer_list timer;int canceling;};
static struct workqueue_struct *system_wq;
static int del_timer(struct timer_list *t){int ret=t->active;t->active=0;return ret;}
static int del_timer_sync(struct timer_list *t){return del_timer(t);}
static void wq_ensure_worker(struct workqueue_struct *q) { (void)q; }
'''
code += '\n'.join(function(s) for s in ('static bool kpi_work_running(', 'static int worker_fn(', 'static bool __queue_work(', 'bool flush_work(', 'bool cancel_work_sync(', 'bool flush_delayed_work(', 'bool cancel_delayed_work(', 'bool cancel_delayed_work_sync(', 'bool queue_delayed_work(', 'bool mod_delayed_work(', 'void flush_workqueue(', 'void drain_workqueue(', 'void destroy_workqueue('))
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
static atomic_int freed;
static void free_callback(struct work_struct *w) { free(w);atomic_fetch_add(&freed,1); }
static void *worker(void *a) { struct wq_worker *wa=malloc(sizeof(*wa));*wa=*(struct wq_worker*)a;worker_fn(wa);return NULL; }
static atomic_bool requeue_now;
static void cancel_callback(struct work_struct *w) {
 atomic_store(&inside,1);
 while(!atomic_load(&requeue_now))usleep(50);
 assert(__queue_work(&q,w));
 atomic_store(&stop,true); /* keep requeued work pending after this returns */
}
static struct delayed_work rearming;
static atomic_bool delayed_entered,delayed_release,delayed_cancel_done;
static void delayed_rearm(struct work_struct *w){
 (void)w;atomic_store(&delayed_entered,true);
 while(!atomic_load(&delayed_release))usleep(50);
 queue_delayed_work(&q,&rearming,100);
}
static void *delayed_canceller(void *a){(void)a;cancel_delayed_work_sync(&rearming);atomic_store(&delayed_cancel_done,true);return NULL;}
static void *canceller(void *a) { (void)a;cancel_work_sync(&work);return NULL; }
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
 pthread_mutex_lock(&q.lock);assert(!work.pending && kpi_work_running(&q,&work));pthread_mutex_unlock(&q.lock);
 assert(__queue_work(&q,&work));assert(!__queue_work(&q,&work));
 assert(!pthread_create(&flush,NULL,flusher,NULL));
 usleep(20000);assert(atomic_load(&calls)==1 && !atomic_load(&flush_done));
 atomic_store(&release_first,true);pthread_join(flush,NULL);
 assert(atomic_load(&calls)==2 && !work.pending && !work.running && q.active==0);
 work.func=cancel_callback;atomic_store(&inside,0);
 assert(__queue_work(&q,&work));while(!atomic_load(&inside))usleep(50);
 pthread_t cancel;assert(!pthread_create(&cancel,NULL,canceller,NULL));
 usleep(20000);atomic_store(&requeue_now,true);pthread_join(cancel,NULL);
 assert(!work.pending&&!work.running&&!q.active&&list_empty(&q.pending));
 for(int i=0;i<2;i++)pthread_join(threads[i],NULL);
 assert(list_empty(&q.pending));
 /* Cancellation removes a queued invocation, even while another runs. */
 assert(__queue_work(&q,&work));
 assert(cancel_work_sync(&work));assert(!work.pending && !q.active && list_empty(&q.pending));
 struct delayed_work dw={0};INIT_LIST_HEAD(&dw.work.entry);dw.work.func=callback;dw.work.wq=&q;dw.timer.active=1;
 atomic_store(&inside,0);atomic_store(&stop,false);q.workers[0]=&tasks[0];q.nr_workers=1;system_wq=&q;
 assert(!pthread_create(&threads[0],NULL,worker,&wa[0]));
 assert(flush_delayed_work(&dw));assert(!dw.timer.active&&!dw.work.pending&&!dw.work.running);
 /* Running delayed work must not leave a timer armed during cancellation. */
 INIT_LIST_HEAD(&rearming.work.entry);rearming.work.func=delayed_rearm;rearming.work.wq=&q;
 assert(__queue_work(&q,&rearming.work));while(!atomic_load(&delayed_entered))usleep(50);
 pthread_t dc;assert(!pthread_create(&dc,NULL,delayed_canceller,NULL));
 usleep(20000);assert(!atomic_load(&delayed_cancel_done));
 atomic_store(&delayed_release,true);pthread_join(dc,NULL);
 assert(!rearming.timer.active&&!rearming.work.pending&&!rearming.canceling);
 assert(queue_delayed_work(&q,&rearming,100));assert(cancel_delayed_work_sync(&rearming));
 rearming.canceling=1;assert(!mod_delayed_work(&q,&rearming,100));assert(!queue_delayed_work(&q,&rearming,0));rearming.canceling=0;
 /* Like TTM delayed_delete's final put: callback releases the work itself. */
 for(int i=0;i<1000;i++){
  struct work_struct *self=calloc(1,sizeof(*self));assert(self);
  INIT_LIST_HEAD(&self->entry);self->func=free_callback;
  assert(__queue_work(&q,self));
  while(atomic_load(&freed)<=i)usleep(50);
  for(;;){pthread_mutex_lock(&q.lock);bool busy=q.active!=0;pthread_mutex_unlock(&q.lock);if(!busy)break;usleep(50);}
 }
 atomic_store(&stop,true);pthread_join(threads[0],NULL);
 assert(__queue_work(&q,&dw.work));assert(cancel_delayed_work(&dw));assert(!dw.work.pending&&!q.active);
 assert(__queue_work(&q,&dw.work));assert(cancel_delayed_work_sync(&dw));assert(!dw.work.pending&&!q.active);
 /* destroy_workqueue must drain queued work before stopping its worker. */
 struct workqueue_struct *dq=calloc(1,sizeof(*dq));assert(dq);
 INIT_LIST_HEAD(&dq->pending);pthread_mutex_init(&dq->lock,NULL);dq->name=strdup("destroy-test");
 struct task_struct dt;struct wq_worker da={dq,0};dq->workers[0]=&dt;dq->nr_workers=1;
 atomic_store(&stop,false);int before=atomic_load(&freed);
 for(int i=0;i<10;i++){struct work_struct *self=calloc(1,sizeof(*self));INIT_LIST_HEAD(&self->entry);self->func=free_callback;assert(__queue_work(dq,self));}
 assert(!pthread_create(&dt.thread,NULL,worker,&da));
 destroy_workqueue(dq);assert(atomic_load(&freed)==before+10);
 puts("PASS: 1000 callback self-frees under ASan/UBSan, requeue serialization, flush/cancel, cancellation across self-requeue, armed delayed flush, queued delayed cancellation and running delayed rearm drain and queue destruction drain");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-kpi-workqueue-') as tmp:
    p = Path(tmp)
    if os.environ.get('KPI_WQ_OLD_LIFETIME'):
        code=code.replace('wq->current_work[idx] = NULL;\n    wq->active--;', '__atomic_store_n(&work->running, 0, __ATOMIC_RELEASE);\n    wq->current_work[idx] = NULL;\n    wq->active--;')
    (p/'check.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-pthread',str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True,timeout=10)
