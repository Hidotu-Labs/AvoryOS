#!/usr/bin/env python3
"""Exercise the real deferred scheduler entry with atomic/nested IRQ guards."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'kernel/src/sched/sched.c').read_text()
a = s.index('__attribute__((optimize("O3"))) void sched_check_resched')
b = s.index('static uint32_t active_itimer_count', a)
shim = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#define THREAD_RUNNING 1
#define THREAD_DEAD 2
#define THREAD_ZOMBIE 3
typedef unsigned long hal_irq_state_t;
struct thread { bool need_resched; uint8_t state; unsigned kpi_irq_depth, kpi_softirq_depth; };
struct cpu_info { struct thread *current_thread, *switching_from; };
static struct thread task;
static struct cpu_info cpu;
static bool irq = true, allowed = true;
static int switches;
static hal_irq_state_t hal_irq_save(void) { bool old=irq; irq=false; return old; }
static void hal_irq_restore(hal_irq_state_t flags) { irq=flags; }
static struct cpu_info *cpu_get_current(void) { assert(!irq); return &cpu; }
bool linuxkpi_preempt_allowed(void) { assert(!irq); return allowed; }
static void sched_yield(void) { assert(!irq); switches++; }
'''
main = r'''
int main(void) {
 for (int user=0; user<2; user++) {
  for (int guard=0; guard<6; guard++) {
   task=(struct thread){.need_resched=true,.state=THREAD_RUNNING};
   cpu=(struct cpu_info){.current_thread=&task}; allowed=true; irq=true;
   if(guard==1) allowed=false;
   if(guard==2) task.kpi_irq_depth=1;
   if(guard==3) task.kpi_softirq_depth=1;
   if(guard==4) cpu.switching_from=&task;
   if(guard==5) task.state=0;
   int before=switches;
   sched_check_resched(user);
   assert(irq && switches==before+(guard==0));
   assert(task.need_resched==(guard!=0));
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='irq-preempt-') as tmp:
 p=Path(tmp)/'test'
 p.with_suffix('.c').write_text(shim+s[a:b]+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-address',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True)
isr=(root/'kernel/src/cpu/isr.c').read_text()
assert 'if (is_hw_irq && (regs->rflags & (1ULL << 9)))' in isr
assert isr.index('    linuxkpi_irq_exit();') < isr.index('    sched_check_resched(')
wake=s[s.index('__attribute__((optimize("O3"))) void sched_wakeup'):s.index('void sched_enforce_affinity')]
assert 'notify = should_kick;' in wake
assert wake.index('sched_unlock_two_rq(prev_cpu, target);') < wake.index('sched_notify_runnable(dest);')
print('PASS: kernel/user IRQ preemption, atomic/nesting/switch guards and pending requests')
