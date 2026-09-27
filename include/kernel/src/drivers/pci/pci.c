#include "drivers/pci/pci.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pcie.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include <stddef.h>
static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;
static struct bus_type pci_bus = {.name = "pci"};

struct bus_type *pci_bus_type(void) { return &pci_bus; }

// PCI Config Space Access

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  if (pcie_available(bus)) {
    return pcie_config_read32(bus, slot, func, offset);
  }
  if (offset > 255)
    return 0xFFFFFFFF;
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t value) {
  if (pcie_available(bus)) {
    pcie_config_write32(bus, slot, func, offset, value);
    return;
  }
  if (offset > 255)
    return;
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFFFC);
  return (uint16_t)(val >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t value) {
  uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFFFC);
  uint32_t mask = 0xFFFFu << ((offset & 2) * 8);
  val &= ~mask;
  val |= (uint32_t)value << ((offset & 2) * 8);
  pci_config_write32(bus, slot, func, offset & 0xFFFC, val);
}

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

static uint64_t pci_probe_bar_size(uint8_t bus, uint8_t slot, uint8_t func,
                                   uint8_t index, uint32_t raw) {
  uint16_t reg = (uint16_t)(0x10 + index * 4);
  bool io = (raw & 1U) != 0;
  bool wide = !io && (((raw >> 1) & 3U) == 2U) && index < 5;
  uint16_t command = pci_config_read16(bus, slot, func, 0x04);
  uint32_t high = wide ? pci_config_read32(bus, slot, func, reg + 4) : 0;
  pci_config_write16(bus, slot, func, 0x04, command & ~3U);
  pci_config_write32(bus, slot, func, reg, 0xffffffffU);
  if (wide)
    pci_config_write32(bus, slot, func, reg + 4, 0xffffffffU);
  uint32_t mask_low = pci_config_read32(bus, slot, func, reg);
  uint32_t mask_high =
      wide ? pci_config_read32(bus, slot, func, reg + 4) : 0;
  pci_config_write32(bus, slot, func, reg, raw);
  if (wide)
    pci_config_write32(bus, slot, func, reg + 4, high);
  pci_config_write16(bus, slot, func, 0x04, command);
  if (io) {
    uint32_t mask = mask_low & ~3U;
    return mask ? (uint64_t)(~mask + 1U) : 0;
  }
  uint64_t mask = wide ? ((uint64_t)mask_high << 32) | (mask_low & ~0xfU)
                       : (uint64_t)(mask_low & ~0xfU);
  return mask ? (wide ? ~mask + 1ULL
                      : (uint64_t)(~(uint32_t)mask + 1U))
              : 0;
}

// Device scanning

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
  uint32_t reg0 = pci_config_read32(bus, slot, func, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  uint16_t device_id = reg0 >> 16;

  if (vendor_id == 0xFFFF)
    return; // No device
  if (device_count >= PCI_MAX_DEVICES)
    return;

  uint32_t reg2 = pci_config_read32(bus, slot, func, 0x08);
  uint32_t reg3 = pci_config_read32(bus, slot, func, 0x0C);
  uint32_t reg_irq = pci_config_read32(bus, slot, func, 0x3C);

  struct pci_device *dev = &devices[device_count];
  dev->bus = bus;
  dev->slot = slot;
  dev->func = func;
  dev->vendor_id = vendor_id;
  dev->device_id = device_id;
  dev->class_code = (reg2 >> 24) & 0xFF;
  dev->subclass = (reg2 >> 16) & 0xFF;
  dev->prog_if = (reg2 >> 8) & 0xFF;
  dev->header_type = (reg3 >> 16) & 0xFF;
  dev->irq_line = reg_irq & 0xFF;

  // Name the device by its PCI bus:slot.function address (BDF).
  char dev_name[16];
  snprintf(dev_name, sizeof(dev_name), "0000:%02x:%02x.%x", bus, slot, func);

  struct device *seg_dev = device_find_by_path("/sys/pci/seg0");
  if (!seg_dev)
    seg_dev = device_find_by_path("/sys/pci"); // Fallback

  struct device *pci_node = device_create_on_bus(&pci_bus, seg_dev, dev_name);
  if (pci_node) {
    pci_node->vendor_id = vendor_id;
    pci_node->device_id = device_id;
    pci_node->pci_class = dev->class_code;
    pci_node->pci_subclass = dev->subclass;
    pci_node->pci_prog_if = dev->prog_if;
    dev->kernel_device = pci_node;
  }

  if ((dev->header_type & 0x7F) == 0x00) {
    for (int i = 0; i < 6; i++)
      dev->bar[i] = pci_config_read32(bus, slot, func, 0x10 + i * 4);
    for (int i = 0; i < 6; i++) {
      uint32_t bar = dev->bar[i];
      if (!bar || bar == 0xffffffffU)
        continue;
      bool io = (bar & 1U) != 0;
      bool wide = !io && (((bar >> 1) & 3U) == 2U) && i < 5;
      uint64_t start = io ? (bar & ~3U) : (bar & ~0xfU);
      if (wide)
        start |= (uint64_t)dev->bar[i + 1] << 32;
      uint64_t size = pci_probe_bar_size(bus, slot, func, (uint8_t)i, bar);
      char name[8];
      snprintf(name, sizeof(name), "bar%d", i);
      device_add_resource(pci_node, io ? RES_IO : RES_MEM, name, start,
                          size ? start + size - 1 : start);
      if (wide)
        i++;
    }
  }
  device_add_resource(pci_node, RES_IRQ, "irq", dev->irq_line, dev->irq_line);

  dm_probe_device(pci_node);

  device_count++;
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
  uint32_t reg0 = pci_config_read32(bus, slot, 0, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  if (vendor_id == 0xFFFF)
    return;

  pci_check_function(bus, slot, 0);

  // Check if multi-function device
  uint32_t reg3 = pci_config_read32(bus, slot, 0, 0x0C);
  uint8_t header_type = (reg3 >> 16) & 0xFF;
  if (header_type & 0x80) {
    for (uint8_t func = 1; func < 8; func++) {
      pci_check_function(bus, slot, func);
    }
  }
}

// Public API

void pci_init(void) {
  dm_register_bus(&pci_bus);
  struct device *sys_node = device_find_by_path("/sys");
  struct device *pci_root = device_find_by_path("/sys/pci");
  if (!pci_root)
    pci_root = device_create(sys_node, "pci");
  if (pci_root && !device_find_by_path("/sys/pci/seg0"))
    device_create(pci_root, "seg0");

  pcie_init();
  device_count = 0;
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Scanning PCI bus...\n");

  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      pci_check_device((uint8_t)bus, slot);
    }
  }

  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " PCI: Found ");
  print_uint32(device_count);
  console_puts(" device(s). Dumping Device Tree...\n");

  dm_dump_tree();
}

