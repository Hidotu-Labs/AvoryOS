#include "acpi/acpi.h"
#include "hal/hal.h"
#include "console/console.h"
#include "drivers/manager/device.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include <limine.h>

// Internal state
static struct limine_rsdp_response *rsdp_response = NULL;
static struct acpi_rsdp *rsdp = NULL;
static struct acpi_sdt_header *root_sdt = NULL;
static bool use_xsdt = false;

// Parsed MADT data
static uint32_t lapic_base_address = 0;
static uint32_t ioapic_address = 0;
static uint32_t ioapic_gsi = 0;

#define MAX_ISOS 24
static struct {
  uint8_t irq_source;
  uint32_t gsi;
  uint16_t flags;
} iso_entries[MAX_ISOS];
static uint32_t iso_count = 0;

// CPU APIC IDs discovered from MADT Local APIC entries
#define MAX_CPUS_ACPI 64
static uint8_t cpu_apic_ids[MAX_CPUS_ACPI];
static uint32_t cpu_count = 0;

// Helpers

static void print_uint32(uint32_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[10];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

static void print_hex32(uint32_t num) {
  const char *hex = "0123456789ABCDEF";
  for (int i = 28; i >= 0; i -= 4) {
    console_putchar(hex[(num >> i) & 0xF]);
  }
}

static bool acpi_validate_checksum(void *table, size_t length) {
  uint8_t *bytes = (uint8_t *)table;
  uint8_t sum = 0;
  for (size_t i = 0; i < length; i++) {
    sum += bytes[i];
  }
  return sum == 0;
}

// Table lookup

void *acpi_find_table(const char *signature) {
  if (!root_sdt)
    return NULL;

  size_t entries = (root_sdt->length - sizeof(struct acpi_sdt_header));

  if (use_xsdt) {
    entries /= 8;
    struct acpi_xsdt *xsdt = (struct acpi_xsdt *)root_sdt;
    for (size_t i = 0; i < entries; i++) {
      struct acpi_sdt_header *header =
          (struct acpi_sdt_header *)(xsdt->pointers[i] + pmm_get_hhdm_offset());
      if (strncmp(header->signature, signature, 4) == 0) {
        if (acpi_validate_checksum(header, header->length)) {
          return header;
        }
      }
    }
  } else {
    entries /= 4;
    struct acpi_rsdt *rsdt = (struct acpi_rsdt *)root_sdt;
    for (size_t i = 0; i < entries; i++) {
      struct acpi_sdt_header *header =
          (struct acpi_sdt_header *)((uint64_t)rsdt->pointers[i] +
                                     pmm_get_hhdm_offset());
      if (strncmp(header->signature, signature, 4) == 0) {
        if (acpi_validate_checksum(header, header->length)) {
          return header;
        }
      }
    }
  }

  return NULL;
}

// MADT Data Accessors

uint32_t acpi_get_lapic_base(void) { return lapic_base_address; }
uint32_t acpi_get_ioapic_base(void) { return ioapic_address; }
uint32_t acpi_get_ioapic_gsi_base(void) { return ioapic_gsi; }

bool acpi_get_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags) {
  for (uint32_t i = 0; i < iso_count; i++) {
    if (iso_entries[i].irq_source == irq) {
      if (gsi)
        *gsi = iso_entries[i].gsi;
      if (flags)
        *flags = iso_entries[i].flags;
      return true;
    }
  }
  return false;
}

uint32_t acpi_get_cpu_count(void) { return cpu_count; }
const uint8_t *acpi_get_cpu_apic_ids(void) { return cpu_apic_ids; }

struct acpi_mcfg *acpi_get_mcfg(void) {
  return (struct acpi_mcfg *)acpi_find_table("MCFG");
}

// FADT Data
static struct acpi_fadt *fadt_table = NULL;

struct acpi_fadt *acpi_get_fadt(void) {
  if (fadt_table)
    return fadt_table;
  fadt_table = (struct acpi_fadt *)acpi_find_table("FACP");
  return fadt_table;
}

