#ifndef APIC_LAPIC_TIMER_H
#define APIC_LAPIC_TIMER_H

#include <stdint.h>

// LAPIC Timer Configuration
// The timer fires at this vector.  Must not collide with existing IRQ vectors
// (32-47) or the spurious vector (255).
#define LAPIC_TIMER_VECTOR 48

// Runnable-thread quantum. The LAPIC itself operates in one-shot mode.
#define LAPIC_SCHED_QUANTUM_MS 2

// Public API

// Calibrate the LAPIC timer against the PIT, then start it in one-shot mode.
// Must be called after lapic_init() and with interrupts enabled.
void lapic_timer_init(void);

// Initialize the LAPIC timer on an AP.
void lapic_timer_init_ap(void);

// Returns total ticks since the LAPIC timer was started.
uint64_t lapic_timer_get_ticks(void);

// Returns uptime in milliseconds.
uint64_t lapic_timer_get_ms(void);

// Returns uptime in nanoseconds (TSC-based, no rounding).
uint64_t lapic_timer_get_ns(void);

// Returns the TSC value recorded at boot calibration.
uint64_t lapic_timer_get_boot_tsc(void);


// Program the current CPU's one-shot timer with an absolute deadline.
void lapic_timer_arm_at(uint64_t deadline_ms);
void lapic_timer_rearm_if_earlier(uint64_t deadline_ms);

// Sleep for approximately `ms` milliseconds using the LAPIC timer.
void lapic_timer_sleep(uint32_t ms);

#endif
