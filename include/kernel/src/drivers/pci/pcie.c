#include "acpi/acpi.h"
#include "drivers/manager/device.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "console/console.h"
#include <stddef.h>

static struct acpi_mcfg *mcfg = NULL;

static void print_hex32(uint32_t val) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        console_putchar(hex[(val >> i) & 0xF]);
    }
}

static void print_hex64(uint64_t val) {
    print_hex32((uint32_t)(val >> 32));
    print_hex32((uint32_t)val);
}

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

void pcie_init(void) {
    mcfg = acpi_get_mcfg();
    if (mcfg) {
        struct device *sys_node = device_find_by_path("/sys");
        struct device *pci_root = device_find_by_path("/sys/pci");
        if (!pci_root)
            pci_root = device_create(sys_node, "pci");

        size_t entries = (mcfg->header.length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_entry);
        for (size_t i = 0; i < entries; i++) {
            struct acpi_mcfg_entry *entry = &mcfg->entries[i];
            
            char seg_name[16];
            // Format: seg0, seg1...
            strcpy(seg_name, "seg0"); // Simplified

            struct device *seg_dev = device_find_by_path("/sys/pci/seg0");
            if (!seg_dev)
                seg_dev = device_create(pci_root, seg_name);
            device_add_resource(seg_dev, RES_MEM, "ecam", entry->base_address, 
                                entry->base_address + ((entry->end_bus_number - entry->start_bus_number + 1) << 20));
            
            console_puts("     Segment ");
            print_uint32(entry->pci_segment_group_number);
            console_puts(": Bus ");
            print_uint32(entry->start_bus_number);
            console_puts("-");
            print_uint32(entry->end_bus_number);
            console_puts(" at 0x");
            print_hex64(entry->base_address);
            console_puts("\n");
        }
    } else {
        console_puts("[INFO] PCIe: MCFG table not found, falling back to legacy PCI\n");
    }
}

bool pcie_available(uint8_t bus) {
    if (!mcfg) return false;

    size_t entries = (mcfg->header.length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_entry);
    for (size_t i = 0; i < entries; i++) {
        if (bus >= mcfg->entries[i].start_bus_number && bus <= mcfg->entries[i].end_bus_number) {
            return true;
        }
    }
    return false;
}

static void *pcie_get_config_addr(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint16_t offset) {
    if (!mcfg || slot >= 32 || func >= 8 || offset >= 4096)
        return NULL;

    size_t entries =
        (mcfg->header.length - sizeof(struct acpi_mcfg)) /
        sizeof(struct acpi_mcfg_entry);
    for (size_t i = 0; i < entries; i++) {
        struct acpi_mcfg_entry *entry = &mcfg->entries[i];
        if (entry->pci_segment_group_number != 0 ||
            bus < entry->start_bus_number || bus > entry->end_bus_number)
            continue;

        /* ECAM bus offsets are relative to this allocation's start bus, not
         * absolute PCI bus numbers. Using bus<<20 breaks firmware which splits
         * segment zero into multiple MCFG allocations. */
        uint64_t addr = entry->base_address;
        addr += (uint64_t)(bus - entry->start_bus_number) << 20;
        addr += (uint64_t)slot << 15;
        addr += (uint64_t)func << 12;
        addr += offset;
        return (void *)(addr + pmm_get_hhdm_offset());
    }
    return NULL;
}

uint32_t pcie_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (!addr) return 0xFFFFFFFF;
    return *(volatile uint32_t *)addr;
}

void pcie_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t value) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (addr) {
        *(volatile uint32_t *)addr = value;
    }
}

uint16_t pcie_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (!addr) return 0xFFFF;
    return *(volatile uint16_t *)addr;
}

void pcie_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t value) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (addr) {
        *(volatile uint16_t *)addr = value;
    }
}

uint8_t pcie_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (!addr) return 0xFF;
    return *(volatile uint8_t *)addr;
}

void pcie_config_write8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t value) {
    void *addr = pcie_get_config_addr(bus, slot, func, offset);
    if (addr) {
        *(volatile uint8_t *)addr = value;
    }
}
