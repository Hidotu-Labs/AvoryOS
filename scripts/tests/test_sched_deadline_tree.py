#!/usr/bin/env python3
"""Exercise the per-CPU scheduler deadline tree against the real native rb_*.

The exact sched_deadline_{remove,insert,expire}_locked() bodies are extracted
from sched.c and driven next to an independent model: red-black invariants,
exact membership, the cached leftmost node, ordering, duplicate keys, and the
state/runqueue bookkeeping of the expiry path.  Any divergence between the
tree and the model fails the run; ASan/UBSan guard the node plumbing.
"""
from pathlib import Path
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


sched = (ROOT / 'kernel/src/sched/sched.c').read_text()

shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rbtree.h"

enum { THREAD_RUNNING, THREAD_READY, THREAD_BLOCKED, THREAD_SLEEPING,
       THREAD_DEAD, THREAD_ZOMBIE };

struct sched_entity { bool on_rq; };
struct eevfd { int unused; };

struct thread {
    struct rb_node deadline_node;
    bool deadline_queued;
    uint64_t wakeup_ticks;
    int state;
    uint64_t ready_since_ms;
    uint32_t cpu_index;
    bool on_runqueue;
    struct sched_entity se;
};

struct cpu_info {
    struct rb_root deadline_tree;
    struct rb_node *deadline_leftmost;
    struct thread *current_thread;
    struct eevfd eevfd;
    uint32_t cpu_id;
    uint32_t runnable_count;
};

static unsigned place_calls, enqueue_calls;
static void eevfd_place_entity(struct eevfd *q, struct sched_entity *se,
                               bool b) {
    (void)q; (void)se; (void)b; place_calls++;
}
static void eevfd_enqueue_entity(struct eevfd *q, struct sched_entity *se) {
    (void)q; assert(!se->on_rq); enqueue_calls++; se->on_rq = true;
}
'''

code = shim
code += function(sched, 'void sched_deadline_remove_locked(')
code += function(sched, 'void sched_deadline_insert_locked(')
code += function(sched, 'void sched_deadline_expire_locked(')

code += r'''
#define N 256
static struct thread pool[N];
static struct cpu_info cpu;
static bool live[N];        /* model: node is in the tree */
static bool seen[N];        /* traversal scratch */
static uint64_t ticks[N];   /* model: thread wakeup_ticks (the tree key) */
static unsigned ops;

static uint64_t rng = 0x243F6A8885A308D3ULL;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static unsigned node_index(const struct rb_node *n) {
    uintptr_t base = (uintptr_t)&pool[0].deadline_node;
    uintptr_t addr = (uintptr_t)n;
    assert(addr >= base);
    assert((addr - base) % sizeof(struct thread) == 0);
    unsigned i = (unsigned)((addr - base) / sizeof(struct thread));
    assert(i < N);
    return i;
}

/* Independent oracle: parent symmetry, total ordering, red adjacency, equal
 * black heights, and bounded depth so cycles cannot hang or pass. */
static unsigned check_node(struct rb_node *n, struct rb_node *parent,
                           uint64_t low, uint64_t high, unsigned depth) {
    if (!n)
        return 1;
    assert(depth < N);
    assert(rb_parent(n) == parent);
    unsigned i = node_index(n);
    assert(live[i] && !seen[i]);
    seen[i] = true;
    uint64_t k = pool[i].wakeup_ticks;
    assert(k >= low && k <= high);
    if (rb_is_red(n))
        assert((!n->rb_left || rb_is_black(n->rb_left)) &&
               (!n->rb_right || rb_is_black(n->rb_right)));
    unsigned l = check_node(n->rb_left, n, low, k, depth + 1);
    unsigned r = check_node(n->rb_right, n, k, high, depth + 1);
    assert(l == r);
    return l + rb_is_black(n);
}

static unsigned live_count(void) {
    unsigned c = 0;
    for (unsigned i = 0; i < N; i++)
        c += live[i];
    return c;
}

static void check(void) {
    memset(seen, 0, sizeof seen);
    if (cpu.deadline_tree.rb_node)
        assert(rb_is_black(cpu.deadline_tree.rb_node));
    check_node(cpu.deadline_tree.rb_node, NULL, 0, UINT64_MAX, 0);
    for (unsigned i = 0; i < N; i++) {
        assert(pool[i].deadline_queued == live[i]);
        assert(seen[i] == live[i]);
        assert(pool[i].wakeup_ticks == ticks[i]);
    }
    assert(cpu.deadline_leftmost == rb_first(&cpu.deadline_tree));

    /* Ordered walk and exact count. */
    unsigned count = 0;
    uint64_t prev = 0;
    for (struct rb_node *n = rb_first(&cpu.deadline_tree); n; n = rb_next(n)) {
        uint64_t k = rb_entry(n, struct thread, deadline_node)->wakeup_ticks;
        assert(count < N);
        assert(count == 0 || k >= prev);
        prev = k;
        count++;
    }
    assert(count == live_count());

    /* Cached leftmost must be the true minimum. */
    if (live_count()) {
        uint64_t mn = UINT64_MAX;
        for (unsigned i = 0; i < N; i++)
            if (live[i] && ticks[i] < mn)
                mn = ticks[i];
        assert(rb_entry(cpu.deadline_leftmost, struct thread, deadline_node)
                   ->wakeup_ticks == mn);
    }
    ops++;
}

static void op_insert(unsigned i, uint64_t k) {
    assert(k != 0);
    pool[i].wakeup_ticks = k;
    sched_deadline_insert_locked(&cpu, &pool[i]);
    live[i] = true;
    ticks[i] = k;
}

