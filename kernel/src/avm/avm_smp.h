#ifndef AVM_AVM_SMP_H
#define AVM_AVM_SMP_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* Multiprocessor support: INIT and SIPI, inter-processor interrupts through
 * the ICR, kicks that move a running vCPU out of guest mode, and address
 * space IDs. */

/* Sets the starting MP state.  vCPU 0 is the boot processor and runs at once. */
void avm_smp_init_vcpu(struct avm_vcpu *vcpu);

/* Blocks until the vCPU is RUNNABLE.  An application processor waits here
 * from creation until a SIPI arrives.  Returns false when interrupted() reports
 * a signal, so the caller returns to the VMM. */
bool avm_smp_wait_runnable(struct avm_vcpu *vcpu,
                           bool (*interrupted)(const struct avm_vcpu *vcpu));

/* Handles a write to the ICR.  dest is the destination APIC ID (the high
 * word in x2APIC, bits 31:24 of ICR_HIGH in xAPIC).  low is the low word.
 * The caller holds vm->irq_lock. */
void avm_smp_write_icr(struct avm_vcpu *src, uint32_t dest, uint32_t low);

/* Forces a vCPU that is running in guest mode on another CPU to exit, so it
 * takes a pending interrupt or a new mapping.  Safe if it is not running.  The
 * kick is an NMI, which an IF-clear guest cannot mask. */
void avm_smp_kick(struct avm_vcpu *target);
void avm_smp_kick_all(struct avm_vm *vm);

/* Returns an ASID for a new VM.  Each VM gets its own, and ASID 0 is not used. */
uint32_t avm_smp_alloc_asid(void);
/* Called by the host NMI handler.  Returns true, and clears the flag, if the
 * NMI on this CPU is a kick from avm_smp_kick(). */
bool avm_nmi_kick_consume(void);

/* Records how many ASIDs the CPU supports (CPUID 8000_000Ah EBX). */
void avm_smp_set_asid_count(uint32_t count);

#endif /* AVM_AVM_SMP_H */
