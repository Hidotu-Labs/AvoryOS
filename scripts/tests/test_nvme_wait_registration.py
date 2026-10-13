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
struct nvme_iovq {int cid_result[1]; wait_queue_t cid_wq[1]; struct {int qid,tail,head,phase;} q;};
struct nvme_namespace {int rmw_state; bool rmw_held; wait_queue_t rmw_wq;};
static struct thread self;
static bool irq_enabled = true;
static int mode, yields;
static struct nvme_iovq *queue;
static struct nvme_namespace *namespace;
static uint64_t now;
static hal_irq_state_t hal_irq_save(void) {int old=irq_enabled; irq_enabled=false; return old;}
static void hal_irq_restore(hal_irq_state_t flags) {
    /* A timer can interrupt immediately here: parked threads need both their
       timeout and queue entry; an acquired owner must already be runnable. */
    if (flags && self.state == THREAD_BLOCKED) assert(self.wakeup_ticks != 0);
    irq_enabled=flags;
}
static struct thread *sched_get_current(void) {return &self;}
static bool nvme_may_block(void) {return true;}
static uint64_t lapic_timer_get_ms(void) {return now;}
static uint64_t lapic_timer_get_ticks(void) {return now;}
static void hal_cpu_relax(void) {assert(0);}
static void wait_queue_add(wait_queue_t *q, wait_queue_entry_t *e) {
    assert(!irq_enabled); /* old code allows timer preemption here */
    assert(e->thread->state == THREAD_BLOCKED && e->thread->wakeup_ticks != 0);
    q->linked=1;
    if(mode == 1 && queue) queue->cid_result[0]=1;
    if(mode == 1 && namespace) namespace->rmw_held=false;
}
static void wait_queue_remove(wait_queue_t *q, wait_queue_entry_t *e) {(void)e; q->linked=0;}
static void spinlock_acquire(int *l) {assert(!*l); *l=1;}
static void spinlock_release(int *l) {assert(*l); *l=0;}
static void sched_yield(void) {
    assert(irq_enabled); assert(self.wakeup_ticks != 0); yields++;
    if(queue) {assert(queue->cid_wq[0].linked); queue->cid_result[0]=1;}
    if(namespace) {assert(namespace->rmw_wq.linked); namespace->rmw_held=false;}
    self.state=THREAD_RUNNING; now+=10;
}
static int nvme_take_verdict(struct nvme_iovq *q, uint32_t cid) {
    int r=q->cid_result[cid]; q->cid_result[cid]=0; return r;
}
static void nvme_drain_cq(struct nvme_controller *c, struct nvme_iovq *q, bool b) {(void)c;(void)q;(void)b;}
static int nvme_read32(struct nvme_controller *c, int reg) {(void)c;(void)reg;return 0;}
static void nvme_fail_stop(struct nvme_controller *c, char *s) {(void)s;c->failed=true;}
#define klogf(...) ((void)0)
'''
suffix = r'''
int main(void) {
    struct nvme_controller c={.generation=1}; struct nvme_iovq q={0};
    queue=&q; self.state=THREAD_RUNNING;
    assert(nvme_io_wait(&c,&q,0,1)==0);
    assert(yields==1 && self.state==THREAD_RUNNING && !self.wakeup_ticks && !q.cid_wq[0].linked);
    mode=1; yields=0;
    assert(nvme_io_wait(&c,&q,0,1)==0);
    assert(!yields && self.state==THREAD_RUNNING && !self.wakeup_ticks && !q.cid_wq[0].linked);
    queue=NULL; struct nvme_namespace ns={.rmw_held=true}; namespace=&ns;
    nvme_rmw_lock(&ns);
    assert(ns.rmw_held && !yields && self.state==THREAD_RUNNING && !self.wakeup_ticks && !ns.rmw_wq.linked);
    mode=0; ns.rmw_held=true;
    nvme_rmw_lock(&ns);
    assert(ns.rmw_held && yields==1 && self.state==THREAD_RUNNING && !self.wakeup_ticks && !ns.rmw_wq.linked);
    puts("PASS: NVMe command and RMW waits register atomically; raced completion/release stays runnable");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix + function('nvme_io_wait(') + '\n' + function('nvme_rmw_lock(') + suffix)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, check=True)
