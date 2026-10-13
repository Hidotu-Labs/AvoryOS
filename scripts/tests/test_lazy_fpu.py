#!/usr/bin/env python3
"""Run the real lazy ownership code with simulated per-CPU FPU hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/cpu/fpu.c').read_text()
source = re.sub(r'^#include.*\n', '', source, flags=re.M)

def replace_function(text, name, replacement):
    match = re.search(r'static (?:inline )?void ' + name + r'\(', text)
    start = match.start()
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[:start] + replacement + text[end:]

source = replace_function(source, 'cr0_clear_ts', 'static void cr0_clear_ts(void) { clears++; ts = false; }')
source = replace_function(source, 'cr0_set_ts', 'static void cr0_set_ts(void) { ts_writes++; ts = true; }')
source = replace_function(source, 'save_state', '''static void save_state(void *area) {
    assert(!irq && !ts); saves++; memcpy(area, hardware, 4096);
}''')
source = replace_function(source, 'restore_state', '''static void restore_state(const void *area) {
    assert(!irq && !ts); restores++; memcpy(hardware, area, 4096);
}''')
header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
struct registers { int unused; };
struct thread { uint8_t fpu_state[4096]; };
struct cpu_info { struct thread *current_thread, *fpu_owner; };
static struct cpu_info cpus[2];
static int cpu_id;
static bool irq, ts;
static unsigned saves, restores, clears, ts_writes;
static uint8_t hardware[4096];
typedef bool hal_irq_state_t;
static hal_irq_state_t hal_irq_save(void) { bool old = irq; irq = false; return old; }
static void hal_irq_restore(hal_irq_state_t old) { irq = old; }
static struct cpu_info *cpu_get_current(void) { return &cpus[cpu_id]; }
static struct thread *sched_get_current(void) { return cpus[cpu_id].current_thread; }
static void register_interrupt_handler(int n, void (*handler)(struct registers *)) { assert(n == 7 && handler); }
static void klog_debug_puts(const char *s) { (void)s; }
'''
main = r'''
int main(void) {
    struct thread a, b, child;
    fpu_init_thread(&a); fpu_init_thread(&b);
    cpus[0].current_thread = &a;
    fpu_init_cpu();
    unsigned r = restores, old_clears = clears, old_ts = ts_writes;
    for (int i = 0; i < 10000; i++) {
        fpu_on_context_switch();
        cpus[0].current_thread = i & 1 ? &a : &b;
    }
    assert(ts && saves == 0 && restores == r && clears == old_clears && ts_writes == old_ts);
    cpus[0].current_thread = &a;
    fpu_nm_handler(NULL);
    assert(!ts && cpus[0].fpu_owner == &a);
    hardware[160] = 0xab; hardware[576] = 0xcd;
    irq = true;
    fpu_copy_thread(&child, &a);
    assert(irq && child.fpu_state[160] == 0xab && child.fpu_state[576] == 0xcd);
    r = restores;
    fpu_ensure_loaded(&a);
    assert(irq && restores == r);
    irq = false;
    fpu_on_context_switch();
    assert(ts && !cpus[0].fpu_owner && hardware[160] == 0 && hardware[576] == 0);
    cpus[0].current_thread = &b;
    r = restores;
    fpu_copy_thread(&child, &a); /* off-CPU parent's authoritative buffer */
    assert(ts && restores == r && child.fpu_state[160] == 0xab);
    fpu_nm_handler(NULL);
    assert(!ts && hardware[160] == 0 && cpus[0].fpu_owner == &b);
    hardware[160] = 0xef;
    fpu_on_context_switch();
    fpu_forget_thread(&b);
    memset(&b, 0xdd, sizeof(b)); /* no stale owner on later #NM */
    cpu_id = 1;
    cpus[1].current_thread = &a;
    fpu_init_cpu();
    fpu_nm_handler(NULL);
    assert(hardware[160] == 0xab && hardware[576] == 0xcd && cpus[1].fpu_owner == &a);
    irq = true;
    fpu_reset_current();
    assert(irq && ts && !cpus[1].fpu_owner && hardware[160] == 0 && a.fpu_state[160] == 0);
    puts("PASS: 10000 integer switches do zero saves/restores; first-use restore, scrub, fork, teardown, migration, exec, IRQ exclusion");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-fpu-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(header + source + main)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
