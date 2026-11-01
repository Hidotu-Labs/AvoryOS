#ifndef AVM_AVM_STATE_H
#define AVM_AVM_STATE_H

#include "avm_internal.h"

#include <stdint.h>

/* vCPU state ioctls: registers, FPU and XSAVE, LAPIC, MP state, events, debug
 * registers, XCRs, the TSC frequency and the KVM_RUN signal mask.  Returns
 * -ENOTTY for a request it does not handle. */
int avm_vcpu_state_ioctl(struct avm_vcpu *vcpu, uint32_t request, uint64_t arg);

#endif /* AVM_AVM_STATE_H */
