#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <x86intrin.h>

#define ITERATIONS 10000000

static inline uint64_t rdtsc_start(void) {
    uint32_t cycles_high, cycles_low;
    asm volatile(
        "cpuid\n\t"
        "rdtsc\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        : "=r"(cycles_high), "=r"(cycles_low)::"rax", "rbx", "rcx", "rdx");
    return ((uint64_t)cycles_high << 32) | cycles_low;
}

static inline uint64_t rdtsc_end(void) {
    uint32_t cycles_high, cycles_low;
    asm volatile(
        "rdtscp\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        "cpuid\n\t"
        : "=r"(cycles_high), "=r"(cycles_low)::"rax", "rbx", "rcx", "rdx");
    return ((uint64_t)cycles_high << 32) | cycles_low;
}

/* -------------------------------------------------------------
 * 1. Bit Search for Pending Signals (64-iteration loop vs __builtin_ctzll)
 * ------------------------------------------------------------- */
static inline int old_find_signal(uint64_t pending) {
    for (int i = 0; i < 64; i++) {
        if (pending & (1ULL << i)) {
            return i + 1;
        }
    }
    return 0;
}

static inline int new_find_signal(uint64_t pending) {
    if (!pending) return 0;
    return __builtin_ctzll(pending) + 1;
}

/* -------------------------------------------------------------
 * 2. Sigaction / Pending Signal thread list scan vs ref_count <= 1
 * ------------------------------------------------------------- */
struct dummy_thread {
    uint32_t tgid;
    uint64_t pending_signals;
    struct dummy_thread *next;
};

static uint64_t old_sigaction_sync(struct dummy_thread *list, uint32_t tgid, int act) {
    // Simulates spinlock acquire/release + iterating thread list
    volatile uint32_t lock = 0;
    lock = 1; // spinlock_acquire
    uint64_t count = 0;
    for (struct dummy_thread *t = list; t; t = t->next) {
        if (t->tgid == tgid) {
            count += act;
        }
    }
    lock = 0; // spinlock_release
    return count;
}

static uint64_t new_sigaction_sync(uint32_t mm_ref_count, struct dummy_thread *list, uint32_t tgid, int act) {
    // Fast path: if single-threaded (mm_ref_count <= 1), no lock or iteration needed!
    if (__builtin_expect(mm_ref_count <= 1, 1)) {
        return act;
    }
    volatile uint32_t lock = 0;
    lock = 1;
    uint64_t count = 0;
    for (struct dummy_thread *t = list; t; t = t->next) {
        if (t->tgid == tgid) {
            count += act;
        }
    }
    lock = 0;
    return count;
}

/* -------------------------------------------------------------
 * 3. User Address Validation: VMA Tree Walk vs Direct User Range Check
 * ------------------------------------------------------------- */
struct vma_node {
    uint64_t start;
    uint64_t end;
    struct vma_node *left, *right;
};

static bool old_vma_lookup(struct vma_node *root, uint64_t addr, uint64_t len) {
    // Simulates lock acquire + AVL tree lookup for addr range
    volatile uint32_t lock = 0;
    lock = 1;
    struct vma_node *curr = root;
    bool found = false;
    while (curr) {
        if (addr >= curr->start && (addr + len) <= curr->end) {
            found = true;
            break;
        }
        if (addr < curr->start) curr = curr->left;
        else curr = curr->right;
    }
    lock = 0;
    return found;
}

static inline bool new_is_user_range(uint64_t addr, uint64_t len) {
    // Fast path: single bounds check against canonical user limit (0x00007fffffffffff)
    return (addr + len >= addr) && ((addr + len) <= 0x00007fffffffffffULL);
}

int main(void) {
    printf("=== AvoryOS Kernel Signal Fast-Path Microbenchmark ===\n");
    printf("Iterations per test: %d\n\n", ITERATIONS);

    // Warm up
    uint64_t t0, t1;

    /* ---------------- Benchmark 1: Pending Signal Bit Scan ---------------- */
    // Test signal at bit 2 (SIGQUIT), bit 14 (SIGTERM), and bit 27 (SIGWINCH)
    uint64_t test_sigs[] = { (1ULL << 2), (1ULL << 14), (1ULL << 27), (1ULL << 55) };
    const char *sig_names[] = { "Low bit (SIGQUIT - bit 2)", "Mid-low bit (SIGTERM - bit 14)", "Mid bit (SIGWINCH - bit 27)", "High bit (RT signal - bit 55)" };

    for (int s = 0; s < 4; s++) {
        uint64_t sig_mask = test_sigs[s];
        volatile int res = 0;

        // Old scalar loop
        t0 = rdtsc_start();
        for (int i = 0; i < ITERATIONS; i++) {
            res = old_find_signal(sig_mask);
        }
        t1 = rdtsc_end();
        double old_cycles = (double)(t1 - t0) / ITERATIONS;

        // New hardware tzcnt
        t0 = rdtsc_start();
        for (int i = 0; i < ITERATIONS; i++) {
            res = new_find_signal(sig_mask);
        }
        t1 = rdtsc_end();
        double new_cycles = (double)(t1 - t0) / ITERATIONS;

        printf("1. Signal Bit Scan [%s]:\n", sig_names[s]);
        printf("   - Old (64-iteration loop):   %6.2f cycles/op\n", old_cycles);
        printf("   - New (Hardware tzcnt):      %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_cycles, old_cycles / new_cycles);
    }

    /* ---------------- Benchmark 2: rt_sigaction / Thread Sync ---------------- */
    // Simulate a process list of 8 threads (typical background daemons)
    struct dummy_thread threads[8];
    for (int i = 0; i < 8; i++) {
        threads[i].tgid = (i == 3) ? 100 : (i + 1);
        threads[i].pending_signals = 0;
        threads[i].next = (i < 7) ? &threads[i + 1] : NULL;
    }

    volatile uint64_t sync_res = 0;
    // Old sync
    t0 = rdtsc_start();
    for (int i = 0; i < ITERATIONS; i++) {
        sync_res = old_sigaction_sync(threads, 100, 1);
    }
    t1 = rdtsc_end();
    double old_sync_cycles = (double)(t1 - t0) / ITERATIONS;

    // New sync (single-threaded fast path)
    t0 = rdtsc_start();
    for (int i = 0; i < ITERATIONS; i++) {
        sync_res = new_sigaction_sync(1, threads, 100, 1);
    }
    t1 = rdtsc_end();
    double new_sync_cycles = (double)(t1 - t0) / ITERATIONS;

    printf("2. Signal Action / Thread Sync (Single-threaded fast path vs global list lock):\n");
    printf("   - Old (tid_lock + list walk):%6.2f cycles/op\n", old_sync_cycles);
    printf("   - New (Single-thread O(1)):  %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_sync_cycles, old_sync_cycles / new_sync_cycles);

    /* ---------------- Benchmark 3: User Address Validation ---------------- */
    // Balanced tree with depth ~3
    struct vma_node n1 = {0x00007f0000000000ULL, 0x00007f0000001000ULL, NULL, NULL};
    struct vma_node n2 = {0x00007f0000002000ULL, 0x00007f0000003000ULL, NULL, NULL};
    struct vma_node root = {0x00007f0000001000ULL, 0x00007f0000002000ULL, &n1, &n2};

    volatile bool val_res = false;
    // Old VMA lookup
    t0 = rdtsc_start();
    for (int i = 0; i < ITERATIONS; i++) {
        val_res = old_vma_lookup(&root, 0x00007f0000001800ULL, 8);
    }
    t1 = rdtsc_end();
    double old_val_cycles = (double)(t1 - t0) / ITERATIONS;

    // New bounds check
    t0 = rdtsc_start();
    for (int i = 0; i < ITERATIONS; i++) {
        val_res = new_is_user_range(0x00007f0000001800ULL, 8);
    }
    t1 = rdtsc_end();
    double new_val_cycles = (double)(t1 - t0) / ITERATIONS;

    printf("3. Syscall Pointer Range Check (vfs/vmm AVL tree walk vs is_user_range):\n");
    printf("   - Old (VMA lock + AVL walk): %6.2f cycles/op\n", old_val_cycles);
    printf("   - New (Direct User Range):   %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_val_cycles, old_val_cycles / new_val_cycles);

    return 0;
}
