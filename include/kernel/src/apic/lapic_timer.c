#include "lapic_timer.h"
#include "hal/hal.h"
#include "lapic.h"
#include "../cpu/isr.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/tsc.h"
#include "../lib/tsc.h"
#include "../drivers/usb/xhci.h"
#include "../io/io.h"
#include "../lock/lockdiag.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../mm/vmm.h"
#include "../drivers/serial.h"


// PIT constants for calibration
#define PIT_CMD_PORT   0x43
#define PIT_DATA_PORT0 0x40
#define PIT_BASE_FREQ  1193182  // PIT oscillator frequency in Hz
#define CALIBRATION_MS 10       // How long to measure (10 ms)

// State
static uint32_t          ticks_per_ms      = 0;   // LAPIC decrements per ms
static uint64_t          boot_tsc          = 0;

static uint64_t monotonic_ms(void) {
    uint64_t khz = tsc_get_freq_khz();
    if (khz == 0) return 0;
    return (rdtsc() - boot_tsc) / khz;
}

static uint64_t monotonic_ns(void) {
    uint64_t khz = tsc_get_freq_khz();
    if (khz == 0) return 0;
    uint64_t elapsed = rdtsc() - boot_tsc;
    /* Split into whole milliseconds + remainder to avoid overflow.
     * khz = cycles/ms, so elapsed/khz = ms, remainder < khz.
     * rem * 1,000,000 fits in uint64 since rem < ~4M (typ. CPU freq). */
    uint64_t ms  = elapsed / khz;
    uint64_t rem = elapsed % khz;
    return ms * 1000000ULL + rem * 1000000ULL / khz;
}

static uint32_t deadline_to_initial_count(uint64_t deadline_ms) {
    uint64_t now = monotonic_ms();
    uint64_t delay_ms = deadline_ms > now ? deadline_ms - now : 1;
    uint64_t max_ms = 0xffffffffULL / ticks_per_ms;
    if (delay_ms > max_ms) delay_ms = max_ms;
    uint64_t count = delay_ms * ticks_per_ms;
    return (uint32_t)(count ? count : 1);
}

// Helpers
static void print_hex32(uint32_t num) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        klog_putchar(hex[(num >> i) & 0xF]);
    }
}

// Timer ISR
void lapic_timer_handler(struct registers *regs) {
    struct cpu_info *cpu = cpu_get_current();
    if (cpu) cpu->timer_deadline_ms = 0;
    extern void timerfd_tick(void);
    timerfd_tick();
    if (cpu == cpu_get_bsp()) {
        extern void watchdog_tick(void);
        watchdog_tick();
        xhci_msix_watchdog();
        serial_flush();
    }

    // Send EOI BEFORE context switch. This is a special case - normally
    // isr_handler sends EOI after the handler returns. But the scheduler
    // may switch to another thread, and we need to keep receiving timer
    // interrupts. Double EOI (here + in isr_handler) is harmless.
    lapic_write(LAPIC_EOI, 0);

    /* Hang detector.  Runs after EOI so a report cannot wedge the local
     * LAPIC in-service bit, and before the scheduler because the report wants
     * to describe the machine exactly as it is. */
    lockdiag_tick();

    // Call the scheduler. Every core handles its own preemption.
    // Safety check: only yield if we have a valid cpu structure and a thread to switch from.
    if (cpu && cpu->current_thread) {
        sched_tick(regs);
    }
}

