#!/usr/bin/env python3
"""Run the real ISR return path when signal delivery never returns."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT/'kernel/src/cpu/isr.c')
args = parser.parse_args()
source = args.source.read_text()
source = source[source.index('static void isr_dispatch(struct registers *regs) {'):]
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#define MAX_CPUS 1
struct registers { uint64_t int_no,cs,rflags,rip; };
struct thread { uint64_t rseq_addr; };
struct cpu_info { unsigned cpu_id; };
typedef void (*isr_t)(struct registers *);
static isr_t interrupt_handlers[256];
static uint64_t vector_interrupt_counts[MAX_CPUS][256];
static struct thread task;
static struct cpu_info cpu;
static unsigned eois,depth,handled,signals,reschedules,compat,rseq;
static bool fatal;
static jmp_buf terminated;
static struct cpu_info *cpu_get_current(void) { return &cpu; }
static struct thread *sched_get_current(void) { return &task; }
static void send_eoi(struct registers *r) {
 if(r->int_no>=32 && r->int_no!=48 && r->int_no!=255)eois++;
}
static void handle(struct registers *r) { handled++;if(r->int_no==48)eois++; }
void linuxkpi_irq_enter(void) { depth++; }
void linuxkpi_irq_exit(void) { assert(depth);depth--; }
static void user_access_begin(void) {}
static void user_access_end(void) {}
static void signal_deliver(struct registers *r) {
 signals++;
 unsigned expected=r->int_no>=32 && r->int_no!=255;
 assert(eois==expected && "fatal signal must not abandon an in-service IRQ");
 assert(depth==0 && "signals run after leaving hardirq context");
 if(fatal)longjmp(terminated,1);
}
static void sched_check_resched(bool user) {
 assert(depth==0);assert(eois==1);if(user)assert(signals==1);reschedules++;
}
static void syscall_compat_dispatch(struct registers *r) { (void)r;compat++; }
static void isr_panic(struct registers *r,const char *s) { (void)r;(void)s;assert(0); }
static void rseq_update_cpu_id(struct thread *t) { (void)t;rseq++; }
static void rseq_maybe_abort(struct thread *t,uint64_t *ip) { (void)t;(void)ip; }
'''
tests = r'''
static void deliver(struct registers *r) {
 if(!setjmp(terminated))isr_handler(r);
}
int main(void) {
 struct registers r={0};
 // Registered NVMe/RTL8139-like IRQ, unregistered IRQ, timer early EOI,
 // and an exception. SIGKILL/SIGSTOP modelled by abandoning the ISR stack.
 unsigned vectors[]={102,43,48,14};
 for(unsigned v=0;v<4;v++)for(unsigned dies=0;dies<2;dies++) {
  eois=depth=handled=signals=reschedules=compat=rseq=0;
  fatal=dies;task.rseq_addr=1;
  r=(struct registers){.int_no=vectors[v],.cs=3,.rflags=1u<<9};
  interrupt_handlers[r.int_no]=(r.int_no==43)?0:handle;
  deliver(&r);
  assert(depth==0 && signals==1);
  assert(eois==(r.int_no>=32));
  assert(reschedules==(!dies && r.int_no>=32));
  assert(rseq==!dies);
 }
 for(unsigned user=0;user<2;user++)for(unsigned enabled=0;enabled<2;enabled++) {
  eois=depth=handled=signals=reschedules=compat=rseq=0;fatal=false;
  r=(struct registers){.int_no=102,.cs=user?3:0,.rflags=enabled?1u<<9:0};
  isr_handler(&r);assert(eois==1 && depth==0 && signals==user);
  assert(reschedules==enabled);
 }
 eois=depth=handled=signals=reschedules=compat=rseq=0;
 r=(struct registers){.int_no=0x80,.cs=3};isr_handler(&r);
 assert(compat==1 && !eois && !depth && !handled && !signals && !reschedules);
 puts("PASS: EOI/hardirq exit precede fatal/stopping signals, normal IRQ returns and compat dispatch");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp/'test.c').write_text(shim+source+tests)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-Wno-address','-fsanitize=undefined',str(tmp/'test.c'),
                    '-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
