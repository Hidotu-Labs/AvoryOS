#!/usr/bin/env python3
"""Exercise the real ICR send functions against a busy, interruptible APIC."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'kernel/src/apic/lapic.c')
source = parser.parse_args().source.read_text()
names = ('lapic_write_icr', 'lapic_send_ipi', 'lapic_send_ipi_all_but_self')
functions = []
for name in names:
    start = source.index('void ' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    functions.append(source[start:end])
header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "kernel/src/apic/lapic.h"
typedef uintptr_t hal_irq_state_t;
static bool irq = true, lapic_x2apic;
static void *lapic_base = (void *)1;
static int pending, reads, writes, msrs, relaxes;
static uint32_t high, low;
static uint64_t msr_value;
static inline hal_irq_state_t hal_irq_save(void) { bool old = irq; irq = false; return old; }
static inline void hal_irq_restore(hal_irq_state_t f) { irq = f; }
static inline void hal_cpu_relax(void) { assert(!irq); relaxes++; }
uint32_t lapic_read(uint32_t reg) {
    assert(!irq && reg == LAPIC_ICR_LOW);
    reads++;
    if (pending) { pending--; return LAPIC_ICR_PENDING; }
    return 0;
}
void lapic_write(uint32_t reg, uint32_t value) {
    /* A timer sending another IPI could replace HIGH if IRQs were open. */
    assert(!irq && pending == 0);
    if (reg == LAPIC_ICR_HIGH) high = value;
    else { assert(reg == LAPIC_ICR_LOW); low = value; pending = 3; }
    writes++;
}
static uint32_t x2apic_msr(uint32_t reg) { return 0x800 + (reg >> 4); }
static void lapic_wrmsr(uint32_t msr, uint64_t value) {
    assert(!irq && msr == x2apic_msr(LAPIC_ICR_LOW));
    msrs++; msr_value = value;
}
'''
main = r'''
int main(void) {
    pending = 3;
    lapic_send_ipi(6, 0xf1);
    assert(irq && high == (6u << 24) && low == 0xf1 && relaxes == 3);
    lapic_send_ipi(7, 0xf1);
    assert(irq && high == (7u << 24) && relaxes == 6 && writes == 4);
    irq = false;
    lapic_send_ipi_all_but_self(0xf2);
    assert(!irq && high == 0 && low == (LAPIC_ICR_DEST_ALL_BUT_SELF | 0xf2));
    pending = 0;
    int before = reads;
    lapic_write_icr(2, LAPIC_ICR_NMI);
    assert(!irq && reads == before && high == (2u << 24));
    lapic_x2apic = true;
    irq = true;
    lapic_send_ipi(0x1234, 0xf1);
    assert(irq && reads == before && msrs == 1);
    assert(msr_value == ((uint64_t)0x1234 << 32 | 0xf1));
    lapic_send_ipi_all_but_self(0xf2);
    assert(irq && msrs == 2 && msr_value == (LAPIC_ICR_DEST_ALL_BUT_SELF | 0xf2));
    puts("LAPIC ICR: PASS (busy sends, IRQ exclusion, broadcast, masked caller, NMI, x2APIC)");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-icr-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(header + '\n'.join(functions) + main)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
