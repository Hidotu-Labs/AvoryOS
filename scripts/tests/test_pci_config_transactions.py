#!/usr/bin/env python3
"""Test native PCI access widths, W1C neighbors and concurrent CF8 use."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/drivers/pci/pci.c').read_text()
start = source.index('static spinlock_t pci_config_lock')
accessors = source[start:source.index('// Helpers', start)]

support = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static _Thread_local bool locked;
static void spinlock_acquire(spinlock_t *l) { assert(!locked); pthread_mutex_lock(l); locked = true; }
static void spinlock_release(spinlock_t *l) { assert(locked); locked = false; pthread_mutex_unlock(l); }
static uint8_t config[2][4096];
static uint32_t latch;
static int reads, writes, last_width;
static bool ecam;
static bool pcie_available(uint8_t bus) { (void)bus; return ecam; }
static uint32_t read_config(int slot, int offset, int width) {
    uint32_t value = 0; reads++; last_width = width;
    memcpy(&value, &config[slot][offset], width); return value;
}
static void write_config(int slot, int offset, int width, uint32_t value) {
    writes++; last_width = width;
    for (int i = 0; i < width; i++) {
        uint8_t byte = value >> (i * 8);
        /* PCI Status (0x06) and PCIe Device Status (0x4a) are W1C. */
        if (offset + i == 6 || offset + i == 7 || offset + i == 0x4a || offset + i == 0x4b)
            config[slot][offset + i] &= ~byte;
        else config[slot][offset + i] = byte;
    }
}
static int selected_slot(void) { return (latch >> 11) & 31; }
static int selected_offset(int port) { return (latch & 0xfc) + port - PCI_CONFIG_DATA; }
static void outl(uint16_t port, uint32_t value) {
    assert(locked);
    if (port == PCI_CONFIG_ADDRESS) { latch = value; sched_yield(); }
    else write_config(selected_slot(), selected_offset(port), 4, value);
}
static void outw(uint16_t port, uint16_t value) { assert(locked); write_config(selected_slot(), selected_offset(port), 2, value); }
static void outb(uint16_t port, uint8_t value) { assert(locked); write_config(selected_slot(), selected_offset(port), 1, value); }
static uint32_t inl(uint16_t port) { assert(locked); return read_config(selected_slot(), selected_offset(port), 4); }
static uint16_t inw(uint16_t port) { assert(locked); return read_config(selected_slot(), selected_offset(port), 2); }
static uint8_t inb(uint16_t port) { assert(locked); return read_config(selected_slot(), selected_offset(port), 1); }
#define ECAM_ACCESS(bits, bytes) \
static uint##bits##_t pcie_config_read##bits(uint8_t b, uint8_t s, uint8_t f, uint16_t o) { \
    (void)b; (void)f; assert(!locked); return read_config(s, o, bytes); } \
static void pcie_config_write##bits(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint##bits##_t v) { \
    (void)b; (void)f; assert(!locked); write_config(s, o, bytes, v); }
ECAM_ACCESS(32, 4)
ECAM_ACCESS(16, 2)
ECAM_ACCESS(8, 1)
'''
tests = r'''
static void *worker(void *arg) {
    int slot = (int)(uintptr_t)arg;
    for (int i = 0; i < 10000; i++) {
        uint32_t value = (slot + 1) * 0x10000 + i;
        pci_config_write32(0, slot, 0, 0x80, value);
        assert(pci_config_read32(0, slot, 0, 0x80) == value);
    }
    return NULL;
}
int main(void) {
    for (int mode = 0; mode < 2; mode++) {
        ecam = mode;
        memset(config, 0, sizeof(config)); reads = writes = 0;
        config[0][6] = config[0][7] = 0xff;
        pci_config_write16(0, 0, 0, 4, 7); /* enable command */
        assert(config[0][6] == 0xff && config[0][7] == 0xff);
        assert(reads == 0 && writes == 1 && last_width == 2);
        assert(pci_config_read16(0, 0, 0, 4) == 7 && last_width == 2);
        config[0][0x4a] = config[0][0x4b] = 0xff;
        pci_config_write16(0, 0, 0, 0x48, 0x1234);
        assert(config[0][0x4a] == 0xff && config[0][0x4b] == 0xff);
        int before = reads;
        pci_config_write8(0, 0, 0, 0x49, 0x56);
        assert(reads == before && last_width == 1 && config[0][0x48] == 0x34);
        assert(pci_config_read8(0, 0, 0, 0x49) == 0x56 && last_width == 1);
        pci_config_write16(0, 0, 0, 6, 0x0001); /* clear only selected W1C bit */
        assert(config[0][6] == 0xfe && config[0][7] == 0xff);
        if (ecam) {
            pci_config_write8(0, 0, 0, 0x303, 0xa5);
            assert(pci_config_read8(0, 0, 0, 0x303) == 0xa5);
            pci_config_write16(0, 0, 0, 0x302, 0x1234);
            assert(pci_config_read16(0, 0, 0, 0x302) == 0x1234);
        } else {
            int total = reads + writes;
            pci_config_write8(0, 0, 0, 0x303, 0xa5);
            assert(pci_config_read8(0, 0, 0, 0x303) == 0xff);
            pci_config_write16(0, 0, 0, 3, 0);
            assert(pci_config_read16(0, 0, 0, 3) == 0xffff);
            assert(reads + writes == total);
        }
    }
    ecam = false;
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, worker, (void *)0));
    assert(!pthread_create(&b, NULL, worker, (void *)1));
    pthread_join(a, NULL); pthread_join(b, NULL);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avory-pci-transactions-') as tmp:
    path = Path(tmp)
    test = path / 'test.c'
    test.write_text(support + accessors + tests)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(test), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    # Recreate the old ECAM word-to-dword RMW as a negative control.
    widened = accessors.replace(
        'pcie_config_write16(bus, slot, func, offset, value);',
        "uint32_t old = pcie_config_read32(bus, slot, func, offset & ~3u);\n"
        "    unsigned shift = (offset & 2) * 8;\n"
        "    pcie_config_write32(bus, slot, func, offset & ~3u,\n"
        "                        (old & ~(0xffffu << shift)) | ((uint32_t)value << shift));")
    assert widened != accessors
    test.write_text(support + widened + tests)
    subprocess.run(['cc', '-O2', '-pthread', str(test), '-o', str(path / 'old')], check=True)
    result = subprocess.run([str(path / 'old')], capture_output=True)
    assert result.returncode != 0, 'negative control passed'
    assert b'config[0][6] == 0xff' in result.stderr, result.stderr
print('PASS: native byte/word/dword widths, neighboring W1C status, extended ECAM and concurrent CF8 transactions; old implementation fails')
