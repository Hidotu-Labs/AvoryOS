#!/usr/bin/env python3
"""Regression for GPU coherency/irq_work fixes (tunnel/Minecraft corruption).

Checks two Linux 6.6 contract violations that affect GPU buffer visibility
and fence-container lifetime:

1. boot_cpu_data.x86_clflush_size must never be 0. drm_cache.c strides
   drm_clflush_page()/drm_clflush_virt_range() by it; zero hangs the page
   loop. Upstream arch/x86/kernel/cpu/common.c derives it from CPUID.01:EBX
   bits 15:8 * 8. The default record and linuxkpi_x86_cpu_init() must both
   provide it (fallback 64).
2. wbinvd_on_all_cpus() must flush (wbinvd), not silently succeed. It is the
   drm_clflush_pages()/sg/virt fallback when CLFLUSH is unavailable.
3. irq_work_sync() must wait until work is neither queued nor running.
   dma-fence array/chain completions are irq_work callbacks; freeing the
   container while one is queued/running corrupts fence signaling and lets
   the GPU reuse a BO that is still in flight (stale/green pixels). The
   kthread must also never touch *work after func() returns (callbacks may
   free their own object).

The functional part compiles the real kernel/linuxkpi/src/irq_work.c queue
worker with mocked spinlocks/services and runs queue/drain/sync lifecycle.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
link_stubs = (root / 'kernel/linuxkpi/src/link_stubs.c').read_text()
x86_stubs = (root / 'kernel/linuxkpi/src/x86_stubs.c').read_text()
irq_work_src = (root / 'kernel/linuxkpi/src/irq_work.c').read_text()
irq_work_hdr = (root / 'kernel/linuxkpi/include/linux/irq_work.h').read_text()

# 1. Default record must carry a nonzero flush stride.
m = re.search(r'struct cpuinfo_x86 boot_cpu_data = \{(.*?)\};', link_stubs, re.S)
assert m, 'boot_cpu_data record missing in link_stubs.c'
assert 'x86_clflush_size' in m.group(1), 'default record lacks x86_clflush_size'
assert re.search(r'\.x86_clflush_size\s*=\s*64', m.group(1)), \
    'default x86_clflush_size must be 64 before CPUID init runs'

# 1b. CPU init must derive the stride from CPUID.01:EBX with a 64 fallback.
assert '((ebx >> 8) & 0xffu) * 8u' in x86_stubs or '((ebx >> 8) & 0xff) * 8' in x86_stubs, \
    'linuxkpi_x86_cpu_init must derive clflush size from CPUID EBX[15:8]*8'
assert 'boot_cpu_data.x86_clflush_size' in x86_stubs, 'init must publish x86_clflush_size'
assert 'boot_cpu_data.x86_cache_alignment' in x86_stubs, 'init must publish x86_cache_alignment'

# 2. Strong wbinvd override must exist and execute wbinvd.
assert re.search(r'^int wbinvd_on_all_cpus\(void\)', x86_stubs, re.M), \
    'x86_stubs.c must strongly override wbinvd_on_all_cpus'
assert 'wbinvd' in x86_stubs.split('int wbinvd_on_all_cpus')[1].split('}')[0], \
    'wbinvd_on_all_cpus must execute the wbinvd instruction'

# 3. irq_work_sync must wait for queued and running work.
sync_body = irq_work_src[irq_work_src.index('void irq_work_sync('):]
assert 'work->queued' in sync_body and 'irq_work_current' in sync_body, \
    'irq_work_sync must poll both queued flag and running instance'
assert 'msleep(1)' in sync_body, 'irq_work_sync must sleep while waiting'
# Kthread must publish/clear the side record and never write the object after func().
assert '__atomic_store_n(&irq_work_current, work' in irq_work_src, \
    'kthread must publish running work before func()'
assert '__atomic_store_n(&irq_work_current, NULL' in irq_work_src, \
    'kthread must clear running work after func() without touching the object'
post = irq_work_src.split('func(work);', 1)[1].split('}')[0]
assert 'work->' not in post and 'work .' not in post, \
    'kthread must not touch *work after func() (self-free callbacks)'

# Functional lifecycle with the real queue worker (sliced before sync).
impl = irq_work_src[:irq_work_src.index('void irq_work_sync(')]
impl = re.sub(r'^#include.*$', '', impl, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
struct irq_work {struct irq_work *next; void (*func)(struct irq_work*); int queued;};
static struct irq_work *irq_work_current;
struct wait_queue_head {int dummy;};
struct kpi_service {int dummy;};
#define DEFINE_SPINLOCK(name) int name
#define spin_lock_irqsave(lock, flags) do{(void)(lock);(flags)=0;}while(0)
#define spin_unlock_irqrestore(lock, flags) do{(void)(lock);(void)(flags);}while(0)
#define wait_event_interruptible_timeout(head, cond, time) ((void)(head),(void)(cond),(void)(time),0)
#define KPI_SERVICE_IDLE_MS 10
#define msecs_to_jiffies(n) (n)
#define msleep(n) do{(void)(n);}while(0)
static unsigned kpi_service_gen(struct kpi_service *s){(void)s;return 0;}
static bool kthread_should_stop(void){return false;}
static void kpi_service_forget(struct kpi_service *s){(void)s;}
static bool kpi_service_retire(struct kpi_service *s,unsigned g){(void)s;(void)g;return true;}
static void kpi_service_kick(struct kpi_service *s,bool wake){(void)s;(void)wake;}
'''
main = r'''
static int callbacks;
static void cb(struct irq_work *w){ (void)w; ++callbacks; }
int main(void){
  struct irq_work w;
  init_irq_work(&w, cb);
  assert(w.queued == 0);
  assert(irq_work_queue(&w));
  assert(w.queued == 1);
  assert(!irq_work_queue(&w));
  assert(irq_work_kthread(NULL) == 0);
  assert(callbacks == 1);
  assert(w.queued == 0);
  assert(irq_work_current == NULL);
  assert(!irq_work_head && !irq_work_tail);
  /* sync on an idle object must return immediately */
  irq_work_sync(&w);
  puts("PASS: irq_work queue/drain/sync lifecycle");
  return 0;
}
'''
# Provide the sync implementation under test as well (polls queued+current).
sync_impl = r'''
void irq_work_sync(struct irq_work *work) {
  if (!work) return;
  while (__atomic_load_n(&work->queued, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&irq_work_current, __ATOMIC_ACQUIRE) == work)
    msleep(1);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-gpu-coherency-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(shim + impl + sync_impl + main)
    subprocess.run(['cc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert 'PASS' in result.stdout, result.stdout

print('PASS: x86 clflush default+CPUID, wbinvd flush, irq_work queue/drain/sync without post-callback writes')
