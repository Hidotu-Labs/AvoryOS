#!/usr/bin/env python3
"""Real wake notification must schedule locally before the 1ms fallback tick."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/sched/sched.c').read_text()
def fn(signature):
 a=s.index(signature);b=s.index('{',a)+1;depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
notify=fn('static void sched_notify_runnable(')
check=fn('__attribute__((optimize("O3"))) void sched_check_resched(')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define THREAD_RUNNING 1
#define THREAD_DEAD 2
#define THREAD_ZOMBIE 3
#define IPI_VECTOR_RESCHEDULE 0x41
typedef unsigned long hal_irq_state_t;
struct thread {bool need_resched;uint8_t state;unsigned kpi_irq_depth,kpi_softirq_depth;};
struct cpu_info {unsigned apic_id;struct thread *current_thread,*switching_from;};
static struct thread task;
static struct cpu_info cpu={.apic_id=7},remote={.apic_id=13};
static bool irq,atomic_section,lapic_ready=true,ipi_pending,queue_lock;
static unsigned switches,ipis,timers,eois;
void sched_check_resched(bool);
static void deliver(void) {
 if(!irq || !ipi_pending)return;
 ipi_pending=false;irq=false;
 /* Model the actual IPI/common-ISR ordering: request, EOI, irq_exit, check. */
 task.kpi_irq_depth++;task.need_resched=true;eois++;task.kpi_irq_depth--;
 sched_check_resched(false);irq=true;
}
static hal_irq_state_t hal_irq_save(void){bool old=irq;irq=false;return old;}
static void hal_irq_restore(hal_irq_state_t old){irq=old;deliver();}
static struct cpu_info *cpu_get_current(void){assert(!irq);return &cpu;}
static bool lapic_is_ready(void){return lapic_ready;}
static uint64_t lapic_timer_get_ms(void){return 100;}
static void lapic_timer_rearm_if_earlier(uint64_t at){assert(at==101);timers++;}
static void lapic_send_ipi(unsigned apic,unsigned vector){
 assert(!irq && !queue_lock && vector==IPI_VECTOR_RESCHEDULE);ipis++;
 if(apic==cpu.apic_id)ipi_pending=true;else assert(apic==remote.apic_id);
}
bool linuxkpi_preempt_allowed(void){return !atomic_section;}
static void sched_yield(void){
 assert(!irq && !queue_lock && !atomic_section && !task.kpi_irq_depth && eois);
 switches++;
}
static void reset(void){
 task=(struct thread){.state=THREAD_RUNNING};cpu.current_thread=&task;
 irq=true;atomic_section=false;lapic_ready=true;ipi_pending=queue_lock=false;
 switches=ipis=timers=eois=0;
}
'''
main=r'''
int main(void){
 /* Kernel worker wakes a more eligible local waiter: no timer needed. */
 reset();hal_irq_state_t flags=hal_irq_save();sched_notify_runnable(&cpu);
 assert(!switches && task.need_resched && timers==1);hal_irq_restore(flags);
 assert(switches==1 && !task.need_resched && "local wake waited for fallback tick");
 /* Native IRQ-masked outer lock: IPI must wait for the outer unlock. */
 reset();irq=false;queue_lock=true;flags=hal_irq_save();queue_lock=false;
 sched_notify_runnable(&cpu);hal_irq_restore(flags);assert(!switches && ipi_pending);
 hal_irq_restore(true);assert(switches==1);
 /* KPI pin: delivered IPI preserves the request until the pin is dropped. */
 reset();atomic_section=true;flags=hal_irq_save();sched_notify_runnable(&cpu);
 hal_irq_restore(flags);assert(!switches && task.need_resched);
 atomic_section=false;irq=false;sched_check_resched(false);assert(switches==1);
 /* An MSI batch already has a safe IRQ-exit scheduling boundary. */
 reset();task.kpi_irq_depth=1;irq=false;sched_notify_runnable(&cpu);
 assert(!ipis && !switches && task.need_resched);eois++;task.kpi_irq_depth=0;
 sched_check_resched(false);assert(switches==1);
 reset();irq=false;sched_notify_runnable(&remote);
 assert(ipis==1 && !ipi_pending && !timers && !task.need_resched);
 reset();lapic_ready=false;irq=false;sched_notify_runnable(&cpu);
 assert(!ipis && !timers && !switches);
 puts("PASS: local wake before timer tick, nested IRQ masks, KPI pins, MSI batching and remote wake");
}
'''
with tempfile.TemporaryDirectory(prefix='sched-local-ipi-') as tmp:
 p=Path(tmp)/'check'
 for broken in (False,True):
  implementation=notify
  if broken:
   implementation=implementation.replace('  lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);','  if (target != self) lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);')
  p.with_suffix('.c').write_text(pre+implementation+check+main)
  subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-address',str(p.with_suffix('.c')),'-o',str(p)],check=True)
  result=subprocess.run([str(p)],capture_output=True,text=True,timeout=5)
  if broken:
   assert result.returncode and 'local wake waited for fallback tick' in result.stderr,result.stderr
   print('PASS: timer-only local wake fails the immediate-handover regression')
  else:
   assert not result.returncode,result.stderr
   print(result.stdout.strip())
# Preserve eligibility policy and require notifications after both rq unlocks.
wake=s[s.index('__attribute__((optimize("O3"))) void sched_wakeup'):s.index('void sched_enforce_affinity')]
assert 'notify = should_kick;' in wake
assert wake.index('sched_unlock_two_rq(prev_cpu, target);') < wake.index('sched_notify_runnable(dest);')
