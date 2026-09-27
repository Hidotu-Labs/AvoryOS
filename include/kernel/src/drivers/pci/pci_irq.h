#ifndef PCI_IRQ_H
#define PCI_IRQ_H

#include "cpu/isr.h"
#include "drivers/pci/pci.h"

#define PCI_IRQ_MAX_VECTORS 32

enum pci_irq_mode {
  PCI_IRQ_NONE = 0,
  PCI_IRQ_MSI,
  PCI_IRQ_MSIX,
  PCI_IRQ_INTX,
};

/* Requested delivery modes for pci_irq_request_modes(). */
#define PCI_IRQ_MODE_MSIX (1u << 0)
#define PCI_IRQ_MODE_MSI (1u << 1)
#define PCI_IRQ_MODE_INTX (1u << 2)
#define PCI_IRQ_MODE_ALL                                                       \
  (PCI_IRQ_MODE_MSIX | PCI_IRQ_MODE_MSI | PCI_IRQ_MODE_INTX)

struct pci_irq {
  struct pci_device *dev;
  enum pci_irq_mode mode;
  uint16_t vector_count;
  uint8_t vectors[PCI_IRQ_MAX_VECTORS];
  struct pci_msi msi;
  struct pci_msix msix;
  uint16_t saved_command;
  bool command_saved;
  /* INTx fallback bookkeeping (only valid when mode == PCI_IRQ_INTX). */
  isr_t handler;
  uint8_t irq_line;
  bool intx_installed;
};

/* Allocates CPU vectors, installs handlers, and routes them to dest_apic.
 * MSI-X is preferred. A one-vector request falls back to MSI. */
bool pci_irq_request(struct pci_device *dev, struct pci_irq *irq,
                     const isr_t *handlers, uint16_t count,
                     uint8_t destination_apic);

/*
 * Same as pci_irq_request() but restricts delivery to `modes` (a mask of
 * PCI_IRQ_MODE_*).  The INTx bit is only honored for a single vector; when all
 * message modes are excluded or fail, the device's legacy INTx line is routed
 * through the I/O APIC.  Returns false when no requested mode works, leaving
 * the caller to fall back to polling.
 */
bool pci_irq_request_modes(struct pci_device *dev, struct pci_irq *irq,
                           const isr_t *handlers, uint16_t count,
                           uint8_t destination_apic, uint32_t modes);

/*
 * Like pci_irq_request_modes() but each MSI-X vector can target its own CPU:
 * destinations[i] is the destination APIC ID for handlers[i].  This is what
 * lets a multi-queue driver deliver queue N's completions to CPU N.  The
 * single-vector fallbacks (MSI, INTx) use destinations[0].
 */
bool pci_irq_request_modes_routed(struct pci_device *dev, struct pci_irq *irq,
                                  const isr_t *handlers,
                                  const uint8_t *destinations, uint16_t count,
                                  uint32_t modes);

bool pci_irq_mask(struct pci_irq *irq, uint16_t index, bool masked);
void pci_irq_release(struct pci_irq *irq);

static inline uint8_t pci_irq_vector(const struct pci_irq *irq,
                                     uint16_t index) {
  return irq && index < irq->vector_count ? irq->vectors[index] : 0xFF;
}

#endif
