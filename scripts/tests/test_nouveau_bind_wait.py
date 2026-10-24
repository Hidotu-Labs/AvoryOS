#!/usr/bin/env python3
"""Interrupted synchronous binds must not expose unfinished GPU mappings."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/linuxkpi/src/nouveau_bind_wait.c').read_text()
source = re.sub(r'^#include.*$', '', source, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#define ERESTARTSYS 512
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
static unsigned warning_count;
#define klogf(fmt,...) do { if ((fmt)[1]=='I') warning_count++; else wait_reports++; } while(0)
struct dma_fence { bool signaled; unsigned calls; unsigned long long context,seqno; unsigned long flags; };
#define READ_ONCE(x) (x)
static unsigned long long clock_ms;
static unsigned timeouts, wait_reports, dumps;
static void linuxkpi_report_stalled_gpu_wait(void) {dumps++;}
static unsigned long msecs_to_jiffies(unsigned long ms) {return ms;}
static unsigned long long linuxkpi_monotonic_ms(void) {return clock_ms;}
static unsigned long linuxkpi_current_tgid(void) {return 405;}

static bool pending_signal;
static long next_error;
static long dma_fence_wait_timeout(struct dma_fence *fence, bool interruptible, long timeout) {
  fence->calls++;
  if (next_error) return next_error;
  if (interruptible && pending_signal) return -ERESTARTSYS;
  assert(timeout > 0);
  if(timeouts) { assert(!interruptible && timeout==1000);timeouts--;clock_ms+=1000;return 0; }
  fence->signaled = true;
  return timeout;
}
'''
test = r'''
int main(void) {
  struct dma_fence original = {0};
  pending_signal = true;
  assert(dma_fence_wait_timeout(&original, true, MAX_SCHEDULE_TIMEOUT) == -ERESTARTSYS);
  assert(!original.signaled);
  struct dma_fence corrected = {0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&corrected, true, MAX_SCHEDULE_TIMEOUT) > 0);
  assert(corrected.signaled && corrected.calls == 2);
  pending_signal = false;
  struct dma_fence normal = {0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&normal, true, MAX_SCHEDULE_TIMEOUT) > 0);
  assert(normal.signaled && normal.calls == 1);
  pending_signal = true;
  struct dma_fence finite = {0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&finite, true, 10) == -ERESTARTSYS);
  assert(!finite.signaled && finite.calls == 1);
  next_error = -22;
  struct dma_fence error = {0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&error, true, MAX_SCHEDULE_TIMEOUT) == -22);
  assert(!error.signaled && error.calls == 1);
  next_error = 0;
  for (unsigned attempt = 0; attempt < 8; attempt++) {
    struct dma_fence repeated = {0};
    assert(linuxkpi_nouveau_bind_wait_timeout(&repeated, true, MAX_SCHEDULE_TIMEOUT) > 0);
    assert(repeated.signaled && repeated.calls == 2);
  }
  assert(warning_count == 4);
  /* A lost notification or delayed GPU must not strand the recovery forever
   * or report mapping success until the fence actually completes. */
  timeouts=6;clock_ms=0;
  struct dma_fence delayed={0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&delayed,true,MAX_SCHEDULE_TIMEOUT)==MAX_SCHEDULE_TIMEOUT);
  assert(delayed.signaled && delayed.calls==8 && clock_ms==6000 && wait_reports==1 && dumps==1);
  next_error=-22;struct dma_fence failed={0};
  assert(linuxkpi_nouveau_bind_wait_timeout(&failed,false,MAX_SCHEDULE_TIMEOUT)==-22);
  assert(!failed.signaled);
  (void)linuxkpi_current_tgid;
  puts("PASS: interrupted synchronous Nouveau bind waits for completion; normal, finite and error paths preserved");
}
'''
makefile = (root/'kernel/GNUmakefile').read_text()
rule = 'obj-$(ARCH)/linux/drivers/gpu/drm/nouveau/nouveau_sched.c.o:'
assert any(line.strip().startswith(rule) and
           '-Ddma_fence_wait_timeout=linuxkpi_nouveau_bind_wait_timeout' in line
           for line in makefile.splitlines())
imported = root/'build/nouveau-618/linux/drivers/gpu/drm/nouveau/nouveau_sched.c'
if imported.exists():
    submission = imported.read_text()
    assert submission.count('dma_fence_wait(done_fence, true);') == 1
    assert 'dma_fence_wait(done_fence, true);\n\t\tdma_fence_put(done_fence);' in submission
with tempfile.TemporaryDirectory(prefix='nouveau-bind-wait-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(shim + source + test)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
