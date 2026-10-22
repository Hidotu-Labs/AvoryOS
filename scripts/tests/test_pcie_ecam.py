#!/usr/bin/env python3
"""Exercise actual ECAM translation with split MCFG and invalid accesses."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('PCIE_SOURCE', ROOT/'kernel/src/drivers/pci/pcie.c')).read_text()
start = source.find('static struct acpi_mcfg_entry *pcie_find_mcfg(')
if start < 0:
    start = source.index('bool pcie_available(')
implementation = source[start:]
support = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct acpi_mcfg_entry {
    uint64_t base_address;
    uint16_t pci_segment_group_number;
    uint8_t start_bus_number, end_bus_number;
};
struct acpi_mcfg { struct { uint32_t length; } header; struct acpi_mcfg_entry entries[]; };
static struct acpi_mcfg *mcfg;
static uint64_t pmm_get_hhdm_offset(void) { return 0x100000000ULL; }
'''
tests = r'''
int main(void) {
    struct { struct { uint32_t length; } header; struct acpi_mcfg_entry entries[2]; } table = {0};
    mcfg = (void *)&table;
    table.header.length = sizeof(struct acpi_mcfg) + sizeof(table.entries);
    table.entries[0] = (struct acpi_mcfg_entry){0xe0000000, 0, 0, 0x3f};
    table.entries[1] = (struct acpi_mcfg_entry){0xe0000000, 0, 0x40, 0x7f};
    assert(pcie_available(0) && pcie_available(0x40) && !pcie_available(0x80));
    assert((uintptr_t)pcie_get_config_addr(0x40, 3, 1, 0xabc, 1) ==
           0x100000000ULL + 0xe0000000ULL + (0x40ULL << 20) + (3 << 15) + (1 << 12) + 0xabc);
    assert((uintptr_t)pcie_get_config_addr(0, 0, 0, 0x100, 4) == 0x1e0000100ULL);
    assert(pcie_config_read32(0, 0, 0, 0xffd) == UINT32_MAX);
    assert(pcie_config_read16(0, 0, 0, 0xfff) == UINT16_MAX);
    assert(pcie_config_read32(0, 0, 0, 2) == UINT32_MAX);
    assert(pcie_config_read16(0, 0, 0, 3) == UINT16_MAX);
    pcie_config_write32(0, 0, 0, 2, 0);
    pcie_config_write16(0, 0, 0, 3, 0);
    pcie_config_write32(0, 0, 0, 0xffd, 0);
    pcie_config_write16(0, 0, 0, 0xfff, 0);
    assert(!pcie_get_config_addr(0, 32, 0, 0, 1));
    assert(!pcie_get_config_addr(0, 0, 8, 0, 1));
    assert(!pcie_get_config_addr(0, 0, 0, 4096, 1));
    table.entries[1].pci_segment_group_number = 1;
    assert(!pcie_available(0x40));
    assert(!pcie_get_config_addr(0x40, 0, 0, 0, 1));
    table.entries[0].pci_segment_group_number = 1;
    assert(!pcie_available(0));
    uint32_t live_config[1024] = {0};
    table.entries[0].pci_segment_group_number = 0;
    table.entries[0].base_address = (uintptr_t)live_config - pmm_get_hhdm_offset();
    pcie_config_write32(0, 0, 0, 0xffc, 0x12345678);
    assert(pcie_config_read32(0, 0, 0, 0xffc) == 0x12345678);
    pcie_config_write16(0, 0, 0, 0xffe, 0xabcd);
    assert(pcie_config_read16(0, 0, 0, 0xffe) == 0xabcd);
    pcie_config_write8(0, 0, 0, 0xfff, 0xef);
    assert(pcie_config_read8(0, 0, 0, 0xfff) == 0xef);
    assert(pcie_config_read32(0, 0, 0, 0xffc) == 0xefcd5678);
    table.header.length = sizeof(struct acpi_mcfg) - 1;
    assert(!pcie_available(0) && !pcie_get_config_addr(0, 0, 0, 0, 1));
    mcfg = NULL;
    assert(!pcie_available(0));
    assert(pcie_config_read8(0, 0, 0, 0) == UINT8_MAX);
}
'''
if 'unsigned int width)' not in implementation:
    tests = tests.replace(', 0xabc, 1)', ', 0xabc)').replace(', 0x100, 4)', ', 0x100)')
    for arguments in ('0, 32, 0, 0', '0, 0, 8, 0', '0, 0, 0, 4096',
                      '0x40, 0, 0, 0', '0, 0, 0, 0'):
        tests = tests.replace(f'pcie_get_config_addr({arguments}, 1)', f'pcie_get_config_addr({arguments})')
with tempfile.TemporaryDirectory(prefix='pcie-ecam-') as temporary:
    directory = Path(temporary)
    program = directory/'test.c'
    program.write_text(support + implementation + tests)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(program), '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True, timeout=10)
print('PASS: absolute MCFG bus addressing, segment isolation, malformed table rejection and width/alignment bounds')
