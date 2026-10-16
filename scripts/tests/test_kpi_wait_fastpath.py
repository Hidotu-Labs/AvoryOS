#!/usr/bin/env python3
"""Actual wait macros and DRM entity flush: early failed GPU-init cleanup."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
wait = Path(os.environ.get('KPI_WAIT_HEADER', root/'kernel/linuxkpi/include/linux/wait.h')).read_text()
# Compile the real macro family, with deterministic scheduler/queue fixtures.
macros = wait[wait.index('#define ___wait_is_interruptible'):wait.index('/* ── locked wait_event family')]
entity = (root/'kernel/linux/drivers/gpu/drm/scheduler/sched_entity.c').read_text()

def function(signature):
    start = entity.index(signature)
    end = entity.index('{', start) + 1
    depth = 1
    while depth:
        depth += (entity[end] == '{') - (entity[end] == '}')
        end += 1
    return entity[start:end] + '\n'

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define TASK_UNINTERRUPTIBLE 2
#define TASK_INTERRUPTIBLE 1
#define TASK_WAKEKILL 128
#define TASK_KILLABLE (TASK_UNINTERRUPTIBLE|TASK_WAKEKILL)
#define TASK_RUNNING 0
#define PF_EXITING 4
#define SIGKILL 9
#define WQ_FLAG_EXCLUSIVE 1
#define EXPORT_SYMBOL(x)
#define rmb() ((void)0)
#define cmpxchg(p,old,new) __sync_val_compare_and_swap(p,old,new)
struct task_struct {unsigned flags;int exit_code;struct task_struct *group_leader;};
static struct task_struct task;
#define current (&task)
struct list_head {struct list_head *next,*prev;};
#define LIST_HEAD_INIT(h) {&(h),&(h)}
static bool list_empty(struct list_head *h){return h->next==h;}
struct wait_queue_entry {unsigned flags;void *private,*func;struct list_head entry;};
typedef struct wait_queue_entry wait_queue_entry_t;
struct queue {bool valid;};
struct drm_gpu_scheduler {struct queue job_scheduled;};
struct drm_sched_rq {struct drm_gpu_scheduler *sched;};
struct drm_sched_entity {struct list_head list;unsigned job_queue;bool stopped;struct drm_sched_rq *rq;struct task_struct *last_user;};
static struct drm_sched_entity *active;
static unsigned prepares,finishes,sleeps,initializes,kills;
static bool ready,complete_on_prepare;
static int interrupt_result,state;
static unsigned spsc_queue_count(unsigned *q){return *q;}
static void drm_sched_entity_kill(struct drm_sched_entity *e){assert(e==active);kills++;}
static void init_wait_entry(struct wait_queue_entry *e,int flags){(void)e;(void)flags;initializes++;}
static int prepare_to_wait_event(struct queue *q,struct wait_queue_entry *e,int s){
    (void)e;assert(q && (uintptr_t)q>4096 && q->valid);prepares++;state=s;
    if(complete_on_prepare)ready=true;
    return interrupt_result;
}
static void finish_wait(struct queue *q,struct wait_queue_entry *e){
    (void)e;assert(q && (uintptr_t)q>4096 && q->valid);finishes++;state=TASK_RUNNING;
}
static void schedule(void){sleeps++;ready=true;if(active)active->job_queue=0;}
static long schedule_timeout(long t){assert(t>0);schedule();return t-1;}
static void *linuxkpi_current_thread(void){return &task;}
#define autoremove_wake_function NULL
static void prepare_to_wait(struct queue *q,struct wait_queue_entry *e,int s){(void)prepare_to_wait_event(q,e,s);}
'''
main = r'''
int main(void){
    task.group_leader=&task;
    struct drm_sched_rq rq={0};
    struct drm_sched_entity e={.rq=&rq};
    e.list.next=e.list.prev=&e.list;
    active=&e;
    /* SDMA created TTM entities, then VCN failed before schedulers were
     * initialized. rq exists but rq->sched is NULL. The empty entity needs
     * no wait queue; evaluating/registering one caused the guest's KPF. */
    assert(drm_sched_entity_flush(&e,100)==100);
    assert(!prepares && !finishes && !initializes && !sleeps);
    active=NULL;
    struct queue *poison=(void *)(uintptr_t)0x148;
    wait_event(*poison,true);
    assert(wait_event_interruptible(*poison,true)==0);
    assert(wait_event_killable(*poison,true)==0);
    assert(wait_event_timeout(*poison,true,0)==1);
    assert(wait_event_interruptible_timeout(*poison,true,0)==1);
    assert(wait_event_timeout(*poison,false,0)==0);
    assert(wait_event_interruptible_timeout(*poison,false,0)==0);
    assert(wait_event_timeout(*poison,true,12)==12);
    assert(!prepares && !finishes && !initializes && !sleeps);

    struct drm_gpu_scheduler sched={.job_scheduled={.valid=true}};
    struct queue q={.valid=true};
    rq.sched=&sched;e.list.next=NULL;e.job_queue=1;active=&e;
    assert(drm_sched_entity_flush(&e,100)==100 && e.job_queue==0);
    assert(prepares==2 && finishes==1 && sleeps==1 && state==TASK_RUNNING);
    active=NULL;unsigned old=sleeps;
    ready=false;wait_event(q,ready);assert(ready && sleeps==old+1);
    ready=false;assert(wait_event_interruptible(q,ready)==0 && ready);
    ready=false;assert(wait_event_killable(q,ready)==0 && ready);
    ready=false;complete_on_prepare=true;old=sleeps;
    assert(wait_event_killable(q,ready)==0 && sleeps==old);
    complete_on_prepare=false;interrupt_result=-512;ready=false;
    assert(wait_event_interruptible(q,ready)==-512 && sleeps==old);
    assert(wait_event_killable(q,ready)==-512 && sleeps==old);
    interrupt_result=0;ready=false;
    assert(wait_event_timeout(q,ready,10)==9);
    ready=false;assert(wait_event_interruptible_timeout(q,ready,10)==9);
    ready=false;assert(wait_event_timeout(q,ready,1)==1);
    /* Satisfied conditions take priority over pending signals. */
    interrupt_result=-512;
    assert(wait_event_killable(*poison,true)==0);
    assert(!kills && finishes && state==TASK_RUNNING);
    puts("PASS: actual DRM idle-entity flush before scheduler init never touches NULL+0x148 wait queue");
    puts("PASS: satisfied/expired waits skip registration; real blocking, raced completion, signals and timeout return values preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-wait-') as directory:
    p=Path(directory)
    (p/'test.c').write_text(prefix+macros+function('static bool drm_sched_entity_is_idle(')+
                           function('long drm_sched_entity_flush(')+main)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
