#!/usr/bin/env python3
"""Execute the actual IRQ core with concurrent dispatch/teardown and threads."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root/'kernel/linuxkpi/src/irq.c').read_text().split('/* ── affinity stubs')[0]
s = re.sub(r'^#include.*$', '', s, flags=re.M)
shim = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#define ENOTSUPP 524
#define IRQF_SHARED 1
#define IRQF_ONESHOT 2
#define IRQ_NONE 0
#define IRQ_WAKE_THREAD 2
typedef int irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int,void*);
typedef pthread_mutex_t spinlock_t;
#define spin_lock_irqsave(l,f) do{(f)=0;assert(!pthread_mutex_lock(l));}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);assert(!pthread_mutex_unlock(l));}while(0)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define GFP_KERNEL 0
#define kzalloc(n,g) calloc(1,n)
#define kfree free
#define msleep(n) usleep((n)*1000)
struct work_struct {void (*func)(struct work_struct*);int pending,running;};
#define INIT_WORK(w,f) do{(w)->func=(f);(w)->pending=(w)->running=0;}while(0)
static pthread_mutex_t pool=PTHREAD_MUTEX_INITIALIZER;
static struct work_struct *job;
static atomic_bool stop_worker;
static bool schedule_work(struct work_struct *w){
 pthread_mutex_lock(&pool);
 bool ok=!w->pending;
 if(ok){assert(!job);w->pending=1;job=w;}
 pthread_mutex_unlock(&pool);return ok;
}
static bool flush_work(struct work_struct *w){
 for(;;){pthread_mutex_lock(&pool);bool busy=w->pending||w->running;pthread_mutex_unlock(&pool);if(!busy)return true;usleep(50);}
}
bool synchronize_hardirq(unsigned);
static void *worker(void *unused){
 (void)unused;
 while(!atomic_load(&stop_worker)){
  pthread_mutex_lock(&pool);struct work_struct *w=job;
  if(w){job=NULL;w->pending=0;w->running=1;}
  pthread_mutex_unlock(&pool);
  if(!w){usleep(50);continue;}
  w->func(w);
  pthread_mutex_lock(&pool);w->running=0;pthread_mutex_unlock(&pool);
 }return NULL;
}
'''
main = r'''
static atomic_bool entered,release_handler,sync_done,free_done,dispatch_stop;
static atomic_int calls,hard_calls,masked;
static void mask(void *p,int m){(void)p;atomic_store(&masked,m);}
static irqreturn_t hard(int irq,void *p){(void)irq;(void)p;atomic_fetch_add(&hard_calls,1);return IRQ_WAKE_THREAD;}
static irqreturn_t threaded(int irq,void *p){(void)irq;(void)p;atomic_fetch_add(&calls,1);atomic_store(&entered,true);while(!atomic_load(&release_handler))usleep(50);return IRQ_NONE;}
static void *syncer(void *p){(void)p;disable_irq(77);atomic_store(&sync_done,true);return NULL;}
static void *freer(void *p){free_irq(77,p);atomic_store(&free_done,true);return NULL;}
static void *dispatch(void *p){(void)p;while(!atomic_load(&dispatch_stop))linuxkpi_irq_handle_vector(77);return NULL;}
int main(void){
 pthread_t worker_thread,sync_thread,free_thread,dispatch_thread;int cookie;
 assert(request_threaded_irq(77,hard,threaded,IRQF_ONESHOT,"no-mask",&cookie)==-ENOTSUPP);
 linuxkpi_irq_register_mask(77,mask,NULL);
 assert(!request_threaded_irq(77,hard,threaded,IRQF_ONESHOT,"test",&cookie));
 assert(!atomic_load(&masked));linuxkpi_irq_handle_vector(77);
 assert(!synchronize_hardirq(77)); /* queued, worker not started */
 assert(!pthread_create(&sync_thread,NULL,syncer,NULL));
 usleep(20000);assert(!atomic_load(&sync_done));
 assert(!pthread_create(&worker_thread,NULL,worker,NULL));
 while(!atomic_load(&entered))usleep(50);
 assert(atomic_load(&masked));assert(!synchronize_hardirq(77));
 for(int i=0;i<1000;i++)linuxkpi_irq_handle_vector(77);
 assert(atomic_load(&hard_calls)==1&&atomic_load(&calls)==1);
 enable_irq(77);assert(atomic_load(&masked));
 disable_irq_nosync(77);
 assert(!pthread_create(&free_thread,NULL,freer,&cookie));
 usleep(20000);assert(!atomic_load(&sync_done)&&!atomic_load(&free_done));
 atomic_store(&release_handler,true);
 pthread_join(sync_thread,NULL);pthread_join(free_thread,NULL);
 assert(synchronize_hardirq(77)&&atomic_load(&masked));
 /* Repeated concurrent dispatch/free, exercising action lifetime. */
 for(int i=0;i<300;i++){
  assert(!request_threaded_irq(77,hard,threaded,IRQF_ONESHOT,"stress",&cookie));
  atomic_store(&dispatch_stop,false);
  assert(!pthread_create(&dispatch_thread,NULL,dispatch,NULL));
  usleep(100);free_irq(77,&cookie);atomic_store(&dispatch_stop,true);pthread_join(dispatch_thread,NULL);
  assert(synchronize_hardirq(77));
 }
 /* Shared registration must agree with every existing action. */
 assert(!request_threaded_irq(77,hard,NULL,0,"exclusive",&cookie));
 int other;assert(request_threaded_irq(77,hard,NULL,IRQF_SHARED,"invalid",&other)==-EBUSY);
 free_irq(77,&cookie);
 atomic_store(&stop_worker,true);pthread_join(worker_thread,NULL);
 puts("PASS: threaded sync, hardirq contract, ONESHOT/disable masking, shared rejection, 300 concurrent dispatch/free iterations");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-irq-sync-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+main)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=20)
