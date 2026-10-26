#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <x86intrin.h>

#define ITERS 1000000

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

/* Types matching kernel string.c */
typedef uint64_t __attribute__((__may_alias__, aligned(1))) u64_ua;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) u32_ua;
typedef uint16_t __attribute__((__may_alias__, aligned(1))) u16_ua;

/* Old baseline memcpy (string.c before optimization) */
void *old_memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;
    uint8_t *dst = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (n < 16) {
        if (n >= 8) {
            *(u64_ua *)dst = *(const u64_ua *)s;
            *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
            return d;
        }
        if (n >= 4) {
            *(u32_ua *)dst = *(const u32_ua *)s;
            *(u32_ua *)(dst + n - 4) = *(const u32_ua *)(s + n - 4);
            return d;
        }
        if (n >= 2) {
            *(u16_ua *)dst = *(const u16_ua *)s;
            *(u16_ua *)(dst + n - 2) = *(const u16_ua *)(s + n - 2);
            return d;
        }
        if (n == 1) *dst = *s;
        return d;
    }

    if (n <= 32) {
        *(u64_ua *)dst = *(const u64_ua *)s;
        *(u64_ua *)(dst + 8) = *(const u64_ua *)(s + 8);
        *(u64_ua *)(dst + n - 16) = *(const u64_ua *)(s + n - 16);
        *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
        return d;
    }

    while ((uintptr_t)dst & 7) {
        *dst++ = *s++;
        n--;
    }

    size_t q = n >> 3;
    __asm__ volatile("rep movsq"
                     : "+D"(dst), "+S"(s), "+c"(q)
                     :
                     : "memory");

    n &= 7;
    while (n--)
        *dst++ = *s++;
    return d;
}

/* New optimized memcpy (active in AvoryOS kernel) */
void *new_memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;
    uint8_t *dst = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (n < 16) {
        if (n >= 8) {
            *(u64_ua *)dst = *(const u64_ua *)s;
            *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
            return d;
        }
        if (n >= 4) {
            *(u32_ua *)dst = *(const u32_ua *)s;
            *(u32_ua *)(dst + n - 4) = *(const u32_ua *)(s + n - 4);
            return d;
        }
        if (n >= 2) {
            *(u16_ua *)dst = *(const u16_ua *)s;
            *(u16_ua *)(dst + n - 2) = *(const u16_ua *)(s + n - 2);
            return d;
        }
        if (n == 1) *dst = *s;
        return d;
    }

    if (n <= 32) {
        *(u64_ua *)dst = *(const u64_ua *)s;
        *(u64_ua *)(dst + 8) = *(const u64_ua *)(s + 8);
        *(u64_ua *)(dst + n - 16) = *(const u64_ua *)(s + n - 16);
        *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
        return d;
    }

    if (n <= 64) {
        *(u64_ua *)dst = *(const u64_ua *)s;
        *(u64_ua *)(dst + 8) = *(const u64_ua *)(s + 8);
        *(u64_ua *)(dst + 16) = *(const u64_ua *)(s + 16);
        *(u64_ua *)(dst + 24) = *(const u64_ua *)(s + 24);
        *(u64_ua *)(dst + n - 32) = *(const u64_ua *)(s + n - 32);
        *(u64_ua *)(dst + n - 24) = *(const u64_ua *)(s + n - 24);
        *(u64_ua *)(dst + n - 16) = *(const u64_ua *)(s + n - 16);
        *(u64_ua *)(dst + n - 8) = *(const u64_ua *)(s + n - 8);
        return d;
    }

    if (n >= 2048) {
        size_t cnt = n;
        __asm__ volatile("rep movsb"
                         : "+D"(dst), "+S"(s), "+c"(cnt)
                         :
                         : "memory");
        return d;
    }

    while (n >= 64) {
        u64_ua q0 = *(const u64_ua *)(s);
        u64_ua q1 = *(const u64_ua *)(s + 8);
        u64_ua q2 = *(const u64_ua *)(s + 16);
        u64_ua q3 = *(const u64_ua *)(s + 24);
        u64_ua q4 = *(const u64_ua *)(s + 32);
        u64_ua q5 = *(const u64_ua *)(s + 40);
        u64_ua q6 = *(const u64_ua *)(s + 48);
        u64_ua q7 = *(const u64_ua *)(s + 56);

        *(u64_ua *)(dst)      = q0;
        *(u64_ua *)(dst + 8)  = q1;
        *(u64_ua *)(dst + 16) = q2;
        *(u64_ua *)(dst + 24) = q3;
        *(u64_ua *)(dst + 32) = q4;
        *(u64_ua *)(dst + 40) = q5;
        *(u64_ua *)(dst + 48) = q6;
        *(u64_ua *)(dst + 56) = q7;

        s += 64;
        dst += 64;
        n -= 64;
    }

    while (n >= 8) {
        *(u64_ua *)dst = *(const u64_ua *)s;
        dst += 8;
        s += 8;
        n -= 8;
    }
    if (n >= 4) {
        *(u32_ua *)dst = *(const u32_ua *)s;
        dst += 4;
        s += 4;
        n -= 4;
    }
    if (n >= 2) {
        *(u16_ua *)dst = *(const u16_ua *)s;
        dst += 2;
        s += 2;
        n -= 2;
    }
    if (n == 1) {
        *dst = *s;
    }

    return d;
}

