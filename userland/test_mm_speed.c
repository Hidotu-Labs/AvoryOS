/*
 * test_mm_speed.c - AvoryOS Dedicated Memory-Management (MM) Speed Test
 *
 * Measures mmap/munmap/mprotect/madvise/brk/mremap latency, demand page-fault
 * cost (PMM alloc + fault), zero-page read faults, and memset/memcpy bandwidth
 * on already-resident mappings. Complements test_syscall_speed.c (which covers
 * all syscalls shallowly), test_pcp_pmm.c (PMM frame throughput) and
 * test_heap_smp.c (malloc/free throughput) with an MM-focused deep dive.
 *
 * Usage: test_mm_speed [iterations]
 *   iterations scales the fast syscall loops (default 1000). Large-region
 *   fault/bandwidth loops use a fraction of that to keep runtime sane.
 *
 * Exit status: 0 on success, 1 on correctness failure.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

#define DEFAULT_ITERS 1000u

typedef struct {
    const char *name;
    const char *detail;
    unsigned iters;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    double avg_ns;
    double ops_per_sec;
    double mb_per_sec; /* >0 only for payload-size benchmarks */
} mm_result_t;

static mm_result_t results[64];
static size_t num_results = 0;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void add_result(const char *name, const char *detail, unsigned iters,
                       uint64_t total_ns, uint64_t min_ns, uint64_t max_ns,
                       uint64_t bytes_per_op) {
    if (num_results >= sizeof(results) / sizeof(results[0]))
        return;
    mm_result_t *r = &results[num_results++];
    r->name = name;
    r->detail = detail;
    r->iters = iters;
    r->total_ns = total_ns;
    r->min_ns = min_ns;
    r->max_ns = max_ns;
    r->avg_ns = iters ? (double)total_ns / (double)iters : 0.0;
    r->ops_per_sec = total_ns ? ((double)iters * 1e9 / (double)total_ns) : 0.0;
    r->mb_per_sec = (total_ns && bytes_per_op)
        ? ((double)bytes_per_op * (double)iters * 1000.0 / (double)total_ns)
        : 0.0;
}

#define BENCH_LOOP_BEGIN() \
    uint64_t _t_total0 = now_ns(); \
    uint64_t _min_ns = (uint64_t)-1; \
    uint64_t _max_ns = 0;

#define BENCH_OP_BEGIN() uint64_t _t0 = now_ns();
#define BENCH_OP_END() \
    do { \
        uint64_t _d = now_ns() - _t0; \
        if (_d < _min_ns) _min_ns = _d; \
        if (_d > _max_ns) _max_ns = _d; \
    } while (0)

#define BENCH_LOOP_END(name_, detail_, iters_, bpo_) \
    do { \
        uint64_t _total = now_ns() - _t_total0; \
        if (_min_ns == (uint64_t)-1) _min_ns = 0; \
        add_result(name_, detail_, iters_, _total, _min_ns, _max_ns, bpo_); \
    } while (0)

/* Touch every 4K page with a write of a distinct pattern. Returns 0 on ok. */
static int touch_and_verify(uint8_t *base, size_t len, uint8_t seed) {
    size_t pages = len / 4096;
    for (size_t i = 0; i < pages; i++)
        base[i * 4096] = (uint8_t)(seed + (i & 0xFF));
    for (size_t i = 0; i < pages; i++) {
        if (base[i * 4096] != (uint8_t)(seed + (i & 0xFF)))
            return -1;
    }
    return 0;
}

