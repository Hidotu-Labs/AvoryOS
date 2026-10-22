#!/usr/bin/env python3
"""Run actual per-CPU storage/macros with real IDR preload and AMD FPU users."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

percpu = (ROOT/'kernel/linuxkpi/src/percpu.c').read_text()
percpu = re.sub(r'^#include.*\n', '', percpu, flags=re.M)
radix = (ROOT/'kernel/linux/lib/radix-tree.c').read_text()
dc = (ROOT/'kernel/linux/drivers/gpu/drm/amd/display/amdgpu_dm/dc_fpu.c').read_text()
header = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define NR_CPUS 8
#define CONFIG_X86 1
#define BUG_ON(x) assert(!(x))
#define WARN_ON_ONCE(x) assert(!(x))
#define in_task() true
#define kernel_fpu_available() true
static unsigned cpu, pins[64], begins[64], ends[64];
static bool irq = true;
static int linuxkpi_cpu_id(void) { return cpu; }
static void preempt_disable(void) { pins[cpu]++; }
static void preempt_enable(void) { assert(pins[cpu]); pins[cpu]--; }
static unsigned long irq_save(void) { bool old=irq; irq=false; return old; }
static void irq_restore(unsigned long old) { irq=old; }
#include <linux/percpu.h>
#include <linux/local_lock.h>
static void migrate_disable(void) { preempt_disable(); }
static void migrate_enable(void) { preempt_enable(); }
static void kernel_fpu_begin(void) { assert(pins[cpu]); begins[cpu]++; }
static void kernel_fpu_end(void) { assert(pins[cpu]); ends[cpu]++; }
#define TRACE_DCN_FPU(enabled, fn, line, depth) do { (void)(enabled); (void)(fn); (void)(line); (void)(depth); } while(0)
static DEFINE_PER_CPU(int, fpu_recursion_depth);
struct radix_tree_node { struct radix_tree_node *parent; unsigned id; };
struct radix_tree_preload { local_lock_t lock; unsigned nr; struct radix_tree_node *nodes; };
DEFINE_PER_CPU(struct radix_tree_preload, radix_tree_preloads) = {.lock = INIT_LOCAL_LOCK(lock)};
DEFINE_PER_CPU(int, initialized) = 47;
static unsigned allocations;
typedef int gfp_t;
#define __GFP_ACCOUNT 1
static void *radix_tree_node_cachep;
static void *kmem_cache_alloc(void *cache, gfp_t gfp) {
    (void)cache; (void)gfp; assert(!pins[cpu]);
    struct radix_tree_node *n=calloc(1,sizeof(*n)); assert(n); n->id=++allocations; return n;
}
static void kmem_cache_free(void *cache, void *n) { (void)cache; free(n); }
'''
main = r'''
int main(void) {
    linuxkpi_percpu_init();
    for(cpu=0;cpu<64;cpu++) {
        assert(per_cpu(initialized,cpu)==47);
        assert((uintptr_t)per_cpu_ptr(&initialized,cpu)%64==0);
        this_cpu_write(initialized,cpu+100); assert(irq);
    }
    for(cpu=0;cpu<64;cpu++) assert(this_cpu_read(initialized)==(int)cpu+100);
    /* CPU1's first FPU entry must not be mistaken for nesting on CPU0. */
    cpu=0; dc_fpu_begin("cpu0",1); assert(pins[0]==1 && begins[0]==1);
    cpu=1; dc_fpu_begin("cpu1",1); assert(pins[1]==1 && begins[1]==1);
    dc_fpu_end("cpu1",2); assert(!pins[1] && ends[1]==1);
    cpu=0; dc_fpu_end("cpu0",2); assert(!pins[0] && ends[0]==1);
    /* GEM/syncobj idr_preload: both CPUs retain distinct initialized node lists
       and a preemption pin until the caller finishes its GFP_NOWAIT insertion. */
    cpu=0; assert(__radix_tree_preload(0,8)==0 && pins[0]==1);
    struct radix_tree_node *a=this_cpu_ptr(&radix_tree_preloads)->nodes;
    cpu=1; assert(__radix_tree_preload(0,8)==0 && pins[1]==1);
    struct radix_tree_node *b=this_cpu_ptr(&radix_tree_preloads)->nodes;
    assert(a!=b && allocations==16);
    local_unlock(&radix_tree_preloads.lock); assert(!pins[1]);
    cpu=0; assert(this_cpu_ptr(&radix_tree_preloads)->nodes==a);
    local_unlock(&radix_tree_preloads.lock); assert(!pins[0]);
    for(cpu=0;cpu<2;cpu++) {
        struct radix_tree_preload *p=this_cpu_ptr(&radix_tree_preloads);
        while(p->nodes) {struct radix_tree_node *n=p->nodes; p->nodes=n->parent; free(n);}
    }
    unsigned long flags;
    local_lock_irqsave(&radix_tree_preloads.lock,flags); assert(!irq);
    local_unlock_irqrestore(&radix_tree_preloads.lock,flags); assert(irq);
    puts("PASS: 64 initialized CPU copies, actual AMD FPU cross-CPU entries, actual IDR preload isolation and pin lifetime, IRQ restoration");
}
'''
linker = '''SECTIONS {
 .kpi_percpu : ALIGN(4096) {
  __kpi_percpu_start = .; KEEP(*(.kpi_percpu))
  . = ALIGN(4096); __kpi_percpu_end = .;
 }
 .kpi_percpu_copies (NOLOAD) : ALIGN(4096) {
  __kpi_percpu_copies = .;
  . += (__kpi_percpu_end - __kpi_percpu_start) * 63;
 }
} INSERT AFTER .data;
'''
with tempfile.TemporaryDirectory(prefix='avory-percpu-') as tmp:
    p=Path(tmp); inc=p/'include/linux'; inc.mkdir(parents=True)
    (inc/'compiler.h').write_text('')
    (inc/'types.h').write_text('')
    (inc/'preempt.h').write_text('')
    (inc/'irqflags.h').write_text('#define local_irq_save(f) ((f)=irq_save())\n#define local_irq_restore(f) irq_restore(f)\n#define local_irq_disable() ((void)irq_save())\n#define local_irq_enable() irq_restore(1)\n')
    (p/'layout.ld').write_text(linker)
    real_header=(ROOT/'kernel/linuxkpi/include/linux/percpu.h').read_text()
    (inc/'percpu.h').write_text(real_header)
    real_lock=(ROOT/'kernel/linuxkpi/include/linux/local_lock.h').read_text()
    (inc/'local_lock.h').write_text(real_lock)
    code=header+percpu+function(radix,'static __must_check int __radix_tree_preload(').replace('__must_check ','')+function(dc,'void dc_fpu_begin(')+function(dc,'void dc_fpu_end(')+main
    (p/'test.c').write_text(code)
    def run(expect):
        subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-fsanitize=undefined','-I'+str(p/'include'),str(p/'test.c'),'-Wl,-T,'+str(p/'layout.ld'),'-o',str(p/'test')],check=True)
        r=subprocess.run([str(p/'test')],capture_output=True,text=True)
        assert (r.returncode==0)==expect, r.stdout+r.stderr
        if expect: print(r.stdout,end='')
    run(True)
    (inc/'percpu.h').write_text(real_header.replace('#define per_cpu_ptr(ptr, cpu) ((typeof(ptr))((char *)(ptr) + linuxkpi_percpu_offset(cpu)))','#define per_cpu_ptr(ptr, cpu) ((typeof(ptr))(ptr))'))
    run(False)
    (inc/'percpu.h').write_text(real_header)
    (inc/'local_lock.h').write_text(real_lock.replace('(void)l; preempt_disable();','(void)l;').replace('(void)l; preempt_enable();','(void)l;'))
    run(False)
    print('PASS: shared per-CPU storage and no-op local locks both fail the same regression')
