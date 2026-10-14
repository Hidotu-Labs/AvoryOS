#!/usr/bin/env python3
"""Run actual irq_work worker/sync against concurrent callback self-requeue."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/irq_work.c').read_text().split('void linuxkpi_irq_work_init(')[0]
s=re.sub(r'^#include.*$', '',s,flags=re.M)
shim=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
struct irq_work {struct irq_work *next;void (*func)(struct irq_work*);int queued;};
struct wait_queue_head {int dummy;};struct kpi_service {int dummy;};
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(l,f) do{(f)=0;pthread_mutex_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do{(void)(f);pthread_mutex_unlock(l);}while(0)
#define wait_event_interruptible_timeout(h,c,t) ((void)(h),(void)(c),(void)(t),usleep(50),0)
#define KPI_SERVICE_IDLE_MS 1
#define msecs_to_jiffies(n) (n)
#define msleep(n) usleep((n)*1000)
static atomic_bool stop;
static unsigned kpi_service_gen(struct kpi_service*s){(void)s;return 0;}
static bool kthread_should_stop(void){return atomic_load(&stop);}
static void kpi_service_forget(struct kpi_service*s){(void)s;}
static bool kpi_service_retire(struct kpi_service*s,unsigned g){(void)s;(void)g;return false;}
static void kpi_service_kick(struct kpi_service*s,bool b){(void)s;(void)b;}
'''
main=r'''
static struct irq_work work;static atomic_int calls;static atomic_bool release_first,release_second,sync_done;
static void callback(struct irq_work*w){
 int n=atomic_fetch_add(&calls,1);
 if(n==0){while(!atomic_load(&release_first))usleep(50);assert(irq_work_queue(w));assert(!irq_work_queue(w));}
 else {assert(n==1);while(!atomic_load(&release_second))usleep(50);}
}
static void *worker(void*p){(void)p;irq_work_kthread(NULL);return NULL;}
static void *syncer(void*p){(void)p;irq_work_sync(&work);atomic_store(&sync_done,true);return NULL;}
int main(void){
 pthread_t wt,st;init_irq_work(&work,callback);assert(irq_work_queue(&work));
 assert(!pthread_create(&st,NULL,syncer,NULL));usleep(20000);assert(!atomic_load(&sync_done));
 assert(!pthread_create(&wt,NULL,worker,NULL));while(!atomic_load(&calls))usleep(50);
 atomic_store(&release_first,true);while(atomic_load(&calls)<2)usleep(50);
 usleep(20000);assert(!atomic_load(&sync_done));atomic_store(&release_second,true);pthread_join(st,NULL);
 assert(atomic_load(&calls)==2&&!work.queued&&!irq_work_current);
 atomic_store(&stop,true);pthread_join(wt,NULL);
 puts("PASS: irq_work_sync waits through pending, running and self-requeued callbacks");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-irqwork-sync-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+s+main)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
