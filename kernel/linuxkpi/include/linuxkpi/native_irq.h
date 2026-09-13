#ifndef LINUXKPI_NATIVE_IRQ_H
#define LINUXKPI_NATIVE_IRQ_H

/* Native IRQ-state bridge (kernel/src/hal/hal.h via asm aliases). */

extern unsigned long asc_irq_save(void) __asm__("hal_irq_save");
extern void asc_irq_restore(unsigned long flags) __asm__("hal_irq_restore");
extern void asc_irq_disable(void) __asm__("hal_irq_disable");
extern void asc_irq_enable(void) __asm__("hal_irq_enable");
extern _Bool asc_irq_enabled(void) __asm__("hal_irq_enabled");

/* Current RFLAGS (without modifying the IF state). */
unsigned long linuxkpi_irq_flags(void);

/* ── IRQ vector / MSI bridging (Phase 5 C2) ─────────────────────────────── */

/* Delivers an interrupt to the LinuxKPI action chain (linuxkpi/src/irq.c).
 * The native trampoline below reads the vector from struct registers and
 * calls this; the PCI bridge registers the trampoline for every vector. */
void linuxkpi_irq_handle_vector(unsigned int vector);

/* Mask callbacks for enable_irq()/disable_irq(); the PCI layer registers one
 * per allocated vector. */
void linuxkpi_irq_register_mask(unsigned int irq,
                                void (*fn)(void *data, int masked), void *data);
void linuxkpi_irq_unregister_mask(unsigned int irq);

/* Raw vector allocation for non-PCI users. */
int linuxkpi_native_irq_alloc(void);
void linuxkpi_native_irq_free(int vector);

/* PCI MSI/MSI-X allocation over the native pci_irq layer.  `dev_handle` is
 * the opaque native handle from linuxkpi/native_pci.h; modes is a mask of
 * LINUXKPI_IRQ_MODE_*.  Returns an opaque native IRQ state or NULL. */
#define LINUXKPI_IRQ_MODE_MSIX (1u << 0)
#define LINUXKPI_IRQ_MODE_MSI (1u << 1)
#define LINUXKPI_IRQ_MODE_INTX (1u << 2)

void *linuxkpi_native_pci_irq_request(void *dev_handle, unsigned int count,
                                      unsigned int modes);
int linuxkpi_native_pci_irq_vector(void *state, unsigned int index);
void linuxkpi_native_pci_irq_mask(void *state, unsigned int index, int masked);
void linuxkpi_native_pci_irq_release(void *state);

#endif /* LINUXKPI_NATIVE_IRQ_H */
