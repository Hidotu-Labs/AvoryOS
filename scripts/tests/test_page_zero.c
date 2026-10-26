#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define PAGE_SIZE 4096
#define BATCH_COUNT 32

/* Assembly functions from kernel/src/arch/x86_64/page_ops.asm */
extern void page_zero_nt(void *page);
extern void page_zero_nt_batch(void **pages, size_t count);
extern void page_copy_nt(void *dst, const void *src);

extern void page_zero_erms(void *page);
extern void page_copy_erms(void *dst, const void *src);

extern void page_zero_avx2(void *page);
extern void page_zero_avx2_batch(void **pages, size_t count);
extern void page_copy_avx2(void *dst, const void *src);

extern void page_zero_avx512(void *page);
extern void page_zero_avx512_batch(void **pages, size_t count);
extern void page_copy_avx512(void *dst, const void *src);

static inline uint64_t rdtsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    printf("===================================================================\n");
    printf("   AvoryOS Hardware-Accelerated Page Operations Benchmark (Zen 4)  \n");
    printf("===================================================================\n\n");

    /* Allocate test pages */
    void *pages[BATCH_COUNT];
    for (int i = 0; i < BATCH_COUNT; i++) {
        pages[i] = aligned_alloc(4096, PAGE_SIZE);
        assert(pages[i] != NULL);
    }
    void *src_page = aligned_alloc(4096, PAGE_SIZE);
    assert(src_page != NULL);
    for (int i = 0; i < PAGE_SIZE; i++) ((uint8_t *)src_page)[i] = (uint8_t)(i ^ 0x5A);

    /* 1. Correctness Verification */
    printf("[1] Correctness Verification (4096 bytes byte-by-byte check):\n");

    // page_zero_erms
    memset(pages[0], 0xAA, PAGE_SIZE);
    page_zero_erms(pages[0]);
    for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[0])[b] == 0);
    printf("    - page_zero_erms (REP STOSQ)      : PASSED\n");

    // page_zero_nt
    memset(pages[0], 0xBB, PAGE_SIZE);
    page_zero_nt(pages[0]);
    for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[0])[b] == 0);
    printf("    - page_zero_nt (64-bit MOVNTI)    : PASSED\n");

    // page_zero_avx2
    memset(pages[0], 0xCC, PAGE_SIZE);
    page_zero_avx2(pages[0]);
    for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[0])[b] == 0);
    printf("    - page_zero_avx2 (256-bit YMM)    : PASSED\n");

    // page_zero_avx512
    memset(pages[0], 0xDD, PAGE_SIZE);
    page_zero_avx512(pages[0]);
    for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[0])[b] == 0);
    printf("    - page_zero_avx512 (512-bit ZMM)  : PASSED\n");

    // Batched zeroing (32 pages)
    for (int i = 0; i < BATCH_COUNT; i++) memset(pages[i], 0x11, PAGE_SIZE);
    page_zero_nt_batch(pages, BATCH_COUNT);
    for (int i = 0; i < BATCH_COUNT; i++)
        for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[i])[b] == 0);
    printf("    - page_zero_nt_batch (32 pages)   : PASSED\n");

    for (int i = 0; i < BATCH_COUNT; i++) memset(pages[i], 0x22, PAGE_SIZE);
    page_zero_avx2_batch(pages, BATCH_COUNT);
    for (int i = 0; i < BATCH_COUNT; i++)
        for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[i])[b] == 0);
    printf("    - page_zero_avx2_batch (32 pages) : PASSED\n");

    for (int i = 0; i < BATCH_COUNT; i++) memset(pages[i], 0x33, PAGE_SIZE);
    page_zero_avx512_batch(pages, BATCH_COUNT);
    for (int i = 0; i < BATCH_COUNT; i++)
        for (int b = 0; b < PAGE_SIZE; b++) assert(((uint8_t *)pages[i])[b] == 0);
    printf("    - page_zero_avx512_batch (32 pages): PASSED\n");

    // Copy operations
    memset(pages[0], 0, PAGE_SIZE);
    page_copy_erms(pages[0], src_page);
    assert(memcmp(pages[0], src_page, PAGE_SIZE) == 0);
    printf("    - page_copy_erms                  : PASSED\n");

    memset(pages[0], 0, PAGE_SIZE);
    page_copy_nt(pages[0], src_page);
    assert(memcmp(pages[0], src_page, PAGE_SIZE) == 0);
    printf("    - page_copy_nt                    : PASSED\n");

    memset(pages[0], 0, PAGE_SIZE);
    page_copy_avx2(pages[0], src_page);
    assert(memcmp(pages[0], src_page, PAGE_SIZE) == 0);
    printf("    - page_copy_avx2                  : PASSED\n");

    memset(pages[0], 0, PAGE_SIZE);
    page_copy_avx512(pages[0], src_page);
    assert(memcmp(pages[0], src_page, PAGE_SIZE) == 0);
    printf("    - page_copy_avx512                : PASSED\n\n");

    /* 2. Scenario 1: Demand-Zero Page Fault Simulation (Zero 4KB + First User Write) */
    printf("[2] Demand-Zero Page Fault Latency (Zero 4KB + First 64-Byte Write, 10,000 faults):\n");
    printf("    Simulates vmm_handle_page_fault: allocating, zeroing, and immediate user write.\n");
    const int FAULT_ITERS = 10000;
    void *fault_page = pages[0];
    volatile uint64_t *write_ptr = (volatile uint64_t *)fault_page;

    // A. Standard memset + first write
    uint64_t t0 = rdtsc_serialized();
    for (int i = 0; i < FAULT_ITERS; i++) {
        memset(fault_page, 0, PAGE_SIZE);
        write_ptr[0] = 0x12345678;
        write_ptr[1] = 0x87654321;
    }
    uint64_t cycles_fault_memset = (rdtsc_serialized() - t0) / FAULT_ITERS;

    // B. ERMS (rep stosq) + first write
    t0 = rdtsc_serialized();
    for (int i = 0; i < FAULT_ITERS; i++) {
        page_zero_erms(fault_page);
        write_ptr[0] = 0x12345678;
        write_ptr[1] = 0x87654321;
    }
    uint64_t cycles_fault_erms = (rdtsc_serialized() - t0) / FAULT_ITERS;

    // C. 64-bit Non-Temporal (page_zero_nt) + first write
    t0 = rdtsc_serialized();
    for (int i = 0; i < FAULT_ITERS; i++) {
        page_zero_nt(fault_page);
        write_ptr[0] = 0x12345678;
        write_ptr[1] = 0x87654321;
    }
    uint64_t cycles_fault_nt = (rdtsc_serialized() - t0) / FAULT_ITERS;

    // D. 256-bit AVX2 (page_zero_avx2) + first write
    t0 = rdtsc_serialized();
    for (int i = 0; i < FAULT_ITERS; i++) {
        page_zero_avx2(fault_page);
        write_ptr[0] = 0x12345678;
        write_ptr[1] = 0x87654321;
    }
    uint64_t cycles_fault_avx2 = (rdtsc_serialized() - t0) / FAULT_ITERS;

    // E. 512-bit AVX-512 (page_zero_avx512) + first write
    t0 = rdtsc_serialized();
    for (int i = 0; i < FAULT_ITERS; i++) {
        page_zero_avx512(fault_page);
        write_ptr[0] = 0x12345678;
        write_ptr[1] = 0x87654321;
    }
    uint64_t cycles_fault_avx512 = (rdtsc_serialized() - t0) / FAULT_ITERS;

    printf("    Standard memset() + User Write   : %5lu cycles/fault\n", cycles_fault_memset);
    printf("    ERMS (rep stosq)  + User Write   : %5lu cycles/fault (L1 HIT)\n", cycles_fault_erms);
    printf("    64-bit NT (MOVNTI)+ User Write   : %5lu cycles/fault (L1 MISS/RFO)\n", cycles_fault_nt);
    printf("    AVX2 Streaming    + User Write   : %5lu cycles/fault (L1 MISS/RFO)\n", cycles_fault_avx2);
    printf("    AVX-512 Streaming + User Write   : %5lu cycles/fault (L1 MISS/RFO)\n", cycles_fault_avx512);
    printf("    -> ERMS Speedup vs NT Streaming  : %.2fX FASTER\n", (double)cycles_fault_avx2 / cycles_fault_erms);
    printf("    -> ERMS Speedup vs 64-bit NT     : %.2fX FASTER\n\n", (double)cycles_fault_nt / cycles_fault_erms);

    /* 3. Scenario 2: Per-CPU Pre-Zero Pool Allocation Fast Path */
    printf("[3] Per-CPU Pre-Zeroed Page Allocation Fast Path (100,000 allocs):\n");
    const int ALLOC_ITERS = 100000;
    void *local_pool[32];
    for (int i = 0; i < 32; i++) local_pool[i] = pages[i % BATCH_COUNT];
    int pool_top = 31;

    // Fast Path: PCP pre-zero pool pop
    t0 = rdtsc_serialized();
    for (int i = 0; i < ALLOC_ITERS; i++) {
        void *p = local_pool[pool_top];
        if (--pool_top < 0) pool_top = 31;
        __asm__ volatile ("" : "+r"(p) :: "memory");
    }
    uint64_t cycles_pool_hit = (rdtsc_serialized() - t0) / ALLOC_ITERS;

    // Slow Path: Synchronous page allocation + zero on the spot
    t0 = rdtsc_serialized();
    for (int i = 0; i < ALLOC_ITERS; i++) {
        void *p = pages[0];
        page_zero_erms(p);
        __asm__ volatile ("" : "+r"(p) :: "memory");
    }
    uint64_t cycles_sync_zero = (rdtsc_serialized() - t0) / ALLOC_ITERS;

    printf("    PCP Pre-Zero Pool Pop (Fast Path): %5lu cycles/alloc\n", cycles_pool_hit);
    printf("    Synchronous On-Demand Zero (Slow): %5lu cycles/alloc\n", cycles_sync_zero);
    printf("    -> Pre-Zero Pool Allocation Gain : %.2fX FASTER\n\n",
           (double)cycles_sync_zero / (cycles_pool_hit ? cycles_pool_hit : 1));

    /* 4. Scenario 3: Pre-Zero Pool Background Refill (32 pages = 128 KB) */
    printf("[4] Pre-Zero Pool Refill Throughput (32 pages = 128 KB, 2,000 refills):\n");
    printf("    Simulates idle CPU background refill in pmm_zero_pool_refill.\n");
    const int REFILL_ITERS = 2000;

    // A. 64-bit MOVNTI batch
    t0 = rdtsc_serialized();
    for (int i = 0; i < REFILL_ITERS; i++) {
        page_zero_nt_batch(pages, BATCH_COUNT);
    }
    uint64_t cycles_batch_nt = (rdtsc_serialized() - t0) / REFILL_ITERS;

    // B. AVX2 VMOVNTDQ batch
    t0 = rdtsc_serialized();
    for (int i = 0; i < REFILL_ITERS; i++) {
        page_zero_avx2_batch(pages, BATCH_COUNT);
    }
    uint64_t cycles_batch_avx2 = (rdtsc_serialized() - t0) / REFILL_ITERS;

    // C. AVX-512 VMOVNTDQ batch
    t0 = rdtsc_serialized();
    for (int i = 0; i < REFILL_ITERS; i++) {
        page_zero_avx512_batch(pages, BATCH_COUNT);
    }
    uint64_t cycles_batch_avx512 = (rdtsc_serialized() - t0) / REFILL_ITERS;

    printf("    Baseline (64-bit MOVNTI batch)  : %6lu cycles/refill (%5.1f cycles/page)\n",
           cycles_batch_nt, (double)cycles_batch_nt / BATCH_COUNT);
    printf("    AVX2 Streaming (VMOVNTDQ batch) : %6lu cycles/refill (%5.1f cycles/page)\n",
           cycles_batch_avx2, (double)cycles_batch_avx2 / BATCH_COUNT);
    printf("    AVX-512 Streaming (ZMM batch)   : %6lu cycles/refill (%5.1f cycles/page)\n",
           cycles_batch_avx512, (double)cycles_batch_avx512 / BATCH_COUNT);
    printf("    -> AVX-512 Speedup vs 64-bit NT : %.2fX FASTER\n\n",
           (double)cycles_batch_nt / cycles_batch_avx512);

    /* 5. Scenario 4: L1 Working Set Preservation */
    printf("[5] Working Set Cache Pollution Benchmark (32 KB Working Set, 500 iterations):\n");
    printf("    Measures impact on active application working set during background refill.\n");
    #define WS_SIZE (32 * 1024)
    uint8_t *working_set = aligned_alloc(64, WS_SIZE);
    for (int i = 0; i < WS_SIZE; i++) working_set[i] = (uint8_t)i;

    const int WARM_ITERS = 500;
    uint64_t total_ws_memset = 0;
    uint64_t total_ws_nt = 0;
    uint64_t total_ws_avx2 = 0;
    uint64_t total_ws_avx512 = 0;

    for (int iter = 0; iter < WARM_ITERS; iter++) {
        uint64_t s = 0;

        // 1. Warm working set into L1 cache
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));

        // Refill via standard memset (evicts L1 cache)
        for (int i = 0; i < BATCH_COUNT; i++) memset(pages[i], 0, PAGE_SIZE);

        t0 = rdtsc_serialized();
        s = 0;
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));
        total_ws_memset += (rdtsc_serialized() - t0);

        // 2. Warm working set into L1 cache
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));

        // Refill via 64-bit NT stores
        page_zero_nt_batch(pages, BATCH_COUNT);

        t0 = rdtsc_serialized();
        s = 0;
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));
        total_ws_nt += (rdtsc_serialized() - t0);

        // 3. Warm working set into L1 cache
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));

        // Refill via AVX2 non-temporal stores
        page_zero_avx2_batch(pages, BATCH_COUNT);

        t0 = rdtsc_serialized();
        s = 0;
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));
        total_ws_avx2 += (rdtsc_serialized() - t0);

        // 4. Warm working set into L1 cache
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));

        // Refill via AVX-512 non-temporal stores
        page_zero_avx512_batch(pages, BATCH_COUNT);

        t0 = rdtsc_serialized();
        s = 0;
        for (int i = 0; i < WS_SIZE; i += 64) s += working_set[i];
        __asm__ volatile ("" : "+r"(s));
        total_ws_avx512 += (rdtsc_serialized() - t0);
    }

    uint64_t avg_ws_memset = total_ws_memset / WARM_ITERS;
    uint64_t avg_ws_nt = total_ws_nt / WARM_ITERS;
    uint64_t avg_ws_avx2 = total_ws_avx2 / WARM_ITERS;
    uint64_t avg_ws_avx512 = total_ws_avx512 / WARM_ITERS;

    printf("    App Working Set Access after memset() : %5lu cycles (L1 Evicted -> L2 Reload)\n", avg_ws_memset);
    printf("    App Working Set Access after 64-bit NT: %5lu cycles\n", avg_ws_nt);
    printf("    App Working Set Access after AVX2     : %5lu cycles (L1 Preserved)\n", avg_ws_avx2);
    printf("    App Working Set Access after AVX-512  : %5lu cycles (L1 Preserved)\n", avg_ws_avx512);
    printf("    -> AVX-512 Cache Preservation Benefit : %.2fX FASTER app access vs memset\n\n",
           (double)avg_ws_memset / avg_ws_avx512);

    for (int i = 0; i < BATCH_COUNT; i++) free(pages[i]);
    free(src_page);
    free(working_set);

    printf("===================================================================\n");
    printf("                      BENCHMARK COMPLETE                           \n");
    printf("===================================================================\n");
    return 0;
}
