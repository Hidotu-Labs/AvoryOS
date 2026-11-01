#ifndef AVM_AVM_IRQ_H
#define AVM_AVM_IRQ_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Interrupt controllers that live in the kernel: the 8259 PIC pair and PIT
 * channel 0.  The LAPIC is in avm_lapic.c.  Delivery priority is the LAPIC
 * first, then the PIC. */

/* Called when the VM is created. */
void avm_irq_vm_init(struct avm_vm *vm);

/* KVM_CREATE_PIT2: enables the PIT ports.  Needs the irqchip. */
int avm_irq_create_pit(struct avm_vm *vm);

/* KVM_IRQ_LINE: drives a GSI (0-23).  Needs the irqchip. */
int avm_irq_set_line(struct avm_vm *vm, uint32_t irq, uint32_t level);

/* KVM_GET_IRQCHIP / KVM_SET_IRQCHIP (the PIC pair and the IOAPIC) and
 * KVM_GET_PIT2 / KVM_SET_PIT2 (channel 0). */
int avm_irq_get_chip(struct avm_vm *vm, struct kvm_irqchip *c);
int avm_irq_set_chip(struct avm_vm *vm, const struct kvm_irqchip *c);
int avm_irq_get_pit2(struct avm_vm *vm, struct kvm_pit_state2 *s);
int avm_irq_set_pit2(struct avm_vm *vm, const struct kvm_pit_state2 *s);

/* Drives a legacy line without routing: 0-15 the PIC, 16-23 the IOAPIC pins.
 * The caller holds irq_lock. */
void avm_irq_line_locked(struct avm_vm *vm, uint32_t irq, bool level);

/* Returns the next vector to inject, already moved to the in-service set,
 * or -1. */
int avm_irq_next_vector(struct avm_vcpu *vcpu);

/* Blocks a halted vCPU in the kernel, as KVM does with an in-kernel interrupt
 * controller.  Returns true when the vCPU should resume: an interrupt is
 * deliverable, or a timer has run out (if_set only).  Returns false when
 * interrupted() reports a signal, so the caller returns to the VMM. */
bool avm_irq_wait(struct avm_vcpu *vcpu, bool if_set,
                  bool (*interrupted)(const struct avm_vcpu *vcpu));

/* Arms a host timer for the vCPU's next guest deadline (LAPIC or PIT), so a
 * guest that runs without exiting still gets its interrupt on time.  The host
 * interrupt causes an exit (INTR is intercepted), and the loop then delivers
 * the guest's timer. */
void avm_irq_arm_timer(struct avm_vcpu *vcpu);

/* The lock that guards the interrupt controllers.  The vCPU's MSR and MMIO
 * accesses to the LAPIC and IOAPIC hold it, and so do the VMM's IRQ lines. */
void avm_irq_lock(struct avm_vm *vm);
void avm_irq_unlock(struct avm_vm *vm);

/* Handles an IN or OUT of one byte on a PIC or PIT port.  Returns false for
 * any other port, which then goes to the VMM. */
bool avm_irq_io(struct avm_vm *vm, uint16_t port, bool in, uint8_t *value);

#endif /* AVM_AVM_IRQ_H */
