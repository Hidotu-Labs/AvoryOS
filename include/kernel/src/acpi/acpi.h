#ifndef ACPI_H
#define ACPI_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ACPI SDT Header
struct acpi_sdt_header {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

// ACPI 1.0 RSDP
struct acpi_rsdp {
  char signature[8]; // "RSD PTR "
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
} __attribute__((packed));

// ACPI 2.0+ Extended RSDP
struct acpi_rsdp_ext {
  struct acpi_rsdp first_part;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __attribute__((packed));

// RSDT (32-bit pointers)
struct acpi_rsdt {
  struct acpi_sdt_header header;
  uint32_t pointers[];
} __attribute__((packed));

// XSDT (64-bit pointers)
struct acpi_xsdt {
  struct acpi_sdt_header header;
  uint64_t pointers[];
} __attribute__((packed));

// MADT (Multiple APIC Description Table)
struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t local_apic_address;
  uint32_t flags;
  uint8_t entries[];
} __attribute__((packed));

// MADT Entry Header
struct acpi_madt_entry_header {
  uint8_t type;
  uint8_t length;
} __attribute__((packed));

// MADT Entry Type 0: Processor Local APIC
struct acpi_madt_local_apic {
  struct acpi_madt_entry_header header;
  uint8_t acpi_processor_id;
  uint8_t apic_id;
  uint32_t flags;
} __attribute__((packed));

// MADT Entry Type 1: I/O APIC
struct acpi_madt_ioapic {
  struct acpi_madt_entry_header header;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_address;
  uint32_t gsi_base;
} __attribute__((packed));

// ACPI MADT Entry Type 2: Interrupt Source Override
struct acpi_madt_iso {
  struct acpi_madt_entry_header header;
  uint8_t bus_source; // 0 = ISA
  uint8_t irq_source; // ISA IRQ number
  uint32_t gsi;       // Global System Interrupt it maps to
  uint16_t flags;     // Polarity (bits 1:0) and Trigger (bits 3:2)
} __attribute__((packed));

// MCFG (Memory Mapped Configuration Space Base Address Description Table)
struct acpi_mcfg_entry {
  uint64_t base_address;
  uint16_t pci_segment_group_number;
  uint8_t start_bus_number;
  uint8_t end_bus_number;
  uint32_t reserved;
} __attribute__((packed));

struct acpi_mcfg {
  struct acpi_sdt_header header;
  uint64_t reserved;
  struct acpi_mcfg_entry entries[];
} __attribute__((packed));

// FADT (Fixed ACPI Description Table)
struct acpi_fadt {
  struct acpi_sdt_header header;
  uint32_t firmware_ctrl; // FACS address (32-bit)
  uint32_t dsdt;          // DSDT address (32-bit)
  uint8_t reserved1;
  uint8_t preferred_pm_profile; // 0=Unspecified, 1=Desktop, 2=Mobile,
                                // 3=Workstation, 4=Enterprise Server, 5=SOHO,
                                // 6=Appliance, 7=Performance Server
  uint16_t sci_int;             // SCI interrupt vector
  uint32_t smi_cmd;             // SMI command port
  uint8_t acpi_enable;          // Value to write to smi_cmd to enable ACPI
  uint8_t acpi_disable;         // Value to write to smi_cmd to disable ACPI
  uint8_t s4bios_req;           // Value to write to enter S4BIOS
  uint8_t pstate_cnt;           // Processor performance state control
  uint32_t pm1a_evt_blk;        // PM1a event register block address
  uint32_t pm1b_evt_blk;        // PM1b event register block address
  uint32_t pm1a_cnt_blk;        // PM1a control register block address
  uint32_t pm1b_cnt_blk;        // PM1b control register block address
  uint32_t pm2_cnt_blk;         // PM2 control register block address
  uint32_t pm_tmr_blk;          // Power management timer register block address
  uint32_t gpe0_blk;            // GPE0 register block address
  uint32_t gpe1_blk;            // GPE1 register block address
  uint8_t pm1_evt_len;          // PM1 event register block length
  uint8_t pm1_cnt_len;          // PM1 control register block length
  uint8_t pm2_cnt_len;          // PM2 control register block length
  uint8_t pm_tmr_len;           // Power management timer length
  uint8_t gpe0_blk_len;         // GPE0 register block length
  uint8_t gpe1_blk_len;         // GPE1 register block length
  uint8_t gpe1_base;            // GPE1 base number
  uint8_t cst_cnt;              // C-state control
  uint16_t p_lvl2_lat;          // C2 latency (us), 0x3E7=unsupported
  uint16_t p_lvl3_lat;          // C3 latency (us), 0x3E7=unsupported
  uint16_t flush_size;          // Processor cache flush size
  uint16_t flush_stride;        // Processor cache flush stride
  uint8_t duty_offset;          // Processor duty cycle offset
  uint8_t duty_width;           // Processor duty cycle width
  uint8_t day_alrm;             // RTC day alarm index
  uint8_t mon_alrm;             // RTC month alarm index
  uint8_t century;              // RTC century index
  uint16_t iapc_boot_arch;      // IA-PC boot architecture flags
  uint8_t reserved2;
  uint32_t flags; // Fixed feature flags
  // ACPI 2.0+ extended fields
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) reset_reg;
  uint8_t reset_value;
  uint16_t arm_boot_arch;
  uint8_t minor_revision;
  uint64_t x_firmware_ctrl; // 64-bit FACS address
  uint64_t x_dsdt;          // 64-bit DSDT address
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm1a_evt_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm1b_evt_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm1a_cnt_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm1b_cnt_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm2_cnt_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_pm_tmr_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_gpe0_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) x_gpe1_blk;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) sleep_control_reg;
  struct {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
  } __attribute__((packed)) sleep_status_reg;
  uint64_t hypervisor_vendor_identity;
} __attribute__((packed));