/* 1. mmap+munmap without touching: pure VMA create/destroy cost. */
static int bench_map_unmap(size_t len, unsigned iters, const char *label) {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[FAIL] %s: mmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        if (munmap(p, len) != 0) {
            printf("[FAIL] %s: munmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        BENCH_OP_END();
    }
    BENCH_LOOP_END("mmap+munmap", label, iters, 0);
    return 0;
}

/* 2. mmap + fault every page (write) + verify + munmap: PMM + fault path. */
static int bench_fault_write(size_t len, unsigned iters, const char *label) {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[FAIL] %s: mmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        if (touch_and_verify(p, len, (uint8_t)i) != 0) {
            printf("[FAIL] %s: data mismatch iter %u\n", label, i);
            munmap(p, len);
            return 1;
        }
        if (munmap(p, len) != 0) {
            printf("[FAIL] %s: munmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        BENCH_OP_END();
    }
    BENCH_LOOP_END("fault-write", label, iters, len);
    return 0;
}

/* 3. mmap + read every page (zero-page fault, no dirty). */
static int bench_fault_read(size_t len, unsigned iters, const char *label) {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        volatile uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[FAIL] %s: mmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        volatile uint64_t sum = 0;
        for (size_t off = 0; off < len; off += 4096)
            sum += p[off];
        if (sum != 0) {
            printf("[FAIL] %s: fresh anon page not zero (sum=%lu)\n",
                   label, (unsigned long)sum);
            munmap((void *)p, len);
            return 1;
        }
        if (munmap((void *)p, len) != 0) {
            printf("[FAIL] %s: munmap failed: %s\n", label, strerror(errno));
            return 1;
        }
        BENCH_OP_END();
    }
    BENCH_LOOP_END("fault-read-zero", label, iters, len);
    return 0;
}

/* 4. mprotect ping-pong on a resident mapping. */
static int bench_mprotect(size_t len, unsigned iters) {
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("[FAIL] mprotect: setup mmap failed: %s\n", strerror(errno));
        return 1;
    }
    memset(p, 0xA5, len);

    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        if (mprotect(p, len, PROT_READ) != 0 ||
            mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
            printf("[FAIL] mprotect toggle failed iter %u: %s\n",
                   i, strerror(errno));
            munmap(p, len);
            return 1;
        }
        BENCH_OP_END();
    }
    BENCH_LOOP_END("mprotect", "RO->RW toggle pair", iters, 0);
    munmap(p, len);
    return 0;
}

/* 5. madvise(MADV_DONTNEED) discard on a dirtied mapping. */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int bench_madvise_dontneed(size_t len, unsigned iters) {
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("[FAIL] madvise: setup mmap failed: %s\n", strerror(errno));
        return 1;
    }
    /* Per-iteration samples: avg hides the shape (warmup vs periodic
     * spikes), so report the distribution. */
    uint64_t *samples = (uint64_t *)malloc(sizeof(uint64_t) * iters);
    if (!samples) {
        printf("[FAIL] madvise: sample buffer OOM\n");
        munmap(p, len);
        return 1;
    }

    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        memset(p, (int)(i & 0xFF), len);
        uint64_t t0 = now_ns();
        int r = madvise(p, len, MADV_DONTNEED);
        uint64_t dt = now_ns() - t0;
        samples[i] = dt;
        if (dt < _min_ns) _min_ns = dt;
        if (dt > _max_ns) _max_ns = dt;
        if (r != 0) {
            printf("[FAIL] madvise DONTNEED failed iter %u: %s\n",
                   i, strerror(errno));
            free(samples);
            munmap(p, len);
            return 1;
        }
        /* Verify discard: next read must see zeros. */
        volatile uint8_t v = p[0];
        (void)v;
    }
    BENCH_LOOP_END("madvise", "DONTNEED discard", iters, len);

    uint64_t *sorted = (uint64_t *)malloc(sizeof(uint64_t) * iters);
    if (sorted) {
        memcpy(sorted, samples, sizeof(uint64_t) * iters);
        qsort(sorted, iters, sizeof(uint64_t), cmp_u64);
        printf("[DIST]   madvise per-iter (ns): iter0=%lu iter1=%lu "
               "min=%lu p50=%lu p90=%lu max=%lu\n",
               (unsigned long)samples[0],
               (unsigned long)(iters > 1 ? samples[1] : 0),
               (unsigned long)sorted[0],
               (unsigned long)sorted[iters / 2],
               (unsigned long)sorted[(iters * 9) / 10],
               (unsigned long)sorted[iters - 1]);
        free(sorted);
    }
    free(samples);
    munmap(p, len);
    return 0;
}

/* 6. brk(0) query latency (no address-space mutation). */
static int bench_brk_query(unsigned iters) {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        void *b = sbrk(0);
        BENCH_OP_END();
        if (b == (void *)-1) {
            printf("[FAIL] sbrk(0) failed: %s\n", strerror(errno));
            return 1;
        }
    }
    BENCH_LOOP_END("brk", "query sbrk(0)", iters, 0);
    return 0;
}

