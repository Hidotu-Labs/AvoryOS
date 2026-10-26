#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <x86intrin.h>

#define ITERS 10000000

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

/* 1. Address Validation */
struct vma_node {
    uint64_t start, end;
    struct vma_node *left, *right;
};

static bool old_vma_check(struct vma_node *root, uint64_t addr) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    volatile uint32_t lock = 0;
    __atomic_test_and_set(&lock, __ATOMIC_ACQUIRE);
    struct vma_node *curr = root;
    bool found = false;
    while (curr) {
        if (addr >= curr->start && (addr + 4) <= curr->end) {
            found = true;
            break;
        }
        if (addr < curr->start) curr = curr->left;
        else curr = curr->right;
    }
    __atomic_clear(&lock, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
    return found;
}

static inline bool new_is_user_range(uint64_t addr) {
    return (addr + 4 >= addr) && ((addr + 4) <= 0x00007fffffffffffULL);
}

/* 2. Uncontended Wakeup */
struct test_futex_bucket {
    volatile uint8_t lock;
    volatile uint32_t waiters;
    void *head;
};

static uint32_t old_uncontended_wake(struct test_futex_bucket *b) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&b->lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
    uint32_t woken = 0;
    if (b->waiters > 0) woken = 1;
    __atomic_clear(&b->lock, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
    return woken;
}

static inline uint32_t new_uncontended_wake(struct test_futex_bucket *b) {
    // Fast path: lockless atomic check
    if (__atomic_load_n(&b->waiters, __ATOMIC_SEQ_CST) == 0) {
        return 0;
    }
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&b->lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
    uint32_t woken = 0;
    if (b->head) woken = 1;
    __atomic_clear(&b->lock, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
    return woken;
}

/* 3. Contended Single Waiter Wakeup (val == 1) */
struct test_waiter {
    uint64_t key;
    void *thread;
    struct test_waiter *next;
};

static uint32_t old_single_wake(struct test_futex_bucket *b, struct test_waiter *w_list, uint64_t key) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&b->lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
    struct test_waiter *w = w_list;
    void *batch[16];
    uint32_t count = 0;
    while (w && count < 1) {
        if (w->key == key) {
            batch[count++] = w->thread;
            break;
        }
        w = w->next;
    }
    __atomic_clear(&b->lock, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
    return count;
}

static inline uint32_t new_single_wake(struct test_futex_bucket *b, struct test_waiter *w_list, uint64_t key) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&b->lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
    struct test_waiter *w = w_list;
    void *thread = NULL;
    while (w) {
        if (w->key == key) {
            thread = w->thread;
            break;
        }
        w = w->next;
    }
    __atomic_clear(&b->lock, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory");
    return thread ? 1 : 0;
}

/* 4. Hash bucket collision rate simulation across 1000 mutexes */
static inline uint32_t hash_key_256(uint64_t addr, uint64_t space) {
    uint64_t h = addr >> 2;
    h ^= space + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)(h & 255);
}

static inline uint32_t hash_key_1024(uint64_t addr, uint64_t space) {
    uint64_t h = addr >> 2;
    h ^= space + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)(h & 1023);
}