// FADT Preferred PM Profile strings
static inline const char *fadt_pm_profile_name(uint8_t profile) {
  switch (profile) {
  case 0:
    return "Unspecified";
  case 1:
    return "Desktop";
  case 2:
    return "Mobile";
  case 3:
    return "Workstation";
  case 4:
    return "Enterprise Server";
  case 5:
    return "SOHO Server";
  case 6:
    return "Appliance PC";
  case 7:
    return "Performance Server";
  default:
    return "Unknown";
  }
}

// FADT Flags helpers
#define FADT_FLAG_WBINVD (1 << 0)              // WBINVD instruction supported
#define FADT_FLAG_WBINVD_FLUSH (1 << 1)        // WBINVD flushes cache
#define FADT_FLAG_PROC_C1 (1 << 2)             // C1 power state supported
#define FADT_FLAG_P_LVL2_UP (1 << 3)           // C2 on all CPUs
#define FADT_FLAG_PWR_BUTTON (1 << 4)          // Power button is control method
#define FADT_FLAG_SLP_BUTTON (1 << 5)          // Sleep button is control method
#define FADT_FLAG_FIX_RTC (1 << 6)             // RTC wake status fixed
#define FADT_FLAG_RTC_S4 (1 << 7)              // RTC can wake from S4
#define FADT_FLAG_TMR_VAL_EXT (1 << 8)         // TMR_VAL is 32-bit
#define FADT_FLAG_DCK_CAP (1 << 9)             // Docking supported
#define FADT_FLAG_RESET_REG_SUP (1 << 10)      // Reset register supported
#define FADT_FLAG_SEALED_CASE (1 << 11)        // Case is sealed
#define FADT_FLAG_HEADLESS (1 << 12)           // Headless system
#define FADT_FLAG_CPU_SW_SLP (1 << 13)         // CPU supports software sleep
#define FADT_FLAG_PCI_EXP_WAK (1 << 14)        // PCIe wake supported
#define FADT_FLAG_USE_PLATFORM_CLK (1 << 15)   // Use platform clock
#define FADT_FLAG_S4_RTC_STS_VALID (1 << 16)   // S4 RTC status valid
#define FADT_FLAG_REMOTE_POWER_ON (1 << 17)    // Remote power on supported
#define FADT_FLAG_APIC_CLUSTER (1 << 18)       // APIC cluster model
#define FADT_FLAG_FORCE_APIC_CLUSTER (1 << 19) // Force APIC cluster
#define FADT_FLAG_HW_REDUCED_ACPI (1 << 20)    // Hardware-reduced ACPI
#define FADT_FLAG_LOW_PWR_IDLE_S0 (1 << 21)    // Low power idle in S0

// ACPI Initialization & Table Lookup
void acpi_init(struct limine_rsdp_response *response);
void *acpi_find_table(const char *signature);

// MADT Data Accessors (valid after acpi_init)

// Returns the physical base address of the Local APIC from the MADT.
uint32_t acpi_get_lapic_base(void);

// Returns the physical base address of the first I/O APIC.
uint32_t acpi_get_ioapic_base(void);

// Returns the GSI base of the first I/O APIC.
uint32_t acpi_get_ioapic_gsi_base(void);

// Looks up an Interrupt Source Override for the given ISA IRQ.
// If found, *gsi and *flags are filled in and the function returns true.
bool acpi_get_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags);

// Returns the number of enabled CPUs discovered in the MADT.
uint32_t acpi_get_cpu_count(void);

// Returns a pointer to the array of APIC IDs for all enabled CPUs.
const uint8_t *acpi_get_cpu_apic_ids(void);

// Returns the MCFG table if present, otherwise NULL.
struct acpi_mcfg *acpi_get_mcfg(void);

// Returns the FADT table if present, otherwise NULL.
struct acpi_fadt *acpi_get_fadt(void);

// Parses and logs FADT information. Returns true if FADT found.
bool acpi_parse_fadt(void);

// Power control (backed by ACPI FADT)
// acpi_poweroff: enter the S5 (soft-off) sleep state; never returns on success.
void acpi_poweroff(void);
// acpi_reboot: trigger a hardware reset via the FADT reset register, falling
// back to the PS/2 keyboard controller reset pulse (port 0x64 / 0xFE).
void acpi_reboot(void);

#endif
