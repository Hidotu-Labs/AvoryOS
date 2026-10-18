#!/usr/bin/env python3
"""Drive real EEVFD and scoped scheduler priority protection against a nice -20 hog."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(text, signature):
    a=text.index(signature); b=text.index('{',a); n=1; end=b+1
    while n:
        n+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[a:end]
source=(ROOT/'kernel/src/sched/eevfd.c').read_text()
source='\n'.join(l for l in source.splitlines() if not l.startswith('#include'))
a=source.index('static inline bool eevfd_ptr_sane(')
b=a+len(function(source,'static inline bool eevfd_ptr_sane('))
source=source[:a]+'static inline bool eevfd_ptr_sane(const void *p) { return p != NULL; }'+source[b:]
sched=(ROOT/'kernel/src/sched/sched.c').read_text()
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "sched/eevfd.h"
_Static_assert(sizeof(struct sched_entity)==96,"priority scope must fit existing entity padding");
static uint64_t now_ns;
static uint64_t lapic_timer_get_ns(void){return now_ns;}
'''+source+r'''
typedef int spinlock_t;
typedef bool hal_irq_state_t;
struct thread {struct sched_entity se; bool is_idle;};
struct cpu_info {struct thread *current_thread;spinlock_t queue_lock;struct eevfd_rq eevfd;};
static struct cpu_info cpu;
static bool irq=true;
static struct cpu_info *cpu_get_current(void){return &cpu;}
static bool hal_irq_save(void){bool old=irq;irq=false;return old;}
static void hal_irq_restore(bool old){irq=old;}
static void spinlock_acquire(int *p){assert(!irq&&!*p);*p=1;}
static void spinlock_release(int *p){assert(*p);*p=0;}
'''+function(sched,'static void sched_critical_change(')+r'''
static uint64_t dispatch(bool protect){
 struct thread owner={0},busy={0};memset(&cpu,0,sizeof(cpu));now_ns=1;
 eevfd_rq_init(&cpu.eevfd);eevfd_entity_init(&owner.se,19,6000000);eevfd_entity_init(&busy.se,-20,1000000);
 cpu.eevfd.vtime=4152808690ULL;
 owner.se.vruntime=4099291871ULL;owner.se.deadline=4508891870ULL;
 busy.se.vruntime=4151456878ULL;busy.se.deadline=4151470433ULL;
 /* Protect ownership before its I/O wake, as the journal does. */
 cpu.current_thread=&owner;cpu.eevfd.curr=&owner.se;
 if(protect)sched_critical_change(true);
 assert(owner.se.nice_value==19&&owner.se.slice_ns==6000000);
 eevfd_put_prev_entity(&cpu.eevfd,&owner.se);
 eevfd_enqueue_entity(&cpu.eevfd,&owner.se);
 cpu.eevfd.curr=&busy.se;cpu.current_thread=&busy;busy.se.exec_start_ns=now_ns;
 for(unsigned ms=1;ms<60000;ms++){
  now_ns+=1000000;eevfd_update_curr(&cpu.eevfd,now_ns);
  if(busy.se.vruntime>=busy.se.deadline)busy.se.deadline=calc_deadline(busy.se.vruntime,busy.se.slice_ns,busy.se.weight,busy.se.wmult);
  eevfd_put_prev_entity(&cpu.eevfd,&busy.se);eevfd_enqueue_entity(&cpu.eevfd,&busy.se);
  struct sched_entity *next=eevfd_pick_next_entity(&cpu.eevfd);assert(next);
  eevfd_set_next_entity(&cpu.eevfd,next);next->exec_start_ns=now_ns;
  if(next==&owner.se)return ms;
 }
 return 60000;
}
int main(void){setvbuf(stdout,NULL,_IONBF,0);
 uint64_t before=dispatch(false),after=dispatch(true);
 printf("nice-19 owner vs nice--20 competitor: unprotected=%llu ms protected=%llu ms\n",(unsigned long long)before,(unsigned long long)after);
 assert(before>5000&&after<250&&after*100<before);
 struct thread t={0};memset(&cpu,0,sizeof(cpu));eevfd_rq_init(&cpu.eevfd);
 eevfd_entity_init(&t.se,19,6000000);cpu.current_thread=&t;cpu.eevfd.curr=&t.se;
 sched_critical_change(true);sched_critical_change(true);assert(t.se.critical_depth==2&&t.se.weight==nice_to_weight(-20));
 eevfd_set_nice(&t.se,10);assert(t.se.nice_value==10&&t.se.weight==nice_to_weight(-20));
 sched_critical_change(false);assert(t.se.critical_depth==1&&t.se.weight==nice_to_weight(-20));
 sched_critical_change(false);assert(!t.se.critical_depth&&t.se.weight==nice_to_weight(10)&&t.se.slice_ns==6000000);
 assert(irq&&!cpu.queue_lock);
 puts("PASS: ownership protection, nesting, user nice changes, requested slice and balanced restore");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-priority-') as d:
    p=Path(d);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined','-I',str(ROOT/'kernel/src'),str(p/'test.c'),str(ROOT/'kernel/src/lib/rbtree.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
