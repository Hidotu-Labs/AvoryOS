#ifndef APIC_IOAPIC_H
#define APIC_IOAPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdbool.h>

// I/O APIC Register Indices (accessed indirectly via IOREGSEL / IOWIN)
#define IOAPIC_REG_ID       0x00    // I/O APIC ID
#define IOAPIC_REG_VER      0x01    // I/O APIC Version + Max Redirection Entry
#define IOAPIC_REG_ARB      0x02    // I/O APIC Arbitration ID
#define IOAPIC_REG_REDTBL   0x10    // Redirection Table (base; 2 regs per entry)

// Redirection Entry Flags
#define IOAPIC_REDIR_MASKED     (1ULL << 16)
#define IOAPIC_REDIR_LEVEL      (1ULL << 15)   // Level-triggered
#define IOAPIC_REDIR_ACTIVELOW  (1ULL << 13)   // Active-low polarity
#define IOAPIC_REDIR_LOGICAL    (1ULL << 11)   // Logical destination mode

// Public API

// Initialize the I/O APIC at the given physical base with a Global System
// Interrupt base offset.  All redirection entries are masked on init.
void ioapic_init(uint64_t base_phys, uint32_t gsi_base);

// Configure a redirection entry to route a GSI (Global System Interrupt) to
// the given interrupt vector on the specified destination APIC.
// `flags` encodes MADT ISO polarity/trigger bits (0 = ISA defaults).
void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t dest_apic_id,
                      uint16_t flags);

// Mask / unmask a single GSI pin on the I/O APIC.
void ioapic_mask_irq(uint8_t gsi);
void ioapic_unmask_irq(uint8_t gsi);
bool ioapic_is_ready(void);

#endif
