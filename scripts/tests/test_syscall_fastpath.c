#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <x86intrin.h>

#define SYS_GETPID 39
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_GETPPID 110
#define SYS_GETTID 186
#define MAX_SYSCALL 512

struct syscall_regs {
    uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
    uint64_t rip, rflags, rsp;
    uint64_t user_cs, user_rcx, user_rsi, user_rdi;
} __attribute__((packed));

struct pid_namespace {
    uint32_t refs, id, depth, next_pid, init_tgid;
    bool dead;
    struct pid_namespace *parent;
};

struct thread {
    uint64_t rsp;
    uint32_t tid;
    uint32_t tgid;
    bool is_main_session;
    struct thread *parent;
    struct thread *global_next;
    struct pid_namespace *pid_ns;
    uint32_t namespace_pids[32];

    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;

    bool syscall_dispatch_enabled;
    uint32_t seccomp_mode_val;
    uint64_t rseq_addr;
    uint64_t pending_signals;
    uint64_t signal_mask;
    bool need_resched;

    bool in_syscall;
    uint64_t last_syscall_num;
    int64_t last_syscall_ret;
    uint64_t last_syscall_args[6];
    uint64_t last_syscall_user_rip;
    uint64_t last_syscall_user_rsp;
    uint64_t last_io_started_ms;

    uint32_t fd_capacity;
    void *fds;
    void *fd_offsets;
    void *fd_flags;
    void *fd_paths;
    struct {
        uint32_t capacity;
        void *fds, *fd_offsets, *fd_flags, *fd_paths;
    } *files;
};

// Global thread list for namespace lookups
static struct thread *global_thread_list = NULL;
static uint64_t g_dummy_lock = 0;

static inline void dummy_spinlock_acquire(uint64_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {}
}
static inline void dummy_spinlock_release(uint64_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static uint32_t pid_namespace_tid(struct thread *viewer, struct thread *target) {
    if (!target) return 0;
    struct pid_namespace *want = viewer ? viewer->pid_ns : NULL;
    if (!want) return target->is_main_session ? 1 : target->tid;
    for (struct pid_namespace *ns = target->pid_ns; ns; ns = ns->parent)
        if (ns == want) return target->namespace_pids[ns->depth];
    return 0;
}

// Baseline unoptimized pid_namespace_visible (walks list under lock)
static uint32_t pid_namespace_visible_baseline(struct thread *viewer, uint32_t global) {
    dummy_spinlock_acquire(&g_dummy_lock);
    uint32_t res = 0;
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tid == global) {
            res = pid_namespace_tid(viewer, t);
            break;
        }
    }
    dummy_spinlock_release(&g_dummy_lock);
    return res;
}

// Optimized pid_namespace_visible (fast path when not in child namespace)
static uint32_t pid_namespace_visible_fast(struct thread *viewer, uint32_t global) {
    if (!viewer || !viewer->pid_ns) return global;
    dummy_spinlock_acquire(&g_dummy_lock);
    uint32_t res = 0;
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tid == global) {
            res = pid_namespace_tid(viewer, t);
            break;
        }
    }
    dummy_spinlock_release(&g_dummy_lock);
    return res;
}

// Handlers for generic dispatcher
static uint64_t handler_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    extern struct thread *g_current_thread;
    struct thread *current = g_current_thread;
    if (current) {
        if (current->is_main_session) return 1;
        return pid_namespace_visible_baseline(current, current->tgid ? current->tgid : current->tid);
    }
    return 0;
}

static uint64_t handler_gettid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    extern struct thread *g_current_thread;
    struct thread *current = g_current_thread;
    if (current && current->is_main_session) return 1;
    return current ? pid_namespace_tid(current, current) : 0;
}

static uint64_t handler_getuid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    extern struct thread *g_current_thread;
    return g_current_thread ? g_current_thread->uid : 0;
}

struct thread *g_current_thread = NULL;

static void sched_sync_files(struct thread *t) {
    if (t && t->files && (t->fd_capacity != t->files->capacity || t->fds != t->files->fds)) {
        t->fds = t->files->fds;
        t->fd_offsets = t->files->fd_offsets;
        t->fd_flags = t->files->fd_flags;
        t->fd_paths = t->files->fd_paths;
        t->fd_capacity = t->files->capacity;
    }
}

