#ifndef LINUXKPI_NATIVE_PCI_H
#define LINUXKPI_NATIVE_PCI_H

/* Native PCI bridge for LinuxKPI implementation files.
 *
 * The native PCI layer (kernel/src/drivers/pci) is compiled with native
 * headers; these entry points let linuxkpi/src/pci.c build `struct pci_dev`
 * wrappers over the enumerated native `struct pci_device` table without
 * pulling native types into the Linux API translation units.
 *
 * Only compiler builtin types are used so this header can be included next to
 * the real Linux headers; the native device is an opaque handle.
 */

/* Snapshot of one native device, decoded once per wrapper at scan time. */
struct linuxkpi_pci_info {
  unsigned char bus;
  unsigned char slot;
  unsigned char func;
  unsigned char class_code;
  unsigned char subclass;
  unsigned char prog_if;
  unsigned char revision;
  unsigned char header_type;
  unsigned char irq_line;
  unsigned char irq_pin;
  unsigned short vendor_id;
  unsigned short device_id;
  unsigned short subsystem_vendor;
  unsigned short subsystem_device;
  unsigned int bar_raw[6];
  unsigned long long bar_phys[6];
  unsigned long long bar_size[6]; /* 0 on the high half of a 64-bit BAR */
};

/* Number of enumerated native devices. */
int linuxkpi_pci_native_count(void);

/* Opaque handle for device `index` (NULL when out of range). */
void *linuxkpi_pci_native_get(int index);

/* Fill `out` with the device's identity, BARs and sizes. */
void linuxkpi_pci_native_snapshot(void *handle, struct linuxkpi_pci_info *out);

/* Configuration space access (ECAM offsets above 0xFF are supported on
 * PCIe-capable buses by the native layer). */
unsigned char linuxkpi_pci_config_read8(void *handle, unsigned short offset);
unsigned short linuxkpi_pci_config_read16(void *handle, unsigned short offset);
unsigned int linuxkpi_pci_config_read32(void *handle, unsigned short offset);
void linuxkpi_pci_config_write8(void *handle, unsigned short offset,
                                unsigned char value);
void linuxkpi_pci_config_write16(void *handle, unsigned short offset,
                                 unsigned short value);
void linuxkpi_pci_config_write32(void *handle, unsigned short offset,
                                 unsigned int value);

/* Capability lookup and command-register helpers. */
unsigned char linuxkpi_pci_find_capability(void *handle, unsigned char cap_id);
void linuxkpi_pci_set_bus_mastering(void *handle, int enabled);
unsigned short linuxkpi_pci_command_read(void *handle);
void linuxkpi_pci_command_update(void *handle, unsigned short set,
                                 unsigned short clear);

#endif /* LINUXKPI_NATIVE_PCI_H */
