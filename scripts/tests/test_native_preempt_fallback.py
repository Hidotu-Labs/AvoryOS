#!/usr/bin/env python3
"""Native-only preemption pins must survive injected migration attempts."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/linuxkpi/native_sched.c').read_text()
a=s.index('#ifndef LINUXKPI_LINUX_IMPORTED');b=s.index('#endif',a)
s=s[a:b]+'\n#endif\n'
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define MAX_CPUS 64
typedef unsigned long hal_irq_state_t;
struct cpu_info { unsigned cpu_id; };
static struct cpu_info cpu={0};
static bool irq=true,armed;
static int migrations;
static void attempt_migration(void);
static struct cpu_info *cpu_get_current(void) { attempt_migration();return &cpu; }
static hal_irq_state_t hal_irq_save(void) { unsigned long old=irq;irq=false;return old; }
static void hal_irq_restore(hal_irq_state_t old) { irq=old;attempt_migration(); }
'''
main=r'''
static void attempt_migration(void) {
 if(armed && irq && !native_preempt_counts[cpu.cpu_id]) { cpu.cpu_id^=1;armed=false;migrations++; }
}
int main(void) {
 for(int enabled=0;enabled<2;enabled++) {
  for(int i=0;i<1000;i++) {
   irq=enabled;cpu.cpu_id=0;armed=true;int before=migrations;
   __kpi_preempt_disable();assert(cpu.cpu_id==0 && migrations==before && irq==enabled);
   assert(!linuxkpi_preempt_allowed());__kpi_preempt_disable();
   assert(native_preempt_counts[0]==2);
   __kpi_preempt_enable();assert(native_preempt_counts[0]==1 && cpu.cpu_id==0);
   __kpi_preempt_enable();assert(!native_preempt_counts[0] && irq==enabled);
   armed=false;assert(linuxkpi_preempt_allowed());
  }
 }
 puts("PASS: native-only nested preemption pins, IRQ restoration and injected migration");
}
'''
with tempfile.TemporaryDirectory(prefix='native-preempt-') as tmp:
 p=Path(tmp)/'test';p.with_suffix('.c').write_text(shim+s+main)
 subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True,timeout=10)
