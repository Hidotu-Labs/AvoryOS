#include "drivers/pci/pci.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pcie.h"
#include "io/io.h"
#include "lock/spinlock.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include <stddef.h>
static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;
static struct bus_type pci_bus = {.name = "pci"};

struct bus_type *asc_pci_bus_type(void) { return &pci_bus; }

/* Keep the requested transaction width: PCI status registers contain
 * write-one-to-clear bits, so widening a byte/word write into a dword RMW
 * changes neighboring registers. CF8/CFC also share one address latch across
 * CPUs and IRQ handlers; hold the native IRQ-saving lock for each transfer.
 */
static spinlock_t pci_config_lock = SPINLOCK_INIT;

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t func,
                                   uint16_t offset) {
  return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
         ((uint32_t)func << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  if (pcie_available(bus))
    return pcie_config_read32(bus, slot, func, offset);
  if (offset > 255 || slot >= 32 || func >= 8 || (offset & 3))
    return 0xFFFFFFFF;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  uint32_t value = inl(PCI_CONFIG_DATA);
  spinlock_release(&pci_config_lock);
  return value;
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t value) {
  if (pcie_available(bus)) {
    pcie_config_write32(bus, slot, func, offset, value);
    return;
  }
  if (offset > 255 || slot >= 32 || func >= 8 || (offset & 3))
    return;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  outl(PCI_CONFIG_DATA, value);
  spinlock_release(&pci_config_lock);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  if (pcie_available(bus))
    return pcie_config_read16(bus, slot, func, offset);
  if (offset > 255 || slot >= 32 || func >= 8 || (offset & 1))
    return 0xFFFF;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  uint16_t value = inw(PCI_CONFIG_DATA + (offset & 2));
  spinlock_release(&pci_config_lock);
  return value;
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t value) {
  if (pcie_available(bus)) {
    pcie_config_write16(bus, slot, func, offset, value);
    return;
  }
  if (offset > 255 || slot >= 32 || func >= 8 || (offset & 1))
    return;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  outw(PCI_CONFIG_DATA + (offset & 2), value);
  spinlock_release(&pci_config_lock);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  if (pcie_available(bus))
    return pcie_config_read8(bus, slot, func, offset);
  if (offset > 255 || slot >= 32 || func >= 8)
    return 0xFF;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  uint8_t value = inb(PCI_CONFIG_DATA + (offset & 3));
  spinlock_release(&pci_config_lock);
  return value;
}

void pci_config_write8(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint8_t value) {
  if (pcie_available(bus)) {
    pcie_config_write8(bus, slot, func, offset, value);
    return;
  }
  if (offset > 255 || slot >= 32 || func >= 8)
    return;
  spinlock_acquire(&pci_config_lock);
  outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
  outb(PCI_CONFIG_DATA + (offset & 3), value);
  spinlock_release(&pci_config_lock);
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
  memset(dev, 0, sizeof(*dev)); /* re-enumeration must not keep stale BARs */
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
      dev->bar_size[i] = size;
      if (wide) {
        dev->bar_size[i + 1] = 0; /* high dword of a 64-bit BAR */
        i++;
      }
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

uint8_t asc_pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
  if (!dev)
    return 0;
  uint16_t status = pci_config_read16(dev->bus, dev->slot, dev->func, 0x06);
  if (!(status & (1 << 4)))
    return 0; // Capabilities bit not set

  uint8_t header = dev->header_type & 0x7f;
  if (header > 2)
    return 0;
  uint8_t cap_ptr = pci_config_read8(dev->bus, dev->slot, dev->func,
                                    header == 2 ? 0x14 : 0x34);
  for (unsigned int remaining = 48; remaining && cap_ptr >= 0x40; remaining--) {
    cap_ptr &= ~3u;
    uint16_t cap_reg =
        pci_config_read16(dev->bus, dev->slot, dev->func, cap_ptr);
    if ((cap_reg & 0xff) == 0xff)
      break;
    if ((cap_reg & 0xFF) == cap_id)
      return cap_ptr;
    cap_ptr = (cap_reg >> 8) & 0xFF;
  }
  return 0;
}

uint64_t pci_bar_phys(struct pci_device *dev, uint8_t bir) {
  if (!dev || bir >= 6 || (dev->bar[bir] & 1U)) return 0;
  uint64_t phys = dev->bar[bir] & ~0xFULL;
  if (((dev->bar[bir] >> 1) & 3U) == 2U && bir < 5)
    phys |= (uint64_t)dev->bar[bir + 1] << 32;
  return phys;
}

void *pci_map_mmio(uint64_t phys, uint64_t length) {
  if (!phys || !length)
    return NULL;

  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t first = phys & ~0xFFFULL;
  uint64_t last = (phys + length - 1) & ~0xFFFULL;
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD |
                   PAGE_FLAG_PWT | PAGE_FLAG_NX;

  uint64_t page = first;
  while (page <= last) {
    uint64_t virt = page + hhdm;
    uint64_t remaining = (last - page) + PAGE_SIZE;

    /* 2 MB huge page optimization: if both phys and virt are 2 MB aligned
     * and at least 2 MB remains, attempt a huge page PDE mapping. */
    if ((page & 0x1FFFFFULL) == 0 &&
        (virt & 0x1FFFFFULL) == 0 &&
        remaining >= 0x200000ULL) {
      if (vmm_map_huge_page(pml4, virt, page, flags)) {
        page += 0x200000ULL;
        continue;
      }
    }

    /* 4 KB fallback / boundary mapping */
    if (!vmm_virt_to_phys(pml4, virt) &&
        !vmm_map_page(pml4, virt, page, flags)) {
      klogf("[PCI] Failed to map MMIO page phys=0x%llx virt=0x%llx\n",
            (unsigned long long)page, (unsigned long long)virt);
      return NULL;
    }
    page += PAGE_SIZE;
  }

  return (void *)(phys + hhdm);
}

void *pci_map_bar(struct pci_device *dev, int bar_idx, uint64_t *out_size) {
  if (!dev || bar_idx < 0 || bar_idx >= 6)
    return NULL;

  if (dev->bar[bar_idx] & 1U) {
    /* I/O port BAR, not MMIO */
    return NULL;
  }

  uint64_t phys = pci_bar_phys(dev, (uint8_t)bar_idx);
  if (!phys)
    return NULL;

  uint64_t size = dev->bar_size[bar_idx];
  if (!size) {
    size = pci_probe_bar_size(dev->bus, dev->slot, dev->func, (uint8_t)bar_idx,
                              dev->bar[bar_idx]);
    dev->bar_size[bar_idx] = size;
  }

  if (out_size)
    *out_size = size;

  if (!size)
    return NULL;

  /* Cap absurd BAR sizes to prevent unbounded loops */
  if (size > (1024ULL * 1024 * 1024)) {
    klogf("[PCI] Refusing unreasonable BAR%d size: 0x%llx\n",
          bar_idx, (unsigned long long)size);
    return NULL;
  }

  return pci_map_mmio(phys, size);
}

bool pci_msix_init(struct pci_device *dev, struct pci_msix *msix) {
  if (!dev || !msix) return false;
  memset(msix, 0, sizeof(*msix));
  uint8_t cap = asc_pci_find_capability(dev, PCI_CAP_ID_MSIX);
  if (!cap) return false;
  uint16_t control = pci_config_read16(dev->bus, dev->slot, dev->func, cap + 2);
  uint32_t table_info = pci_config_read32(dev->bus, dev->slot, dev->func, cap + 4);
  uint8_t bir = table_info & 7U;
  uint64_t table_phys = pci_bar_phys(dev, bir) + (table_info & ~7U);
  uint16_t count = (control & 0x7FFU) + 1U;
  if (!table_phys || !count) return false;
  if (!pci_map_mmio(table_phys, (uint64_t)count * 16U))
    return false;
  uint64_t hhdm = pmm_get_hhdm_offset();
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
  uint8_t cap = asc_pci_find_capability(dev, PCI_CAP_ID_MSI);
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

struct pci_device *asc_pci_get_device(uint32_t index) {
  if (index >= device_count)
    return NULL;
  return &devices[index];
}
