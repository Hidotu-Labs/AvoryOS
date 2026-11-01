#ifndef AVM_AVM_LAPIC_H
#define AVM_AVM_LAPIC_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* In-kernel local APIC.  The guest reaches it through x2APIC MSRs, so no
 * instruction decoding is needed.  The xAPIC MMIO page is not emulated yet. */

#define MSR_APIC_BASE 0x1B
#define X2APIC_MSR_ICR 0x830
#define X2APIC_MSR_FIRST 0x800
#define X2APIC_MSR_LAST 0x8FF

void avm_lapic_init(struct avm_vcpu *vcpu);

/* Returns 0 and fills out for a supported APIC MSR read, or -1 for #GP. */
int avm_lapic_rdmsr(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *out);
/* Returns 0 for a supported APIC MSR write, or -1 for #GP. */
int avm_lapic_wrmsr(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val);

/* True for the MSRs the LAPIC serves: the APIC base and the x2APIC range. */
static inline bool avm_lapic_owns_msr(uint32_t msr) {
  return msr == MSR_APIC_BASE || (msr >= X2APIC_MSR_FIRST && msr <= X2APIC_MSR_LAST);
}

/* Advances the timer and returns the next vector to inject (already moved
 * to the in-service set), or -1 when nothing can be delivered. */
int avm_lapic_next_vector(struct avm_vcpu *vcpu);

/* Advances the timer to the current host time. */
void avm_lapic_tick(struct avm_vcpu *vcpu);

/* Sets a vector in IRR, for an external interrupt from the IOAPIC.  Vectors
 * below 16 are reserved and ignored.  The caller holds vm->irq_lock. */
void avm_lapic_raise(struct avm_vcpu *vcpu, uint8_t vector);

/* xAPIC MMIO, offsets 0x020-0x3F0 in the APIC page.  Each register maps onto
 * its x2APIC MSR.  ICR is not supported yet.  Unsupported offsets return -1
 * and leave the value as 0. */
int avm_lapic_mmio_read(struct avm_vcpu *vcpu, uint32_t off, uint32_t *out);
int avm_lapic_mmio_write(struct avm_vcpu *vcpu, uint32_t off, uint32_t val);
/* True when a pending vector can be delivered now (TPR and ISR permitting).
 * Does not change state. */
bool avm_lapic_deliverable_now(const struct avm_vcpu *vcpu);
/* True when the timer is armed, unmasked, and the APIC is enabled, so the
 * timer can wake a halted vCPU. */
bool avm_lapic_can_wake(const struct avm_vcpu *vcpu);

#endif /* AVM_AVM_LAPIC_H */
