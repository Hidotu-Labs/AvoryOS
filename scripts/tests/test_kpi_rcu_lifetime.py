#!/usr/bin/env python3
"""Execute the native RCU code at deterministic callback/grace-period races."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT/'kernel/linuxkpi/src/rcu.c')
parser.add_argument('--scenario', choices=['all', 'drain'], default='all')
parser.add_argument('--repeat', type=int, default=1)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
if args.repeat < 1: parser.error('--repeat must be positive')
source = args.source.read_text().split('#define RCU_BENCH_ITERS')[0]
source = re.sub(r'^#include.*\n', '', source, flags=re.M)
source = source.replace('    rcu_wait_grace_period();\n',
                        '    rcu_wait_grace_period();\n    test_after_grace();\n')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_RELAXED)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELAXED)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define GFP_KERNEL 0
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define klogf(...) ((void)0)
struct rcu_head {struct rcu_head *next;void (*func)(struct rcu_head*);};
typedef void (*rcu_callback_t)(struct rcu_head*);
struct wait_queue_head {pthread_mutex_t lock;pthread_cond_t cond;};
typedef pthread_mutex_t spinlock_t;
#define DEFINE_SPINLOCK(n) spinlock_t n=PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(p,f) do {(f)=0;assert(!pthread_mutex_lock(p));}while(0)
#define spin_unlock_irqrestore(p,f) do {(void)(f);assert(!pthread_mutex_unlock(p));}while(0)
static void init_waitqueue_head(struct wait_queue_head *q){pthread_mutex_init(&q->lock,NULL);pthread_cond_init(&q->cond,NULL);}
static int __kpi_wake_up(struct wait_queue_head *q,int n,int flags){(void)n;(void)flags;pthread_mutex_lock(&q->lock);pthread_cond_broadcast(&q->cond);pthread_mutex_unlock(&q->lock);return 1;}
static _Thread_local int nesting,role;
static void preempt_disable(void){nesting++;}
static void preempt_enable(void){assert(nesting>0);nesting--;}
static bool stop_worker;
static bool kthread_should_stop(void){return READ_ONCE(stop_worker);}
struct kpi_service {int dummy;};
static void kpi_service_init(struct kpi_service *s,const char *n,int(*f)(void*),void *a){(void)s;(void)n;(void)f;(void)a;}
static void kpi_service_forget(struct kpi_service *s){(void)s;}
static unsigned kpi_service_gen(struct kpi_service *s){(void)s;return 0;}
static bool kpi_service_retire(struct kpi_service *s,unsigned g){(void)s;(void)g;return false;}
static void kpi_service_kick(struct kpi_service *s,bool b);
static void test_after_grace(void);
static void test_before_sleep(void);
static void test_after_wake(void);
/* Queue registration/condition testing is serialized with the mock wakeup.
 * The actual implementation's atomic state and batch handling are unchanged. */
#define wait_event(q,condition) do { \
 pthread_mutex_lock(&(q).lock); \
 while(!(condition)){ \
  test_before_sleep(); \
  pthread_cond_wait(&(q).cond,&(q).lock); \
  pthread_mutex_unlock(&(q).lock);test_after_wake();pthread_mutex_lock(&(q).lock); \
 } \
 pthread_mutex_unlock(&(q).lock); \
}while(0)
#define wait_event_timeout(q,condition,timeout) ({(void)(timeout);wait_event(q,condition);1;})
#define msecs_to_jiffies(x) (x)
#define KPI_SERVICE_IDLE_MS 10
'''
suffix = r'''
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static int after_grace,release_grace,hook_calls,a_done,b_done,gp_done,gp_waiting,wake_paused,release_wake;
static bool hold_wake;
static void notify_gate(void){pthread_cond_broadcast(&gate_cond);}
static void await(int *flag,const char *failure){
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;
 while(!*flag){int ret=pthread_cond_timedwait(&gate_cond,&gate,&deadline);if(ret==ETIMEDOUT){fprintf(stderr,"FAIL: %s\n",failure);abort();}assert(!ret);}
}
static void test_after_grace(void){
 if(role!=1 || hook_calls++)return;
 pthread_mutex_lock(&gate);after_grace=1;notify_gate();
 await(&release_grace,"worker grace gate was not released");pthread_mutex_unlock(&gate);
}
static void test_before_sleep(void){
 if(role==2){pthread_mutex_lock(&gate);gp_waiting++;notify_gate();pthread_mutex_unlock(&gate);}
}
static void test_after_wake(void){
 if(role==2 && hold_wake){
  pthread_mutex_lock(&gate);wake_paused=1;notify_gate();
  await(&release_wake,"woken grace waiter was not released");pthread_mutex_unlock(&gate);
 }
}
static void kpi_service_kick(struct kpi_service *s,bool b){(void)s;(void)b;__kpi_wake_up(&rcu_wait,0,0);}
static void callback_a(struct rcu_head *h){(void)h;pthread_mutex_lock(&gate);a_done=1;notify_gate();pthread_mutex_unlock(&gate);}
static void callback_b(struct rcu_head *h){(void)h;pthread_mutex_lock(&gate);b_done=1;notify_gate();pthread_mutex_unlock(&gate);}
static void *worker(void *p){role=1;rcu_kthread(p);return NULL;}
static void *synchronizer(void *p){(void)p;role=2;synchronize_rcu();pthread_mutex_lock(&gate);gp_done++;notify_gate();pthread_mutex_unlock(&gate);return NULL;}
static void late_callback(void){
 struct rcu_head a,b;pthread_t thread;
 call_rcu(&a,callback_a);pthread_create(&thread,NULL,worker,NULL);
 pthread_mutex_lock(&gate);await(&after_grace,"worker did not finish first grace period");pthread_mutex_unlock(&gate);
 /* A new reader starts AFTER the completed grace period, then its object is
  * removed and queued. B requires a different grace period. */
 __kpi_rcu_read_lock();call_rcu(&b,callback_b);
 pthread_mutex_lock(&gate);release_grace=1;notify_gate();await(&a_done,"callback A did not run");
 if(b_done){fputs("FAIL: late callback freed while its reader remained active\n",stderr);abort();}
 pthread_mutex_unlock(&gate);
 __kpi_rcu_read_unlock();
 pthread_mutex_lock(&gate);await(&b_done,"late callback did not complete after reader exit");pthread_mutex_unlock(&gate);
 rcu_barrier();WRITE_ONCE(stop_worker,true);__kpi_wake_up(&rcu_wait,0,0);pthread_join(thread,NULL);
}
static void completed_drain(void){
 pthread_t thread;hold_wake=true;
 __kpi_rcu_read_lock();pthread_create(&thread,NULL,synchronizer,NULL);
 pthread_mutex_lock(&gate);await(&gp_waiting,"grace waiter did not block");pthread_mutex_unlock(&gate);
 __kpi_rcu_read_unlock();
 pthread_mutex_lock(&gate);await(&wake_paused,"grace waiter did not wake");pthread_mutex_unlock(&gate);
 /* The old readers have drained; a later reader must not hide that event. */
 __kpi_rcu_read_lock();
 pthread_mutex_lock(&gate);release_wake=1;notify_gate();
 await(&gp_done,"completed grace period lost when new reader entered");pthread_mutex_unlock(&gate);
 __kpi_rcu_read_unlock();pthread_join(thread,NULL);hold_wake=false;
}
static void concurrent_waiters(void){
 pthread_t threads[8];gp_done=gp_waiting=0;
 __kpi_rcu_read_lock();
 for(int i=0;i<8;i++)pthread_create(&threads[i],NULL,synchronizer,NULL);
 pthread_mutex_lock(&gate);while(gp_waiting<8){struct timespec d;clock_gettime(CLOCK_REALTIME,&d);d.tv_sec+=3;assert(!pthread_cond_timedwait(&gate_cond,&gate,&d));}pthread_mutex_unlock(&gate);
 assert(!gp_done);__kpi_rcu_read_unlock();
 for(int i=0;i<8;i++)pthread_join(threads[i],NULL);
 assert(gp_done==8);
}
static int barrier_started,barrier_done,protected_callback_done;
static void protected_callback(struct rcu_head *h){(void)h;pthread_mutex_lock(&gate);protected_callback_done=1;notify_gate();pthread_mutex_unlock(&gate);}
static void *barrier_thread(void *p){(void)p;pthread_mutex_lock(&gate);barrier_started=1;notify_gate();pthread_mutex_unlock(&gate);rcu_barrier();pthread_mutex_lock(&gate);barrier_done=1;notify_gate();pthread_mutex_unlock(&gate);return NULL;}
static void detached_barrier(void){
 pthread_t runner,barrier;struct rcu_head h;WRITE_ONCE(stop_worker,false);
 __kpi_rcu_read_lock();call_rcu(&h,protected_callback);pthread_create(&runner,NULL,worker,NULL);
 /* The implementation-specific state is not used: the reader prevents this
  * callback from executing, irrespective of whether the batch was detached. */
 pthread_create(&barrier,NULL,barrier_thread,NULL);
 pthread_mutex_lock(&gate);await(&barrier_started,"barrier did not start");pthread_mutex_unlock(&gate);
 usleep(20000);assert(!READ_ONCE(barrier_done) && !READ_ONCE(protected_callback_done));
 __kpi_rcu_read_unlock();pthread_join(barrier,NULL);assert(barrier_done && protected_callback_done);
 WRITE_ONCE(stop_worker,true);__kpi_wake_up(&rcu_wait,0,0);pthread_join(runner,NULL);
}
static int executed,read_active;
struct item {struct rcu_head head;};
static void counted_callback(struct rcu_head *h){__atomic_add_fetch(&executed,1,__ATOMIC_SEQ_CST);free(container_of(h,struct item,head));}
static void *producer(void *arg){(void)arg;for(int i=0;i<2000;i++){struct item *x=malloc(sizeof(*x));assert(x);call_rcu(&x->head,counted_callback);}return NULL;}
static void *reader(void *arg){(void)arg;for(int i=0;i<30000;i++){__kpi_rcu_read_lock();__atomic_add_fetch(&read_active,1,__ATOMIC_RELAXED);__atomic_sub_fetch(&read_active,1,__ATOMIC_RELAXED);__kpi_rcu_read_unlock();}return NULL;}
static void stress(void){
 pthread_t runner,producers[4],readers[4];WRITE_ONCE(stop_worker,false);pthread_create(&runner,NULL,worker,NULL);
 for(int i=0;i<4;i++){pthread_create(&producers[i],NULL,producer,NULL);pthread_create(&readers[i],NULL,reader,NULL);}
 for(int i=0;i<4;i++){pthread_join(producers[i],NULL);pthread_join(readers[i],NULL);}
 rcu_barrier();assert(executed==8000 && !read_active);
 WRITE_ONCE(stop_worker,true);__kpi_wake_up(&rcu_wait,0,0);pthread_join(runner,NULL);
}
int main(int argc,char **argv){
 linuxkpi_rcu_init();
 if(argc==2 && !strcmp(argv[1],"drain"))completed_drain();
 else {late_callback();completed_drain();concurrent_waiters();detached_barrier();stress();}
 assert(!nesting);
 puts("PASS: native RCU late-callback lifetime, completed-drain retention, concurrent waiters, callback barrier, 8000 callbacks and 120000 reader sections");
}
'''
prefix += '#include <string.h>\n'
with tempfile.TemporaryDirectory(prefix='avory-rcu-lifetime-') as temp:
    path = Path(temp)
    (path/'check.c').write_text(prefix+source+suffix)
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run(['cc', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', *extra,
                    str(path/'check.c'), '-o', str(path/'check')], check=True)
    for _ in range(args.repeat):
        subprocess.run([str(path/'check'), *(['drain'] if args.scenario=='drain' else [])], check=True, timeout=20)
