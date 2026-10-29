#!/usr/bin/env python3
"""Exercise the native PCI MSI-to-INTx fallback and retained MSI controls."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "drivers/pci/pci_irq.h"

static struct pci_device device;
static uint16_t command, msi_control;
static bool vector_alloc_ok, msi_program_ok, intx_install_ok;
static unsigned vector_allocs, vector_frees, intx_installs, intx_uninstalls;
static unsigned msi_enables, msi_disables;

int interrupt_vector_alloc(isr_t handler) {
  assert(handler);
  vector_allocs++;
  return vector_alloc_ok ? 56 : -1;
}
void interrupt_vector_free(uint8_t vector) {
  assert(vector == 56);
  vector_frees++;
}
bool irq_install_handler(uint8_t line, isr_t handler, uint16_t flags) {
  assert(line == 11 && handler && flags == 0x000f);
  intx_installs++;
  return intx_install_ok;
}
void irq_uninstall_handler(uint8_t line, isr_t handler) {
  assert(line == 11 && handler);
  intx_uninstalls++;
}
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  (void)bus; (void)slot; (void)func;
  return offset == 4 ? command : offset == 0x52 ? msi_control : 0;
}
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t value) {
  (void)bus; (void)slot; (void)func;
  if (offset == 4) command = value;
  else if (offset == 0x52) msi_control = value;
}
bool pci_msix_init(struct pci_device *dev, struct pci_msix *msix) {
  (void)dev; (void)msix; return false;
}
bool pci_msix_program(struct pci_msix *msix, uint16_t entry,
                      uint8_t vector, uint8_t destination_apic) {
  (void)msix; (void)entry; (void)vector; (void)destination_apic; return false;
}
void pci_msix_mask(struct pci_msix *msix, uint16_t entry, bool masked) {
  (void)msix; (void)entry; (void)masked;
}
bool pci_msix_enable(struct pci_msix *msix) { (void)msix; return false; }
void pci_msix_disable(struct pci_msix *msix) { (void)msix; }
bool pci_msi_enable(struct pci_device *dev, struct pci_msi *msi,
                    uint8_t vector, uint8_t destination_apic) {
  (void)vector; (void)destination_apic;
  msi_enables++;
  if (!msi_program_ok) return false;
  msi->dev = dev; msi->capability = 0x50; msi->enabled = true;
  msi_control |= 1;
  return true;
}
void pci_msi_disable(struct pci_msi *msi) {
  msi_disables++;
  if (msi->dev) { msi_control &= ~1u; msi->enabled = false; }
}

static void handler(struct registers *regs) { (void)regs; }
static void setup(void) {
  device = (struct pci_device){.bus=0,.slot=8,.func=0,.irq_line=11};
  command = 7; msi_control = 0;
  vector_alloc_ok = msi_program_ok = intx_install_ok = true;
  vector_allocs = vector_frees = intx_installs = intx_uninstalls = 0;
  msi_enables = msi_disables = 0;
}

int main(void) {
  struct pci_irq irq;
  const isr_t callback = handler;

  /* A programmed MSI failure frees its vector, then successfully falls back. */
  setup(); msi_program_ok = false;
  assert(pci_irq_request_modes(&device, &irq, &callback, 1, 0,
                               PCI_IRQ_MODE_MSI | PCI_IRQ_MODE_INTX));
  assert(irq.mode == PCI_IRQ_INTX && irq.intx_installed);
  assert(vector_allocs == 1 && vector_frees == 1);
  assert(msi_control == 0 && !(command & (1u << 10)));
  assert(intx_installs == 1 && !intx_uninstalls);

  /* Forced MSI failure does not install INTx or retain an allocated vector. */
  setup(); msi_program_ok = false;
  assert(!pci_irq_request_modes(&device, &irq, &callback, 1, 0,
                                PCI_IRQ_MODE_MSI));
  assert(irq.mode == PCI_IRQ_NONE && vector_allocs == 1 && vector_frees == 1);
  assert(!irq.intx_installed && !intx_installs && !intx_uninstalls);
  assert(msi_control == 0 && !(command & (1u << 10)));

  /* Vector allocation failure also falls back without leaking state. */
  setup(); vector_alloc_ok = false;
  assert(pci_irq_request_modes(&device, &irq, &callback, 1, 0,
                               PCI_IRQ_MODE_MSI | PCI_IRQ_MODE_INTX));
  assert(irq.mode == PCI_IRQ_INTX && vector_allocs == 1 && !vector_frees);
  assert(!msi_control && !(command & (1u << 10)));

  /* MSI disables legacy INTx, masks across inactivity, and release restores it. */
  setup();
  assert(pci_irq_request_modes(&device, &irq, &callback, 1, 0,
                               PCI_IRQ_MODE_MSI | PCI_IRQ_MODE_INTX));
  assert(irq.mode == PCI_IRQ_MSI && irq.vector_count == 1 &&
         pci_irq_vector(&irq, 0) == 56);
  assert((command & (1u << 10)) && (msi_control & 1));
  assert(pci_irq_mask(&irq, 0, true) && !(msi_control & 1));
  assert(pci_irq_mask(&irq, 0, false) && (msi_control & 1));
  pci_irq_release(&irq);
  assert(irq.mode == PCI_IRQ_NONE && vector_frees == 1);
  assert(!(command & (1u << 10)) && !(msi_control & 1));
  puts("PASS: native PCI MSI allocation/program failure cleanup, INTx fallback, forced failure and MSI masking");
}
'''

with tempfile.TemporaryDirectory(prefix='avory-pci-irq-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(source)
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'kernel/src'), str(path / 'test.c'),
        str(ROOT / 'kernel/src/drivers/pci/pci_irq.c'), '-o', str(path / 'test')
    ], check=True)
    subprocess.run([str(path / 'test')], check=True)
