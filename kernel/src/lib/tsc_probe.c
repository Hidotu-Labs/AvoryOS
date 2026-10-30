/*
 * tsc_probe.c — TSC calibration + named-probe accumulator
 *
 * Enable probes at individual call sites by defining TSC_PROBES_ENABLE before
 * including lib/tsc.h.  This translation unit is always compiled so that
 * tsc_calibrate() / tsc_probe_dump() are always available regardless.
 */

#include "tsc.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"

/* ── Calibration ─────────────────────────────────────────────────────────── */

static uint64_t g_tsc_mhz = 0;

/*
 * Busy-wait calibration using a simple RDTSC loop.
 *
 * We count TSC ticks over a ~10 ms spin using a fixed iteration count that
 * was chosen to take roughly 10 ms at 1 GHz.  The loop is intentionally kept
 * tight so the compiler does not optimise it away.
 *
 * For a more accurate measurement, call this after the LAPIC timer has been
 * calibrated and replace the spin with a LAPIC one-shot tick comparison.  For
 * boot-time use (where we just need an order-of-magnitude MHz figure to
 * convert cycles to ns) this is good enough.
 */
void tsc_calibrate(void) {
    /* ~10 million iterations at ~1 ns/iter ≈ 10 ms on a 1 GHz core.
     * On a modern 3–5 GHz core the loop is faster, so the window is ~2–3 ms
     * — still plenty to get a reasonable MHz estimate. */
    const uint64_t ITERS = 10000000ULL;

    uint64_t t0 = rdtsc();
    volatile uint64_t sink = 0;
    for (uint64_t i = 0; i < ITERS; i++)
        sink = i;
    uint64_t t1 = rdtsc();
    (void)sink;

    uint64_t delta_cycles = t1 - t0;

    /*
     * We don't have a wall-clock reference here, so we use a known-good
     * iteration cost.  On x86-64 with -O2 a tight counter loop costs
     * roughly 1 cycle/iter after the branch predictor warms up.
     * Therefore:  delta_cycles ≈ ITERS cycles
     * MHz = delta_cycles / (ITERS / MHz_scale)
     *
     * For a better calibration replace this with a PIT/HPET/LAPIC reference.
     * The placeholder below keeps g_tsc_mhz = delta_cycles / ITERS * 1000
     * which gives a reasonable ballpark.
     *
     * If you already calibrate the TSC elsewhere (e.g. lapic_timer.c), call
     * tsc_set_mhz() from there instead.
     */

    /* cycles per iteration × 1000 to get MHz assuming ~1 cycle/iter */
    g_tsc_mhz = (delta_cycles * 1000ULL) / ITERS;
    if (g_tsc_mhz == 0)
        g_tsc_mhz = 1; /* guard against zero division */

    klog_puts("[TSC] Calibrated: ~");
    klog_uint64(g_tsc_mhz);
    klog_puts(" MHz (loop-based estimate)\n");
}

/* Allow the LAPIC timer (or any other precise source) to override the MHz. */
void tsc_set_mhz(uint64_t mhz) {
    if (mhz)
        g_tsc_mhz = mhz;
}

uint64_t tsc_get_mhz(void) {
    return g_tsc_mhz;
}

/* ── Probe table ──────────────────────────────────────────────────────────── */

static tsc_probe_t g_probes[TSC_PROBE_MAX];
static int         g_probe_count = 0;
static spinlock_t g_probe_lock = SPINLOCK_INIT;

int tsc_probe_register(const char *name) {
    spinlock_acquire(&g_probe_lock);
    /* Linear scan — only called once per static site. */
    for (int i = 0; i < g_probe_count; i++) {
        if (g_probes[i].name == name) {
            spinlock_release(&g_probe_lock);
            return i;
        }
        /* Also compare by string content in case of separate TUs. */
        const char *a = g_probes[i].name;
        const char *b = name;
        int same = 1;
        while (*a && *b) {
            if (*a++ != *b++) { same = 0; break; }
        }
        if (same && *a == '\0' && *b == '\0') {
            spinlock_release(&g_probe_lock);
            return i;
        }
    }

    if (g_probe_count >= TSC_PROBE_MAX) {
        spinlock_release(&g_probe_lock);
        return -1;
    }

    int idx = g_probe_count++;
    g_probes[idx].name        = name;
    g_probes[idx].total_cycles = 0;
    g_probes[idx].count        = 0;
    g_probes[idx].min_cycles   = (uint64_t)-1;
    g_probes[idx].max_cycles   = 0;
    spinlock_release(&g_probe_lock);
    return idx;
}

