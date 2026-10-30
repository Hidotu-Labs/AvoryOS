#!/usr/bin/env python3
"""Check real TSC locking/formatting and klog's single buffered-record path."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/lib/tsc_probe.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))

def function(text, signature):
    begin = text.index(signature)
    opening = text.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define kmalloc malloc
#define kfree free
#include "kernel/src/lib/tsc.h"
int avory_snprintf(char *, size_t, const char *, ...);
#define snprintf avory_snprintf
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
typedef pthread_mutex_t spinlock_t;
static _Thread_local bool held;
static void spinlock_acquire(spinlock_t *lock) {
    assert(!held); assert(pthread_mutex_lock(lock) == 0); held = true;
}
static void spinlock_release(spinlock_t *lock) {
    assert(held); held = false; assert(pthread_mutex_unlock(lock) == 0);
}
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static char output[8192];
static unsigned writes;
static void klog_puts(const char *text) {
    assert(!held);
    assert(pthread_mutex_lock(&output_lock) == 0);
    assert(strlen(text) < sizeof(output));
    strcpy(output, text); writes++;
    if (strstr(text, "Execution Time Summary")) {
        assert(strstr(text, "[TSC] ========================================\n\n"));
        assert(!strstr(strstr(text, "Execution Time Summary") + 1,
                       "Execution Time Summary"));
    }
    assert(pthread_mutex_unlock(&output_lock) == 0);
}
static void klog_uint64(uint64_t number) { (void)number; assert(!held); }
'''
tests = r'''
static void *worker(void *unused) {
    (void)unused;
    int idx = tsc_probe_register("parallel");
    for (unsigned i = 0; i < 10000; i++) {
        tsc_probe_record(idx, 5);
        if (i % 1000 == 0) tsc_probe_dump();
    }
    return NULL;
}
int main(void) {
    tsc_set_mhz(1000);
    int idx = tsc_probe_register("vfork_wait");
    assert(idx == tsc_probe_register("vfork_wait"));
    tsc_probe_record(idx, 100); tsc_probe_record(idx, 300);
    unsigned before = writes; tsc_probe_dump();
    assert(writes == before + 1);
    char *row = strstr(output, "[TSC]  vfork_wait"); assert(row);
    char name[32]; unsigned long long count, total, avg, min, max;
    assert(sscanf(row, "[TSC] %31s %llu %llu %llu %llu %llu",
                  name, &count, &total, &avg, &min, &max) == 6);
    assert(count == 2 && total == 400 && avg == 200 && min == 100 && max == 300);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    tsc_probe_dump();
    row = strstr(output, "[TSC]  parallel"); assert(row);
    assert(sscanf(row, "[TSC] %31s %llu %llu %llu %llu %llu",
                  name, &count, &total, &avg, &min, &max) == 6);
    assert(count == 40000 && total == 200000 && avg == 5 && min == 5 && max == 5);
    static char names[TSC_PROBE_MAX][80];
    for (unsigned i = 2; i < TSC_PROBE_MAX; i++) {
        snprintf(names[i], sizeof(names[i]), "probe_%02u_with_a_very_long_name_to_bound", i);
        tsc_probe_record(tsc_probe_register(names[i]), 123456789);
    }
    assert(tsc_probe_register("overflow") == -1);
    tsc_probe_record(-1, 1); tsc_probe_record(TSC_PROBE_MAX, 1);
    tsc_probe_dump(); assert(strstr(output, "probe_31_with_a_very_lon"));
    tsc_probe_reset(); tsc_probe_dump(); assert(!strstr(output, "[TSC]  parallel"));
    assert(!held);
    puts("PASS: balanced probe locks, concurrent counters/snapshots, real formatter, one complete report");
}
'''

klog = (root / 'kernel/src/console/klog.c').read_text()
console = (root / 'kernel/src/console/console.c').read_text()
dispatch_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define KD_GRAPHICS 1
typedef int spinlock_t;
static spinlock_t klog_lock, console_lock;
static bool screen_logging_enabled, console_sink_enabled, console_initialized;
static bool serial_mirror_enabled = true, busy;
static unsigned records, rings, draws, ticks;
static int fb_get_kd_mode(void) { return 0; }
static bool spinlock_try_acquire(spinlock_t *lock) {
    if (busy) return false;
    assert(!*lock); *lock=1; return true;
}
static void spinlock_release(spinlock_t *lock) { assert(*lock); *lock=0; }
static void console_serial_flush(void) {}
static void console_write_batch_locked(const char *s, size_t len) {
    (void)s; assert(len); assert(console_lock); assert(!serial_mirror_enabled); draws++;
}
static void console_tick(void) { ticks++; }
static void klog_ring_append(const char *s, size_t len) { (void)s; assert(len); rings++; }
static void serial_write_async(const char *s, size_t len) { (void)s; assert(len); records++; }
static void klog_putchar_screen_unlocked(char c) { (void)c; draws++; }
'''
dispatch_tests = r'''
int main(void) {
    const char text[] = "[TSC] complete report\n";
    klog_write_dispatch(text, sizeof(text)-1);
    assert(records == 1 && rings == 1 && !draws);
    screen_logging_enabled = console_sink_enabled = console_initialized = true;
    klog_write_dispatch(text, sizeof(text)-1);
    assert(records == 2 && rings == 2 && draws == 1 && ticks == 1);
    assert(serial_mirror_enabled && !console_lock);
    busy = true; klog_write_dispatch(text, sizeof(text)-1);
    assert(records == 3 && rings == 3 && draws == 1);
    klog_write_dispatch(NULL, 0); assert(records == 3);
    puts("PASS: one buffered serial record, screen without duplicate mirror, busy-console fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-tsc-records-') as tmp:
    path = Path(tmp)
    (path / 'tsc.c').write_text(prefix + source + tests)
    # Use the kernel formatter, including bounded string precision and padding.
    subprocess.run(['cc', '-std=gnu11', '-O1', '-fno-builtin',
                    '-Dsnprintf=avory_snprintf', '-Dvsnprintf=avory_vsnprintf',
                    '-c', str(root / 'kernel/src/lib/string.c'),
                    '-o', str(path / 'string.o')], check=True)
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I', str(root), str(path / 'tsc.c'),
                    str(path / 'string.o'), '-o', str(path / 'tsc')], check=True)
    subprocess.run([str(path / 'tsc')], check=True, timeout=30)
    (path / 'dispatch.c').write_text(dispatch_prefix +
        function(console, 'bool console_write_screen_batch_try(') + '\n' +
        function(klog, 'static inline void klog_write_dispatch(') + dispatch_tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'dispatch.c'), '-o', str(path / 'dispatch')], check=True)
    subprocess.run([str(path / 'dispatch')], check=True)