struct pci_device *pci_find_device(uint8_t class_code, uint8_t subclass) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].class_code == class_code &&
        devices[i].subclass == subclass) {
      return &devices[i];
    }
  }
  return NULL;
}

struct pci_device *pci_find_device_by_id(uint16_t vendor_id,
                                         uint16_t device_id) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].vendor_id == vendor_id &&
        devices[i].device_id == device_id) {
      return &devices[i];
    }
  }
  return NULL;
}

void pci_set_bus_mastering(struct pci_device *dev, bool enabled) {
  if (!dev)
    return;
  uint16_t command =
      pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
  if (enabled)
    command |= (1U << 2);
  else
    command &= ~(1U << 2);
  pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, command);
}

void pci_enable_bus_mastering(struct pci_device *dev) {
  pci_set_bus_mastering(dev, true);
}

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
  uint16_t status = pci_config_read16(dev->bus, dev->slot, dev->func, 0x06);
  if (!(status & (1 << 4)))
    return 0; // Capabilities bit not set

  uint8_t cap_ptr =
      pci_config_read32(dev->bus, dev->slot, dev->func, 0x34) & 0xFF;
  while (cap_ptr != 0) {
    uint32_t cap_reg =
        pci_config_read32(dev->bus, dev->slot, dev->func, cap_ptr);
    if ((cap_reg & 0xFF) == cap_id)
      return cap_ptr;
    cap_ptr = (cap_reg >> 8) & 0xFF;
  }
  return 0;
}

static uint64_t pci_bar_phys(struct pci_device *dev, uint8_t bir) {
  if (!dev || bir >= 6 || (dev->bar[bir] & 1U)) return 0;
  uint64_t phys = dev->bar[bir] & ~0xFULL;
  if (((dev->bar[bir] >> 1) & 3U) == 2U && bir < 5)
    phys |= (uint64_t)dev->bar[bir + 1] << 32;
  return phys;
}

bool pci_msix_init(struct pci_device *dev, struct pci_msix *msix) {
  if (!dev || !msix) return false;
  memset(msix, 0, sizeof(*msix));
  uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
  if (!cap) return false;
  uint16_t control = pci_config_read16(dev->bus, dev->slot, dev->func, cap + 2);
  uint32_t table_info = pci_config_read32(dev->bus, dev->slot, dev->func, cap + 4);
  uint8_t bir = table_info & 7U;
  uint64_t table_phys = pci_bar_phys(dev, bir) + (table_info & ~7U);
  uint16_t count = (control & 0x7FFU) + 1U;
  if (!table_phys || !count) return false;
  uint64_t first = table_phys & ~(PAGE_SIZE - 1ULL);
  uint64_t last = (table_phys + (uint64_t)count * 16U - 1U) & ~(PAGE_SIZE - 1ULL);
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | (1ULL << 3) | (1ULL << 4);
  for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
    uint64_t virt = page + hhdm;
    if (!vmm_virt_to_phys(pml4, virt) && !vmm_map_page(pml4, virt, page, flags))
      return false;
    vmm_flush_tlb(virt);
  }
  msix->dev = dev;msix->capability = cap;msix->table_size = count;
  msix->table = (volatile uint32_t *)(table_phys + hhdm);msix->initialized = true;
  /* Function-mask while callers populate entries. */
  pci_config_write16(dev->bus, dev->slot, dev->func, cap + 2,
                     (uint16_t)((control | (1U << 14)) & ~(1U << 15)));
  for (uint16_t i = 0; i < count; i++) msix->table[i * 4 + 3] = PCI_MSIX_VECTOR_MASK;
  return true;
}

