#ifndef AVM_AVM_IOAPIC_H
#define AVM_AVM_IOAPIC_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* IOAPIC at 0xFEC00000.  Registers are reached through IOREGSEL (offset 0x00)
 * and IOWIN (offset 0x10).  The guest reaches it through MMIO, which
 * avm_mmio.c decodes.  The caller holds vm->irq_lock. */

void avm_ioapic_init(struct avm_vm *vm);

/* Offsets are relative to the IOAPIC base. */
int avm_ioapic_mmio_read(struct avm_vm *vm, uint32_t off, uint32_t *out);
int avm_ioapic_mmio_write(struct avm_vm *vm, uint32_t off, uint32_t val);

/* Drives an input pin (0-23).  A rising edge delivers the pin's entry. */
void avm_ioapic_set_pin(struct avm_vm *vm, uint32_t pin, bool level);

#endif /* AVM_AVM_IOAPIC_H */
