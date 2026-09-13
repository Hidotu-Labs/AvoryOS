/* Native IRQ bridge for LinuxKPI (Phase 5 C2).
 *
 * Compiled with native headers only.  Owns the `isr_t` trampoline handed to
 * the native vector allocator and the PCI MSI/MSI-X layer, and wraps the
 * opaque native `struct pci_irq` state for linuxkpi/src/pci.c.  All decision
 * making (which vectors exist, which actions run) stays in the LinuxKPI core;
 * this file only translates types. */

#include "linuxkpi/native_irq.h"

#include "cpu/isr.h"
#include "cpu/irq.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/pci_irq.h"
#include "apic/lapic.h"
#include "mm/heap.h"

#include <stddef.h>

/* Reads the vector from the CPU frame and hands it to the LinuxKPI action
 * dispatcher.  `int_no` is the IDT vector for both MSI vectors and the
 * 32 + legacy-line vectors the native irq manager uses. */
static void linuxkpi_native_irq_trampoline(struct registers *regs) {
  linuxkpi_irq_handle_vector((unsigned int)(regs->int_no & 0xFFu));
}

int linuxkpi_native_irq_alloc(void) {
  return interrupt_vector_alloc(linuxkpi_native_irq_trampoline);
}

void linuxkpi_native_irq_free(int vector) {
  if (vector >= 0)
    interrupt_vector_free((uint8_t)vector);
}

void *linuxkpi_native_pci_irq_request(void *dev_handle, unsigned int count,
                                      unsigned int modes) {
  struct pci_device *dev = dev_handle;
  struct pci_irq *irq;
  isr_t handlers[PCI_IRQ_MAX_VECTORS];
  uint8_t destinations[PCI_IRQ_MAX_VECTORS];
  uint32_t native_modes = 0;
  uint8_t dest;

  if (!dev || !count || count > PCI_IRQ_MAX_VECTORS)
    return NULL;

  dest = (uint8_t)lapic_get_id();
  for (unsigned int i = 0; i < count; i++) {
    handlers[i] = linuxkpi_native_irq_trampoline;
    destinations[i] = dest;
  }
  if (modes & LINUXKPI_IRQ_MODE_MSIX)
    native_modes |= PCI_IRQ_MODE_MSIX;
  if (modes & LINUXKPI_IRQ_MODE_MSI)
    native_modes |= PCI_IRQ_MODE_MSI;
  if (modes & LINUXKPI_IRQ_MODE_INTX)
    native_modes |= PCI_IRQ_MODE_INTX;

  irq = kmalloc(sizeof(*irq));
  if (!irq)
    return NULL;
  if (!pci_irq_request_modes_routed(dev, irq, handlers, destinations,
                                    (uint16_t)count, native_modes)) {
    kfree(irq);
    return NULL;
  }
  return irq;
}

int linuxkpi_native_pci_irq_vector(void *state, unsigned int index) {
  struct pci_irq *irq = state;

  if (!irq)
    return -1;
  if (irq->mode == PCI_IRQ_INTX) {
    if (index != 0)
      return -1;
    /* The native legacy path routes line N to IDT vector 32 + N. */
    return 32 + (int)irq->irq_line;
  }
  if (index >= irq->vector_count || irq->vectors[index] == 0xFF)
    return -1;
  return (int)irq->vectors[index];
}

void linuxkpi_native_pci_irq_mask(void *state, unsigned int index, int masked) {
  struct pci_irq *irq = state;

  if (irq)
    pci_irq_mask(irq, (uint16_t)index, masked != 0);
}

void linuxkpi_native_pci_irq_release(void *state) {
  struct pci_irq *irq = state;

  if (!irq)
    return;
  pci_irq_release(irq);
  kfree(irq);
}