// PIT polling sleep for calibration
// Uses the PIT counter-latch to busy-wait for a known duration.
// We program the PIT in mode 0 (one-shot) and poll the 16-bit counter until
// we detect it has wrapped (current > previous means the countdown finished).
static void pit_calibration_sleep_ms(uint32_t ms) {
    uint32_t divisor = (PIT_BASE_FREQ * ms) / 1000;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    // Channel 0, lobyte/hibyte access, mode 0, binary
    outb(PIT_CMD_PORT, 0x30);
    outb(PIT_DATA_PORT0, (uint8_t)(divisor & 0xFF));
    outb(PIT_DATA_PORT0, (uint8_t)((divisor >> 8) & 0xFF));

    // Poll: latch counter 0 and read 16-bit value.
    // In mode 0, counter counts from `divisor` down to 0 and then stops.
    // We detect completion when the counter has reached a very low value.
    uint16_t prev = (uint16_t)divisor;
    for (;;) {
        // Latch channel 0 counter (command byte: channel 0, latch count)
        outb(PIT_CMD_PORT, 0x00);
        uint8_t lo = inb(PIT_DATA_PORT0);
        uint8_t hi = inb(PIT_DATA_PORT0);
        uint16_t curr = (uint16_t)hi << 8 | lo;

        // If counter wrapped around (curr > prev) or reached 0, we're done
        if (curr > prev || curr == 0) break;
        prev = curr;
    }
}

// Calibration
// We program the LAPIC timer to count down from 0xFFFFFFFF and measure how
// many decrements occur during a known PIT-based delay.
static uint32_t calibrate_lapic_timer(void) {
    // Set divider to 16 for a reasonable range
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Start the counter at max, one-shot (masked so it doesn't fire)
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // Wait a known duration using the PIT
    pit_calibration_sleep_ms(CALIBRATION_MS);

    // Stop the LAPIC timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);

    // ticks_per_ms = elapsed / CALIBRATION_MS
    return elapsed / CALIBRATION_MS;
}

// Public API

void lapic_timer_init(void) {
    klog_puts("[INFO] Calibrating LAPIC timer against PIT...\n");

    ticks_per_ms = calibrate_lapic_timer();
    boot_tsc = rdtsc();

    klog_puts("     LAPIC ticks/ms: 0x");
    print_hex32(ticks_per_ms);
    klog_puts(" (");
    klog_uint64(ticks_per_ms);
    klog_puts(")\n");

    if (ticks_per_ms == 0) {
        klog_puts("[ERR] LAPIC timer calibration failed (0 ticks/ms). Aborting.\n");
        return;
    }

    // Register ISR for our timer vector
    register_interrupt_handler(LAPIC_TIMER_VECTOR, lapic_timer_handler);

    // One-shot mode; scheduler and timer users arm the earliest deadline.
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
    klog_puts("[OK] LAPIC Timer started: one-shot (vector ");
    klog_uint64(LAPIC_TIMER_VECTOR);
    klog_puts(")\n");
    lapic_timer_arm_at(monotonic_ms() + LAPIC_SCHED_QUANTUM_MS);
    vmm_update_vdso_data();
}


void lapic_timer_init_ap(void) {
    if (ticks_per_ms == 0) return;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
    lapic_timer_arm_at(monotonic_ms() + LAPIC_SCHED_QUANTUM_MS);
}

uint64_t lapic_timer_get_ticks(void) {
    return monotonic_ms();
}

uint64_t lapic_timer_get_ms(void) {
    return monotonic_ms();
}

uint64_t lapic_timer_get_ns(void) {
    return monotonic_ns();
}

uint64_t lapic_timer_get_boot_tsc(void) {
    return boot_tsc;
}


void lapic_timer_arm_at(uint64_t deadline_ms) {
    if (!ticks_per_ms || !lapic_is_ready()) return;
    struct cpu_info *cpu = cpu_get_current();
    if (cpu) cpu->timer_deadline_ms = deadline_ms;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, deadline_to_initial_count(deadline_ms));
}

void lapic_timer_rearm_if_earlier(uint64_t deadline_ms) {
    struct cpu_info *cpu = cpu_get_current();
    if (!cpu || cpu->timer_deadline_ms == 0 ||
        deadline_ms < cpu->timer_deadline_ms)
        lapic_timer_arm_at(deadline_ms);
}

void lapic_timer_sleep(uint32_t ms) {
    uint64_t target = monotonic_ms() + ms;
    while (monotonic_ms() < target) {
        lapic_timer_rearm_if_earlier(target);
        hal_cpu_halt();
    }
}
