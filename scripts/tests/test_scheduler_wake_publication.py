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
enum { THREAD_RUNNING,THREAD_READY,THREAD_BLOCKED,THREAD_SLEEPING,THREAD_DEAD,THREAD_ZOMBIE };
enum { CPU_STATUS_OFFLINE,CPU_STATUS_ONLINE };
#define IPI_VECTOR_RESCHEDULE 1
struct sched_entity { bool on_rq; };
struct thread {
 bool is_idle,has_started,deadline_queued,on_runqueue,need_resched;
 bool on_cpu;
 bool kpi_block_pending,kpi_timeout_active,kpi_wait_prepared,kpi_wake_pending;
 int state;uint32_t cpu_index;uint64_t cpu_affinity,wakeup_ticks,ready_since_ms;
 struct sched_entity se;
};
struct cpu_info {
 int status;uint32_t cpu_id,apic_id;struct thread *current_thread,*switching_from,*idle_thread;
 pthread_mutex_t queue_lock;int eevfd;unsigned runnable_count;
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
static void eevfd_place_entity(int *q,struct sched_entity *e,bool b) { (void)q;(void)e;(void)b; }
static void eevfd_enqueue_entity(int *q,struct sched_entity *e) { (void)q;e->on_rq=true; }
static bool eevfd_check_preempt(int *q,struct sched_entity *a,struct sched_entity *b) { (void)q;(void)a;(void)b;return true; }
static bool lapic_is_ready(void) { return false; }
static void lapic_send_ipi(unsigned a,unsigned b) { (void)a;(void)b; }
static void lapic_timer_rearm_if_earlier(uint64_t a) { (void)a; }
static uint64_t lapic_timer_get_ms(void) { return 1234; }
static void sched_enqueue_thread(struct thread *t,struct cpu_info *c) { (void)t;(void)c; }
'''
code += extract('kernel/src/sched/sched.c','__attribute__((optimize("O3"))) void sched_wakeup(')
code += extract('kernel/src/linuxkpi/native_sched.c','void linuxkpi_wake_thread(')
code += r'''
static void *wake(void *p) { linuxkpi_wake_thread(p);atomic_store(&done,true);return NULL; }
static void run_boundary(bool still_current,bool timed,bool on_cpu) {
 target=(struct thread){.state=THREAD_RUNNING,.has_started=true,.on_cpu=on_cpu,.cpu_index=0,.cpu_affinity=3,.kpi_wait_prepared=true,.kpi_timeout_active=timed};
 cpus[0].current_thread=still_current?&target:&idle;
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
 assert(scan_count==(on_cpu?1u:0u));
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
 cpus[0].current_thread=&idle;
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
