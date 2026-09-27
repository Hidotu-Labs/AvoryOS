#ifndef PCI_PCIE_H
#define PCI_PCIE_H

#include <stdint.h>
#include <stdbool.h>

// Initialize PCIe support (finds MCFG)
void pcie_init(void);

// Check if PCIe is available for a given bus
bool pcie_available(uint8_t bus);

// ECAM based configuration space access
uint32_t pcie_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pcie_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t value);

uint16_t pcie_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pcie_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t value);

uint8_t pcie_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pcie_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t value);

#endif