// 1. Baseline Full Generic Dispatcher
void dispatch_baseline(struct syscall_regs *regs) {
    struct thread *t = g_current_thread;
    uint64_t syscall_num = regs->rax;
    uint64_t syscall_return_ip = regs->rip;

    if (t) {
        __atomic_store_n(&t->in_syscall, true, __ATOMIC_RELAXED);
        sched_sync_files(t);
        t->last_syscall_num = syscall_num;
        t->last_syscall_args[0] = regs->rdi;
        t->last_syscall_args[1] = regs->rsi;
        t->last_syscall_args[2] = regs->rdx;
        t->last_syscall_args[3] = regs->r10;
        t->last_syscall_args[4] = regs->r8;
        t->last_syscall_args[5] = regs->r9;
        t->last_syscall_user_rip = syscall_return_ip;
        t->last_syscall_user_rsp = regs->rsp;
    }

    if (syscall_num == SYS_GETPID) {
        regs->rax = handler_getpid(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);
    } else if (syscall_num == SYS_GETTID) {
        regs->rax = handler_gettid(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);
    } else if (syscall_num == SYS_GETUID) {
        regs->rax = handler_getuid(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);
    }

    if (t) {
        t->last_syscall_ret = (int64_t)regs->rax;
        __atomic_store_n(&t->in_syscall, false, __ATOMIC_RELAXED);
    }
}

// 2. Optimized Dispatcher with Ultra-Fast Path
void dispatch_fastpath(struct syscall_regs *regs) {
    struct thread *t = g_current_thread;
    uint64_t syscall_num = regs->rax;

    /* Ultra-Fast Path: Trivial read-only process and thread identity queries */
    if (__builtin_expect(t && !t->syscall_dispatch_enabled &&
                         !t->seccomp_mode_val && !t->rseq_addr &&
                         !(t->pending_signals & ~t->signal_mask) &&
                         !__atomic_load_n(&t->need_resched, __ATOMIC_RELAXED), 1)) {
        switch (syscall_num) {
            case SYS_GETPID: {
                uint64_t pid = t->is_main_session ? 1 : (t->pid_ns ? pid_namespace_visible_fast(t, t->tgid ? t->tgid : t->tid) : (t->tgid ? t->tgid : t->tid));
                regs->rax = pid;
                t->last_syscall_num = SYS_GETPID;
                t->last_syscall_ret = pid;
                return;
            }
            case SYS_GETTID: {
                uint64_t tid = t->is_main_session ? 1 : (t->pid_ns ? pid_namespace_tid(t, t) : t->tid);
                regs->rax = tid;
                t->last_syscall_num = SYS_GETTID;
                t->last_syscall_ret = tid;
                return;
            }
            case SYS_GETUID:
                regs->rax = t->uid;
                t->last_syscall_num = SYS_GETUID;
                t->last_syscall_ret = t->uid;
                return;
            case SYS_GETGID:
                regs->rax = t->gid;
                t->last_syscall_num = SYS_GETGID;
                t->last_syscall_ret = t->gid;
                return;
            case SYS_GETEUID:
                regs->rax = t->euid;
                t->last_syscall_num = SYS_GETEUID;
                t->last_syscall_ret = t->euid;
                return;
            case SYS_GETEGID:
                regs->rax = t->egid;
                t->last_syscall_num = SYS_GETEGID;
                t->last_syscall_ret = t->egid;
                return;
            case SYS_GETPPID: {
                uint64_t ppid = t->parent ? (t->pid_ns ? pid_namespace_visible_fast(t, t->parent->tgid ? t->parent->tgid : t->parent->tid) : (t->parent->tgid ? t->parent->tgid : t->parent->tid)) : 0;
                regs->rax = ppid;
                t->last_syscall_num = SYS_GETPPID;
                t->last_syscall_ret = ppid;
                return;
            }
        }
    }

    // Fallback to full generic dispatcher
    dispatch_baseline(regs);
}

