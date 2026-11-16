#include "acpi/acpi.h"
#include "hal/hal.h"
#include "console/console.h"
#include "drivers/serial.h"
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

#ifdef USE_RUST_ACPI
struct rust_acpi_iso_entry {
  uint8_t irq_source;
  uint8_t _pad;
  uint16_t flags;
  uint32_t gsi;
};

struct rust_acpi_madt_parsed {
  uint32_t lapic_base_address;
  uint32_t ioapic_address;
  uint32_t ioapic_gsi;
  bool has_ioapic;
  uint32_t cpu_count;
  uint8_t cpu_apic_ids[MAX_CPUS_ACPI];
  uint32_t iso_count;
  struct rust_acpi_iso_entry iso_entries[MAX_ISOS];
};

extern bool rust_acpi_validate_checksum(const void *table, size_t length);
extern bool rust_acpi_parse_rsdp(const void *rsdp, bool *out_use_xsdt,
                                 uint64_t *out_sdt_phys);
extern bool rust_acpi_parse_madt(const void *madt, size_t madt_len,
                                 struct rust_acpi_madt_parsed *out);
#endif

static bool acpi_validate_checksum(void *table, size_t length) {
#ifdef USE_RUST_ACPI
  return rust_acpi_validate_checksum(table, length);
#else
  uint8_t *bytes = (uint8_t *)table;
  uint8_t sum = 0;
  for (size_t i = 0; i < length; i++) {
    sum += bytes[i];
  }
  return sum == 0;
#endif
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

struct acpi_srat *acpi_get_srat(void) {
  return (struct acpi_srat *)acpi_find_table("SRAT");
}

struct acpi_slit *acpi_get_slit(void) {
  return (struct acpi_slit *)acpi_find_table("SLIT");
}

void *acpi_early_find_table(struct limine_rsdp_response *response,
                            uint64_t hhdm_offset, const char *signature) {
  if (!response || !response->address)
    return NULL;

  struct acpi_rsdp *p_rsdp = (struct acpi_rsdp *)response->address;
  struct acpi_sdt_header *root = NULL;
  bool is_xsdt = false;

  if (p_rsdp->revision >= 2) {
    struct acpi_rsdp_ext *rsdp_ext = (struct acpi_rsdp_ext *)p_rsdp;
    if (acpi_validate_checksum(rsdp_ext, rsdp_ext->length)) {
      is_xsdt = true;
      root = (struct acpi_sdt_header *)(rsdp_ext->xsdt_address + hhdm_offset);
    }
  }

  if (!is_xsdt) {
    if (acpi_validate_checksum(p_rsdp, 20)) {
      root = (struct acpi_sdt_header *)((uint64_t)p_rsdp->rsdt_address + hhdm_offset);
    } else {
      return NULL;
    }
  }

  if (!root || !acpi_validate_checksum(root, root->length))
    return NULL;

  size_t entries = (root->length - sizeof(struct acpi_sdt_header));
  if (is_xsdt) {
    entries /= 8;
    struct acpi_xsdt *xsdt = (struct acpi_xsdt *)root;
    for (size_t i = 0; i < entries; i++) {
      struct acpi_sdt_header *hdr =
          (struct acpi_sdt_header *)(xsdt->pointers[i] + hhdm_offset);
      if (strncmp(hdr->signature, signature, 4) == 0) {
        if (acpi_validate_checksum(hdr, hdr->length))
          return hdr;
      }
    }
  } else {
    entries /= 4;
    struct acpi_rsdt *rsdt = (struct acpi_rsdt *)root;
    for (size_t i = 0; i < entries; i++) {
      struct acpi_sdt_header *hdr =
          (struct acpi_sdt_header *)((uint64_t)rsdt->pointers[i] + hhdm_offset);
      if (strncmp(hdr->signature, signature, 4) == 0) {
        if (acpi_validate_checksum(hdr, hdr->length))
          return hdr;
      }
    }
  }
  return NULL;
}

// FADT Data
static struct acpi_fadt *fadt_table = NULL;

struct acpi_fadt *acpi_get_fadt(void) {
  if (fadt_table)
    return fadt_table;
  fadt_table = (struct acpi_fadt *)acpi_find_table("FACP");
  return fadt_table;
}

struct acpi_facs *acpi_get_facs(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();
  if (!fadt || fadt->header.length <
                   offsetof(struct acpi_fadt, firmware_ctrl) +
                       sizeof(fadt->firmware_ctrl))
    return NULL;
  uint64_t phys = fadt->firmware_ctrl;
  if (fadt->header.length >= offsetof(struct acpi_fadt, x_firmware_ctrl) +
                                 sizeof(fadt->x_firmware_ctrl) &&
      fadt->x_firmware_ctrl)
    phys = fadt->x_firmware_ctrl;
  if (!phys)
    return NULL;
  struct acpi_facs *facs =
      (struct acpi_facs *)(phys + pmm_get_hhdm_offset());
  if (memcmp(facs->signature, "FACS", 4) != 0 ||
      facs->length < 64)
    return NULL;
  return facs;
}

bool acpi_set_waking_vector(uint32_t physical_address) {
  // The real-mode fallback vector must be below 1 MiB on IA-PC systems.
  if (!physical_address || physical_address >= 0x100000)
    return false;
  struct acpi_facs *facs = acpi_get_facs();
  if (!facs)
    return false;
  volatile struct acpi_facs *fw = (volatile struct acpi_facs *)facs;
  fw->firmware_waking_vector = physical_address;
  /* This trampoline is an IA-PC real-mode entry. Do not advertise the
   * extended wake vector: firmware may otherwise enter it in a 64-bit
   * environment that this low-memory trampoline does not establish. */
  if (facs->length >= offsetof(struct acpi_facs, x_firmware_waking_vector) +
                          sizeof(facs->x_firmware_waking_vector))
    fw->x_firmware_waking_vector = 0;
  if (facs->length >= offsetof(struct acpi_facs, ospm_flags) +
                          sizeof(facs->ospm_flags))
    fw->ospm_flags &= ~ACPI_FACS_OSPM_FLAG_64BIT_WAKE;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return true;
}

void acpi_clear_waking_vector(void) {
  struct acpi_facs *facs = acpi_get_facs();
  if (!facs)
    return;
  volatile struct acpi_facs *fw = (volatile struct acpi_facs *)facs;
  fw->firmware_waking_vector = 0;
  if (facs->length >= offsetof(struct acpi_facs, x_firmware_waking_vector) +
                          sizeof(facs->x_firmware_waking_vector))
    fw->x_firmware_waking_vector = 0;
  if (facs->length >= offsetof(struct acpi_facs, ospm_flags) +
                          sizeof(facs->ospm_flags))
    fw->ospm_flags &= ~ACPI_FACS_OSPM_FLAG_64BIT_WAKE;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

bool acpi_parse_fadt(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();
  if (!fadt) {
    console_puts("[WARN] No FADT (FACP) table found.\n");
    return false;
  }

  console_puts("[OK] FADT table found.\n");

  uint8_t sleep_type_a, sleep_type_b;
  console_puts("     Sleep states in DSDT:");
  for (uint8_t state = 1; state <= 5; state++) {
    if (acpi_get_sleep_types(state, &sleep_type_a, &sleep_type_b)) {
      console_puts(" S");
      console_putchar('0' + state);
    }
  }
  console_puts("\n");

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

// PM1 control register SLP_EN bit. SLP_TYP comes from the DSDT's _S5 object.
#define ACPI_PM1_SLP_EN (1u << 13)

static bool aml_read_integer(const uint8_t *aml, size_t end, size_t *cursor,
                             uint64_t *value) {
  if (*cursor >= end)
    return false;
  uint8_t op = aml[(*cursor)++];
  if (op == 0x00 || op == 0x01) { // ZeroOp / OneOp
    *value = op;
    return true;
  }
  size_t bytes;
  switch (op) {
  case 0x0A: bytes = 1; break; // BytePrefix
  case 0x0B: bytes = 2; break; // WordPrefix
  case 0x0C: bytes = 4; break; // DWordPrefix
  case 0x0E: bytes = 8; break; // QWordPrefix
  default: return false;
  }
  if (bytes > end - *cursor)
    return false;
  uint64_t result = 0;
  for (size_t i = 0; i < bytes; i++)
    result |= (uint64_t)aml[(*cursor)++] << (i * 8);
  *value = result;
  return true;
}

/* Decode the standard AML Name(_S5, Package(...)) declaration. This is a
 * deliberately narrow AML reader: it accepts the integer encodings needed
 * for sleep types and refuses malformed or unsupported packages. */
bool acpi_get_sleep_types(uint8_t state, uint8_t *type_a, uint8_t *type_b) {
  if (state < 1 || state > 5 || !type_a || !type_b)
    return false;
  struct acpi_fadt *fadt = acpi_get_fadt();
  if (!fadt)
    return false;
  uint64_t dsdt_phys = fadt->dsdt;
  if (fadt->header.length >= offsetof(struct acpi_fadt, x_dsdt) + sizeof(fadt->x_dsdt) &&
      fadt->x_dsdt)
    dsdt_phys = fadt->x_dsdt;
  if (!dsdt_phys)
    return false;
  struct acpi_sdt_header *dsdt =
      (struct acpi_sdt_header *)(dsdt_phys + pmm_get_hhdm_offset());
  if (dsdt->length < sizeof(*dsdt) || !acpi_validate_checksum(dsdt, dsdt->length))
    return false;

  const uint8_t *aml = (const uint8_t *)dsdt + sizeof(*dsdt);
  size_t length = dsdt->length - sizeof(*dsdt);
  char name[4] = {'_', 'S', (char)('0' + state), '_'};
  for (size_t i = 1; i + 7 < length; i++) {
    bool name_op = aml[i - 1] == 0x08 ||
                   (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == 0x5c);
    if (!name_op || memcmp(aml + i, name, sizeof(name)) != 0)
      continue;
    size_t p = i + 4;
    if (p >= length || aml[p++] != 0x12) // PackageOp
      continue;
    if (p >= length)
      continue;
    size_t pkg_start = p;
    uint8_t lead = aml[p++];
    unsigned follow = lead >> 6;
    uint64_t pkg_len = lead & (follow ? 0x0f : 0x3f);
    if (follow > 3 || follow > length - p)
      continue;
    for (unsigned n = 0; n < follow; n++)
      pkg_len |= (uint64_t)aml[p++] << (4 + 8 * n);
    if (pkg_len < p - pkg_start || pkg_len > length - pkg_start)
      continue;
    size_t end = pkg_start + (size_t)pkg_len;
    if (p >= end || aml[p++] < 2)
      continue;
    uint64_t a, b;
    if (!aml_read_integer(aml, end, &p, &a) ||
        !aml_read_integer(aml, end, &p, &b) || a > 7 || b > 7)
      continue;
    *type_a = (uint8_t)a;
    *type_b = (uint8_t)b;
    return true;
  }
  return false;
}

static bool acpi_write_gas(uint8_t space, uint8_t width, uint8_t offset,
                           uint64_t address, uint32_t value) {
  if (!address || offset != 0 || width < 16)
    return false;
  if (space == 1) { // System I/O
    if (address > 0xffff)
      return false;
    outw((uint16_t)address, (uint16_t)value);
    return true;
  }
  if (space == 0) { // System memory, mapped by the kernel HHDM
    volatile uint16_t *reg =
        (volatile uint16_t *)(address + pmm_get_hhdm_offset());
    *reg = (uint16_t)value;
    return true;
  }
  return false;
}

static bool acpi_write_pm1(struct acpi_fadt *fadt, bool pm1b, uint16_t value) {
  size_t gas_end = pm1b
      ? offsetof(struct acpi_fadt, x_pm1b_cnt_blk) + sizeof(fadt->x_pm1b_cnt_blk)
      : offsetof(struct acpi_fadt, x_pm1a_cnt_blk) + sizeof(fadt->x_pm1a_cnt_blk);
  if (fadt->header.length >= gas_end) {
    if (pm1b) {
      if (fadt->x_pm1b_cnt_blk.address &&
          acpi_write_gas(fadt->x_pm1b_cnt_blk.space_id,
                         fadt->x_pm1b_cnt_blk.bit_width,
                         fadt->x_pm1b_cnt_blk.bit_offset,
                         fadt->x_pm1b_cnt_blk.address, value))
        return true;
    } else if (fadt->x_pm1a_cnt_blk.address &&
               acpi_write_gas(fadt->x_pm1a_cnt_blk.space_id,
                              fadt->x_pm1a_cnt_blk.bit_width,
                              fadt->x_pm1a_cnt_blk.bit_offset,
                              fadt->x_pm1a_cnt_blk.address, value)) {
      return true;
    }
  }
  uint32_t port = pm1b ? fadt->pm1b_cnt_blk : fadt->pm1a_cnt_blk;
  if (!port || port > 0xffff || fadt->pm1_cnt_len < 2)
    return false;
  outw((uint16_t)port, value);
  return true;
}

static bool acpi_write_pm1_status(struct acpi_fadt *fadt, bool pm1b,
                                  uint16_t value) {
  size_t gas_end = pm1b
      ? offsetof(struct acpi_fadt, x_pm1b_evt_blk) + sizeof(fadt->x_pm1b_evt_blk)
      : offsetof(struct acpi_fadt, x_pm1a_evt_blk) + sizeof(fadt->x_pm1a_evt_blk);
  if (fadt->header.length >= gas_end) {
    if (pm1b) {
      if (fadt->x_pm1b_evt_blk.address &&
          acpi_write_gas(fadt->x_pm1b_evt_blk.space_id,
                         fadt->x_pm1b_evt_blk.bit_width,
                         fadt->x_pm1b_evt_blk.bit_offset,
                         fadt->x_pm1b_evt_blk.address, value))
        return true;
    } else if (fadt->x_pm1a_evt_blk.address &&
               acpi_write_gas(fadt->x_pm1a_evt_blk.space_id,
                              fadt->x_pm1a_evt_blk.bit_width,
                              fadt->x_pm1a_evt_blk.bit_offset,
                              fadt->x_pm1a_evt_blk.address, value)) {
      return true;
    }
  }
  uint32_t port = pm1b ? fadt->pm1b_evt_blk : fadt->pm1a_evt_blk;
  if (!port || port > 0xffff || fadt->pm1_evt_len < 4)
    return false;
  outw((uint16_t)port, value);
  return true;
}

static bool acpi_pm1_control_present(const struct acpi_fadt *fadt,
                                    bool pm1b) {
  size_t gas_end = pm1b
      ? offsetof(struct acpi_fadt, x_pm1b_cnt_blk) + sizeof(fadt->x_pm1b_cnt_blk)
      : offsetof(struct acpi_fadt, x_pm1a_cnt_blk) + sizeof(fadt->x_pm1a_cnt_blk);
  bool gas = false;
  if (fadt->header.length >= gas_end)
    gas = pm1b ? fadt->x_pm1b_cnt_blk.address != 0
               : fadt->x_pm1a_cnt_blk.address != 0;
  uint32_t port = pm1b ? fadt->pm1b_cnt_blk : fadt->pm1a_cnt_blk;
  return gas || (port && port <= 0xffff && fadt->pm1_cnt_len >= 2);
}

static bool acpi_sleep_state_supported(uint8_t state, bool require_facs) {
  struct acpi_fadt *fadt = acpi_get_fadt();
  uint8_t type_a, type_b;
  if (!fadt || (require_facs && !acpi_get_facs()) ||
      fadt->header.length < offsetof(struct acpi_fadt, flags) +
                                sizeof(fadt->flags) ||
      (fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) ||
      !acpi_get_sleep_types(state, &type_a, &type_b) ||
      !acpi_pm1_control_present(fadt, false))
    return false;
  bool has_pm1b = fadt->pm1b_cnt_blk ||
      (fadt->header.length >= offsetof(struct acpi_fadt, x_pm1b_cnt_blk) +
                                  sizeof(fadt->x_pm1b_cnt_blk) &&
       fadt->x_pm1b_cnt_blk.address);
  return !has_pm1b || acpi_pm1_control_present(fadt, true);
}

static bool acpi_enter_sleep_state(uint8_t state, bool require_facs,
                                   const char *label) {
  struct acpi_fadt *fadt = acpi_get_fadt();
  uint8_t type_a, type_b;
  if (!fadt || !acpi_sleep_state_supported(state, require_facs) ||
      !acpi_get_sleep_types(state, &type_a, &type_b))
    return false;

  /* WAK_STS is write-one-to-clear. Clear it before arming the wake vector. */
  if (fadt->pm1a_evt_blk ||
      (fadt->header.length >= offsetof(struct acpi_fadt, x_pm1a_evt_blk) +
                                  sizeof(fadt->x_pm1a_evt_blk) &&
       fadt->x_pm1a_evt_blk.address))
    (void)acpi_write_pm1_status(fadt, false, 1u << 15);
  if (fadt->pm1b_evt_blk ||
      (fadt->header.length >= offsetof(struct acpi_fadt, x_pm1b_evt_blk) +
                                  sizeof(fadt->x_pm1b_evt_blk) &&
       fadt->x_pm1b_evt_blk.address))
    (void)acpi_write_pm1_status(fadt, true, 1u << 15);

  console_puts("[ACPI] Initiating system ");
  console_puts(label);
  console_puts("...\n");
  serial_flush_sync();
  bool has_pm1b = fadt->pm1b_cnt_blk ||
      (fadt->header.length >= offsetof(struct acpi_fadt, x_pm1b_cnt_blk) +
                                  sizeof(fadt->x_pm1b_cnt_blk) &&
       fadt->x_pm1b_cnt_blk.address);
  /* Program the sleep type first while SLP_EN is clear. With APs already
   * parked and their caches flushed, flush the BSP cache immediately before
   * the final SLP_EN writes. */
  if (has_pm1b && !acpi_write_pm1(fadt, true, (uint16_t)(type_b << 10)))
    return false;
  if (!acpi_write_pm1(fadt, false, (uint16_t)(type_a << 10)))
    return false;
  __asm__ volatile("wbinvd" ::: "memory");
  if (has_pm1b &&
      !acpi_write_pm1(fadt, true,
                      (uint16_t)((type_b << 10) | ACPI_PM1_SLP_EN)))
    return false;
  return acpi_write_pm1(fadt, false,
                        (uint16_t)((type_a << 10) | ACPI_PM1_SLP_EN));
}

bool acpi_s3_supported(void) {
  return acpi_sleep_state_supported(3, true);
}

bool acpi_enter_s3(void) {
  return acpi_enter_sleep_state(3, true, "suspend (S3)");
}

bool acpi_s4_supported(void) {
  return acpi_sleep_state_supported(4, false);
}

bool acpi_enter_s4(void) {
  return acpi_enter_sleep_state(4, false, "hibernate (S4)");
}

void acpi_poweroff(void) {
  struct acpi_fadt *fadt = acpi_get_fadt();

  console_puts("[ACPI] Initiating system power-off (S5)...\n");

  if (!fadt) {
    console_puts("[ACPI] No FADT — cannot perform ACPI power-off.\n");
    goto hang;
  }
  if (fadt->header.length < offsetof(struct acpi_fadt, flags) +
                            sizeof(fadt->flags)) {
    console_puts("[ACPI] FADT is too short for power control.\n");
    goto hang;
  }

  uint8_t type_a, type_b;
  if (!acpi_get_sleep_types(5, &type_a, &type_b)) {
    console_puts("[ACPI] DSDT has no supported _S5 package; power-off unavailable.\n");
    goto hang;
  }
  if (!(fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) && !fadt->pm1a_cnt_blk &&
      (fadt->header.length < offsetof(struct acpi_fadt, x_pm1a_cnt_blk) +
                             sizeof(fadt->x_pm1a_cnt_blk) ||
       !fadt->x_pm1a_cnt_blk.address)) {
    console_puts("[ACPI] PM1a_CNT register not present.\n");
    goto hang;
  }
  if (fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
    console_puts("[ACPI] Hardware-reduced sleep control is not supported yet.\n");
    goto hang;
  }
  serial_flush_sync();
  // Set the secondary control block first; setting SLP_EN on PM1a triggers S5.
  bool has_pm1b = fadt->pm1b_cnt_blk ||
      (fadt->header.length >= offsetof(struct acpi_fadt, x_pm1b_cnt_blk) +
                              sizeof(fadt->x_pm1b_cnt_blk) &&
       fadt->x_pm1b_cnt_blk.address);
  if (has_pm1b && !acpi_write_pm1(
                      fadt, true,
                      (uint16_t)((type_b << 10) | ACPI_PM1_SLP_EN))) {
    console_puts("[ACPI] Cannot access PM1b_CNT register.\n");
    goto hang;
  }
  if (!acpi_write_pm1(fadt, false,
                      (uint16_t)((type_a << 10) | ACPI_PM1_SLP_EN))) {
    console_puts("[ACPI] Cannot access PM1a_CNT register.\n");
    goto hang;
  }

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

#ifdef USE_RUST_ACPI
  uint64_t sdt_phys = 0;
  if (!rust_acpi_parse_rsdp(rsdp, &use_xsdt, &sdt_phys)) {
    console_puts("[ERR] RSDP Checksum invalid!\n");
    return;
  }
  root_sdt = (struct acpi_sdt_header *)(sdt_phys + pmm_get_hhdm_offset());
  if (use_xsdt)
    console_puts("[OK] ACPI 2.0+ (XSDT) detected.\n");
  else
    console_puts("[OK] ACPI 1.0 (RSDT) detected.\n");
#else
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
#endif

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

#ifdef USE_RUST_ACPI
  struct rust_acpi_madt_parsed madt_parsed;
  if (!rust_acpi_parse_madt(madt, madt->header.length, &madt_parsed)) {
    console_puts("[ERR] MADT parse failed.\n");
    return;
  }

  lapic_base_address = madt_parsed.lapic_base_address;
  console_puts("     Local APIC Base: 0x");
  print_hex32(lapic_base_address);
  console_puts("\n");

  struct device *sys_node = device_find_by_path("/sys");
  struct device *lapic_dev = device_create(sys_node, "lapic");
  if (lapic_dev)
    lapic_dev->pm_managed_by_core = true;
  device_add_resource(lapic_dev, RES_MEM, "regs", lapic_base_address,
                      lapic_base_address + 0x400);

  uint32_t core_count = madt_parsed.cpu_count;
  uint32_t copy_cpus = core_count < MAX_CPUS_ACPI ? core_count : MAX_CPUS_ACPI;
  for (uint32_t i = 0; i < copy_cpus; i++) {
    cpu_apic_ids[i] = madt_parsed.cpu_apic_ids[i];
  }

  if (madt_parsed.has_ioapic) {
    ioapic_address = madt_parsed.ioapic_address;
    ioapic_gsi = madt_parsed.ioapic_gsi;
    console_puts("\n");
    char ioapic_name[16];
    strcpy(ioapic_name, "ioapic0");
    struct device *ioapic_dev =
        device_create(device_find_by_path("/sys"), ioapic_name);
    if (ioapic_dev)
      ioapic_dev->pm_managed_by_core = true;
    device_add_resource(ioapic_dev, RES_MEM, "regs", ioapic_address,
                        ioapic_address + 0x20);
    device_add_resource(ioapic_dev, RES_IRQ, "gsi_base", ioapic_gsi,
                        ioapic_gsi);
  }

  for (uint32_t i = 0; i < madt_parsed.iso_count && iso_count < MAX_ISOS; i++) {
    iso_entries[iso_count].irq_source = madt_parsed.iso_entries[i].irq_source;
    iso_entries[iso_count].gsi = madt_parsed.iso_entries[i].gsi;
    iso_entries[iso_count].flags = madt_parsed.iso_entries[i].flags;
    iso_count++;
    console_puts("     ISO: IRQ");
    print_uint32(madt_parsed.iso_entries[i].irq_source);
    console_puts(" -> GSI ");
    print_uint32(madt_parsed.iso_entries[i].gsi);
    console_puts(" (flags=0x");
    print_hex32(madt_parsed.iso_entries[i].flags);
    console_puts(")\n");
  }
#else
  // Save the Local APIC physical base from the MADT header
  lapic_base_address = madt->local_apic_address;
  console_puts("     Local APIC Base: 0x");
  print_hex32(lapic_base_address);
  console_puts("\n");

  // Add LAPIC to device tree
  struct device *sys_node = device_find_by_path("/sys");
  struct device *lapic_dev = device_create(sys_node, "lapic");
  if (lapic_dev)
    lapic_dev->pm_managed_by_core = true;
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
        if (ioapic_dev)
          ioapic_dev->pm_managed_by_core = true;
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
#endif

  cpu_count = core_count;

  console_puts("     CPU Cores active: ");
  print_uint32(core_count);
  console_puts("\n");
}
