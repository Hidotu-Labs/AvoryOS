#!/usr/bin/env python3
"""Run native and LinuxKPI capability walkers against simulated PCI config."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


native = Path(os.environ.get('PCI_NATIVE_SOURCE', ROOT/'kernel/src/drivers/pci/pci.c')).read_text()
kpi = Path(os.environ.get('PCI_KPI_SOURCE', ROOT/'kernel/linuxkpi/src/pci.c')).read_text()
support = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define PCIBIOS_SUCCESSFUL 0
struct pci_device { uint8_t bus, slot, func, header_type; };
struct pci_dev { int cfg_size; void *sysdata; };
static uint8_t config[4096];
static int reads, fail_offset = -1;
static uint32_t read_config(unsigned offset, unsigned width) {
    assert(offset + width <= sizeof(config));
    assert(++reads <= 600);
    uint32_t value = 0;
    memcpy(&value, config + offset, width);
    return value;
}
static uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    (void)bus; (void)slot; (void)func;
    return read_config(offset, 1);
}
static uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    (void)bus; (void)slot; (void)func;
    return read_config(offset, 2);
}
static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    (void)bus; (void)slot; (void)func;
    return read_config(offset, 4);
}
static int pci_read_config_byte(struct pci_dev *dev, int offset, u8 *value) {
    if (offset < 0 || offset >= dev->cfg_size || offset == fail_offset) return 1;
    *value = read_config(offset, 1); return 0;
}
static int pci_read_config_word(struct pci_dev *dev, int offset, u16 *value) {
    if (offset < 0 || offset + 2 > dev->cfg_size || (offset & 1) || offset == fail_offset) return 1;
    *value = read_config(offset, 2); return 0;
}
static int pci_read_config_dword(struct pci_dev *dev, int offset, u32 *value) {
    if (offset < 0 || offset + 4 > dev->cfg_size || (offset & 3) || offset == fail_offset) return 1;
    *value = read_config(offset, 4); return 0;
}
static void extended(unsigned offset, unsigned id, unsigned next) {
    uint32_t value = id | (1U << 16) | (next << 20);
    memcpy(config + offset, &value, 4);
}
'''
implementation = '\n'.join((
    function(native, 'uint8_t asc_pci_find_capability('),
    function(kpi, 'u8 pci_find_next_capability('),
    function(kpi, 'u16 pci_find_next_ext_capability('),
    function(kpi, 'u16 pci_find_ext_capability('),
))
tests = r'''
int main(void) {
    struct pci_device native = {0};
    struct pci_dev dev = {.cfg_size = 4096, .sysdata = &native};
    config[6] = 0x10; config[0x34] = 0x40;
    config[0x40] = 9; config[0x41] = 0x60; config[0x60] = 9;
    assert(pci_find_next_capability(&dev, 0x40, 9) == 0x60);
    reads = 0; assert(pci_find_next_capability(&dev, 0x60, 9) == 0);
    reads = 0; assert(asc_pci_find_capability(&native, 9) == 0x40);
    reads = 0; config[0x34] = 0x40; config[0x41] = 0x40;
    assert(!asc_pci_find_capability(&native, 5));
    reads = 0; config[0x34] = 0x43;
    assert(asc_pci_find_capability(&native, 9) == 0x40);
    reads = 0; config[0x40] = 0xff;
    assert(!asc_pci_find_capability(&native, 0xff));
    reads = 0; config[0x34] = 0x20;
    assert(!asc_pci_find_capability(&native, 5));
    reads = 0; assert(!asc_pci_find_capability(NULL, 5));
    reads = 0; native.header_type = 2; config[0x14] = 0x60;
    assert(asc_pci_find_capability(&native, 9) == 0x60);
    reads = 0; native.header_type = 3;
    assert(!asc_pci_find_capability(&native, 9));
    native.header_type = 0; config[6] = 0;
    assert(!asc_pci_find_capability(&native, 9)); config[6] = 0x10;
    reads = 0; config[0x34] = 0x40; config[0x40] = 5; config[0x41] = 0x60;
    config[0x60] = 9; config[0x61] = 0;
    fail_offset = 0x41; assert(!pci_find_next_capability(&dev, 0x40, 9));
    fail_offset = -1; reads = 0; config[0x41] = 0x20;
    assert(!pci_find_next_capability(&dev, 0x40, 9));
    reads = 0; config[0x41] = 0x60; config[0x61] = 0x40;
    assert(!pci_find_next_capability(&dev, 0x40, 7));
    reads = 0; extended(0x100, 0xb, 0x180); extended(0x180, 0xb, 0x300);
    extended(0x300, 0x15, 0);
    assert(pci_find_ext_capability(&dev, 0xb) == 0x100);
    assert(pci_find_next_ext_capability(&dev, 0x100, 0xb) == 0x180);
    assert(!pci_find_next_ext_capability(&dev, 0x180, 0xb));
    reads = 0; assert(pci_find_next_ext_capability(&dev, 0x180, 0x15) == 0x300);
    reads = 0; extended(0x100, 0xb, 0x100);
    assert(!pci_find_next_ext_capability(&dev, 0x100, 0xb));
    reads = 0; extended(0x100, 0xb, 0x180); extended(0x180, 0x15, 0x100);
    assert(!pci_find_ext_capability(&dev, 0x23));
    reads = 0; dev.cfg_size = 256; assert(!pci_find_ext_capability(&dev, 0xb) && !reads);
    dev.cfg_size = 4096; fail_offset = 0x100;
    assert(!pci_find_ext_capability(&dev, 0xb)); fail_offset = -1;
    reads = 0; memset(config + 0x100, 0xff, 4);
    assert(!pci_find_ext_capability(&dev, 0xffff));
}
'''
with tempfile.TemporaryDirectory(prefix='pci-capabilities-') as temporary:
    directory = Path(temporary)
    source = directory/'test.c'
    source.write_text(support + implementation + tests)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                    str(source), '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True, timeout=10)
print('PASS: successive capability matches, bounded malformed/cyclic lists, absent config, alignment and config read failures')