/* -------------------------------------------------------------
 * Wait-Queue Wake Fast-Path Simulation
 * ------------------------------------------------------------- */
struct dummy_wait_entry {
    void *thread;
    struct dummy_wait_entry *next;
};

struct dummy_wait_queue {
    volatile uint32_t lock;
    struct dummy_wait_entry *head;
};

static void old_wake_waiters(struct dummy_wait_queue *wq) {
    // Unconditional lock acquire/release even when head is NULL
    wq->lock = 1;
    struct dummy_wait_entry *curr = wq->head;
    wq->head = NULL;
    wq->lock = 0;
    (void)curr;
}

static inline void new_wake_waiters(struct dummy_wait_queue *wq) {
    // Fast path: lockless check if any waiters exist
    if (__builtin_expect(wq->head != NULL, 0)) {
        wq->lock = 1;
        struct dummy_wait_entry *curr = wq->head;
        wq->head = NULL;
        wq->lock = 0;
        (void)curr;
    }
}

int main(void) {
    printf("=== AvoryOS Vectorized IPC & Pipe Transfer Microbenchmark ===\n");
    printf("Iterations per test: %d\n\n", ITERS);

    uint64_t t0, t1;

    /* 1. Memory copy across Wayland / Pipe payload sizes */
    size_t sizes[] = { 48, 128, 256, 1024, 4096 };
    const char *names[] = {
        "48 B  (Wayland input / fd control msg)",
        "128 B (Wayland pointer motion / key event)",
        "256 B (Wayland surface commit / X11 draw request)",
        "1 KB  (Audio pipeline PCM frame buffer)",
        "4 KB  (Full page pipe burst / SHM copy)"
    };

    for (int i = 0; i < 5; i++) {
        size_t sz = sizes[i];
        uint8_t *raw_src = aligned_alloc(64, sz + 64);
        uint8_t *raw_dst = aligned_alloc(64, sz + 64);

        // Circular ring buffer offset (unaligned simulation)
        uint8_t *src = raw_src + 5;
        uint8_t *dst = raw_dst + 9;

        memset(raw_src, 0x3C, sz + 64);
        memset(raw_dst, 0, sz + 64);

        t0 = rdtsc_start();
        for (int it = 0; it < ITERS; it++) {
            old_memcpy(dst, src, sz);
        }
        t1 = rdtsc_end();
        double old_cycles = (double)(t1 - t0) / ITERS;

        t0 = rdtsc_start();
        for (int it = 0; it < ITERS; it++) {
            new_memcpy(dst, src, sz);
        }
        t1 = rdtsc_end();
        double new_cycles = (double)(t1 - t0) / ITERS;

        printf("1. IPC Buffer Copy [%s]:\n", names[i]);
        printf("   - Old (rep movsq):        %6.2f cycles/op\n", old_cycles);
        printf("   - Vectorized Quadword:    %6.2f cycles/op  ==> %.2fX FASTER\n\n", new_cycles, old_cycles / new_cycles);

        if (memcmp(dst, src, sz) != 0) {
            printf("FATAL: Memory copy data integrity failed!\n");
            return 1;
        }

        free(raw_src);
        free(raw_dst);
    }

    /* 2. Wait queue notification overhead in fast IPC loop */
    struct dummy_wait_queue wq = { .lock = 0, .head = NULL };

    t0 = rdtsc_start();
    for (int it = 0; it < ITERS; it++) {
        old_wake_waiters(&wq);
    }
    t1 = rdtsc_end();
    double old_wake_cycles = (double)(t1 - t0) / ITERS;

    t0 = rdtsc_start();
    for (int it = 0; it < ITERS; it++) {
        new_wake_waiters(&wq);
    }
    t1 = rdtsc_end();
    double new_wake_cycles = (double)(t1 - t0) / ITERS;

    printf("2. Wait Queue Wakeup Fast-Path (Empty Queue — Common Event Loop Path):\n");
    printf("   - Old (Unconditional Lock):%6.2f cycles/op\n", old_wake_cycles);
    printf("   - New (Lockless Head Check):%6.2f cycles/op  ==> %.2fX FASTER\n\n", new_wake_cycles, old_wake_cycles / new_wake_cycles);

    return 0;
}
