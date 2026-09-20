#include "tsc.h"
#include "../lib/tsc.h"
#include "hal/hal.h"
#include "../drivers/timer/hpet.h"
#include "../io/io.h"
#include "../console/klog.h"

#define PIT_CMD_PORT   0x43
#define PIT_DATA_PORT0 0x40
#define PIT_BASE_FREQ  1193182
#define CALIBRATION_MS 50

static uint64_t tsc_freq_khz = 0;

static void pit_calibration_sleep_ms(uint32_t ms) {
    uint32_t divisor = (PIT_BASE_FREQ * ms) / 1000;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    // Channel 0, lobyte/hibyte access, mode 0, binary
    outb(PIT_CMD_PORT, 0x30);
    outb(PIT_DATA_PORT0, (uint8_t)(divisor & 0xFF));
    outb(PIT_DATA_PORT0, (uint8_t)((divisor >> 8) & 0xFF));

    uint16_t prev = (uint16_t)divisor;
    for (;;) {
        // Latch channel 0 counter
        outb(PIT_CMD_PORT, 0x00);
        uint8_t lo = inb(PIT_DATA_PORT0);
        uint8_t hi = inb(PIT_DATA_PORT0);
        uint16_t curr = (uint16_t)hi << 8 | lo;

        if (curr > prev || curr == 0) break;
        prev = curr;
    }
}

void tsc_init(void) {
    klog_puts("[CPU] Calibrating TSC against PIT...\n");

    uint64_t tsc_start = rdtsc();
    pit_calibration_sleep_ms(CALIBRATION_MS);
    uint64_t tsc_end = rdtsc();

    uint64_t tsc_elapsed = tsc_end - tsc_start;
    tsc_freq_khz = tsc_elapsed / CALIBRATION_MS;

    /* Feed the calibrated MHz into the probe library so tsc_cycles_to_ns()
     * works correctly throughout the rest of boot. */
    tsc_set_mhz(tsc_freq_khz / 1000);

    klog_puts("     TSC Frequency: ");
    klog_uint64(tsc_freq_khz / 1000);
    klog_puts(".");
    uint64_t mhz_frac = (tsc_freq_khz % 1000);
    if (mhz_frac < 100) klog_puts("0");
    if (mhz_frac < 10) klog_puts("0");
    klog_uint64(mhz_frac);
    klog_puts(" MHz (");
    klog_uint64(tsc_freq_khz);
    klog_puts(" kHz)\n");

    if (tsc_freq_khz < 100000) {
        klog_puts(KLOG_CLR_RED
                  "[WARN] TSC calibration is implausible; waiting for the "
                  "HPET to re-measure it." KLOG_CLR_RESET "\n");
    }
}

/* The PIT-based calibration above runs before ACPI has brought the HPET up,
 * and on some chipsets the PIT channel-0 latch read is not reliable enough
 * for timing: the measured TSC rate comes out zero or orders of magnitude
 * off.  monotonic_ms() then freezes or runs at the wrong speed, which breaks
 * every timeout, sleep and the console's deferred-swap throttle.  Once the
 * HPET is available, re-measure the TSC against its main counter. */
void tsc_recalibrate_hpet(void) {
    if (!hpet_is_available())
        return;

    uint64_t hz = hpet_get_frequency();
    if (hz < 1000)
        return;

    uint64_t wait = hz / 100; // ~10 ms
    if (wait == 0)
        wait = 1;

    uint64_t h0 = hpet_read_counter();
    uint64_t t0 = rdtsc();
    /* Bound the wait with a raw cycle cap: even a 10 GHz TSC would have to
     * run for 10 s to reach it, so a stuck HPET cannot hang boot here. */
    while ((hpet_read_counter() - h0) < wait) {
        if (rdtsc() - t0 > 100000000000ULL)
            break;
        hal_cpu_relax();
    }
    uint64_t t1 = rdtsc();
    uint64_t h1 = hpet_read_counter();

    uint64_t hpet_delta = h1 - h0;
    uint64_t tsc_delta = t1 - t0;
    if (hpet_delta == 0 || tsc_delta == 0)
        return;

    uint64_t khz = (tsc_delta * hz) / hpet_delta / 1000;
    /* Reject obviously broken ratios instead of poisoning every timer. */
    if (khz < 100000 || khz > 100000000)
        return;

    tsc_freq_khz = khz;
    tsc_set_mhz(khz / 1000);

    klog_puts("[CPU] TSC re-calibrated against HPET: ");
    klog_uint64(khz / 1000);
    klog_puts(".");
    uint64_t frac = khz % 1000;
    if (frac < 100) klog_puts("0");
    if (frac < 10) klog_puts("0");
    klog_uint64(frac);
    klog_puts(" MHz\n");
}

uint64_t tsc_get_freq_khz(void) {
    return tsc_freq_khz;
}
