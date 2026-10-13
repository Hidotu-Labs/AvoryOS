#!/usr/bin/env python3
"""Exercise actual KPI virtual frees and FPU entry with migration injected."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


page_source = (ROOT / 'kernel/linuxkpi/src/page.c').read_text()
fpu_source = (ROOT / 'kernel/src/linuxkpi/fpu.c').read_text()
free = function(page_source, 'void free_pages(')
begin = function(fpu_source, 'void kernel_fpu_begin(void)')
end = function(fpu_source, 'void kernel_fpu_end(void)')
cpu_index = function(fpu_source, 'static inline uint32_t fpu_cpu_index(')

header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t phys_addr_t;
typedef bool hal_irq_state_t;
#define PAGE_SHIFT 12
#define MAX_CPUS 2
#define FPU_AREA_SIZE 4096
struct page { int live; };
static struct page page;
static unsigned freed, expected_order;
static uint64_t expected_phys;
static uint64_t asc_pmm_get_hhdm_offset(void) { return 0xffff800000000000ULL; }
static struct page *pfn_to_page(unsigned long pfn) {
    return pfn == (expected_phys >> PAGE_SHIFT) ? &page : NULL;
}
static struct page *phys_to_page(phys_addr_t phys) { return pfn_to_page(phys >> PAGE_SHIFT); }
static void __free_pages(struct page *p, unsigned order) {
    if (!p) return;
    assert(p == &page && p->live && order == expected_order);
    p->live = 0; freed++;
}
struct cpu_info { uint32_t cpu_id; };
static struct cpu_info cpus[2] = {{0}, {1}};
static unsigned cpu_id, migrations, saves, restores;
static bool irq = true, inject_migration;
static uint8_t hardware[2][FPU_AREA_SIZE];
static uint8_t fpu_save_area[MAX_CPUS][FPU_AREA_SIZE];
static uint32_t fpu_nesting[MAX_CPUS];
static hal_irq_state_t fpu_irq_flags[MAX_CPUS];
static bool fpu_section_active[MAX_CPUS];
static struct cpu_info *cpu_get_current(void) {
    struct cpu_info *result = &cpus[cpu_id];
    if (irq && inject_migration) {
        cpu_id ^= 1; migrations++; inject_migration = false;
    }
    return result;
}
static hal_irq_state_t hal_irq_save(void) { bool old = irq; irq = false; return old; }
static void hal_irq_restore(hal_irq_state_t old) { irq = old; }
static void *sched_get_current(void) { return &cpus[cpu_id]; }
static void fpu_ensure_loaded(const void *t) { assert(!irq && t == &cpus[cpu_id]); }
static void fpu_save_state(void *area) {
    assert(!irq && area == fpu_save_area[cpu_id]);
    memcpy(area, hardware[cpu_id], FPU_AREA_SIZE); saves++;
}
static void fpu_restore_state(const void *area) {
    assert(!irq && area == fpu_save_area[cpu_id]);
    memcpy(hardware[cpu_id], area, FPU_AREA_SIZE); restores++;
}
'''
main = r'''
int main(void) {
    free_pages(0, 0); assert(freed == 0);
    for (unsigned i = 0; i < 2000; i++) {
        expected_order = i % 10;
        expected_phys = (i & 1 ? 0x100000000ULL : 0x100000ULL) + (uint64_t)i * 0x200000;
        page.live = 1;
        free_pages(asc_pmm_get_hhdm_offset() + expected_phys, expected_order);
        assert(!page.live && freed == i + 1);
    }
    for (unsigned i = 0; i < 2000; i++) {
        cpu_id = i & 1; irq = true; inject_migration = true;
        memset(hardware[cpu_id], (i & 255), FPU_AREA_SIZE);
        kernel_fpu_begin();
        assert(!irq && fpu_section_active[cpu_id]);
        kernel_fpu_begin();
        memset(hardware[cpu_id], 0xee, FPU_AREA_SIZE);
        kernel_fpu_end(); assert(!irq && fpu_section_active[cpu_id]);
        kernel_fpu_end();
        assert(irq && !fpu_section_active[cpu_id]);
        for (unsigned j = 0; j < FPU_AREA_SIZE; j++) assert(hardware[cpu_id][j] == (i & 255));
    }
    assert(migrations == 0 && saves == 2000 && restores == 2000);
    irq = false; inject_migration = false;
    kernel_fpu_begin(); kernel_fpu_end(); assert(!irq);
    puts("PASS: 2000 virtual frees, high RAM, 2000 nested FPU sections, migration exclusion, register and IRQ restoration");
}
'''

with tempfile.TemporaryDirectory(prefix='avory-gpu-kernel-') as tmp:
    path = Path(tmp)
    code = header + free + cpu_index + begin + end + main
    source = path / 'test.c'
    exe = path / 'test'

    def run(text, must_pass):
        source.write_text(text)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', str(source), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert (result.returncode == 0) == must_pass, result.stdout + result.stderr
        if must_pass:
            print(result.stdout, end='')

    run(code, True)
    old_free = 'void free_pages(unsigned long addr, unsigned int order) { if (addr) __free_pages(pfn_to_page(addr >> PAGE_SHIFT), order); }\n'
    run(code.replace(free, old_free), False)
    old_begin = '''void kernel_fpu_begin(void) {
        uint32_t cpu = fpu_cpu_index();
        if (fpu_section_active[cpu]) { fpu_nesting[cpu]++; return; }
        fpu_irq_flags[cpu] = hal_irq_save();
        fpu_ensure_loaded(sched_get_current());
        fpu_save_state(fpu_save_area[cpu]);
        fpu_nesting[cpu] = 1; fpu_section_active[cpu] = true;
    }\n'''
    run(code.replace(begin, old_begin), False)
    print('PASS: both original defects fail the regression')