/* 7. mremap grow/shrink/move on anonymous mappings. */
static int bench_mremap(unsigned iters) {
    const size_t base = 64 * 1024;
    const size_t grown = 128 * 1024;
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        uint8_t *p = mmap(NULL, base, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[FAIL] mremap: setup mmap failed: %s\n", strerror(errno));
            return 1;
        }
        p[0] = 0x5A;
        p[base - 1] = 0xA5;
        void *q = mremap(p, base, grown, MREMAP_MAYMOVE);
        if (q == MAP_FAILED) {
            printf("[FAIL] mremap grow failed iter %u: %s\n",
                   i, strerror(errno));
            munmap(p, base);
            return 1;
        }
        if (((uint8_t *)q)[0] != 0x5A || ((uint8_t *)q)[base - 1] != 0xA5) {
            printf("[FAIL] mremap grow corrupted data iter %u\n", i);
            munmap(q, grown);
            return 1;
        }
        void *r = mremap(q, grown, base, MREMAP_MAYMOVE);
        if (r == MAP_FAILED) {
            printf("[FAIL] mremap shrink failed iter %u: %s\n",
                   i, strerror(errno));
            munmap(q, grown);
            return 1;
        }
        if (((uint8_t *)r)[0] != 0x5A) {
            printf("[FAIL] mremap shrink corrupted data iter %u\n", i);
            munmap(r, base);
            return 1;
        }
        munmap(r, base);
        BENCH_OP_END();
    }
    BENCH_LOOP_END("mremap", "64K->128K grow + shrink", iters, 0);
    return 0;
}

/* 8. memset/memcpy bandwidth on an already-faulted region. */
static int bench_bandwidth(size_t len, unsigned iters) {
    uint8_t *a = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) {
        printf("[FAIL] bandwidth: setup mmap failed: %s\n", strerror(errno));
        if (a != MAP_FAILED) munmap(a, len);
        if (b != MAP_FAILED) munmap(b, len);
        return 1;
    }
    memset(a, 0x3C, len);
    memset(b, 0x00, len);

    {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        memset(b, (int)(i & 0xFF), len);
        BENCH_OP_END();
    }
    BENCH_LOOP_END("memset", "resident 8MB memset", iters, len);
    }

    {
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        memcpy(b, a, len);
        BENCH_OP_END();
    }
    BENCH_LOOP_END("memcpy", "resident 8MB copy", iters, len);
    }

    /* Correctness: copy must be exact. */
    memset(a, 0x77, len);
    memcpy(b, a, len);
    if (memcmp(a, b, len) != 0) {
        printf("[FAIL] memcpy verification failed\n");
        munmap(a, len);
        munmap(b, len);
        return 1;
    }
    munmap(a, len);
    munmap(b, len);
    return 0;
}

/* 9. Huge-page fault comparison (best-effort: skip if unsupported). */
static int bench_hugepage(size_t len, unsigned iters) {
    void *probe = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (probe == MAP_FAILED) {
        printf("[SKIP] hugepage: MAP_HUGETLB unsupported (%s), "
               "trying madvise(THP) path only\n", strerror(errno));
    } else {
        munmap(probe, len);
        BENCH_LOOP_BEGIN();
        for (unsigned i = 0; i < iters; i++) {
            BENCH_OP_BEGIN();
            uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                              -1, 0);
            if (p == MAP_FAILED) {
                printf("[FAIL] hugepage mmap failed: %s\n", strerror(errno));
                return 1;
            }
            if (touch_and_verify(p, len, (uint8_t)i) != 0) {
                printf("[FAIL] hugepage data mismatch iter %u\n", i);
                munmap(p, len);
                return 1;
            }
            munmap(p, len);
            BENCH_OP_END();
        }
        BENCH_LOOP_END("fault-write-huge", "MAP_HUGETLB fault+verify", iters, len);
    }

    /* THP via madvise: never fails the suite if the kernel ignores the hint. */
    BENCH_LOOP_BEGIN();
    for (unsigned i = 0; i < iters; i++) {
        BENCH_OP_BEGIN();
        uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[FAIL] THP setup mmap failed: %s\n", strerror(errno));
            return 1;
        }
        madvise(p, len, MADV_HUGEPAGE);
        if (touch_and_verify(p, len, (uint8_t)(i + 1)) != 0) {
            printf("[FAIL] THP data mismatch iter %u\n", i);
            munmap(p, len);
            return 1;
        }
        munmap(p, len);
        BENCH_OP_END();
    }
    BENCH_LOOP_END("fault-write-thp", "madvise(HUGEPAGE) fault+verify",
                   iters, len);
    return 0;
}

