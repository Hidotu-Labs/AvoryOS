#ifndef AVM_AVM_VM_MISC_H
#define AVM_AVM_VM_MISC_H

#include "avm_internal.h"

#include <stdint.h>

/* VM ioctls for GSI routing, irqfd, MSI, and the clock, plus the x86 setup
 * calls that SVM does not need (the TSS address and the identity map).  Returns
 * -ENOTTY for a request it does not handle. */
int avm_vm_misc_ioctl(struct avm_vm *vm, uint32_t request, uint64_t arg);

#endif /* AVM_AVM_VM_MISC_H */
