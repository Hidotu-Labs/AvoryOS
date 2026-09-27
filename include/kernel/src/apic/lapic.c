#include "lapic.h"
#include "../console/console.h"
#include "../cpu/isr.h"
#include "../mm/pmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// MMIO Base (virtual address after HHDM translation)
static volatile uint32_t *lapic_base = NULL;
static uint64_t lapic_phys_base = 0;

uint64_t lapic_get_va(void) { return (uint64_t)lapic_base; }
uint64_t lapic_get_phys(void) { return lapic_phys_base; }

// Helper: print a 32-bit hex value
static void print_hex32(uint32_t num) {
  const char *hex = "0123456789ABCDEF";
  for (int i = 28; i >= 0; i -= 4) {
    console_putchar(hex[(num >> i) & 0xF]);
  }
}

// MMIO Register Access

uint32_t lapic_read(uint32_t reg) { return lapic_base[reg / 4]; }

void lapic_write(uint32_t reg, uint32_t value) { lapic_base[reg / 4] = value; }

// EOI

void lapic_send_eoi(void) {
  if (lapic_base) {
    lapic_write(LAPIC_EOI, 0);
  }
}

bool lapic_is_ready(void) {
    return lapic_base != NULL;
}

// APIC ID

uint32_t lapic_get_id(void) { return lapic_read(LAPIC_ID) >> 24; }

// Spurious interrupt handler (must NOT send EOI)
static void spurious_handler(struct registers *regs) {
  (void)regs;
  // Spurious interrupts require no action — no EOI allowed.
}

// Initialization

void lapic_init(uint64_t base_phys) {
  // Map the LAPIC registers into virtual memory via the HHDM
  lapic_phys_base = base_phys;
  lapic_base = (volatile uint32_t *)(base_phys + pmm_get_hhdm_offset());

  console_puts("[OK] Local APIC Base: 0x");
  print_hex32((uint32_t)base_phys);
  console_puts("\n");

  // Register the spurious interrupt handler (vector 0xFF)
  register_interrupt_handler(LAPIC_SPURIOUS_VECTOR, spurious_handler);

  // Step 1: Clear the Task Priority Register
  // A TPR of 0 means we accept all interrupt priority classes.
  lapic_write(LAPIC_TPR, 0);

  // Step 2: Set the Destination Format Register to Flat Model
  lapic_write(LAPIC_DFR, 0xFFFFFFFF);

  // Step 3: Set the Logical Destination Register
  lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | 0x01000000);

  // Step 4: Enable the APIC via the Spurious Interrupt Vector Register
  // Set the spurious vector to 0xFF and flip the enable bit.
  uint32_t svr = lapic_read(LAPIC_SVR);
  svr |= LAPIC_SVR_ENABLE;
  svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
  lapic_write(LAPIC_SVR, svr);

  // Step 5: Clear any pending error status
  lapic_write(LAPIC_ESR, 0);
  lapic_read(LAPIC_ESR);

  // Step 6: Mask all LVT entries we won't use yet
  lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

  // Step 7: Clear the EOI register
  lapic_write(LAPIC_EOI, 0);

  // Report
  console_puts("     APIC ID: 0x");
  print_hex32(lapic_get_id());
  console_puts(", Version: 0x");
  print_hex32(lapic_read(LAPIC_VERSION) & 0xFF);
  console_puts("\n");
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
  if (!lapic_base) return;
  lapic_write(LAPIC_ICR_HIGH, lapic_id << 24);
  /* Fixed IPIs use edge trigger; level+deassert is not deliverable. */
  lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_FIXED | LAPIC_ICR_EDGE | vector);
}

void lapic_send_ipi_all_but_self(uint8_t vector) {
  if (!lapic_base) return;
  lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_FIXED | LAPIC_ICR_EDGE |
                               LAPIC_ICR_DEST_ALL_BUT_SELF | vector);
}
