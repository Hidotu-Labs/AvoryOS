#include "lapic_timer.h"
#include "hal/hal.h"
#include "lapic.h"
#include "../console/debug.h"
#include "../cpu/isr.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../cpu/msr.h"
#include "../cpu/tsc.h"
#include "../drivers/timer/hpet.h"
#include "../drivers/timer/rtc.h"
#include "../lib/string.h"
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

#define IA32_TSC_DEADLINE 0x6E0U

// State
static uint32_t          ticks_per_ms      = 0;   // LAPIC decrements per ms
static bool              use_tsc_deadline  = false;
static uint64_t          boot_tsc          = 0;
static uint64_t          boot_mono_ms      = 0;
static uint64_t          boot_mono_ns      = 0;
static bool              s3_timer_suspended = false;
static uint64_t          s3_saved_mono_ms  = 0;
static uint64_t          s3_saved_mono_ns  = 0;
static uint64_t          s3_saved_rtc       = 0;

static uint64_t monotonic_ms(void) {
    uint64_t khz = tsc_get_freq_khz();
    if (khz == 0) return 0;
    uint64_t tsc = rdtsc();
    uint64_t elapsed = tsc >= boot_tsc ? tsc - boot_tsc : 0;
    return boot_mono_ms + elapsed / khz;
}

static uint64_t monotonic_ns(void) {
    uint64_t khz = tsc_get_freq_khz();
    if (khz == 0) return 0;
    uint64_t tsc = rdtsc();
    uint64_t elapsed = tsc >= boot_tsc ? tsc - boot_tsc : 0;
    /* Split into whole milliseconds + remainder to avoid overflow.
     * khz = cycles/ms, so elapsed/khz = ms, remainder < khz.
     * rem * 1,000,000 fits in uint64 since rem < ~4M (typ. CPU freq). */
    uint64_t ms  = elapsed / khz;
    uint64_t rem = elapsed % khz;
    return boot_mono_ns + ms * 1000000ULL + rem * 1000000ULL / khz;
}

static uint32_t deadline_to_initial_count(uint64_t deadline_ms) {
    /* The deadline is absolute. Subtract the precise current time before
     * converting to LAPIC ticks; subtracting floor(now_ms) adds up to a
     * millisecond to every arm and can repeatedly postpone short requests. */
    uint64_t now_ns = monotonic_ns();
    __uint128_t target_ns = (__uint128_t)deadline_ms * 1000000ULL;
    if (target_ns <= now_ns || !ticks_per_ms)
        return 1;
    __uint128_t count = ((target_ns - now_ns) * ticks_per_ms + 999999ULL) / 1000000ULL;
    if (count > 0xffffffffULL)
        return 0xffffffffU;
    return (uint32_t)(count ? count : 1);
}

// Helpers

// Timer ISR
void lapic_timer_handler(struct registers *regs) {
    /* First-tick breadcrumb: this is the first time the timer ISR has ever
     * run on a machine booted with a broken PIT calibration, and if the box
     * wedges in here the on-screen log has to say the tick was reached. */
    static bool first_tick_reported;
    if (!first_tick_reported) {
        first_tick_reported = true;
        struct cpu_info *c = cpu_get_current();
        DBG("[TMR] first LAPIC tick on CPU %u\n", c ? c->cpu_id : 0U);
    }

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

    /* LinuxKPI time base: advance jiffies.  Defined by the imported LinuxKPI
     * layer (linuxkpi/src/time.c); absent when the Linux tree is not
     * imported, hence the guarded weak reference. */
    extern void linuxkpi_timer_tick(void) __attribute__((weak));
    if (linuxkpi_timer_tick)
        linuxkpi_timer_tick();

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
    uint32_t current = lapic_read(LAPIC_TIMER_CURRENT);
    uint32_t elapsed = 0xFFFFFFFF - current;

    DBG("[TMR] PIT reference: lapic current=0x%x elapsed=%u over %u ms\n",
        current, elapsed, CALIBRATION_MS);

    // ticks_per_ms = elapsed / CALIBRATION_MS
    return elapsed / CALIBRATION_MS;
}

