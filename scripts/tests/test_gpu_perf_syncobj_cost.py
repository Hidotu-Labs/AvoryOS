#!/usr/bin/env python3
"""Actual syncobj profiling bins must separate polling, waits and disabled mode."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/linuxkpi/gpu_perf.c').read_text()
a=s.index('static struct syncobj_wait_cost {');b=s.index('void gpu_perf_ioctl_finish(',a)
shim=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
struct thread {uint64_t perf_syncobj_active_ns,perf_syncobj_park_ns,perf_syncobj_wake_ns;char comm[16];};
static struct thread task;
static struct thread *sched_get_current(void){return &task;}

static unsigned enabled;
static uint64_t now;
static bool inject_new_park;
static uint64_t lapic_timer_get_ns(void){
 if(inject_new_park){task.perf_syncobj_park_ns=5000;task.perf_syncobj_wake_ns=5000;inject_new_park=false;}
 return now;
}
'''
main=r'''
int main(void){
 assert(!gpu_perf_syncobj_wait_start());
 gpu_perf_syncobj_wait_finish(0,0,-1);assert(!syncobj_wait_costs[0].calls);
 enabled=1;now=100;
 const uint64_t durations[]={4999,5000,49999,50000,249999,250000,999999,1000000,9999999,10000000};
 const unsigned bins[]={0,1,1,2,2,3,3,4,4,5};
 uint64_t sum=0;
 for(unsigned i=0;i<10;i++){
  uint64_t start=gpu_perf_syncobj_wait_start();now+=durations[i];
  gpu_perf_syncobj_wait_finish(start,0,i==9?-62:0);sum+=durations[i];
  assert(syncobj_wait_costs[0].latency[bins[i]]>0);
 }
 assert(syncobj_wait_costs[0].calls==10 && syncobj_wait_costs[0].errors==1);
 assert(syncobj_wait_costs[0].total_ns==sum && !syncobj_wait_costs[1].calls);
 uint64_t start=gpu_perf_syncobj_wait_start();now+=100;
 gpu_perf_syncobj_wait_finish(start,1,-62);
 assert(syncobj_wait_costs[1].calls==1 && syncobj_wait_costs[1].latency[0]==1 && syncobj_wait_costs[1].errors==1);
 assert(syncobj_wait_costs[0].latency[0]==1 && syncobj_wait_costs[0].latency[5]==1);
 /* Separate completion/notification delay from time until this task resumes. */
 strcpy(task.comm,"glxgears");now=100;gpu_perf_syncobj_wait_start();
 gpu_perf_syncobj_park(&task);now=900;gpu_perf_syncobj_wake(&task);
 now=950;gpu_perf_syncobj_wake(&task);now=1000;gpu_perf_syncobj_resume(&task);
 assert(syncobj_sleep_costs[1].parks==1 && syncobj_sleep_costs[1].notified==1);
 assert(syncobj_sleep_costs[1].before_notify_ns==800 && syncobj_sleep_costs[1].after_notify_ns==100);
 now=2000;gpu_perf_syncobj_park(&task);now=3000;gpu_perf_syncobj_resume(&task);
 assert(syncobj_sleep_costs[1].parks==2 && syncobj_sleep_costs[1].unnotified_ns==1000);
 /* A delayed writer from one park cannot stamp a newer park's wake slot. */
 now=4000;gpu_perf_syncobj_park(&task);now=4500;inject_new_park=true;
 gpu_perf_syncobj_wake(&task);assert(task.perf_syncobj_wake_ns==5000);
 now=6000;gpu_perf_syncobj_resume(&task);
 gpu_perf_syncobj_wait_finish(100,0,0);
 assert(!task.perf_syncobj_active_ns);
 gpu_perf_syncobj_park(&task);assert(!task.perf_syncobj_park_ns);
 enabled=0;assert(!gpu_perf_syncobj_wait_start());
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-syncobj-cost-') as tmp:
 p=Path(tmp)/'check';p.with_suffix('.c').write_text(shim+s[a:b]+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True)
print('PASS: exclusive timing bins, poll/wait separation, errors and disabled profiling')