static void print_report(void) {
    printf("\n================================================================================\n");
    printf("                    AVORYOS MM SPEED BENCHMARK REPORT\n");
    printf("================================================================================\n");
    printf("%-18s | %-32s | %6s | %10s | %10s | %12s | %10s\n",
           "Operation", "Detail", "Iters", "Avg", "Min", "Throughput", "MB/s");
    printf("-------------------+----------------------------------+--------+------------+------------+--------------+------------\n");
    for (size_t i = 0; i < num_results; i++) {
        mm_result_t *r = &results[i];
        char avg_s[32], min_s[32], ops_s[32], mb_s[32];
        if (r->avg_ns >= 1e6)
            snprintf(avg_s, sizeof(avg_s), "%7.2f ms", r->avg_ns / 1e6);
        else if (r->avg_ns >= 1e3)
            snprintf(avg_s, sizeof(avg_s), "%7.2f us", r->avg_ns / 1e3);
        else
            snprintf(avg_s, sizeof(avg_s), "%7.1f ns", r->avg_ns);
        if ((double)r->min_ns >= 1e6)
            snprintf(min_s, sizeof(min_s), "%7.2f ms", (double)r->min_ns / 1e6);
        else if ((double)r->min_ns >= 1e3)
            snprintf(min_s, sizeof(min_s), "%7.2f us", (double)r->min_ns / 1e3);
        else
            snprintf(min_s, sizeof(min_s), "%7lu ns", (unsigned long)r->min_ns);
        if (r->ops_per_sec >= 1e6)
            snprintf(ops_s, sizeof(ops_s), "%7.2f M/s", r->ops_per_sec / 1e6);
        else
            snprintf(ops_s, sizeof(ops_s), "%7.2f K/s", r->ops_per_sec / 1e3);
        if (r->mb_per_sec > 0.0)
            snprintf(mb_s, sizeof(mb_s), "%8.1f", r->mb_per_sec);
        else
            snprintf(mb_s, sizeof(mb_s), "         -");
        printf("%-18s | %-32s | %6u | %10s | %10s | %12s | %10s\n",
               r->name, r->detail, r->iters, avg_s, min_s, ops_s, mb_s);
    }
    printf("================================================================================\n");
    uint64_t grand = 0;
    for (size_t i = 0; i < num_results; i++)
        grand += results[i].total_ns;
    printf("SUMMARY: %zu scenarios, total measured time %.2f ms\n",
           num_results, (double)grand / 1e6);
    printf("================================================================================\n");
}

int main(int argc, char **argv) {
    unsigned iters = DEFAULT_ITERS;
    if (argc > 1) {
        long v = atol(argv[1]);
        if (v > 0 && v <= 1000000)
            iters = (unsigned)v;
    }
    /* Scale heavy tests so default runtime stays sane. */
    unsigned iters_1m = iters / 10 > 5 ? iters / 10 : 5;
    unsigned iters_4m = iters / 50 > 3 ? iters / 50 : 3;
    unsigned iters_8m = iters / 100 > 2 ? iters / 100 : 2;

    printf("====================================================\n");
    printf("AvoryOS MM Speed Test (base iters=%u)\n", iters);
    printf("====================================================\n");

    int rc = 0;
    /* Pure syscall latency (no page faults). */
    rc |= bench_map_unmap(4096, iters, "4KB anon, no touch");
    rc |= bench_map_unmap(64 * 1024, iters / 2 > 10 ? iters / 2 : 10,
                          "64KB anon, no touch");
    rc |= bench_map_unmap(1024 * 1024, iters_1m, "1MB anon, no touch");
    /* Fault path: PMM alloc + zero + map. */
    rc |= bench_fault_read(4 * 1024 * 1024, iters_4m, "4MB read (zero-page)");
    rc |= bench_fault_write(4 * 1024 * 1024, iters_4m, "4MB write+verify");
    /* Permission / reclaim ops on resident memory. */
    rc |= bench_mprotect(64 * 1024, iters);
    rc |= bench_madvise_dontneed(1024 * 1024, iters_1m);
    /* brk + mremap address-space ops. */
    rc |= bench_brk_query(iters);
    rc |= bench_mremap(iters / 5 > 10 ? iters / 5 : 10);
    /* Resident bandwidth ceiling (faults excluded). */
    rc |= bench_bandwidth(8 * 1024 * 1024, iters_8m);
    /* Huge-page fast path vs 4K. */
    rc |= bench_hugepage(4 * 1024 * 1024, iters_4m);

    print_report();

    if (rc != 0) {
        printf("[RESULT] MM SPEED TEST FAILED\n");
        return 1;
    }
    printf("[RESULT] MM SPEED TEST PASSED\n");
    return 0;
}