// HPET-referenced calibration, used when the PIT latch measurement is
// unusable (some chipsets do not make channel 0 readable).  The HPET main
// counter is a free-running clock this kernel already uses for its backup
// timer, and hpet_init() has run by the time the LAPIC timer is calibrated.
static uint32_t calibrate_lapic_timer_hpet(void) {
    if (!hpet_is_available())
        return 0;

    uint64_t hz = hpet_get_frequency();
    if (hz < 1000)
        return 0;

    uint64_t wait = hz / 100; // ~10 ms
    if (wait == 0)
        wait = 1;

    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint64_t h0 = hpet_read_counter();
    uint64_t guard = rdtsc();
    while ((hpet_read_counter() - h0) < wait) {
        if (rdtsc() - guard > 100000000000ULL)
            break;
        hal_cpu_relax();
    }

    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    uint32_t current = lapic_read(LAPIC_TIMER_CURRENT);
    uint32_t elapsed = 0xFFFFFFFF - current;

    DBG("[TMR] HPET reference: lapic current=0x%x elapsed=%u over %u ms\n",
        current, elapsed, CALIBRATION_MS);

    return elapsed / CALIBRATION_MS;
}

// Public API

// A plausible LAPIC timer rate.  Anything outside this range means the
// reference wait itself was broken (a PIT latch read that returns instantly
// yields a tiny nonzero rate, which arms the next one-shot for a few
// microseconds and turns the first tick into an interrupt storm), not a real
// APIC clock.
#define LAPIC_TICKS_PER_MS_MIN 1000U
#define LAPIC_TICKS_PER_MS_MAX 50000000U

static bool lapic_rate_plausible(uint32_t ticks) {
    return ticks >= LAPIC_TICKS_PER_MS_MIN && ticks <= LAPIC_TICKS_PER_MS_MAX;
}

void lapic_timer_init(void) {
    klog_puts("[INFO] Calibrating LAPIC timer...\n");

    /* Prefer the HPET: it is a free-running clock with a well-defined rate,
     * while the PIT channel-0 latch read is unreliable on some chipsets.
     * Fall back to the PIT only when the HPET result is implausible. */
    uint32_t hpet_ticks = calibrate_lapic_timer_hpet();
    uint32_t pit_ticks = 0;
    const char *ref = NULL;

    if (lapic_rate_plausible(hpet_ticks)) {
        ticks_per_ms = hpet_ticks;
        ref = "HPET";
    } else {
        pit_ticks = calibrate_lapic_timer();
        if (lapic_rate_plausible(pit_ticks)) {
            ticks_per_ms = pit_ticks;
            ref = "PIT";
        } else {
            ticks_per_ms = 0;
        }
    }

    boot_tsc = rdtsc();
    boot_mono_ms = 0;
    boot_mono_ns = 0;

    /* One complete line, so the value can never be split across two frames
     * on screen (the "LAPIC ticks/ms: 0x" with no digits symptom). */
    {
        char line[112];
        int n;
        if (ref) {
            n = snprintf(line, sizeof(line),
                         "     LAPIC ticks/ms: 0x%08X (%u), reference %s\n",
                         ticks_per_ms, ticks_per_ms, ref);
        } else {
            n = snprintf(line, sizeof(line),
                         "[ERR] LAPIC calibration implausible "
                         "(HPET=%u, PIT=%u ticks/ms); timer left disabled\n",
                         hpet_ticks, pit_ticks);
        }
        if (n > 0)
            klog_puts(line);
    }

    if (!ref)
        return;

    // Register ISR for our timer vector
    register_interrupt_handler(LAPIC_TIMER_VECTOR, lapic_timer_handler);

    if (cpu_has_tsc_deadline() && tsc_get_freq_khz() > 0) {
        use_tsc_deadline = true;
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_TSC | LAPIC_TIMER_VECTOR);
        __asm__ volatile("mfence" ::: "memory");
        klog_puts("[OK] LAPIC Timer started: TSC-deadline (vector ");
    } else {
        // One-shot mode; scheduler and timer users arm the earliest deadline.
        lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
        klog_puts("[OK] LAPIC Timer started: one-shot (vector ");
    }
    klog_uint64(LAPIC_TIMER_VECTOR);
    klog_puts(")\n");
    lapic_timer_arm_at(monotonic_ms() + LAPIC_SCHED_QUANTUM_MS);
    DBG("[TMR] timer armed: ticks/ms=%u\n", ticks_per_ms);
    vmm_update_vdso_data();
    DBG("[TMR] vdso data updated\n");
}


