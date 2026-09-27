#ifndef CPU_IRQ_H
#define CPU_IRQ_H

#include "isr.h"
#include <stdint.h>

/**
 * @brief Register a handler for a legacy IRQ (0-15).
 *
 * This function automates:
 * 1. Finding the correct Global System Interrupt (GSI) via ACPI overrides.
 * 2. Mapping the GSI to a CPU interrupt vector (starting at 32).
 * 3. Configuring the I/O APIC to route the GSI to the current CPU.
 * 4. Registering the ISR function in the dispatcher.
 *
 * @param irq_no The legacy IRQ number (0=PIT, 1=KBD, 4=COM1, 12=MOUSE, etc.)
 * @param handler The function to call when the interrupt occurs.
 * @param flags IOAPIC flags (0=Auto/ISA defaults, 0x000F=PCI Level/Active-Low)
 * @return true if successful, false otherwise.
 */
bool irq_install_handler(uint8_t irq_no, isr_t handler, uint16_t flags);

/**
 * @brief Remove a handler from an IRQ's handler chain.
 *
 * This function safely removes a handler from the linked list,
 * freeing the node memory. If no handlers remain for the IRQ,
 * the IRQ is marked as unregistered.
 *
 * @param irq_no The IRQ number (0-223).
 * @param handler The handler function to remove.
 * @return true if handler was found and removed, false otherwise.
 */
bool irq_uninstall_handler(uint8_t irq_no, isr_t handler);

void irq_manager_sync(void);
void irq_register_stats_dev(void);

// Hardware helpers (from apic/)
uint32_t lapic_get_id(void);

/**
 * @brief Testing function to verify the interrupt manager.
 * Triggers a software interrupt for a registered IRQ.
 */
void irq_test_trigger(uint8_t irq_no);

#endif
