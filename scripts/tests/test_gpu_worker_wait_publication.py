#!/usr/bin/env python3
"""Inject timer preemption at each statement of the real GPU worker waits."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
def block(name,start,end):
 a=s.index(start,s.index('static void '+name+'(void) {'));b=s.index(end,a)
 return s[a:b]
worker=block('virtio_gpu_worker','        /* A timer must not switch','\n    }\n}')
flip=block('virtio_gpu_flip_worker','        /* Publish the wait','        if (__atomic_load_n(&gpu_async_flip_state, __ATOMIC_ACQUIRE) != 1)\n            continue;')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define THREAD_RUNNING 1
#define THREAD_BLOCKED 2
#define DEFINE_CASE(n,code) static void n(void) { struct thread *self=&task; struct entry entry; code }
typedef unsigned long hal_irq_state_t;
struct thread {int state;uint64_t wakeup_ticks;};
struct entry {int unused;};
struct queue {int last_used_idx;struct {int idx;} *used;};
static struct {struct queue controlq,cursorq;} gpu;
static struct thread task;
static bool irq,registered,gpu_irq_installed,is_flip;
static int gpu_worker_wait,gpu_flip_wait,gpu_async_flip_state;
static uint32_t gpu_pending_work;
static int deliver_at,stage,yields;
static void checkpoint(void) {
 /* A timer switching away here loses an unregistered indefinite waiter. */
 assert(!(irq && task.state==THREAD_BLOCKED && !registered) && "timer stranded GPU worker before registration/after removal");
}
static void produce(void) {
 if(is_flip)gpu_async_flip_state=1;else gpu_pending_work=1;
 if(registered)task.state=THREAD_RUNNING;
}
static hal_irq_state_t hal_irq_save(void) {
 bool before=irq;
#ifndef BROKEN
 irq=false;
#endif
 return before;
}
static void hal_irq_restore(hal_irq_state_t flags) {
 irq=flags;if(deliver_at==2 && ++stage==1)produce();checkpoint();
}
static uint64_t lapic_timer_get_ticks(void) {return 100;}
static void wait_queue_add(int *q,struct entry *e) {
 (void)q;(void)e;checkpoint();registered=true;
 if(deliver_at==1)produce();
}
static void wait_queue_remove(int *q,struct entry *e) {(void)q;(void)e;registered=false;}
static void sched_yield(void) {
 checkpoint();yields++;assert(registered);if(deliver_at==3)produce();
 /* Polling fallback may expire without a device event. */
 if(task.wakeup_ticks)task.state=THREAD_RUNNING;
 assert(task.state==THREAD_RUNNING); /* otherwise this is a permanent park */
}
'''
# Neither wait block has a for header. Inject at statement boundaries to make
# the previously tiny timer window deterministic, including the ready branch.
code=pre
for name,body in [('run_worker',worker),('run_flip',flip)]:
 code+='static void '+name+'(void) {struct thread *self=&task;struct entry entry;\n'+body.replace(';',';checkpoint();')+'\n}\n'
code+=r'''
int main(void) {
 typeof(*gpu.controlq.used) used={0};gpu.controlq.used=gpu.cursorq.used=&used;
 for(int flip=0;flip<2;flip++)for(int mode=0;mode<2;mode++)for(int wake=0;wake<4;wake++) {
  is_flip=flip;gpu_irq_installed=mode;deliver_at=wake;
  irq=true;registered=false;task=(struct thread){THREAD_RUNNING,0};
  gpu_pending_work=0;gpu_async_flip_state=0;yields=stage=0;
  if(!wake)produce();
  if(flip)run_flip();else run_worker();
  assert(irq && !registered && task.state==THREAD_RUNNING && !task.wakeup_ticks);
  assert(yields==(!wake || wake==1 ? 0 : 1));
 }
 puts("PASS: completion/presentation wait publication, ready branches, IRQ/poll mode and wake-before-yield");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-worker-wait-') as tmp:
 p=Path(tmp)/'check';p.with_suffix('.c').write_text(code)
 for broken in (False,True):
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',*(['-DBROKEN'] if broken else []),str(p.with_suffix('.c')),'-o',str(p)],check=True)
  r=subprocess.run([str(p)],capture_output=True,text=True,timeout=5)
  if broken:
   assert r.returncode and 'timer stranded GPU worker' in r.stderr,r.stderr
   print('PASS: unprotected wait publication reproduces stranded display worker')
  else:
   assert r.returncode==0,r.stderr
   print(r.stdout.strip())