int main(void) {
    printf("===================================================================\n");
    printf("      Ultra Fast-Path for Thread & Process Identity Syscalls        \n");
    printf("===================================================================\n\n");

    // Setup 50 simulated threads on global thread list (realistic server/desktop)
    struct thread threads[50];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < 50; i++) {
        threads[i].tid = 1000 + i;
        threads[i].tgid = 1000; // All belong to process 1000
        threads[i].uid = 1000;
        threads[i].gid = 1000;
        threads[i].euid = 1000;
        threads[i].egid = 1000;
        if (i > 0) threads[i - 1].global_next = &threads[i];
    }
    global_thread_list = &threads[0];

    // Current active thread is thread 15
    g_current_thread = &threads[15];

    printf("[1] Correctness Verification Across All Identity Queries:\n");

    struct syscall_regs regs_base, regs_fast;

    // Test getpid
    memset(&regs_base, 0, sizeof(regs_base));
    memset(&regs_fast, 0, sizeof(regs_fast));
    regs_base.rax = SYS_GETPID;
    regs_fast.rax = SYS_GETPID;
    dispatch_baseline(&regs_base);
    dispatch_fastpath(&regs_fast);
    assert(regs_base.rax == regs_fast.rax);
    assert(regs_fast.rax == 1000);
    printf("    - SYS_GETPID (39)  : PASSED (returned %lu)\n", regs_fast.rax);

    // Test gettid
    regs_base.rax = SYS_GETTID;
    regs_fast.rax = SYS_GETTID;
    dispatch_baseline(&regs_base);
    dispatch_fastpath(&regs_fast);
    assert(regs_base.rax == regs_fast.rax);
    assert(regs_fast.rax == 1015);
    printf("    - SYS_GETTID (186) : PASSED (returned %lu)\n", regs_fast.rax);

    // Test getuid
    regs_base.rax = SYS_GETUID;
    regs_fast.rax = SYS_GETUID;
    dispatch_baseline(&regs_base);
    dispatch_fastpath(&regs_fast);
    assert(regs_base.rax == regs_fast.rax);
    assert(regs_fast.rax == 1000);
    printf("    - SYS_GETUID (102) : PASSED (returned %lu)\n", regs_fast.rax);

    // Test geteuid
    regs_fast.rax = SYS_GETEUID;
    dispatch_fastpath(&regs_fast);
    assert(regs_fast.rax == 1000);
    printf("    - SYS_GETEUID (107): PASSED (returned %lu)\n", regs_fast.rax);

    // Test getgid
    regs_fast.rax = SYS_GETGID;
    dispatch_fastpath(&regs_fast);
    assert(regs_fast.rax == 1000);
    printf("    - SYS_GETGID (104) : PASSED (returned %lu)\n", regs_fast.rax);

    // Test getegid
    regs_fast.rax = SYS_GETEGID;
    dispatch_fastpath(&regs_fast);
    assert(regs_fast.rax == 1000);
    printf("    - SYS_GETEGID (108): PASSED (returned %lu)\n", regs_fast.rax);

    // Test main session fallback (PID 1)
    threads[15].is_main_session = true;
    regs_fast.rax = SYS_GETPID;
    dispatch_fastpath(&regs_fast);
    assert(regs_fast.rax == 1);
    regs_fast.rax = SYS_GETTID;
    dispatch_fastpath(&regs_fast);
    assert(regs_fast.rax == 1);
    threads[15].is_main_session = false;
    printf("    - Main Session (PID 1) : PASSED\n\n");

    /* Performance Microbenchmarks */
    printf("===================================================================\n");
    printf("             Syscall Dispatch Latency Microbenchmarks              \n");
    printf("===================================================================\n\n");

    const int ITERS = 5000000;

    // Benchmark 1: SYS_GETPID
    uint64_t t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETPID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_baseline(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_getpid_baseline = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETPID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_fastpath(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_getpid_fast = (__rdtsc() - t0) / ITERS;

    printf("[1] SYS_GETPID (39) [5,000,000 calls, 50 system threads]:\n");
    printf("    Baseline Generic Dispatcher : %4lu cycles/call\n", cycles_getpid_baseline);
    printf("    Optimized Ultra Fast-Path   : %4lu cycles/call\n", cycles_getpid_fast);
    printf("    -> Speedup                  : %.2fX FASTER\n\n", (double)cycles_getpid_baseline / cycles_getpid_fast);

    // Benchmark 2: SYS_GETTID
    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETTID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_baseline(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_gettid_baseline = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETTID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_fastpath(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_gettid_fast = (__rdtsc() - t0) / ITERS;

    printf("[2] SYS_GETTID (186) [5,000,000 calls]:\n");
    printf("    Baseline Generic Dispatcher : %4lu cycles/call\n", cycles_gettid_baseline);
    printf("    Optimized Ultra Fast-Path   : %4lu cycles/call\n", cycles_gettid_fast);
    printf("    -> Speedup                  : %.2fX FASTER\n\n", (double)cycles_gettid_baseline / cycles_gettid_fast);

    // Benchmark 3: SYS_GETUID
    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETUID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_baseline(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_getuid_baseline = (__rdtsc() - t0) / ITERS;

    t0 = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        struct syscall_regs r;
        r.rax = SYS_GETUID;
        r.rip = 0x401000;
        r.rsp = 0x7fffffffe000;
        r.rdi = 0; r.rsi = 0; r.rdx = 0; r.r10 = 0; r.r8 = 0; r.r9 = 0;
        dispatch_fastpath(&r);
        __asm__ volatile("" : "+r"(r.rax));
    }
    uint64_t cycles_getuid_fast = (__rdtsc() - t0) / ITERS;

    printf("[3] SYS_GETUID (102) [5,000,000 calls]:\n");
    printf("    Baseline Generic Dispatcher : %4lu cycles/call\n", cycles_getuid_baseline);
    printf("    Optimized Ultra Fast-Path   : %4lu cycles/call\n", cycles_getuid_fast);
    printf("    -> Speedup                  : %.2fX FASTER\n\n", (double)cycles_getuid_baseline / cycles_getuid_fast);

    printf("===================================================================\n");
    printf("                      BENCHMARK COMPLETE                           \n");
    printf("===================================================================\n");

    return 0;
}
