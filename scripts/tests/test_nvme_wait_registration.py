#!/usr/bin/env python3
"""Compile actual NVMe wait functions with deterministic wake/preemption mocks."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1] if len(sys.argv) > 1 else 'kernel/src/drivers/storage/nvme.c').read_text()
def function(name):
    start = source.index('static ', source.index(name) - 30)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define NVME_TIMEOUT_MS 100
#define NVME_REG_CSTS 0
#define NVME_CSTS_CFS 1
struct thread {int state; uint64_t wakeup_ticks;};
typedef int hal_irq_state_t;
typedef struct { int linked; } wait_queue_t;
typedef struct {struct thread *thread; void *next;} wait_queue_entry_t;
struct nvme_controller {uint32_t generation; bool failed; char *name; int wait_blocks;};
struct nvme_iovq {int cid_result[4]; wait_queue_t cid_wq[1]; wait_queue_t batch_wq;
                  struct {int qid,tail,head,phase;} q;};
struct nvme_namespace {int rmw_state; bool rmw_held; wait_queue_t rmw_wq;};
static struct thread self;
static bool irq_enabled = true;
static int mode, yields, polls, drains;
static bool may_block=true;
static struct nvme_iovq *queue;
static struct nvme_namespace *namespace;
static unsigned batch_cids;
static uint64_t now;
static hal_irq_state_t hal_irq_save(void) {int old=irq_enabled; irq_enabled=false; return old;}
static void hal_irq_restore(hal_irq_state_t flags) {
    /* A timer can interrupt immediately here: parked threads need both their
       timeout and queue entry; an acquired owner must already be runnable. */
    if (flags && self.state == THREAD_BLOCKED) assert(self.wakeup_ticks != 0);
    irq_enabled=flags;
}
static struct thread *sched_get_current(void) {return &self;}
static bool nvme_may_block(void) {return may_block;}
static uint64_t cycles;
static uint64_t rdtsc(void) {cycles+=5000; return cycles;}
static uint64_t tsc_get_mhz(void) {return 1000;}
static uint64_t lapic_timer_get_ms(void) {return now;}
static uint64_t lapic_timer_get_ticks(void) {return now;}
static void hal_cpu_relax(void) {polls++; assert(polls<10);}
static void wait_queue_add(wait_queue_t *q, wait_queue_entry_t *e) {
    assert(!irq_enabled); /* old code allows timer preemption here */
    assert(e->thread->state == THREAD_BLOCKED && e->thread->wakeup_ticks != 0);
    q->linked=1;
    if(mode == 1 && queue && q == &queue->cid_wq[0]) queue->cid_result[0]=1;
    if(mode == 1 && namespace) namespace->rmw_held=false;
    /* mode 10: every batch member completes while the waiter is mid-linking. */
    if(mode == 10 && queue && q == &queue->batch_wq)
        for(unsigned i=0;i<batch_cids;i++) queue->cid_result[i]=1;
}
static void wait_queue_remove(wait_queue_t *q, wait_queue_entry_t *e) {(void)e; q->linked=0;}
static void spinlock_acquire(int *l) {assert(!*l); *l=1;}
static void spinlock_release(int *l) {assert(*l); *l=0;}
static void sched_yield(void) {
    assert(irq_enabled); assert(self.wakeup_ticks != 0); yields++;
    if(queue) {
        if(queue->batch_wq.linked) {
            /* A batch waiter parks on batch_wq; the drain wakes it for any
               member and it reaps the whole CID set itself. */
            for(unsigned i=0;i<batch_cids;i++) queue->cid_result[i]=1;
        } else {
            assert(queue->cid_wq[0].linked); queue->cid_result[0]=1;
        }
    }
    if(namespace) {assert(namespace->rmw_wq.linked); namespace->rmw_held=false;}
    self.state=THREAD_RUNNING; now+=10;
}
static int nvme_take_verdict(struct nvme_iovq *q, uint32_t cid) {
    int r=q->cid_result[cid]; q->cid_result[cid]=0; return r;
}
static void nvme_drain_cq(struct nvme_controller *c, struct nvme_iovq *q, bool b) {
    (void)c;(void)b; drains++;
    if(mode==2 && drains==2) q->cid_result[0]=1;
    if(mode==3 && drains==2) q->cid_result[0]=-1;
    if(mode==4 && drains==2) c->generation++;
    if(mode==5) now+=NVME_TIMEOUT_MS;
    if(mode==6) for(unsigned i=0;i<batch_cids;i++) q->cid_result[i]=1;
    if(mode==7) {q->cid_result[0]=1; q->cid_result[1]=-1; q->cid_result[2]=1;}
    if(mode==10) { /* verdicts appear at registration instead. */ }
}
static int nvme_read32(struct nvme_controller *c, int reg) {(void)c;(void)reg;return 0;}
static void nvme_fail_stop(struct nvme_controller *c, char *s) {(void)s;c->failed=true;}
#define klogf(...) ((void)0)
'''
suffix = r'''
int main(void) {
    struct nvme_controller c={.generation=1}; struct nvme_iovq q={0};
    queue=&q; self.state=THREAD_RUNNING;
    assert(nvme_io_wait(&c,&q,0,1)==0);
    assert(yields==1 && polls==3 && self.state==THREAD_RUNNING && !self.wakeup_ticks && !q.cid_wq[0].linked);
    mode=1; yields=0;
    assert(nvme_io_wait(&c,&q,0,1)==0);
    assert(!yields && self.state==THREAD_RUNNING && !self.wakeup_ticks && !q.cid_wq[0].linked);
    mode=2; polls=drains=0;
    assert(nvme_io_wait(&c,&q,0,1)==0);
    assert(!yields && polls==1 && self.state==THREAD_RUNNING);
    mode=3; polls=drains=0;
    assert(nvme_io_wait(&c,&q,0,1)==-1 && !yields);
    mode=4; polls=drains=0;
    assert(nvme_io_wait(&c,&q,0,1)==-2 && !yields);
    c.generation=1; mode=2; polls=drains=0; may_block=false;
    assert(nvme_io_wait(&c,&q,0,1)==0 && !yields && polls==1);
    may_block=true; mode=5; polls=drains=0;
    assert(nvme_io_wait(&c,&q,0,1)==-2 && c.failed && !yields);
    mode=1;
    queue=NULL; struct nvme_namespace ns={.rmw_held=true}; namespace=&ns;
    nvme_rmw_lock(&ns);
    assert(ns.rmw_held && !yields && self.state==THREAD_RUNNING && !self.wakeup_ticks && !ns.rmw_wq.linked);
    mode=0; ns.rmw_held=true;
    nvme_rmw_lock(&ns);
    assert(ns.rmw_held && yields==1 && self.state==THREAD_RUNNING && !self.wakeup_ticks && !ns.rmw_wq.linked);

    /* Batch wait: verdicts for every member land in one drain; the wave must
       reap all CIDs and return without a scheduler handoff. */
    namespace=NULL;
    queue=&q; c.generation=1; c.failed=false; mode=6; polls=drains=yields=0; batch_cids=3;
    memset(q.cid_result,0,sizeof(q.cid_result));
    uint32_t cids[3]={0,1,2}; int verdicts[3]={9,9,9};
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==0);
    assert(verdicts[0]==1 && verdicts[1]==1 && verdicts[2]==1 && !yields && drains==1);
    assert(!q.cid_result[0] && !q.cid_result[1] && !q.cid_result[2]);

    /* One status error among the wave: wait-all still reaps every member and
       reports -1 with per-member verdicts. */
    mode=7; polls=drains=yields=0;
    memset(q.cid_result,0,sizeof(q.cid_result));
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==-1 && !yields);
    assert(verdicts[0]==1 && verdicts[1]==-1 && verdicts[2]==1);
    assert(!q.cid_result[0] && !q.cid_result[1] && !q.cid_result[2]);

    /* Verdicts already recorded (an interrupt drain or a raced completion)
       short-circuit before any device poll. */
    mode=0; polls=drains=0; memset(q.cid_result,0,sizeof(q.cid_result));
    q.cid_result[0]=1; q.cid_result[1]=-1; q.cid_result[2]=1;
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==-1 && !drains && !polls);
    assert(verdicts[0]==1 && verdicts[1]==-1 && verdicts[2]==1);

    /* A stale generation means a reset discarded the wave: nothing is reaped,
       every verdict stays 0 and the caller treats the members as unfinished. */
    mode=0; polls=drains=0; memset(q.cid_result,0,sizeof(q.cid_result));
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,2)==-2);
    assert(verdicts[0]==0 && verdicts[1]==0 && verdicts[2]==0 && !drains && !polls);

    /* Shared deadline: one slow drain trips the wave timeout. */
    c.failed=false; mode=5; polls=drains=0;
    memset(q.cid_result,0,sizeof(q.cid_result));
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==-2 && c.failed && !yields);
    assert(verdicts[0]==0 && verdicts[1]==0 && verdicts[2]==0 && drains==1);

    /* Slow wave with may_block: one park, one wake (any member's completion
       makes the whole set runnable), then every member is reaped. */
    c.failed=false; mode=0; polls=drains=yields=0; batch_cids=3;
    memset(q.cid_result,0,sizeof(q.cid_result));
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==0 && yields==1);
    assert(verdicts[0]==1 && verdicts[1]==1 && verdicts[2]==1);
    assert(!q.cid_result[0] && !q.cid_result[1] && !q.cid_result[2]);

    /* Completion landed while the waiter was mid-registration: the recheck
       path must consume it without sleeping. */
    mode=10; polls=drains=yields=0;
    memset(q.cid_result,0,sizeof(q.cid_result));
    verdicts[0]=verdicts[1]=verdicts[2]=9;
    assert(nvme_io_wait_many(&c,&q,cids,verdicts,3,1)==0 && !yields);
    assert(verdicts[0]==1 && verdicts[1]==1 && verdicts[2]==1);

    puts("PASS: NVMe command and RMW waits register atomically; raced completion/release stays runnable");
    puts("PASS: bounded fast polling avoids sleep for quick I/O; slow I/O sleeps; errors, reset, timeout and nonblocking callers preserved");
    puts("PASS: batch wait reaps every CID, keeps per-member verdicts, shares one deadline and one batch wake queue");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix +
        function('nvme_reap_ready(') + '\n' +
        function('nvme_io_wait_many(') + '\n' +
        function('nvme_io_wait(') + '\n' +
        function('nvme_rmw_lock(') + suffix)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, check=True)