static void op_remove(unsigned i) {
    sched_deadline_remove_locked(&cpu, &pool[i]);
    live[i] = false;
}

/* Expire at `now`: every queued thread with wakeup_ticks <= now must be
 * popped; BLOCKED/SLEEPING ones move to RUNNING when they are their CPU's
 * current thread, otherwise to READY and enqueued once unless the entity was
 * already linked.  Everything else only has its timer field cleared. */
static void op_expire(uint64_t now) {
    bool exp[N], exp_wake[N], exp_still[N], exp_enq[N];
    for (unsigned i = 0; i < N; i++) {
        exp[i] = live[i] && ticks[i] <= now;
        exp_wake[i] = exp_still[i] = exp_enq[i] = false;
        if (!exp[i])
            continue;
        exp_still[i] = cpu.current_thread == &pool[i];
        if (pool[i].state == THREAD_BLOCKED ||
            pool[i].state == THREAD_SLEEPING) {
            exp_wake[i] = true;
            exp_enq[i] = !exp_still[i] && !pool[i].se.on_rq;
        }
    }
    unsigned before = enqueue_calls;
    sched_deadline_expire_locked(&cpu, now);
    unsigned expected = 0;
    for (unsigned i = 0; i < N; i++) {
        if (!exp[i])
            continue;
        expected += exp_enq[i];
        assert(pool[i].wakeup_ticks == 0);
        assert(!pool[i].deadline_queued);
        if (exp_wake[i]) {
            assert(pool[i].state ==
                   (exp_still[i] ? THREAD_RUNNING : THREAD_READY));
            assert(pool[i].ready_since_ms == (exp_still[i] ? 0 : now));
            if (exp_enq[i])
                assert(pool[i].on_runqueue && pool[i].se.on_rq);
        }
        live[i] = false;
        ticks[i] = 0;
    }
    assert(enqueue_calls - before == expected);
    ops++;
}

int main(void) {
    cpu.deadline_tree.rb_node = NULL;
    cpu.deadline_leftmost = NULL;
    cpu.cpu_id = 3;
    for (unsigned i = 0; i < N; i++) {
        memset(&pool[i], 0, sizeof pool[i]);
        pool[i].cpu_index = cpu.cpu_id;
    }
    check();

    /* wakeup_ticks == 0 is never queued and must leave the tree untouched. */
    pool[0].wakeup_ticks = 0;
    sched_deadline_insert_locked(&cpu, &pool[0]);
    assert(!pool[0].deadline_queued);
    assert(!cpu.deadline_tree.rb_node && !cpu.deadline_leftmost);
    check();

    /* Descending sweep: rotations plus leftmost replacement. */
    for (unsigned i = 0; i < N; i++) {
        pool[i].state = THREAD_BLOCKED;
        op_insert(i, N - i);
        if ((i & 7) == 0)
            check();
    }
    check();
    assert(live_count() == N);

    /* Re-key to ascending while queued (remove+insert inside the helper). */
    for (unsigned i = 0; i < N; i++) {
        op_insert(i, i + 1);
        if ((i & 7) == 0)
            check();
    }
    check();

    /* Remove the current leftmost out of order, then every third sleeper. */
    op_remove(0);
    check();
    for (unsigned i = 3; i < N; i += 3) {
        op_remove(i);
        if ((i & 15) == 0)
            check();
    }
    check();

    /* Expire a strict prefix; one of them is the CPU's current thread. */
    cpu.current_thread = &pool[2];
    op_expire(40);
    check();
    assert(!pool[1].deadline_queued && pool[1].wakeup_ticks == 0);
    assert(pool[N - 2].deadline_queued);

    /* Duplicate keys: every remaining sleeper shares one deadline. */
    for (unsigned i = 0; i < N; i++)
        if (live[i])
            op_insert(i, 1000);
    check();
    cpu.current_thread = NULL;
    op_expire(1000);
    check();
    assert(live_count() == 0 && !cpu.deadline_tree.rb_node);

    /* Randomised churn against the model. */
    for (unsigned step = 0; step < 20000; step++) {
        unsigned i = rnd() % N;
        unsigned op = rnd() % 10;
        if (op < 3) {
            pool[i].state = (rnd() & 1) ? THREAD_BLOCKED : THREAD_SLEEPING;
            pool[i].se.on_rq = pool[i].on_runqueue = false;
            op_insert(i, 1 + rnd() % 96);
        } else if (op < 5) {
            op_remove(i);
        } else if (op < 7) {
            cpu.current_thread = (rnd() & 1) ? &pool[rnd() % N] : NULL;
            op_expire(rnd() % 128);
        } else if (op < 8) {
            /* Emulate a racing path that already linked the entity. */
            pool[i].se.on_rq = pool[i].on_runqueue = true;
        } else {
            pool[i].state = (rnd() & 1) ? THREAD_BLOCKED : THREAD_READY;
            op_insert(i, 1 + rnd() % 96);
        }
        if ((step & 31) == 0)
            check();
    }
    check();
    cpu.current_thread = NULL;
    op_expire(UINT64_MAX);
    check();
    assert(live_count() == 0 && !cpu.deadline_tree.rb_node);

    printf("PASS: %u verified deadline-tree operations over %u threads "
           "(ordering, membership, leftmost, expiry bookkeeping)\n", ops, N);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='avory-deadline-tree-') as tmp:
    p = Path(tmp)
    (p / 'check.c').write_text(code)
    subprocess.run(
        ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-I' + str(ROOT / 'kernel/src/lib'),
         str(ROOT / 'kernel/src/lib/rbtree.c'), str(p / 'check.c'),
         '-o', str(p / 'check')],
        check=True)
    result = subprocess.run([str(p / 'check')], capture_output=True, text=True,
                            timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')
