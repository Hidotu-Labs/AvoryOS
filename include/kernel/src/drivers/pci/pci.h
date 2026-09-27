#ifndef PCI_PCI_H
#define PCI_PCI_H

#include <stdbool.h>
#include <stdint.h>

struct bus_type;
struct device;

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAX_DEVICES 256

struct pci_device {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t header_type;
  uint32_t bar[6];
  uint8_t irq_line;
  struct device *kernel_device;
};

// Read/write PCI configuration space
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t value);

// Initialize PCI and enumerate all devices
void pci_init(void);
struct bus_type *pci_bus_type(void);

// Find a device by class/subclass. Returns NULL if not found.
struct pci_device *pci_find_device(uint8_t class_code, uint8_t subclass);

// Find a device by vendor/device ID. Returns NULL if not found.
struct pci_device *pci_find_device_by_id(uint16_t vendor_id,
                                         uint16_t device_id);

// Enable or disable PCI bus-mastering for a device.
void pci_set_bus_mastering(struct pci_device *dev, bool enabled);
void pci_enable_bus_mastering(struct pci_device *dev);

// Find a capability in the PCI configuration space. Returns offset or 0 if not
// found.
uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);

// Get the number of discovered devices
uint32_t pci_get_device_count(void);

// Get device by index
struct pci_device *pci_get_device(uint32_t index);

#define PCI_CAP_ID_MSI  0x05
#define PCI_CAP_ID_MSIX 0x11
#define PCI_MSIX_VECTOR_MASK 1U

struct pci_msix {
  struct pci_device *dev;
  volatile uint32_t *table;
  uint16_t capability, table_size;
  bool initialized, enabled;
};

struct pci_msi {
  struct pci_device *dev;
  uint8_t capability;
  bool enabled;
};

bool pci_msix_init(struct pci_device *dev, struct pci_msix *msix);
bool pci_msix_program(struct pci_msix *msix, uint16_t entry,
                      uint8_t vector, uint8_t destination_apic);
void pci_msix_mask(struct pci_msix *msix, uint16_t entry, bool masked);
bool pci_msix_enable(struct pci_msix *msix);
void pci_msix_disable(struct pci_msix *msix);
bool pci_msi_enable(struct pci_device *dev, struct pci_msi *msi,
                    uint8_t vector, uint8_t destination_apic);
void pci_msi_disable(struct pci_msi *msi);

#endif