bool pci_msix_program(struct pci_msix *msix, uint16_t entry,
                      uint8_t vector, uint8_t destination_apic) {
  if (!msix || !msix->initialized || entry >= msix->table_size || vector < 32)
    return false;
  volatile uint32_t *e = msix->table + entry * 4;
  e[3] = PCI_MSIX_VECTOR_MASK;
  e[0] = 0xFEE00000U | ((uint32_t)destination_apic << 12);
  e[1] = 0;e[2] = vector;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return true;
}

void pci_msix_mask(struct pci_msix *msix, uint16_t entry, bool masked) {
  if (!msix || !msix->initialized || entry >= msix->table_size) return;
  volatile uint32_t *control = msix->table + entry * 4 + 3;
  if (masked) *control |= PCI_MSIX_VECTOR_MASK; else *control &= ~PCI_MSIX_VECTOR_MASK;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

bool pci_msix_enable(struct pci_msix *msix) {
  if (!msix || !msix->initialized) return false;
  struct pci_device *d = msix->dev;
  uint16_t control = pci_config_read16(d->bus, d->slot, d->func, msix->capability + 2);
  control = (uint16_t)((control | (1U << 15)) & ~(1U << 14));
  pci_config_write16(d->bus, d->slot, d->func, msix->capability + 2, control);
  msix->enabled = true;return true;
}

void pci_msix_disable(struct pci_msix *msix) {
  if (!msix || !msix->initialized) return;
  struct pci_device *d = msix->dev;
  uint16_t control = pci_config_read16(d->bus, d->slot, d->func, msix->capability + 2);
  pci_config_write16(d->bus, d->slot, d->func, msix->capability + 2,
                     (uint16_t)((control & ~(1U << 15)) | (1U << 14)));
  msix->enabled = false;
}

bool pci_msi_enable(struct pci_device *dev, struct pci_msi *msi,
                    uint8_t vector, uint8_t destination_apic) {
  if (!dev || !msi || vector < 32)
    return false;
  uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
  if (!cap)
    return false;

  uint16_t control = pci_config_read16(dev->bus, dev->slot, dev->func, cap + 2);
  bool address_64 = (control & (1U << 7)) != 0;
  bool per_vector_mask = (control & (1U << 8)) != 0;
  uint16_t data_offset = address_64 ? 12 : 8;
  uint16_t mask_offset = address_64 ? 16 : 12;

  /* Program one fixed, edge-triggered message while delivery is disabled. */
  pci_config_write16(dev->bus, dev->slot, dev->func, cap + 2,
                     (uint16_t)(control & ~1U));
  pci_config_write32(dev->bus, dev->slot, dev->func, cap + 4,
                     0xFEE00000U | ((uint32_t)destination_apic << 12));
  if (address_64)
    pci_config_write32(dev->bus, dev->slot, dev->func, cap + 8, 0);
  pci_config_write16(dev->bus, dev->slot, dev->func, cap + data_offset,
                     vector);
  if (per_vector_mask) {
    uint32_t mask = pci_config_read32(dev->bus, dev->slot, dev->func,
                                      cap + mask_offset);
    pci_config_write32(dev->bus, dev->slot, dev->func, cap + mask_offset,
                       mask & ~1U);
  }

  control = (uint16_t)((control & ~(7U << 4)) | 1U);
  pci_config_write16(dev->bus, dev->slot, dev->func, cap + 2, control);
  msi->dev = dev;
  msi->capability = cap;
  msi->enabled = true;
  return true;
}

void pci_msi_disable(struct pci_msi *msi) {
  if (!msi || !msi->enabled || !msi->dev)
    return;
  struct pci_device *dev = msi->dev;
  uint16_t control = pci_config_read16(dev->bus, dev->slot, dev->func,
                                       msi->capability + 2);
  pci_config_write16(dev->bus, dev->slot, dev->func, msi->capability + 2,
                     (uint16_t)(control & ~1U));
  msi->enabled = false;
}

uint32_t pci_get_device_count(void) { return device_count; }

struct pci_device *pci_get_device(uint32_t index) {
  if (index >= device_count)
    return NULL;
  return &devices[index];
}