bool acpi_parse_fadt(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();
  if (!fadt) {
    console_puts("[WARN] No FADT (FACP) table found.\n");
    return false;
  }

  console_puts("[OK] FADT table found.\n");

  // Print basic info
  console_puts("     Revision: ");
  print_uint32(fadt->header.revision);
  console_puts(".");
  print_uint32(fadt->minor_revision);
  console_puts("\n");

  console_puts("     PM Profile: ");
  console_puts(fadt_pm_profile_name(fadt->preferred_pm_profile));
  console_puts("\n");

  console_puts("     SCI Interrupt: 0x");
  print_hex32(fadt->sci_int);
  console_puts("\n");

  console_puts("     SMI Command Port: 0x");
  print_hex32(fadt->smi_cmd);
  console_puts("\n");

  console_puts("     ACPI Enable Value: 0x");
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[(fadt->acpi_enable >> 4) & 0xF]);
  console_putchar(hex[fadt->acpi_enable & 0xF]);
  console_puts("\n");

  console_puts("     ACPI Disable Value: 0x");
  console_putchar(hex[(fadt->acpi_disable >> 4) & 0xF]);
  console_putchar(hex[fadt->acpi_disable & 0xF]);
  console_puts("\n");

  // Power management register blocks
  console_puts("     PM1a Event Block: 0x");
  print_hex32(fadt->pm1a_evt_blk);
  console_puts("\n");

  console_puts("     PM1a Control Block: 0x");
  print_hex32(fadt->pm1a_cnt_blk);
  console_puts("\n");

  console_puts("     PM Timer Block: 0x");
  print_hex32(fadt->pm_tmr_blk);
  console_puts(" (length: ");
  print_uint32(fadt->pm_tmr_len);
  console_puts(")\n");

  console_puts("     GPE0 Block: 0x");
  print_hex32(fadt->gpe0_blk);
  console_puts(" (length: ");
  print_uint32(fadt->gpe0_blk_len);
  console_puts(")\n");

  // C-state latencies
  console_puts("     C2 Latency: ");
  if (fadt->p_lvl2_lat == 0x3E7) {
    console_puts("Unsupported\n");
  } else {
    print_uint32(fadt->p_lvl2_lat);
    console_puts(" us\n");
  }

  console_puts("     C3 Latency: ");
  if (fadt->p_lvl3_lat == 0x3E7) {
    console_puts("Unsupported\n");
  } else {
    print_uint32(fadt->p_lvl3_lat);
    console_puts(" us\n");
  }

  // Feature flags
  console_puts("     Feature Flags: 0x");
  print_hex32(fadt->flags);
  console_puts("\n");

  if (fadt->flags & FADT_FLAG_PROC_C1) {
    console_puts("       - C1 power state supported\n");
  }
  if (fadt->flags & FADT_FLAG_P_LVL2_UP) {
    console_puts("       - C2 on all CPUs\n");
  }
  if (fadt->flags & FADT_FLAG_RESET_REG_SUP) {
    console_puts("       - Reset register supported\n");
  }
  if (fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
    console_puts("       - Hardware-reduced ACPI\n");
  }
  if (fadt->flags & FADT_FLAG_LOW_PWR_IDLE_S0) {
    console_puts("       - Low power idle in S0 (Modern Standby)\n");
  }

  // Reset register (ACPI 2.0+)
  if (fadt->header.revision >= 2 && fadt->reset_reg.address) {
    console_puts("     Reset Register: 0x");
    print_hex32((uint32_t)fadt->reset_reg.address);
    console_puts(" (value: 0x");
    console_putchar(hex[(fadt->reset_value >> 4) & 0xF]);
    console_putchar(hex[fadt->reset_value & 0xF]);
    console_puts(")\n");
  }

  // Boot architecture flags
  console_puts("     Boot Arch Flags: 0x");
  print_hex32(fadt->iapc_boot_arch);
  console_puts("\n");

  if (fadt->iapc_boot_arch & (1 << 0)) {
    console_puts("       - Legacy VGA supported\n");
  }
  if (fadt->iapc_boot_arch & (1 << 1)) {
    console_puts("       - MSI devices supported\n");
  }
  if (fadt->iapc_boot_arch & (1 << 2)) {
    console_puts("       - PC-AT keyboard supported\n");
  }
  if (fadt->iapc_boot_arch & (1 << 3)) {
    console_puts("       - 8042 controller present\n");
  }
  if (fadt->iapc_boot_arch & (1 << 4)) {
    console_puts("       - VGA not required\n");
  }

  return true;
}

// ---------------------------------------------------------------------------
// Power control
// ---------------------------------------------------------------------------

#include "../io/io.h"

