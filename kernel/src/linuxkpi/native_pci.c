/* Native PCI bridge implementation for LinuxKPI.
 *
 * Compiled with native headers only (see linuxkpi/native_pci.h for the
 * contract).  Everything here is a thin adapter over kernel/src/drivers/pci:
 * the LinuxKPI `struct pci_dev` wrappers are built from these snapshots so the
 * Linux side never sees a native `struct pci_device`. */

#include "linuxkpi/native_pci.h"

#include "drivers/pci/pci.h"
#include "lib/string.h"

#include <stdint.h>

int linuxkpi_pci_native_count(void) { return (int)pci_get_device_count(); }

void *linuxkpi_pci_native_get(int index) {
  if (index < 0)
    return NULL;
  return asc_pci_get_device((uint32_t)index);
}

static uint64_t native_bar_phys(const struct pci_device *dev, int bir) {
  uint32_t raw = dev->bar[bir];

  if (raw & 1u)
    return (uint64_t)(raw & ~3u);
  uint64_t phys = (uint64_t)(raw & ~0xfu);
  if (((raw >> 1) & 3u) == 2u && bir < 5)
    phys |= (uint64_t)dev->bar[bir + 1] << 32;
  return phys;
}

void linuxkpi_pci_native_snapshot(void *handle, struct linuxkpi_pci_info *out) {
  struct pci_device *dev = handle;
  uint32_t reg2, subsys, reg_irq;

  if (!dev || !out)
    return;
  memset(out, 0, sizeof(*out));

  /* The native struct caches the class fields; revision, subsystem IDs and
   * interrupt pin are read straight from config space. */
  reg2 = pci_config_read32(dev->bus, dev->slot, dev->func, 0x08);
  subsys = pci_config_read32(dev->bus, dev->slot, dev->func, 0x2C);
  reg_irq = pci_config_read32(dev->bus, dev->slot, dev->func, 0x3C);

  out->bus = dev->bus;
  out->slot = dev->slot;
  out->func = dev->func;
  out->vendor_id = dev->vendor_id;
  out->device_id = dev->device_id;
  out->class_code = dev->class_code;
  out->subclass = dev->subclass;
  out->prog_if = dev->prog_if;
  out->revision = (unsigned char)(reg2 & 0xFFu);
  out->header_type = dev->header_type;
  out->irq_line = dev->irq_line;
  out->irq_pin = (unsigned char)((reg_irq >> 8) & 0xFFu);
  out->subsystem_vendor = (unsigned short)(subsys & 0xFFFFu);
  out->subsystem_device = (unsigned short)(subsys >> 16);

  for (int i = 0; i < 6; i++) {
    out->bar_raw[i] = dev->bar[i];
    if (!dev->bar[i] || dev->bar[i] == 0xFFFFFFFFu)
      continue;
    out->bar_phys[i] = native_bar_phys(dev, i);
    out->bar_size[i] = dev->bar_size[i];
  }
}

#define NATIVE_DEV(handle) ((struct pci_device *)(handle))

unsigned char linuxkpi_pci_config_read8(void *handle, unsigned short offset) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (!dev)
    return 0xFF;
  return pci_config_read8(dev->bus, dev->slot, dev->func, offset);
}

unsigned short linuxkpi_pci_config_read16(void *handle, unsigned short offset) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (!dev)
    return 0xFFFF;
  return pci_config_read16(dev->bus, dev->slot, dev->func, offset);
}

unsigned int linuxkpi_pci_config_read32(void *handle, unsigned short offset) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (!dev)
    return 0xFFFFFFFFu;
  return pci_config_read32(dev->bus, dev->slot, dev->func, offset);
}

void linuxkpi_pci_config_write8(void *handle, unsigned short offset,
                                unsigned char value) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (dev)
    pci_config_write8(dev->bus, dev->slot, dev->func, offset, value);
}

void linuxkpi_pci_config_write16(void *handle, unsigned short offset,
                                 unsigned short value) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (dev)
    pci_config_write16(dev->bus, dev->slot, dev->func, offset, value);
}

void linuxkpi_pci_config_write32(void *handle, unsigned short offset,
                                 unsigned int value) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (dev)
    pci_config_write32(dev->bus, dev->slot, dev->func, offset, value);
}

unsigned char linuxkpi_pci_find_capability(void *handle, unsigned char cap_id) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (!dev)
    return 0;
  return asc_pci_find_capability(dev, cap_id);
}

void linuxkpi_pci_set_bus_mastering(void *handle, int enabled) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (dev)
    pci_set_bus_mastering(dev, enabled != 0);
}

unsigned short linuxkpi_pci_command_read(void *handle) {
  struct pci_device *dev = NATIVE_DEV(handle);

  if (!dev)
    return 0;
  return pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
}

void linuxkpi_pci_command_update(void *handle, unsigned short set,
                                 unsigned short clear) {
  struct pci_device *dev = NATIVE_DEV(handle);
  uint16_t command;

  if (!dev)
    return;
  command = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
  command = (uint16_t)((command | set) & ~clear);
  pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, command);
}
