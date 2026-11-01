/* MSR access from the VMM through KVM_GET_MSRS and KVM_SET_MSRS.
 *
 * The APIC's MSRs go to the in-kernel LAPIC, which owns that state.  EFER, PAT,
 * the SYSCALL and SYSENTER MSRs, KernelGSBase and the FS and GS bases live in
 * the VMCB, so the guest and the VMM share one copy (svm_msr_get and
 * svm_msr_set).  The MSRs only the VMM uses are kept in vcpu->vmm_msrs. */

#include "avm_msr.h"
#include "avm_lapic.h"
#include "avm_irq.h"
#include "svm.h"

#include "../include/linux/errno.h"

/* Index values from the Intel SDM, as QEMU's cpu.h names them. */
#define MSR_IA32_TSC 0x010u
#define MSR_MTRRphysBase0 0x200u
#define MSR_MTRRfix64K_00000 0x250u
#define MSR_MTRRfix16K_80000 0x258u
#define MSR_MTRRfix16K_A0000 0x259u
#define MSR_MTRRfix4K_C0000 0x268u
#define MSR_MTRRcap 0xFEu
#define MSR_MTRRdefType 0x2FFu
#define MSR_MCG_STATUS 0x17Au
#define MSR_MCG_CTL 0x17Bu
#define MSR_MC0_CTL 0x400u
#define MSR_K7_EVNTSEL0 0xC0010000u
#define MSR_K7_PERFCTR0 0xC0010004u

/* The slot in vcpu->vmm_msrs that holds msr, or NULL if the VMM does not set
 * it.  Bank MSRs exist only up to the bank count that KVM_X86_SETUP_MCE chose. */
static uint64_t *vmm_msr_slot(struct avm_vcpu *vcpu, uint32_t msr) {
  struct avm_vmm_msrs *m = &vcpu->vmm_msrs;

  switch (msr) {
  case MSR_IA32_TSC:
    return &m->tsc;
  case MSR_MTRRdefType:
    return &m->mtrr_deftype;
  case MSR_MTRRfix64K_00000:
    return &m->mtrr_fixed[0];
  case MSR_MTRRfix16K_80000:
    return &m->mtrr_fixed[1];
  case MSR_MTRRfix16K_A0000:
    return &m->mtrr_fixed[2];
  case MSR_MCG_STATUS:
    return &m->mcg_status;
  case MSR_MCG_CTL:
    return &m->mcg_ctl;
  }

  if (msr >= MSR_MTRRfix4K_C0000 && msr < MSR_MTRRfix4K_C0000 + 8)
    return &m->mtrr_fixed[3 + (msr - MSR_MTRRfix4K_C0000)];
  if (msr >= MSR_MTRRphysBase0 && msr < MSR_MTRRphysBase0 + 2 * AVM_MTRR_VCNT)
    return &m->mtrr_var[msr - MSR_MTRRphysBase0];
  if (msr >= MSR_MC0_CTL && msr - MSR_MC0_CTL < (vcpu->mcg_cap & 0xff) * 4)
    return &m->mc[msr - MSR_MC0_CTL];
  if (msr >= MSR_K7_EVNTSEL0 && msr < MSR_K7_EVNTSEL0 + 4)
    return &m->k7_evntsel[msr - MSR_K7_EVNTSEL0];
  if (msr >= MSR_K7_PERFCTR0 && msr < MSR_K7_PERFCTR0 + 4)
    return &m->k7_perfctr[msr - MSR_K7_PERFCTR0];
  return NULL;
}

static const uint32_t msr_index[] = {
    MSR_APIC_BASE,
    MSR_EFER,
};

uint32_t avm_msr_count(void) {
  return sizeof(msr_index) / sizeof(msr_index[0]);
}

const uint32_t *avm_msr_indices(void) {
  return msr_index;
}

int avm_msr_get(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *val) {
  uint64_t *slot;

  if (avm_lapic_owns_msr(msr)) {
    avm_irq_lock(vcpu->vm);
    int err = avm_lapic_rdmsr(vcpu, msr, val);
    avm_irq_unlock(vcpu->vm);
    return err ? -EINVAL : 0;
  }
  if (svm_msr_in_vmcb(msr))
    return svm_msr_get(vcpu, msr, val);
  /* Read-only: the fixed-range bit (0x100), write-combining (0x400), and the
   * variable-range count.  Writes fall through to EINVAL, as for any MSR with
   * no slot. */
  if (msr == MSR_MTRRcap) {
    *val = 0x500 | AVM_MTRR_VCNT;
    return 0;
  }

  slot = vmm_msr_slot(vcpu, msr);
  if (!slot)
    return -EINVAL;
  *val = *slot;
  return 0;
}

int avm_msr_set(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val) {
  uint64_t *slot;

  if (avm_lapic_owns_msr(msr)) {
    avm_irq_lock(vcpu->vm);
    int err = avm_lapic_wrmsr(vcpu, msr, val);
    avm_irq_unlock(vcpu->vm);
    return err ? -EINVAL : 0;
  }
  if (svm_msr_in_vmcb(msr))
    return svm_msr_set(vcpu, msr, val);

  slot = vmm_msr_slot(vcpu, msr);
  if (!slot)
    return -EINVAL;
  *slot = val;
  return 0;
}
