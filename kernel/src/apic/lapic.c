#include "lapic.h"
#include "../console/console.h"
#include "../cpu/isr.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// MMIO Base (virtual address after HHDM translation)
static volatile uint32_t *lapic_base = NULL;
static uint64_t lapic_phys_base = 0;

// Set when the LAPIC is in x2APIC mode and must be driven through MSRs.
// lapic_init() detects the mode the firmware left the LAPIC in; the same
// accessors below then serve IPIs, EOI and the timer in both xAPIC (MMIO)
// and x2APIC (MSR) modes.
static bool lapic_x2apic = false;

#define MSR_IA32_APIC_BASE 0x1B
#define APIC_BASE_X2APIC_ENABLE (1ULL << 10)
#define X2APIC_MSR_BASE 0x800u

static inline uint64_t lapic_rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static inline void lapic_wrmsr(uint32_t msr, uint64_t value) {
  uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
  uint32_t hi = (uint32_t)(value >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

// x2APIC exposes each LAPIC register as an MSR at 0x800 + (MMIO offset / 16).
static inline uint32_t x2apic_msr(uint32_t reg) {
  return X2APIC_MSR_BASE + (reg >> 4);
}

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

uint32_t lapic_read(uint32_t reg) {
  if (lapic_x2apic)
    return (uint32_t)lapic_rdmsr(x2apic_msr(reg));
  return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
  if (lapic_x2apic) {
    lapic_wrmsr(x2apic_msr(reg), value);
    return;
  }
  lapic_base[reg / 4] = value;
}

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

uint32_t lapic_get_id(void) {
  uint32_t id = lapic_read(LAPIC_ID);
  // The xAPIC ID register keeps the ID in bits 31:24; the x2APIC MSR holds it
  // in bits 31:0.
  return lapic_x2apic ? id : (id >> 24);
}

bool lapic_is_x2apic(void) { return lapic_x2apic; }

// Spurious interrupt handler (must NOT send EOI)
static void spurious_handler(struct registers *regs) {
  (void)regs;
  // Spurious interrupts require no action — no EOI allowed.
}

// Initialization

void lapic_init(uint64_t base_phys) {
  /* ACPI reports 0 in the MADT Local APIC Address field on some x2APIC-only
   * firmware; the architectural default is still 0xFEE00000.  Without this,
   * the boot log prints "Local APIC Base: 0x00000000" and xAPIC MMIO (if the
   * firmware happened to stay in xAPIC mode) would talk to physical 0. */
  if (base_phys == 0)
    base_phys = 0xFEE00000ULL;

  // Map the LAPIC registers into virtual memory via the HHDM
  lapic_phys_base = base_phys;
  lapic_base = (volatile uint32_t *)(base_phys + pmm_get_hhdm_offset());

  /* Firmware can leave the LAPIC in x2APIC mode, where the MMIO interface
   * this kernel uses everywhere else is not decoded at all.  In that mode
   * every "IPI" write below would silently go nowhere and AP bring-up would
   * spin forever.  Stay in x2APIC (clearing the enable bit is rejected by
   * some hypervisors and would strand APIC IDs wider than 8 bits) and drive
   * the registers through their MSR interface instead; the accessors below
   * dispatch transparently. */
  uint64_t apic_base = lapic_rdmsr(MSR_IA32_APIC_BASE);
  if (apic_base & APIC_BASE_X2APIC_ENABLE) {
    lapic_x2apic = true;
    console_puts("[INFO] LAPIC is in x2APIC mode; using MSR access.\n");
  }

  console_puts("[OK] Local APIC Base: 0x");
  print_hex32((uint32_t)base_phys);
  console_puts("\n");

  // Register the spurious interrupt handler (vector 0xFF)
  register_interrupt_handler(LAPIC_SPURIOUS_VECTOR, spurious_handler);

  // Step 1: Clear the Task Priority Register
  // A TPR of 0 means we accept all interrupt priority classes.
  lapic_write(LAPIC_TPR, 0);

  // Step 2: Set the Destination Format Register to Flat Model.
  // DFR/LDR have no useful meaning in x2APIC (physical destination mode is
  // used throughout, and some implementations reject writes to them there).
  if (!lapic_x2apic) {
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);

    // Step 3: Set the Logical Destination Register
    lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | 0x01000000);
  }

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

void lapic_write_icr(uint32_t apic_id, uint32_t icr_low) {
  if (lapic_x2apic) {
    // x2APIC: one 64-bit MSR write; destination ID lives in bits 63:32.
    lapic_wrmsr(x2apic_msr(LAPIC_ICR_LOW),
                ((uint64_t)apic_id << 32) | (uint64_t)icr_low);
    return;
  }
  lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
  lapic_write(LAPIC_ICR_LOW, icr_low);
}

uint32_t lapic_read_icr(void) {
  if (lapic_x2apic)
    return (uint32_t)lapic_rdmsr(x2apic_msr(LAPIC_ICR_LOW));
  return lapic_read(LAPIC_ICR_LOW);
}

bool lapic_icr_idle(void) { return (lapic_read_icr() & LAPIC_ICR_PENDING) == 0; }

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
  if (!lapic_base) return;
  /* Fixed IPIs use edge trigger; level+deassert is not deliverable. */
  lapic_write_icr(lapic_id, LAPIC_ICR_FIXED | LAPIC_ICR_EDGE | vector);
}

void lapic_send_ipi_all_but_self(uint8_t vector) {
  if (!lapic_base) return;
  uint32_t icr_low =
      LAPIC_ICR_FIXED | LAPIC_ICR_EDGE | LAPIC_ICR_DEST_ALL_BUT_SELF | vector;
  if (lapic_x2apic) {
    lapic_wrmsr(x2apic_msr(LAPIC_ICR_LOW), icr_low);
    return;
  }
  lapic_write(LAPIC_ICR_LOW, icr_low);
}

/* A panic-stop NMI reaches one online CPU even when it has interrupts
 * disabled or is stuck in a lock holder. Keep the wait bounded: panic
 * reporting must not turn a busy LAPIC into an unbounded secondary failure. */
void lapic_send_nmi(uint32_t apic_id) {
  if (!lapic_base)
    return;

  /* xAPIC delivery uses the MMIO window, which user address spaces do not
   * always map. Borrow the permanent kernel CR3 only for the ICR write, then
   * restore the crashing task's CR3 so its fault report remains accurate. */
  uint64_t saved_cr3 = 0;
  bool switched_cr3 = false;
  if (!lapic_x2apic) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    uint64_t kernel_cr3 =
        (uint64_t)(uintptr_t)vmm_get_kernel_pml4() & PAGE_MASK;
    if (kernel_cr3 && (saved_cr3 & PAGE_MASK) != kernel_cr3) {
      __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
      switched_cr3 = true;
    }
  }

  for (uint32_t spins = 0; spins < 100000; spins++) {
    if (lapic_icr_idle())
      break;
    __asm__ volatile("pause" ::: "memory");
  }
  uint32_t icr_low = LAPIC_ICR_NMI | LAPIC_ICR_EDGE;
  lapic_write_icr(apic_id, icr_low);
  if (switched_cr3)
    __asm__ volatile("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
}
