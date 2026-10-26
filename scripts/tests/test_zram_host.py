#!/usr/bin/env python3
"""Compile the actual compression/store code with pthread/PMM shims and sanitizers."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='zram-host-') as name:
 p=Path(name)
 headers={
 'lib/string.h':'#include <string.h>\n',
 'mm/heap.h':'#include <stddef.h>\nvoid *kmalloc(size_t);\nvoid kfree(void *);\n',
 'mm/pmm.h':'#include <stdint.h>\nvoid *pmm_alloc(void);\nvoid pmm_free_page(void *);\nuint64_t pmm_get_hhdm_offset(void);\n',
 'lock/spinlock.h':'''#include <pthread.h>
#include <assert.h>
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void spinlock_acquire(spinlock_t *p){assert(!pthread_mutex_lock(p));}
static inline void spinlock_release(spinlock_t *p){assert(!pthread_mutex_unlock(p));}
'''}
 for path,data in headers.items():
  f=p/path;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(data)
 sources=[ROOT/'scripts/tests/test_zram_host.c',ROOT/'kernel/src/mm/zram/store.c',ROOT/'kernel/src/mm/zram/lz4.c']
 subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fno-omit-frame-pointer','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'kernel/src'),*map(str,sources),'-pthread','-llz4','-o',str(p/'test')],check=True)
 # The permanent metadata table intentionally lives for the kernel lifetime.
 subprocess.run([str(p/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1'))
