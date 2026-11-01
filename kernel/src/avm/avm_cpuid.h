#ifndef AVM_AVM_CPUID_H
#define AVM_AVM_CPUID_H

#include "avm_internal.h"

#include <stdint.h>

/* Writes the default CPUID table into e (at most AVM_MAX_CPUID entries) for a
 * vCPU with the given APIC ID.  Returns the entry count.  Also the table that
 * KVM_GET_SUPPORTED_CPUID reports, with APIC ID 0. */
uint32_t avm_cpuid_default(struct kvm_cpuid_entry2 *e, uint32_t apic_id);

/* Looks up leaf/subleaf in the vCPU's table.  A leaf that is not in the table
 * reads as zero, which is what the guest sees for an absent leaf. */
void avm_cpuid_lookup(const struct avm_vcpu *vcpu, uint32_t leaf,
                      uint32_t subleaf, uint32_t out[4]);

#endif /* AVM_AVM_CPUID_H */