void tsc_probe_record(int idx, uint64_t cycles) {
    if (idx < 0)
        return;
    spinlock_acquire(&g_probe_lock);
    if (idx >= g_probe_count) {
        spinlock_release(&g_probe_lock);
        return;
    }
    tsc_probe_t *p = &g_probes[idx];
    p->total_cycles += cycles;
    p->count++;
    if (cycles < p->min_cycles) p->min_cycles = cycles;
    if (cycles > p->max_cycles) p->max_cycles = cycles;
    spinlock_release(&g_probe_lock);
}

void tsc_probe_reset(void) {
    spinlock_acquire(&g_probe_lock);
    for (int i = 0; i < g_probe_count; i++) {
        g_probes[i].total_cycles = 0;
        g_probes[i].count        = 0;
        g_probes[i].min_cycles   = (uint64_t)-1;
        g_probes[i].max_cycles   = 0;
    }
    spinlock_release(&g_probe_lock);
}

/* ── Dump ─────────────────────────────────────────────────────────────────── */

/*
 * Simple insertion sort so we can print probes ordered by total_cycles
 * descending — the hottest sites appear first.
 */
static void sort_by_total(const tsc_probe_t *probes, int *order, int n) {
    for (int i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 &&
               probes[order[j]].total_cycles < probes[key].total_cycles) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

void tsc_probe_dump(void) {
    /* Snapshot while records are serialized, then drop the lock before any
     * formatting or console I/O. Each caller owns its buffers, so simultaneous
     * dumps cannot overwrite one another or mix counters from different times. */
    struct dump_buffers {
        tsc_probe_t probes[TSC_PROBE_MAX];
        int order[TSC_PROBE_MAX];
        char report[640 + TSC_PROBE_MAX * 144];
    } *buffers = kmalloc(sizeof(*buffers));
    if (!buffers) return;
    tsc_probe_t *probes = buffers->probes;
    int *order = buffers->order;
    char *report = buffers->report;
    spinlock_acquire(&g_probe_lock);
    int count = g_probe_count;
    memcpy(probes, g_probes, (size_t)count * sizeof(*probes));
    uint64_t mhz = g_tsc_mhz;
    spinlock_release(&g_probe_lock);
    if (count == 0) {
        klog_puts("[TSC] No probes recorded.\n");
        kfree(buffers);
        return;
    }

    for (int i = 0; i < count; i++)
        order[i] = i;
    sort_by_total(probes, order, count);

    /* Bounded names plus five 20-digit numbers fit in 144 bytes per row.
     * Keep the whole report in one klog write; never hold a logging lock
     * across formatting, and never emit individual fields or padding bytes. */
    size_t used = (size_t)snprintf(report, sizeof(buffers->report),
        "\n[TSC] ===== Execution Time Summary =====\n"
        "[TSC]  MHz: %llu (calibrated)\n"
        "[TSC] -------------------------------------------------------\n"
        "[TSC]  Probe                    count  total(ns)  avg(ns)  min(ns)  max(ns)\n"
        "[TSC] -------------------------------------------------------\n",
        (unsigned long long)mhz);
    for (int si = 0; si < count; si++) {
        const tsc_probe_t *p = &probes[order[si]];
        if (p->count == 0)
            continue;
        uint64_t total_ns = mhz ? p->total_cycles * 1000ULL / mhz : 0;
        uint64_t min_ns = mhz ? p->min_cycles * 1000ULL / mhz : 0;
        uint64_t max_ns = mhz ? p->max_cycles * 1000ULL / mhz : 0;
        used += (size_t)snprintf(report + used, sizeof(buffers->report) - used,
            "[TSC]  %-24.24s %llu  %llu  %llu  %llu  %llu\n", p->name,
            (unsigned long long)p->count,
            (unsigned long long)total_ns,
            (unsigned long long)(total_ns / p->count),
            (unsigned long long)min_ns,
            (unsigned long long)max_ns);
    }
    snprintf(report + used, sizeof(buffers->report) - used,
             "[TSC] ========================================\n\n");
    klog_puts(report);
    kfree(buffers);
}
