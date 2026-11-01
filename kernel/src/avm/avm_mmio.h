#ifndef AVM_AVM_MMIO_H
#define AVM_AVM_MMIO_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Memory-mapped I/O for accesses that leave the NPT.  The faulting instruction
 * is read from guest memory and decoded, and the access is then done in the
 * kernel (APIC, IOAPIC) or handed to the VMM as KVM_EXIT_MMIO.
 *
 * Decoded: mov between a register and memory (opcodes 88, 89, 8A, 8B, A0-A3,
 * C6, C7), with 16- or 32-bit operand and address sizes, and the usual
 * prefixes.  Other instructions are not decoded.  Guest paging must be off,
 * so the linear address is the guest-physical address. */

/* Result of avm_mmio_fault(). */
#define AVM_MMIO_HANDLED 1  /* done in the kernel; RIP is past the access */
#define AVM_MMIO_TO_VMM 0   /* run->exit_reason is KVM_EXIT_MMIO */
#define AVM_MMIO_ERROR (-1) /* the access could not be decoded */

int avm_mmio_fault(struct avm_vcpu *vcpu, uint64_t gpa);

/* Called before KVM_RUN re-enters: stores the VMM's answer to a read and
 * advances RIP past an access that went to the VMM.  For a string element,
 * RIP moves on only after the last element. */
void avm_mmio_complete(struct avm_vcpu *vcpu);

/* Writes an index or count register back.  The low bits follow the address
 * size.  As on hardware, A16 keeps the upper bits and A32 clears them. */
static inline void avm_set_addr_reg(uint64_t *reg, uint64_t mask, uint64_t value) {
  uint64_t keep = mask == 0xFFFFull ? (*reg & ~mask) : 0;
  *reg = keep | (value & mask);
}

/* Copies guest-physical memory out of a memslot.  Returns -1 if the range is
 * not entirely inside one slot. */
int avm_mmio_guest_read(struct avm_vm *vm, uint64_t gpa, void *buf, uint64_t len);

/* Copies into guest-physical memory of a writable memslot.  Returns -1 if the
 * range is not entirely inside one such slot. */
int avm_mmio_guest_write(struct avm_vm *vm, uint64_t gpa, const void *buf, uint64_t len);

#endif /* AVM_AVM_MMIO_H */