int main(void) {
    printf("=== AvoryOS Futex O(1) Fast-Path Microbenchmark ===\n");
    printf("Iterations per test: %d\n\n", ITERS);

    uint64_t t0, t1;

    /* Benchmark 1: Address Validation */
    struct vma_node left = {0x00007f0000000000ULL, 0x00007f0000001000ULL, NULL, NULL};
    struct vma_node right = {0x00007f0000002000ULL, 0x00007f0000003000ULL, NULL, NULL};
    struct vma_node root = {0x00007f0000001000ULL, 0x00007f0000002000ULL, &left, &right};

    volatile bool val_res = false;
    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        val_res = old_vma_check(&root, 0x00007f0000001800ULL);
    }
    t1 = rdtsc_end();
    double old_val_cycles = (double)(t1 - t0) / ITERS;

    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        val_res = new_is_user_range(0x00007f0000001800ULL);
    }
    t1 = rdtsc_end();
    double new_val_cycles = (double)(t1 - t0) / ITERS;

    printf("1. Futex Address Validation (VMA tree walk & mm->lock vs Direct User Range):\n");
    printf("   - Old (VMA lock + AVL search):%6.2f cycles/op\n", old_val_cycles);
    printf("   - New (Lockless user range):  %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_val_cycles, old_val_cycles / new_val_cycles);

    /* Benchmark 2: Uncontended Futex Wake */
    struct test_futex_bucket bucket = { .lock = 0, .waiters = 0, .head = NULL };
    volatile uint32_t wake_res = 0;

    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        wake_res = old_uncontended_wake(&bucket);
    }
    t1 = rdtsc_end();
    double old_wake_cycles = (double)(t1 - t0) / ITERS;

    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        wake_res = new_uncontended_wake(&bucket);
    }
    t1 = rdtsc_end();
    double new_wake_cycles = (double)(t1 - t0) / ITERS;

    printf("2. Uncontended FUTEX_WAKE (Lock acquisition vs Lockless Atomic Check):\n");
    printf("   - Old (Bucket Spinlock):      %6.2f cycles/op\n", old_wake_cycles);
    printf("   - New (Lockless Atomic Check):%6.2f cycles/op  ==> %.2fX FASTER\n\n", new_wake_cycles, old_wake_cycles / new_wake_cycles);

    /* Benchmark 3: Contended Single Waiter Wakeup (val == 1) */
    struct test_waiter w1 = { .key = 0x1234, .thread = (void*)0x999, .next = NULL };
    bucket.waiters = 1;
    bucket.head = &w1;

    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        wake_res = old_single_wake(&bucket, &w1, 0x1234);
    }
    t1 = rdtsc_end();
    double old_single_cycles = (double)(t1 - t0) / ITERS;

    t0 = rdtsc_start();
    for (int i = 0; i < ITERS; i++) {
        wake_res = new_single_wake(&bucket, &w1, 0x1234);
    }
    t1 = rdtsc_end();
    double new_single_cycles = (double)(t1 - t0) / ITERS;

    printf("3. Contended Single Waiter Wakeup (val == 1 Fast-Path):\n");
    printf("   - Old (Batching Loop):        %6.2f cycles/op\n", old_single_cycles);
    printf("   - New (Direct Single Wake):   %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_single_cycles, old_single_cycles / new_single_cycles);

    /* Benchmark 4: Hash Table Collision Rate across 1,000 active mutexes */
    uint32_t counts_256[256] = {0};
    uint32_t counts_1024[1024] = {0};
    for (int i = 0; i < 1000; i++) {
        uint64_t addr = 0x7fff00000000ULL + (i * 64);
        counts_256[hash_key_256(addr, 0x1000)]++;
        counts_1024[hash_key_1024(addr, 0x1000)]++;
    }

    uint32_t max_collisions_256 = 0;
    for (int i = 0; i < 256; i++) {
        if (counts_256[i] > max_collisions_256) max_collisions_256 = counts_256[i];
    }
    uint32_t max_collisions_1024 = 0;
    for (int i = 0; i < 1024; i++) {
        if (counts_1024[i] > max_collisions_1024) max_collisions_1024 = counts_1024[i];
    }

    printf("4. Hash Table Bucket Collision Spread (1,000 active mutex addresses):\n");
    printf("   - 256 Buckets (Old):          Max %u colliding mutexes/bucket (Avg 3.91/bucket)\n", max_collisions_256);
    printf("   - 1024 Buckets (New):         Max %u colliding mutexes/bucket (Avg 0.98/bucket) ==> %.2fX LESS CONTENTION\n\n",
           max_collisions_1024, (double)max_collisions_256 / max_collisions_1024);

    return 0;
}
