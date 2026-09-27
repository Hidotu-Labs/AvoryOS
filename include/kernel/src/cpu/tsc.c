#include "tsc.h"
#include "../lib/tsc.h"
#include "hal/hal.h"
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
}

uint64_t tsc_get_freq_khz(void) {
    return tsc_freq_khz;
}
