#ifndef APIC_LAPIC_H
#define APIC_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

// Local APIC Register Offsets
#define LAPIC_ID 0x020            // Local APIC ID
#define LAPIC_VERSION 0x030       // Local APIC Version
#define LAPIC_TPR 0x080           // Task Priority Register
#define LAPIC_APR 0x090           // Arbitration Priority Register
#define LAPIC_PPR 0x0A0           // Processor Priority Register
#define LAPIC_EOI 0x0B0           // End of Interrupt
#define LAPIC_RRD 0x0C0           // Remote Read Register
#define LAPIC_LDR 0x0D0           // Logical Destination Register
#define LAPIC_DFR 0x0E0           // Destination Format Register
#define LAPIC_SVR 0x0F0           // Spurious Interrupt Vector Register
#define LAPIC_ISR_BASE 0x100      // In-Service Register (8 x 32-bit)
#define LAPIC_TMR_BASE 0x180      // Trigger Mode Register (8 x 32-bit)
#define LAPIC_IRR_BASE 0x200      // Interrupt Request Register (8 x 32-bit)
#define LAPIC_ESR 0x280           // Error Status Register
#define LAPIC_ICR_LOW 0x300       // Interrupt Command Register (low 32-bit)
#define LAPIC_ICR_HIGH 0x310      // Interrupt Command Register (high 32-bit)
#define LAPIC_LVT_TIMER 0x320     // LVT Timer Register
#define LAPIC_LVT_THERMAL 0x330   // LVT Thermal Sensor Register
#define LAPIC_LVT_PERF 0x340      // LVT Performance Counter Register
#define LAPIC_LVT_LINT0 0x350     // LVT LINT0 Register
#define LAPIC_LVT_LINT1 0x360     // LVT LINT1 Register
#define LAPIC_LVT_ERROR 0x370     // LVT Error Register
#define LAPIC_TIMER_INIT 0x380    // Timer Initial Count Register
#define LAPIC_TIMER_CURRENT 0x390 // Timer Current Count Register
#define LAPIC_TIMER_DIV 0x3E0     // Timer Divide Configuration Register

// SVR Flags
#define LAPIC_SVR_ENABLE (1 << 8)

// LVT Mask Bit
#define LAPIC_LVT_MASKED (1 << 16)

// Spurious Interrupt Vector
#define LAPIC_SPURIOUS_VECTOR 0xFF
#define IPI_VECTOR_RESCHEDULE 49

// ICR Delivery Modes
#define LAPIC_ICR_FIXED (0 << 8)
#define LAPIC_ICR_SMI (2 << 8)
#define LAPIC_ICR_NMI (4 << 8)
#define LAPIC_ICR_INIT (5 << 8)
#define LAPIC_ICR_STARTUP (6 << 8)
#define LAPIC_ICR_ASSERT (1 << 14) // Level field: assert
#define LAPIC_ICR_DEASSERT (0 << 14)
#define LAPIC_ICR_LEVEL (1 << 15)  // Trigger mode: level
#define LAPIC_ICR_EDGE (0 << 15)
#define LAPIC_ICR_PENDING (1 << 12)

// Timer Modes
#define LAPIC_TIMER_ONESHOT 0x00
#define LAPIC_TIMER_PERIODIC 0x20000
#define LAPIC_TIMER_TSC 0x40000

// Timer Divisors
#define LAPIC_TIMER_DIV_1 0x0B
#define LAPIC_TIMER_DIV_2 0x00
#define LAPIC_TIMER_DIV_4 0x01
#define LAPIC_TIMER_DIV_8 0x02
#define LAPIC_TIMER_DIV_16 0x03
#define LAPIC_TIMER_DIV_32 0x08
#define LAPIC_TIMER_DIV_64 0x09
#define LAPIC_TIMER_DIV_128 0x0A

// Public API

// Initialize the Local APIC using the given physical base address from the
// MADT.
void lapic_init(uint64_t base_phys);

// Signal End-of-Interrupt to the Local APIC.
void lapic_send_eoi(void);

// Raw MMIO read/write for the LAPIC register space.
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// Returns true if the LAPIC MMIO is mapped and ready for use.
bool lapic_is_ready(void);

// Returns the BSP's APIC ID.
uint32_t lapic_get_id(void);

// Bring-up diagnostics: the LAPIC MMIO as seen through the HHDM (0 when the
// LAPIC was never initialized) and its physical base.
uint64_t lapic_get_va(void);
uint64_t lapic_get_phys(void);

// IPI support
#define LAPIC_ICR_DEST_SELF        (1 << 18)
#define LAPIC_ICR_DEST_ALL         (2 << 18)
#define LAPIC_ICR_DEST_ALL_BUT_SELF (3 << 18)

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);
void lapic_send_ipi_all_but_self(uint8_t vector);

#endif
