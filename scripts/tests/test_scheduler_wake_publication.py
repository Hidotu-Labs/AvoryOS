#!/usr/bin/env python3
"""Force a KPI wake between the pending check and blocked-state publication."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def extract(path, signature):
    s = (root/path).read_text(); start=s.index(signature); opening=s.index('{',start)
    depth=1;end=opening+1
    while depth:
        depth += (s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
code = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
static void gpu_perf_syncobj_park(void *t){(void)t;}
static void gpu_perf_syncobj_wake(void *t){(void)t;}
static void gpu_perf_syncobj_resume(void *t){(void)t;}

enum { THREAD_RUNNING,THREAD_READY,THREAD_BLOCKED,THREAD_SLEEPING,THREAD_DEAD,THREAD_ZOMBIE };
enum { CPU_STATUS_OFFLINE,CPU_STATUS_ONLINE };
#define IPI_VECTOR_RESCHEDULE 1
struct sched_entity { bool on_rq;unsigned critical_depth; };
struct eevfd_rq {unsigned nr_running;};
#define MPOL_DEFAULT 0
struct mempolicy {int mode;};
struct thread {
 bool is_idle,has_started,deadline_queued,on_runqueue,need_resched;
 bool on_cpu;
 bool kpi_block_pending,kpi_timeout_active,kpi_wait_prepared,kpi_wake_pending;
 unsigned kpi_irq_depth;
 int state;uint32_t cpu_index;uint64_t cpu_affinity,wakeup_ticks,ready_since_ms;
 struct sched_entity se;struct mempolicy numa_policy;
};
struct cpu_info {
 int status;uint32_t cpu_id,apic_id;struct thread *current_thread,*switching_from,*idle_thread;
 pthread_mutex_t queue_lock;struct eevfd_rq eevfd;unsigned runnable_count;
};
static struct cpu_info cpus[2];
static struct thread target,idle;
static atomic_bool entered,done;
typedef int hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void) { atomic_store(&entered,true);return 0; }
static void hal_irq_restore(hal_irq_state_t f) { (void)f; }
static struct cpu_info *cpu_get_current(void) { return &cpus[1]; }
static struct cpu_info *cpu_get_info(unsigned n) { return n<2?&cpus[n]:NULL; }
static unsigned scan_count;
static unsigned cpu_get_count(void) { scan_count++; return 2; }
static struct cpu_info *cpu_get_bsp(void) { return &cpus[0]; }
#define spinlock_acquire pthread_mutex_lock
#define spinlock_release pthread_mutex_unlock
static void sched_deadline_remove_locked(struct cpu_info *c,struct thread *t) { (void)c;t->deadline_queued=false; }
static void eevfd_place_entity(struct eevfd_rq *q,struct sched_entity *e,bool b) { (void)q;(void)e;(void)b; }
static void eevfd_enqueue_entity(struct eevfd_rq *q,struct sched_entity *e) { (void)q;e->on_rq=true; }
static bool eevfd_check_preempt(struct eevfd_rq *q,struct sched_entity *a,struct sched_entity *b) { (void)q;(void)a;(void)b;return true; }
static bool lapic_is_ready(void) { return false; }
static void lapic_send_ipi(unsigned a,unsigned b) { (void)a;(void)b; }
static void lapic_timer_rearm_if_earlier(uint64_t a) { (void)a; }
static uint64_t lapic_timer_get_ms(void) { return 1234; }
static void sched_enqueue_thread(struct thread *t,struct cpu_info *c) { (void)t;(void)c; }
'''
code += r'''
static bool sched_cpu_accepting_work(struct cpu_info *c){return c && c->status!=CPU_STATUS_OFFLINE;}
static unsigned sched_cpu_load(struct cpu_info *c){return c->eevfd.nr_running+(c->current_thread&&!c->current_thread->is_idle);}
static int mempolicy_select_node(struct mempolicy *p,int a){(void)p;(void)a;return -1;}
static struct cpu_info *sched_least_loaded_cpu(uint64_t a,int n,unsigned *load){(void)a;(void)n;*load=0;return &cpus[0];}
static void eevfd_migrate_entity(struct eevfd_rq *a,struct eevfd_rq *b,struct sched_entity *e){(void)a;(void)b;(void)e;}
static void sched_lock_two_rq(struct cpu_info *a,struct cpu_info *b){
 if(a==b){pthread_mutex_lock(&a->queue_lock);return;}
 struct cpu_info *first=a->cpu_id<b->cpu_id?a:b,*last=first==a?b:a;
 pthread_mutex_lock(&first->queue_lock);pthread_mutex_lock(&last->queue_lock);
}
static void sched_unlock_two_rq(struct cpu_info *a,struct cpu_info *b){
 pthread_mutex_unlock(&a->queue_lock);if(a!=b)pthread_mutex_unlock(&b->queue_lock);
}
'''
code += extract('kernel/src/sched/sched.c','static void sched_notify_runnable(')
code += extract('kernel/src/sched/sched.c','__attribute__((optimize("O3"))) void sched_wakeup(')
code += extract('kernel/src/linuxkpi/native_sched.c','void linuxkpi_wake_thread(')
code += r'''
static void *wake(void *p) { linuxkpi_wake_thread(p);atomic_store(&done,true);return NULL; }
static void run_boundary(bool still_current,bool timed,bool on_cpu) {
 target=(struct thread){.state=THREAD_RUNNING,.has_started=true,.on_cpu=on_cpu,.cpu_index=0,.cpu_affinity=3,.kpi_wait_prepared=true,.kpi_timeout_active=timed};
 cpus[0].current_thread=still_current?&target:&idle;
 cpus[0].switching_from=on_cpu&&!still_current?&target:NULL;
 atomic_store(&entered,false);atomic_store(&done,false);scan_count=0;
 pthread_mutex_lock(&cpus[0].queue_lock);
 /* This is the waiter's final flag check while holding queue_lock. */
 assert(!atomic_exchange((_Atomic bool*)&target.kpi_block_pending,false));
 pthread_t t;assert(!pthread_create(&t,NULL,wake,&target));
 while(!atomic_load(&entered)&&!atomic_load(&done))usleep(50);
 /* Waker has observed RUNNING, but cannot yet acquire the publication lock. */
 target.state=timed?THREAD_SLEEPING:THREAD_BLOCKED;
 pthread_mutex_unlock(&cpus[0].queue_lock);
 pthread_join(t,NULL);
 /* on_cpu gates the all-CPU scan; the chosen target must be the same CPU 0
  * the scan finds (still_current) or the recorded cpu_index (switched out). */
 assert(on_cpu ? scan_count>0 : scan_count==0);
 assert(target.state==(still_current?THREAD_RUNNING:THREAD_READY));
 assert(target.ready_since_ms==(still_current?0:1234));
 assert(target.kpi_block_pending && target.kpi_wake_pending);
 if(!still_current)assert(target.se.on_rq);
}
int main(void) {
 for(int i=0;i<2;i++) { cpus[i].status=CPU_STATUS_ONLINE;cpus[i].cpu_id=cpus[i].apic_id=i;pthread_mutex_init(&cpus[i].queue_lock,NULL); }
 idle.is_idle=true;
 for(int c=0;c<2;c++)for(int t=0;t<2;t++)for(int f=0;f<2;f++)run_boundary(c,t,f);
 /* A deadline may have linked a blocked waiter before the wake arrives.
  * Its existing wait age survives, and a duplicate READY wake preserves it. */
 target=(struct thread){.state=THREAD_BLOCKED,.has_started=true,.cpu_index=0,.cpu_affinity=3,.ready_since_ms=900,.se={.on_rq=true}};
 cpus[0].current_thread=&idle;cpus[0].switching_from=NULL;
 sched_wakeup(&target);
 assert(target.state==THREAD_READY && target.ready_since_ms==900);
 sched_wakeup(&target);
 assert(target.ready_since_ms==900);
 puts("PASS: wake serialized with BLOCKED/SLEEPING publication for current and switched-out waiters, scan gated by on_cpu");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-wake-publication-') as tmp:
    p=Path(tmp);(p/'check.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True,timeout=10)