void lapic_timer_init_ap(void) {
    if (!use_tsc_deadline && ticks_per_ms == 0) return;
    if (use_tsc_deadline) {
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_TSC | LAPIC_TIMER_VECTOR);
        __asm__ volatile("mfence" ::: "memory");
    } else {
        lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
    }
    lapic_timer_arm_at(monotonic_ms() + LAPIC_SCHED_QUANTUM_MS);
}

void lapic_timer_save_s4_state(void) {
    if (s3_timer_suspended)
        return;
    s3_saved_mono_ms = monotonic_ms();
    s3_saved_mono_ns = monotonic_ns();
    s3_saved_rtc = rtc_get_timestamp();
    s3_timer_suspended = true;
}

void lapic_timer_suspend_for_s3(void) {
    lapic_timer_save_s4_state();
    struct cpu_info *cpu = cpu_get_current();
    if (cpu)
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
}

void lapic_timer_resume_from_s3(bool slept) {
    if (!s3_timer_suspended)
        return;

    if (slept) {
        uint64_t rtc_now = rtc_get_timestamp();
        uint64_t elapsed_ms = rtc_now >= s3_saved_rtc
                                  ? (rtc_now - s3_saved_rtc) * 1000ULL
                                  : 0;
        boot_mono_ms = s3_saved_mono_ms + elapsed_ms;
        boot_mono_ns = s3_saved_mono_ns + elapsed_ms * 1000000ULL;
        boot_tsc = rdtsc();

        if (hpet_is_available()) {
            hpet_enable(true);
            tsc_recalibrate_hpet();
            uint32_t hpet_ticks = calibrate_lapic_timer_hpet();
            if (lapic_rate_plausible(hpet_ticks))
                ticks_per_ms = hpet_ticks;
        }
        if (!lapic_rate_plausible(ticks_per_ms)) {
            uint32_t pit_ticks = calibrate_lapic_timer();
            if (lapic_rate_plausible(pit_ticks))
                ticks_per_ms = pit_ticks;
        }
        vmm_update_vdso_data();
    }

    s3_timer_suspended = false;
    if ((!use_tsc_deadline && !ticks_per_ms) || !lapic_is_ready())
        return;
    if (use_tsc_deadline) {
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_TSC | LAPIC_TIMER_VECTOR);
        __asm__ volatile("mfence" ::: "memory");
    } else {
        lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_ONESHOT | LAPIC_TIMER_VECTOR);
    }
    struct cpu_info *cpu = cpu_get_current();
    uint64_t deadline = cpu ? cpu->timer_deadline_ms : 0;
    if (!deadline)
        deadline = monotonic_ms() + LAPIC_SCHED_QUANTUM_MS;
    lapic_timer_arm_at(deadline);
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
    if ((!use_tsc_deadline && !ticks_per_ms) || !lapic_is_ready()) return;
    struct cpu_info *cpu = cpu_get_current();
    if (cpu) cpu->timer_deadline_ms = deadline_ms;
    if (use_tsc_deadline) {
        uint64_t khz = tsc_get_freq_khz();
        uint64_t target_tsc = (deadline_ms > boot_mono_ms)
                                  ? (boot_tsc + (deadline_ms - boot_mono_ms) * khz)
                                  : boot_tsc;
        if (target_tsc == 0)
            target_tsc = 1;
        wrmsr(IA32_TSC_DEADLINE, target_tsc);
        return;
    }
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
    /* If the LAPIC timer never calibrated (or the TSC timebase is missing),
     * the halt loop below would never see monotonic_ms() advance and would
     * sleep forever.  Fall back to a bounded busy-wait in that case. */
    if (tsc_get_freq_khz() == 0 || ticks_per_ms == 0) {
        for (uint64_t i = 0; i < (uint64_t)ms * 100000; i++)
            hal_cpu_relax();
        return;
    }

    uint64_t target = monotonic_ms() + ms;
    while (monotonic_ms() < target) {
        lapic_timer_rearm_if_earlier(target);
        hal_cpu_halt();
    }
}