// ACPI SLP_EN bit in PM1x_CNT register
#define ACPI_PM1_SLP_EN  (1u << 13)
// Sleep type for S5 (soft-off).  The actual value is encoded in bits [12:10]
// of PM1x_CNT as (SLP_TYP << 10).  The ACPI spec assigns the per-platform
// SLP_TYP values in the DSDT \_S5 object; without an AML interpreter we use
// the QEMU / OVMF well-known encoding: SLP_TYP=7 -> bits = 7<<10 = 0x1C00.
// Combined with SLP_EN: 0x1C00 | 0x2000 = 0x3C00.
// (QEMU's PIIX4 ACPI uses SLP_TYP=0 -> 0x0000|0x2000 = 0x2000 instead; we
//  try both so we work on common QEMU configs.)
#define ACPI_S5_SLP_TYP_QEMU  (0u << 10)   // QEMU PIIX4
#define ACPI_S5_SLP_EN_QEMU   (ACPI_S5_SLP_TYP_QEMU | ACPI_PM1_SLP_EN)

void acpi_poweroff(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();

  console_puts("[ACPI] Initiating system power-off (S5)...\n");

  if (!fadt) {
    console_puts("[ACPI] No FADT — cannot perform ACPI power-off.\n");
    goto hang;
  }

  uint32_t pm1a = fadt->pm1a_cnt_blk;
  uint32_t pm1b = fadt->pm1b_cnt_blk;

  if (!pm1a) {
    console_puts("[ACPI] PM1a_CNT block not present.\n");
    goto hang;
  }

  // Write SLP_TYP | SLP_EN to PM1a_CNT (and optionally PM1b_CNT)
  outw((uint16_t)pm1a, (uint16_t)ACPI_S5_SLP_EN_QEMU);
  if (pm1b)
    outw((uint16_t)pm1b, (uint16_t)ACPI_S5_SLP_EN_QEMU);

  // If the machine is still running, the firmware may use SLP_TYP=5 (0x5<<10)
  outw((uint16_t)pm1a, (uint16_t)((5u << 10) | ACPI_PM1_SLP_EN));
  if (pm1b)
    outw((uint16_t)pm1b, (uint16_t)((5u << 10) | ACPI_PM1_SLP_EN));

hang:
  // Should not reach here; halt all CPUs
  hal_irq_disable();
  for (;;)
    hal_cpu_halt();
}

void acpi_reboot(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();

  console_puts("[ACPI] Initiating system reboot...\n");

  // Method 1: ACPI 2.0+ reset register (FADT revision >= 2, reset_reg valid)
  if (fadt && fadt->header.revision >= 2 && fadt->reset_reg.address &&
      (fadt->flags & FADT_FLAG_RESET_REG_SUP)) {
    uint8_t  space  = fadt->reset_reg.space_id;
    uint64_t addr   = fadt->reset_reg.address;
    uint8_t  value  = fadt->reset_value;

    if (space == 1) {
      // SystemIO — write reset value to the I/O port
      outb((uint16_t)addr, value);
    } else if (space == 0) {
      // SystemMemory
      volatile uint8_t *mem = (volatile uint8_t *)(addr + pmm_get_hhdm_offset());
      *mem = value;
    }
    // Give the hardware a moment to act
    for (int i = 0; i < 0x1000; i++)
      io_wait();
  }

  // Method 2: PS/2 keyboard controller reset pulse (port 0x64 bit 0)
  // Wait for KBC input buffer to be empty, then send "pulse output port" cmd
  {
    int timeout = 0x10000;
    while ((inb(0x64) & 0x02) && --timeout > 0)
      io_wait();
    outb(0x64, 0xFE); // pulse reset line
  }

  for (int i = 0; i < 0x10000; i++)
    io_wait();

  // Last resort: triple-fault via loading a zero-length IDT and triggering #UD
  struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = {0, 0};
  __asm__ volatile("cli; lidt %0; ud2" :: "m"(idtr));

  for (;;)
    hal_cpu_halt();
}

// Initialization

