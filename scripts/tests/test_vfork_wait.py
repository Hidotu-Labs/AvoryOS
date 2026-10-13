#!/usr/bin/env python3
"""Exercise the real vfork rendezvous helpers at completion/sleep boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/syscalls/sys_process.c').read_text()

def function(signature):
    begin = source.index(signature)
    opening = source.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#define sched_yield mock_sched_yield
#define CLONE_VFORK 0x4000
#define THREAD_RUNNING 0
#define THREAD_BLOCKED 1
#define THREAD_READY 2
struct thread {
    uint64_t clone_flags;
    struct thread *parent;
    unsigned cpu_index, vfork_child;
    bool vfork_done, need_resched;
    uint64_t vfork_release_tsc;
    int state;
};
typedef pthread_mutex_t spinlock_t;
struct cpu_info { spinlock_t queue_lock; struct thread *current_thread; }
    cpu = {.queue_lock = PTHREAD_MUTEX_INITIALIZER};
static pthread_cond_t runnable = PTHREAD_COND_INITIALIZER;
static struct thread parent, child;
static unsigned mode, yields, starts, ends, dumps, resume_records;
static bool release_on_unlock;
static _Thread_local bool held;
void vfork_release_parent(struct thread *child);
static struct cpu_info *cpu_get_current(void) { return &cpu; }
static struct cpu_info *cpu_get_info(unsigned index) { assert(index == 0); return &cpu; }
static void spinlock_acquire(spinlock_t *lock) {
    assert(!held); assert(pthread_mutex_lock(lock) == 0); held = true;
}
static void spinlock_release(spinlock_t *lock) {
    assert(held); held = false; assert(pthread_mutex_unlock(lock) == 0);
    if (mode == 1 && release_on_unlock) {
        // Child completes after check-and-block, before the parent's yield.
        release_on_unlock = false;
        assert(parent.state == THREAD_BLOCKED);
        vfork_release_parent(&child);
    }
}
static void sched_wakeup(struct thread *thread) {
    assert(!held); // real wakeup takes queue_lock itself
    spinlock_acquire(&cpu.queue_lock);
    if (thread->state == THREAD_BLOCKED) thread->state = THREAD_READY;
    pthread_cond_signal(&runnable);
    spinlock_release(&cpu.queue_lock);
}
static void sched_yield(void) {
    assert(!held); yields++;
    if (mode == 2) vfork_release_parent(&child);
    if (mode == 3) {
        if (yields == 1) sched_wakeup(&parent); // signal-induced spurious wake
        else vfork_release_parent(&child);
    }
    spinlock_acquire(&cpu.queue_lock);
    while (parent.state == THREAD_BLOCKED) {
        struct timespec deadline;
        assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += 2;
        assert(pthread_cond_timedwait(&runnable, &cpu.queue_lock, &deadline) == 0);
    }
    parent.state = THREAD_RUNNING;
    spinlock_release(&cpu.queue_lock);
}
#define TSC_BEGIN(tag) do { assert(!held); starts++; } while (0)
#define TSC_END(tag) do { assert(!held); ends++; } while (0)
static void tsc_probe_dump(void) { assert(!held); dumps++; }
static uint64_t rdtsc_fence(void) {
    struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}
static int tsc_probe_register(const char *name) {
    assert(!held); assert(!strcmp(name, "vfork_resume")); return 0;
}
static void tsc_probe_record(int slot, uint64_t cycles) {
    assert(!held); assert(slot == 0); (void)cycles; resume_records++;
}
'''
tests = r'''
static void setup(unsigned scenario) {
    parent = (struct thread){.state = THREAD_RUNNING, .vfork_child = 42};
    child = (struct thread){.clone_flags = CLONE_VFORK, .parent = &parent};
    mode = scenario; yields = resume_records = 0; release_on_unlock = scenario == 1;
    cpu.current_thread = &child;
}
static void verify(unsigned old_starts, unsigned old_ends) {
    assert(parent.vfork_done && !parent.vfork_child);
    assert(parent.state == THREAD_RUNNING);
    assert(!(child.clone_flags & CLONE_VFORK));
    assert(parent.vfork_release_tsc && resume_records == 1);
    assert(starts == old_starts + 1 && ends == old_ends + 1);
    assert(!held);
}
static void *complete(void *unused) {
    (void)unused; vfork_release_parent(&child); return NULL;
}
int main(void) {
    // Completion before wait, between unlock/yield, while sleeping, and after
    // a spurious signal wake must all finish without leaving a blocked task.
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        setup(scenario);
        if (!scenario) vfork_release_parent(&child);
        unsigned old_starts = starts, old_ends = ends;
        vfork_wait_for_child(&parent); verify(old_starts, old_ends);
        assert(yields == (scenario == 0 ? 0u : scenario == 3 ? 2u : 1u));
        assert(child.need_resched == (scenario != 0));
        vfork_release_parent(&child); // repeated release is a no-op
    }
    for (unsigned i = 0; i < 2000; i++) {
        setup(4); pthread_t worker;
        unsigned old_starts = starts, old_ends = ends;
        assert(pthread_create(&worker, NULL, complete, NULL) == 0);
        vfork_wait_for_child(&parent);
        assert(pthread_join(worker, NULL) == 0);
        verify(old_starts, old_ends);
    }
    setup(0); child.clone_flags = 0;
    vfork_release_parent(&child); assert(!parent.vfork_done);
    child.clone_flags = CLONE_VFORK; child.parent = NULL;
    vfork_release_parent(&child); assert(!(child.clone_flags & CLONE_VFORK));
    assert(dumps == 6); // first four, then completions 16 and 64
    puts("PASS: vfork completion boundaries, spurious wakes, 2000 concurrent races, and wait probes");
}
'''
# Both entry points must use the same measured handshake.
assert source.count('vfork_wait_for_child(parent);') == 2
with tempfile.TemporaryDirectory(prefix='avory-vfork-wait-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + function('void vfork_release_parent(') +
        '\n' + function('static void vfork_wait_for_child(') + tests)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
