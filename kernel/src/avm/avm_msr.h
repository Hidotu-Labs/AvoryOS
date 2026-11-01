#ifndef AVM_AVM_MSR_H
#define AVM_AVM_MSR_H

#include "avm_internal.h"

#include <stdint.h>

/* KVM_GET_MSR_INDEX_LIST reports these.  QEMU reads that list to decide which
 * optional MSRs to set, so it stays short.  KVM_GET_MSRS and KVM_SET_MSRS also
 * accept the VMM-only MSRs in avm_vmm_msrs (TSC, SYSENTER, PAT, MTRR, MCE),
 * which are stored but not reported in the list.  Guest WRMSR and RDMSR of
 * other MSRs still raise #GP, as they did before. */
uint32_t avm_msr_count(void);
const uint32_t *avm_msr_indices(void);

/* Returns 0 on success, or a negative errno for an MSR outside the list. */
int avm_msr_get(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *val);
int avm_msr_set(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val);

#endif /* AVM_AVM_MSR_H */
