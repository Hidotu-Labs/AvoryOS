#include "drivers/pci/pci_irq.h"
#include "cpu/irq.h"
#include "lib/string.h"

#define PCI_COMMAND_INTX_DISABLE (1U << 10)

static void disable_intx(struct pci_irq *irq) {
  struct pci_device *d = irq->dev;
  irq->saved_command = pci_config_read16(d->bus, d->slot, d->func, 0x04);
  irq->command_saved = true;
  pci_config_write16(d->bus, d->slot, d->func, 0x04,
                     irq->saved_command | PCI_COMMAND_INTX_DISABLE);
}

static void restore_intx(struct pci_irq *irq) {
  if (!irq->command_saved || !irq->dev) return;
  struct pci_device *d = irq->dev;
  uint16_t command = pci_config_read16(d->bus, d->slot, d->func, 0x04);
  command = (command & ~PCI_COMMAND_INTX_DISABLE) |
            (irq->saved_command & PCI_COMMAND_INTX_DISABLE);
  pci_config_write16(d->bus, d->slot, d->func, 0x04, command);
  irq->command_saved = false;
}

static void free_vectors(struct pci_irq *irq) {
  for (uint16_t i = 0; i < irq->vector_count; i++) {
    if (irq->vectors[i] != 0xFF) {
      interrupt_vector_free(irq->vectors[i]);
      irq->vectors[i] = 0xFF;
    }
  }
  irq->vector_count = 0;
}

static bool alloc_vectors(struct pci_irq *irq, const isr_t *handlers,
                          uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    int vector = interrupt_vector_alloc(handlers[i]);
    if (vector < 0) return false;
    irq->vectors[i] = (uint8_t)vector;
    irq->vector_count++;
  }
  return true;
}

static bool try_msix(struct pci_irq *irq, const isr_t *handlers,
                     const uint8_t *destinations, uint16_t count) {
  if (!pci_msix_init(irq->dev, &irq->msix) || irq->msix.table_size < count)
    return false;
  if (!alloc_vectors(irq, handlers, count)) goto fail;
  for (uint16_t i = 0; i < count; i++)
    if (!pci_msix_program(&irq->msix, i, irq->vectors[i], destinations[i]))
      goto fail;
  if (!pci_msix_enable(&irq->msix)) goto fail;
  for (uint16_t i = 0; i < count; i++) pci_msix_mask(&irq->msix, i, false);
  irq->mode = PCI_IRQ_MSIX;
  return true;
fail:
  pci_msix_disable(&irq->msix);
  free_vectors(irq);
  return false;
}

static bool try_msi(struct pci_irq *irq, const isr_t *handlers,
                    uint8_t destination_apic) {
  if (!alloc_vectors(irq, handlers, 1) ||
      !pci_msi_enable(irq->dev, &irq->msi, irq->vectors[0], destination_apic)) {
    free_vectors(irq);
    return false;
  }
  irq->mode = PCI_IRQ_MSI;
  return true;
}

/* Route the device's legacy INTx line to its handler.  Only meaningful for a
 * single vector: the line is shared and all handlers on it fire together. */
static bool try_intx(struct pci_irq *irq, isr_t handler) {
  struct pci_device *dev = irq->dev;
  if (!dev || dev->irq_line >= 16)
    return false;
  if (!irq_install_handler(dev->irq_line, handler, 0x000F))
    return false;
  irq->handler = handler;
  irq->irq_line = dev->irq_line;
  irq->intx_installed = true;
  irq->mode = PCI_IRQ_INTX;
  return true;
}

bool pci_irq_request_modes_routed(struct pci_device *dev, struct pci_irq *irq,
                                  const isr_t *handlers,
                                  const uint8_t *destinations, uint16_t count,
                                  uint32_t modes) {
  if (!dev || !irq || !handlers || !destinations || !count ||
      count > PCI_IRQ_MAX_VECTORS)
    return false;
  for (uint16_t i = 0; i < count; i++)
    if (!handlers[i]) return false;
  memset(irq, 0, sizeof(*irq));
  irq->dev = dev;
  for (uint16_t i = 0; i < PCI_IRQ_MAX_VECTORS; i++) irq->vectors[i] = 0xFF;

  if ((modes & PCI_IRQ_MODE_MSIX) &&
      try_msix(irq, handlers, destinations, count)) {
    disable_intx(irq);
    return true;
  }
  pci_msix_disable(&irq->msix);
  memset(&irq->msix, 0, sizeof(irq->msix));

  if ((modes & PCI_IRQ_MODE_MSI) && count == 1 &&
      try_msi(irq, handlers, destinations[0])) {
    disable_intx(irq);
    return true;
  }
  pci_msi_disable(&irq->msi);
  memset(&irq->msi, 0, sizeof(irq->msi));

  if ((modes & PCI_IRQ_MODE_INTX) && count == 1 && try_intx(irq, handlers[0]))
    return true;

  pci_irq_release(irq);
  return false;
}

bool pci_irq_request_modes(struct pci_device *dev, struct pci_irq *irq,
                           const isr_t *handlers, uint16_t count,
                           uint8_t destination_apic, uint32_t modes) {
  if (!handlers || !count || count > PCI_IRQ_MAX_VECTORS)
    return false;
  uint8_t destinations[PCI_IRQ_MAX_VECTORS];
  for (uint16_t i = 0; i < count; i++)
    destinations[i] = destination_apic;
  return pci_irq_request_modes_routed(dev, irq, handlers, destinations, count,
                                      modes);
}

bool pci_irq_request(struct pci_device *dev, struct pci_irq *irq,
                     const isr_t *handlers, uint16_t count,
                     uint8_t destination_apic) {
  return pci_irq_request_modes(dev, irq, handlers, count, destination_apic,
                               PCI_IRQ_MODE_MSIX | PCI_IRQ_MODE_MSI);
}

bool pci_irq_mask(struct pci_irq *irq, uint16_t index, bool masked) {
  if (!irq || index >= irq->vector_count) return false;
  if (irq->mode == PCI_IRQ_MSIX) {
    pci_msix_mask(&irq->msix, index, masked);
    return true;
  }
  if (irq->mode == PCI_IRQ_MSI && index == 0) {
    struct pci_device *d = irq->dev;
    uint16_t control = pci_config_read16(d->bus, d->slot, d->func,
                                         irq->msi.capability + 2);
    if (masked) control &= ~1U; else control |= 1U;
    pci_config_write16(d->bus, d->slot, d->func,
                       irq->msi.capability + 2, control);
    irq->msi.enabled = !masked;
    return true;
  }
  return false;
}

void pci_irq_release(struct pci_irq *irq) {
  if (!irq) return;
  if (irq->mode == PCI_IRQ_MSIX || irq->msix.initialized) {
    for (uint16_t i = 0; i < irq->vector_count; i++)
      pci_msix_mask(&irq->msix, i, true);
    pci_msix_disable(&irq->msix);
  } else if (irq->mode == PCI_IRQ_MSI || irq->msi.dev) {
    pci_msi_disable(&irq->msi);
  }
  if (irq->intx_installed) {
    irq_uninstall_handler(irq->irq_line, irq->handler);
    irq->intx_installed = false;
  }
  restore_intx(irq);
  free_vectors(irq);
  irq->mode = PCI_IRQ_NONE;
  irq->dev = NULL;
}
