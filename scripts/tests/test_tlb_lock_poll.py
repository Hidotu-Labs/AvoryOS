#!/usr/bin/env python3
"""Run the real lock loops and TLB handler with mocked privileged instructions.

The owner cannot release its lock until the IRQ-masked waiter invalidates its
TLB. No IPI is delivered: this reproduces the shootdown/outer-lock wait cycle.
"""
from pathlib import Path
import re
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/mm/tlb_shootdown.c').read_text()
handler = source[source.index('void tlb_shootdown_handle_ipi_regs('):
                 source.index('static void tlb_shootdown_isr(')]
# Only privileged instructions are substituted; the request selection,
# invalidation branches, acquire/release atomics and acknowledgement are real.
handler = re.sub(r'__asm__ volatile\("mov %%cr3, %0"\s*: "=r"\((\w+)\)\);',
                 r'\1 = 3;', handler)
handler = re.sub(r'__asm__ volatile\("mov %0, %%cr3".*?\);',
                 'mock_flush();', handler)
handler = re.sub(r'__asm__ volatile\("invlpg \(%0\)".*?\);',
                 'mock_flush();', handler)
assert '__asm__' not in handler
arrays = re.findall(r'static (?:volatile )?.*cpu_shootdown_.*?;', source)
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define MAX_CPUS 64
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL
#define TLB_SHOOTDOWN_ALL UINT64_MAX
#define TLB_SHOOTDOWN_CONTEXT (UINT64_MAX-1)
#define CR3_NOFLUSH (1ULL<<63)
#define CR3_PCID_MASK 4095
#define PCID_KERNEL 0
#define INVPCID_TYPE_SINGLE_CTXT 1
#define INVPCID_TYPE_ALL_NON_GLOBAL 2
#define INVPCID_TYPE_INDIV_ADDR 0
struct thread { uint64_t tid, tgid; char comm[16]; };
struct cpu_info { uint32_t cpu_id; struct thread *current_thread; };
struct registers { uint64_t rip, cs, rflags, rsp; };
struct invpcid_desc { uint64_t pcid, addr; };
static struct cpu_info cpu = { .cpu_id = 1 };
static _Thread_local bool irq_enabled;
static bool pcid_enabled, invpcid_enabled;
static atomic_uint flushes;
static struct { uint64_t handler_full_flushes; } tlb_stats;
static bool hal_irq_enabled(void) { return irq_enabled; }
static struct cpu_info *cpu_get_current(void) { return &cpu; }
static uint64_t rdtsc(void) { return 1; }
static bool cpu_has_pcid(void) { return pcid_enabled; }
static bool cpu_has_invpcid(void) { return invpcid_enabled; }
static void tlb_stat_add(uint64_t *v, uint64_t n) { *v += n; }
'''
hardware = r'''
static void mock_flush(void) {
 assert(!irq_enabled);
 assert(__atomic_load_n(&cpu_shootdown_ack[1], __ATOMIC_ACQUIRE) == 1);
 atomic_fetch_add(&flushes, 1);
}
static void cpu_pcid_invalidate_all(struct cpu_info *c) { assert(c == &cpu); }
static void cpu_pcid_invalidate(struct cpu_info *c, uint16_t p) {
 assert(c == &cpu && p != 3); mock_flush();
}
static void pcid_flush_all(void) { mock_flush(); }
static void invpcid(int t, const struct invpcid_desc *d) {
 (void)t; (void)d; mock_flush();
}
'''
test = r'''
static atomic_bool ready;
static void publish(uint64_t addr, uint16_t pcid) {
 __atomic_store_n(&cpu_shootdown_addr[1], addr, __ATOMIC_RELAXED);
 __atomic_store_n(&cpu_shootdown_pcid[1], pcid, __ATOMIC_RELAXED);
 __atomic_store_n(&cpu_shootdown_ack[1], 1, __ATOMIC_RELEASE);
}
static void *waiter(void *unused) {
 (void)unused;
 irq_enabled = false;
 atomic_store(&ready, true);
 acquire_test_lock();
 assert(!irq_enabled);
 release_test_lock();
 assert(!irq_enabled);
 return NULL;
}
int main(void) {
 // All-page, context and page invalidation, PCID and fallback paths.
 for (int p = 0; p < 2; p++) for (int inv = 0; inv < 2; inv++)
  for (int a = 0; a < 3; a++) for (int tag = 0; tag < 3; tag++) {
   pcid_enabled = p; invpcid_enabled = inv;
   uint64_t addr = a == 0 ? TLB_SHOOTDOWN_ALL :
                   a == 1 ? TLB_SHOOTDOWN_CONTEXT : 0x70001000;
   uint16_t pcid = tag == 0 ? 0 : tag == 1 ? 3 : 7;
   publish(addr, pcid);
   unsigned before = atomic_load(&flushes);
   irq_enabled = true;
   tlb_shootdown_poll(); // Enabled IRQs leave service to the normal IPI.
   assert(cpu_shootdown_ack[1] == 1 && atomic_load(&flushes) == before);
   irq_enabled = false;
   tlb_shootdown_poll();
   assert(cpu_shootdown_ack[1] == 0 && atomic_load(&flushes) > before);
   unsigned completed = atomic_load(&flushes);
   tlb_shootdown_handle_ipi(); // Delayed IPI for the polled request.
   assert(atomic_load(&flushes) == completed);
   publish(addr, pcid); // A new published request can be serviced by an IPI.
   tlb_shootdown_handle_ipi();
   assert(cpu_shootdown_ack[1] == 0 && atomic_load(&flushes) > completed);
  }
 pcid_enabled = true; invpcid_enabled = true;
 // Deterministic owner/waiter cycle, repeated to catch acknowledgement races.
 for (int i = 0; i < 100; i++) {
  lock_for_owner();
  atomic_store(&ready, false);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, waiter, NULL));
  while (!atomic_load(&ready)) {}
  unsigned before = atomic_load(&flushes);
  publish(0x70001000, 3);
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (__atomic_load_n(&cpu_shootdown_ack[1], __ATOMIC_ACQUIRE)) {
   clock_gettime(CLOCK_MONOTONIC, &now);
   assert(now.tv_sec - start.tv_sec < 2);
  }
  assert(atomic_load(&flushes) > before);
  unlock_for_owner();
  assert(!pthread_join(thread, NULL));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='avory-tlb-poll-') as tmp:
    p = Path(tmp)
    (p / 'hal').mkdir()
    (p / 'hal/hal.h').write_text('''#include <stdint.h>
typedef uint64_t hal_irq_state_t;
static uint64_t hal_irq_save(void) { uint64_t f = irq_enabled ? 512 : 0; irq_enabled = false; return f; }
static void hal_irq_enable(void) { irq_enabled = true; }
static void hal_irq_disable(void) { irq_enabled = false; }
static void hal_irq_restore(uint64_t f) { irq_enabled = !!(f & 512); }
''')
    for variant in ('native', 'save', 'raw', 'linuxkpi'):
        if variant == 'linuxkpi':
            h = (root / 'kernel/linuxkpi/include/linux/spinlock.h').read_text()
            h = re.sub(r'#include <linux/.*?>', '', h)
            h = '#undef __always_inline\n#define __always_inline inline\n' + h
            h = (root / 'kernel/linuxkpi/include/linux/spinlock_types.h').read_text().replace(
                '#include <linux/types.h>', '') + h
            h = '''#include "hal/hal.h"
#define local_irq_save(f) ((f) = hal_irq_save())
#define local_irq_restore(f) hal_irq_restore(f)
#define local_irq_disable() hal_irq_disable()
#define local_irq_enable() hal_irq_enable()
#define preempt_disable() ((void)0)
#define preempt_enable() ((void)0)
#define preempt_enable_resched() ((void)0)
''' + h
            wrappers = '''void linuxkpi_tlb_poll(void) { tlb_shootdown_poll(); }
static spinlock_t lock;
static _Thread_local unsigned long flags;
static void acquire_test_lock(void) { spin_lock_irqsave(&lock, flags); }
static void release_test_lock(void) { spin_unlock_irqrestore(&lock, flags); }
'''
        else:
            h = (root / 'kernel/src/lock/spinlock.h').read_text().replace(
                '#include "lockdiag.h"', '''#define LOCKDIAG_SPIN_BEGIN(l) ((void)0)
#define LOCKDIAG_SPIN_END(l) ((void)0)
#define LOCKDIAG_HOLD(l) ((void)0)
#define LOCKDIAG_UNHOLD(l) ((void)0)''')
            if variant == 'raw':
                wrappers = '''static rawspinlock_t lock;
static void acquire_test_lock(void) { rawspinlock_acquire(&lock); }
static void release_test_lock(void) { rawspinlock_release(&lock); }
'''
            elif variant == 'save':
                wrappers = '''static spinlock_t lock;
static _Thread_local uint64_t flags;
static void acquire_test_lock(void) { spinlock_acquire_save(&lock, &flags); }
static void release_test_lock(void) { spinlock_release_restore(&lock, flags); }
'''
            else:
                wrappers = '''static spinlock_t lock;
static void acquire_test_lock(void) { spinlock_acquire(&lock); }
static void release_test_lock(void) { spinlock_release(&lock); }
'''
        wrappers += '''static void lock_for_owner(void) { __atomic_store_n(&lock.locked, 1, __ATOMIC_RELEASE); }
static void unlock_for_owner(void) { __atomic_store_n(&lock.locked, 0, __ATOMIC_RELEASE); }
'''
        (p / 'test.c').write_text(preamble + '\n'.join(arrays) + hardware +
                                handler + h + wrappers + test)
        subprocess.run(['cc', '-O2', '-pthread', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        '-Wno-unused-but-set-variable',
                        '-I'+tmp, str(p/'test.c'), '-o', str(p/'test')], check=True)
        subprocess.run([str(p/'test')], check=True, timeout=10)
        print(f'PASS: {variant}: IRQ-masked wait cycle, flush-before-ack, PCID paths, delayed IPI')
        if variant == 'native':
            # Removing service from the real waiter must recreate the cycle.
            negative = h.replace('tlb_shootdown_poll();', '(void)0;')
            (p / 'negative.c').write_text(preamble + '\n'.join(arrays) + hardware +
                                          handler + negative + wrappers + test)
            subprocess.run(['cc', '-O2', '-pthread', '-I'+tmp,
                            str(p/'negative.c'), '-o', str(p/'negative')], check=True)
            result = subprocess.run([str(p/'negative')], timeout=5,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            assert result.returncode == -signal.SIGABRT and b'now.tv_sec - start.tv_sec < 2' in result.stderr, \
                'negative control failed to detect the acknowledgement deadlock'
            print('PASS: negative control detects the original IRQ-masked wait cycle')