void acpi_init(struct limine_rsdp_response *response) {
  rsdp_response = response;
  console_puts("[INFO] Initializing ACPI...\n");

  if (!rsdp_response || !rsdp_response->address) {
    console_puts("[ERR] ACPI RSDP not provided by bootloader!\n");
    return;
  }

  rsdp = (struct acpi_rsdp *)rsdp_response->address;

  if (rsdp->revision >= 2) {
    struct acpi_rsdp_ext *rsdp_ext = (struct acpi_rsdp_ext *)rsdp;
    if (acpi_validate_checksum(rsdp_ext, rsdp_ext->length)) {
      use_xsdt = true;
      root_sdt = (struct acpi_sdt_header *)((uint64_t)rsdp_ext->xsdt_address +
                                            pmm_get_hhdm_offset());
      console_puts("[OK] ACPI 2.0+ (XSDT) detected.\n");
    } else {
      console_puts("[WARN] XSDT Checksum invalid!\n");
    }
  }

  if (!use_xsdt) {
    if (acpi_validate_checksum(rsdp, 20)) {
      root_sdt = (struct acpi_sdt_header *)((uint64_t)rsdp->rsdt_address +
                                            pmm_get_hhdm_offset());
      console_puts("[OK] ACPI 1.0 (RSDT) detected.\n");
    } else {
      console_puts("[ERR] RSDT Checksum invalid!\n");
      return;
    }
  }

  if (!acpi_validate_checksum(root_sdt, root_sdt->length)) {
    console_puts("[ERR] Root SDT Checksum invalid!\n");
    return;
  }

  // Parse the MADT
  struct acpi_madt *madt = (struct acpi_madt *)acpi_find_table("APIC");
  if (!madt) {
    console_puts("[ERR] No MADT (APIC Table) found.\n");
    return;
  }

  console_puts("[OK] MADT table found.\n");

  // Save the Local APIC physical base from the MADT header
  lapic_base_address = madt->local_apic_address;
  console_puts("     Local APIC Base: 0x");
  print_hex32(lapic_base_address);
  console_puts("\n");

  // Add LAPIC to device tree
  struct device *sys_node = device_find_by_path("/sys");
  struct device *lapic_dev = device_create(sys_node, "lapic");
  device_add_resource(lapic_dev, RES_MEM, "regs", lapic_base_address,
                      lapic_base_address + 0x400);

  uint32_t core_count = 0;
  uint8_t *entries = madt->entries;
  uint8_t *end = (uint8_t *)madt + madt->header.length;

  while (entries < end) {
    struct acpi_madt_entry_header *header =
        (struct acpi_madt_entry_header *)entries;

    switch (header->type) {
    case 0: { // Local APIC
      struct acpi_madt_local_apic *lapic =
          (struct acpi_madt_local_apic *)entries;
      if (lapic->flags & 1) {
        if (core_count < MAX_CPUS_ACPI) {
          cpu_apic_ids[core_count] = lapic->apic_id;
        }
        core_count++;
      }
      break;
    }
    case 1: { // I/O APIC
      struct acpi_madt_ioapic *io = (struct acpi_madt_ioapic *)entries;
      // Store the first I/O APIC we find
      if (ioapic_address == 0) {
        ioapic_address = io->ioapic_address;
        console_puts("\n");

        // Add IOAPIC to device tree
        char ioapic_name[16];
        strcpy(ioapic_name, "ioapic0"); // Simplification for first one
        struct device *ioapic_dev =
            device_create(device_find_by_path("/sys"), ioapic_name);
        device_add_resource(ioapic_dev, RES_MEM, "regs", ioapic_address,
                            ioapic_address + 0x20);
        device_add_resource(ioapic_dev, RES_IRQ, "gsi_base", ioapic_gsi,
                            ioapic_gsi);
      }
      break;
    }
    case 2: { // Interrupt Source Override
      struct acpi_madt_iso *iso = (struct acpi_madt_iso *)entries;
      if (iso_count < MAX_ISOS) {
        iso_entries[iso_count].irq_source = iso->irq_source;
        iso_entries[iso_count].gsi = iso->gsi;
        iso_entries[iso_count].flags = iso->flags;
        iso_count++;
        console_puts("     ISO: IRQ");
        print_uint32(iso->irq_source);
        console_puts(" -> GSI ");
        print_uint32(iso->gsi);
        console_puts(" (flags=0x");
        print_hex32(iso->flags);
        console_puts(")\n");
      }
      break;
    }
    default:
      break;
    }

    if (header->length == 0)
      break; // prevent infinite loop on corrupt table
    entries += header->length;
  }

  cpu_count = core_count;

  console_puts("     CPU Cores active: ");
  print_uint32(core_count);
  console_puts("\n");
}
