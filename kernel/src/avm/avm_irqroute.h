#ifndef AVM_AVM_IRQROUTE_H
#define AVM_AVM_IRQROUTE_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* GSI routing, irqfd and MSI.  A GSI is an interrupt input the VMM names.
 * With no routing table, GSIs 0-15 are the PIC lines and 16-23 the IOAPIC
 * pins, as KVM_IRQ_LINE has always used them.  KVM_SET_GSI_ROUTING can send a
 * GSI to a chosen PIC or IOAPIC pin, or to an MSI message. */

#define AVM_MAX_GSI 1024

/* Validates and installs a routing table.  Entries are a kernel copy. */
int avm_irq_set_routing(struct avm_vm *vm, const struct kvm_irq_routing_entry *e,
                        uint32_t n);

/* Drives a GSI through the routing table, with a kick for rising edges. */
void avm_irq_gsi_set(struct avm_vm *vm, uint32_t gsi, bool level);

/* KVM_SIGNAL_MSI.  Returns 1 when the message was delivered. */
int avm_irq_signal_msi(struct avm_vm *vm, const struct kvm_msi *m);

/* KVM_IRQFD: an eventfd signal pulses the GSI. */
int avm_irqfd_assign(struct avm_vm *vm, const struct kvm_irqfd *a);
int avm_irqfd_deassign(struct avm_vm *vm, const struct kvm_irqfd *a);
void avm_irqfd_destroy(struct avm_vm *vm);

#endif /* AVM_AVM_IRQROUTE_H */
