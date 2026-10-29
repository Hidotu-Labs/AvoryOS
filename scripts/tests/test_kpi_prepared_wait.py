#!/usr/bin/env python3
"""Exercise native wait/wake code at the fence-before-timeout boundary."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/linuxkpi/native_sched.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { THREAD_RUNNING, THREAD_SLEEPING, THREAD_BLOCKED };
struct thread {
 int state;
 bool kpi_wake_pending, kpi_timeout_active, kpi_wait_prepared;
 bool kpi_block_pending, is_idle;
 uint64_t wakeup_ticks;
};
struct cpu_info { int queue_lock; };
static struct thread task;
static struct cpu_info cpu;
static uint64_t ticks;
static int yields;
static bool wake_in_yield;
static struct thread *sched_get_current(void) { return &task; }
typedef unsigned long hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void) { return 0; }
static void hal_irq_restore(hal_irq_state_t flags) { (void)flags; }
static struct cpu_info *cpu_get_current(void) { return &cpu; }
static void spinlock_acquire(int *lock) { assert(!*lock); *lock=1; }
static void spinlock_release(int *lock) { assert(*lock); *lock=0; }
static uint64_t lapic_timer_get_ticks(void) { return ticks; }
static void linuxkpi_jiffies_sync(void) {}
void linuxkpi_wake_thread(void *thread);
static void sched_wakeup(struct thread *t) {
 if (t->state != THREAD_RUNNING) {
  t->state=THREAD_RUNNING; t->wakeup_ticks=0;
 }
}
static void sched_yield(void) {
 assert(!cpu.queue_lock); yields++;
 if (wake_in_yield) { ticks++; linuxkpi_wake_thread(&task); }
 else ticks=task.wakeup_ticks;
}
'''
for signature in ('void linuxkpi_set_current_state(',
                  'void linuxkpi_thread_block(',
                  'int linuxkpi_schedule_timeout_ms(',
                  'void linuxkpi_wake_thread('):
    code += function(signature) + '\n'
code += r'''
int main(void) {
 /* A completed DRM syncobj can wake before timeout_active is published. */
 linuxkpi_set_current_state(1);
 linuxkpi_wake_thread(&task);
 assert(!task.kpi_timeout_active);
 assert(linuxkpi_schedule_timeout_ms(2000)==1);
 assert(!yields && !ticks && task.state==THREAD_RUNNING);
 linuxkpi_set_current_state(0);
 /* An unrelated, unprepared wake must not shorten a later msleep. */
 linuxkpi_wake_thread(&task);
 assert(linuxkpi_schedule_timeout_ms(20)==0 && ticks==20);
 /* The existing wake-after-park rendezvous must still work. */
 linuxkpi_set_current_state(1);
 wake_in_yield=true;
 assert(linuxkpi_schedule_timeout_ms(2000)==1 && ticks==21);
 linuxkpi_set_current_state(0);
 assert(!task.kpi_wait_prepared && !task.kpi_wake_pending && !task.kpi_block_pending);
 /* Finishing an unslept condition wait discards its unused notification. */
 linuxkpi_set_current_state(1); linuxkpi_wake_thread(&task);
 linuxkpi_set_current_state(0); wake_in_yield=false;
 assert(linuxkpi_schedule_timeout_ms(20)==0 && ticks==41);
 /* An unprepared wake must not arm a later plain block. */
 linuxkpi_wake_thread(&task); assert(!task.kpi_block_pending);
 linuxkpi_set_current_state(1); linuxkpi_wake_thread(&task);
 int before=yields; linuxkpi_thread_block();
 assert(yields==before && !task.kpi_block_pending && !task.kpi_wait_prepared);
 /* Completion of an actual parked wait consumes its wake notification. */
 linuxkpi_set_current_state(1); wake_in_yield=true;
 linuxkpi_thread_block();
 assert(yields==before+1 && !task.kpi_block_pending && !task.kpi_wait_prepared);
 puts("PASS: fence wake before timeout, wake after park, stale wake isolation and wait cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-kpi-prepared-wait-') as tmp:
    path = Path(tmp)
    (path / 'check.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-address', str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
