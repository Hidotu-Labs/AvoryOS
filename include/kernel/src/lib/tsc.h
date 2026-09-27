/*
 * tsc.h — TSC-based execution-time measurement helpers
 *
 * Usage (named probes, aggregated):
 *
 *   #define TSC_PROBES_ENABLE   // enable at the top of one or all files
 *   #include "lib/tsc.h"
 *
 *   TSC_BEGIN(my_section);
 *   ... code to measure ...
 *   TSC_END(my_section);
 *
 *   // At any point (e.g. after boot) dump the summary:
 *   tsc_probe_dump();
 *
 * Usage (raw, one-shot):
 *
 *   uint64_t t0 = rdtsc();
 *   ... code ...
 *   uint64_t t1 = rdtsc();
 *   uint64_t cycles = t1 - t0;
 *
 * When TSC_PROBES_ENABLE is not defined, TSC_BEGIN/TSC_END expand to nothing
 * so instrumentation can be left in production code with zero cost.
 */

#ifndef LIB_TSC_H
#define LIB_TSC_H

#include <stdint.h>

/* ── raw TSC read ─────────────────────────────────────────────────────────── */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Serialising variant — flushes the pipeline before sampling.
 * Use around microbenchmarks where OOO execution would skew results. */
static inline uint64_t rdtsc_fence(void) {
    __asm__ volatile ("lfence" ::: "memory");
    return rdtsc();
}

/* ── TSC frequency calibration ───────────────────────────────────────────── */

/* Returns approximate TSC frequency in MHz.  Returns 0 before calibration. */
uint64_t tsc_get_mhz(void);

/* Run a brief busy-wait calibration (~10 ms).  Call from kernel init.
 * tsc_get_mhz() returns 0 until this is called. */
void tsc_calibrate(void);

/* Override the cached MHz value with a measurement from a more precise source
 * (e.g. LAPIC timer, HPET, CPUID 0x15). */
void tsc_set_mhz(uint64_t mhz);

/* Convert a raw cycle delta to nanoseconds using the cached MHz value.
 * Returns 0 if tsc_calibrate() has not been called yet. */
static inline uint64_t tsc_cycles_to_ns(uint64_t cycles) {
    uint64_t mhz = tsc_get_mhz();
    if (!mhz)
        return 0;
    /* cycles / (MHz * 1e6) * 1e9  =  cycles * 1000 / MHz */
    return (cycles * 1000ULL) / mhz;
}

/* ── Named probe API ─────────────────────────────────────────────────────── */

/* Maximum number of distinct probe sites.  Increase if needed. */
#define TSC_PROBE_MAX 32

/* Probe slot — updated atomically on x86 for non-SMP "good enough" stats. */
typedef struct {
    const char *name;
    uint64_t    total_cycles;
    uint64_t    count;
    uint64_t    min_cycles;
    uint64_t    max_cycles;
} tsc_probe_t;

/* Internal: register a probe by name and return its slot index.
 * Returns -1 when the table is full. */
int  tsc_probe_register(const char *name);

/* Internal: record one sample for slot idx. */
void tsc_probe_record(int idx, uint64_t cycles);

/* Print all accumulated probe stats via klog. */
void tsc_probe_dump(void);

/* Reset all probe counters. */
void tsc_probe_reset(void);

/* ── Convenience macros ──────────────────────────────────────────────────── */

#ifdef TSC_PROBES_ENABLE

/*
 * TSC_BEGIN(tag) / TSC_END(tag)
 *
 * 'tag' must be a valid C identifier.  The probe is registered once per call
 * site (using a local static slot index) and updated on every pass.
 *
 * Example:
 *   TSC_BEGIN(elf_load);
 *   do_elf_load(...);
 *   TSC_END(elf_load);
 */
#define TSC_BEGIN(tag)                                          \
    static int _tsc_slot_##tag = -2;                           \
    if (__builtin_expect(_tsc_slot_##tag == -2, 0))            \
        _tsc_slot_##tag = tsc_probe_register(#tag);            \
    uint64_t _tsc_t0_##tag = rdtsc_fence()

#define TSC_END(tag)                                            \
    do {                                                        \
        uint64_t _tsc_t1_##tag = rdtsc_fence();                \
        if (_tsc_slot_##tag >= 0)                              \
            tsc_probe_record(_tsc_slot_##tag,                  \
                             _tsc_t1_##tag - _tsc_t0_##tag);  \
    } while (0)

/*
 * TSC_MEASURE(tag, stmt)  — single-statement shorthand.
 *
 * Example:
 *   TSC_MEASURE(vfs_read, vfs_read(file, offset, len, buf));
 */
#define TSC_MEASURE(tag, stmt)  \
    TSC_BEGIN(tag);             \
    (stmt);                     \
    TSC_END(tag)

#else  /* TSC_PROBES_ENABLE not defined — zero overhead */

#define TSC_BEGIN(tag)          ((void)0)
#define TSC_END(tag)            ((void)0)
#define TSC_MEASURE(tag, stmt)  (stmt)

#endif /* TSC_PROBES_ENABLE */

#endif /* LIB_TSC_H */
