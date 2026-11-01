/* The CPUID table a guest sees.
 *
 * Each leaf starts from the host's value, and the bits AVM does not implement
 * are cleared, so a guest does not use a feature whose MSRs or intercepts are
 * missing.  Hypervisor leaves identify the guest's host as KVM-compatible; no
 * paravirtual features are advertised yet. */

#include "avm_cpuid.h"
#include "svm.h"

#include "../cpu/features.h"

#define LEAF_MAX_BASIC 0xDu
#define LEAF_HV_BASE 0x40000000u
#define LEAF_HV_MAX 0x40000001u
#define LEAF_EXT_MAX 0x80000008u
/* CPUID 8000_0001h ECX bit 23: the AMD core performance counters (F15h MSRs).
 * They are not virtualized, so the bit is cleared.  The legacy K7 counters
 * remain, and QEMU writes those for any AMD vCPU. */
#define CPUID_80000001_ECX_PERFCORE (1u << 23)

/* Leaf 1 ECX: DTES64, MONITOR, DS-CPL, VMX, SMX, EIST, TM2, CNXT-ID, SDBG,
 * xTPR, PDCM, DCA and TSC-deadline are hidden.  MONITOR would let a guest
 * block its CPU natively, and TSC-deadline is rejected by the LAPIC.  OSXSAVE
 * is cleared here and set from CR4 at each CPUID exit. */
#define LEAF1_ECX_HIDE                                                        \
  ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |    \
   (1u << 8) | (1u << 10) | (1u << 11) | (1u << 14) | (1u << 15) |            \
   (1u << 18) | (1u << 24) | (1u << 27))
/* Hypervisor present (CPUID.1:ECX[31]). */
#define LEAF1_ECX_HYPERVISOR (1u << 31)
/* Leaf 1 EDX: MCE, MTRR, MCA, PAT, PSN, DS, ACPI, thermal monitor and PBE are
 * hidden, because their MSRs are not implemented. */
#define LEAF1_EDX_HIDE                                                        \
  ((1u << 7) | (1u << 12) | (1u << 14) | (1u << 16) | (1u << 18) |            \
   (1u << 21) | (1u << 22) | (1u << 29) | (1u << 31))
/* Leaf 7 EBX: TSC_ADJUST (MSR 3Bh), SGX, HLE, RTM, RDT and MPX are hidden. */
#define LEAF7_EBX_HIDE                                                        \
  ((1u << 1) | (1u << 2) | (1u << 4) | (1u << 11) | (1u << 12) | (1u << 14) | \
   (1u << 15))
/* Leaf 7 ECX: CET shadow stacks (bit 7) are hidden, with their MSRs (U_CET,
 * S_CET, the SSP MSRs).  QEMU writes those MSRs whenever the bit is set. */
#define LEAF7_ECX_HIDE (1u << 7)
/* Leaf 7 EDX: CET indirect branch tracking (bit 20), AMX, and the
 * speculation-control MSRs (SPEC_CTRL, FLUSH_CMD, ARCH_CAPABILITIES,
 * CORE_CAPABILITIES, SSBD) are hidden. */
#define LEAF7_EDX_HIDE                                                        \
  ((1u << 20) | (1u << 22) | (1u << 24) | (1u << 25) | (1u << 26) |          \
   (1u << 27) | (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31))
/* Leaf 0xD subleaf 1 EAX: XSAVES needs the IA32_XSS MSR, which is not
 * implemented. */
#define XSAVE1_EAX_HIDE (1u << 3)

static void emit(struct kvm_cpuid_entry2 *e, uint32_t *n, uint32_t leaf,
                 uint32_t sub, uint32_t flags, const uint32_t r[4]) {
  struct kvm_cpuid_entry2 *x = &e[(*n)++];
  x->function = leaf;
  x->index = sub;
  x->flags = flags;
  x->eax = r[0];
  x->ebx = r[1];
  x->ecx = r[2];
  x->edx = r[3];
  x->padding[0] = 0;
  x->padding[1] = 0;
  x->padding[2] = 0;
}

static void host(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
  svm_cpuid(leaf, sub, &r[0], &r[1], &r[2], &r[3]);
}

uint32_t avm_cpuid_default(struct kvm_cpuid_entry2 *e, uint32_t apic_id) {
  uint32_t n = 0;
  uint32_t r[4];

  /* Basic leaves.  The vendor string is the host's; the maximum leaf is the
   * highest one in this table. */
  host(0, 0, r);
  r[0] = LEAF_MAX_BASIC;
  emit(e, &n, 0, 0, 0, r);

  host(1, 0, r);
  r[1] = (r[1] & 0x00FFFFFFu) | ((apic_id & 0xFFu) << 24);
  r[2] = (r[2] & ~LEAF1_ECX_HIDE) | LEAF1_ECX_HYPERVISOR;
  r[3] &= ~LEAF1_EDX_HIDE;
  emit(e, &n, 1, 0, 0, r);

  host(7, 0, r);
  r[0] = 0; /* no subleaves */
  r[1] &= ~LEAF7_EBX_HIDE;
  r[2] &= ~LEAF7_ECX_HIDE;
  r[3] &= ~LEAF7_EDX_HIDE;
  emit(e, &n, 7, 0, KVM_CPUID_FLAG_SIGNIFCANT_INDEX, r);

  /* XSAVE: subleaves 0 and 1, then one per state component that the host
   * enables in XCR0.  A guest sizes its XSAVE area from these. */
  host(0xD, 0, r);
  emit(e, &n, 0xD, 0, KVM_CPUID_FLAG_SIGNIFCANT_INDEX, r);
  host(0xD, 1, r);
  r[0] &= ~XSAVE1_EAX_HIDE;
  r[2] = 0;
  emit(e, &n, 0xD, 1, KVM_CPUID_FLAG_SIGNIFCANT_INDEX, r);
  for (uint32_t i = 2; i < 32; i++) {
    if (!(cpu_xsave_mask & (1ull << i)))
      continue;
    host(0xD, i, r);
    emit(e, &n, 0xD, i, KVM_CPUID_FLAG_SIGNIFCANT_INDEX, r);
  }

  /* Hypervisor leaves: the KVM signature, then no paravirtual features. */
  r[0] = LEAF_HV_MAX;
  r[1] = 0x4B4D564Bu; /* "KVMK" */
  r[2] = 0x564B4D56u; /* "VMKV" */
  r[3] = 0x0000004Du; /* "M" */
  emit(e, &n, LEAF_HV_BASE, 0, 0, r);
  r[0] = 0;
  r[1] = 0;
  r[2] = 0;
  r[3] = 0;
  emit(e, &n, LEAF_HV_BASE + 1, 0, 0, r);

  /* Extended leaves.  Brand string and address sizes come from the host. */
  host(0x80000000u, 0, r);
  r[0] = LEAF_EXT_MAX;
  emit(e, &n, 0x80000000u, 0, 0, r);

  host(0x80000001u, 0, r);
  r[2] &= ~(CPUID_80000001_ECX_SVM | CPUID_80000001_ECX_PERFCORE);
  emit(e, &n, 0x80000001u, 0, 0, r);

  for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
    host(leaf, 0, r);
    emit(e, &n, leaf, 0, 0, r);
  }

  host(0x80000008u, 0, r);
  emit(e, &n, 0x80000008u, 0, 0, r);

  return n;
}

void avm_cpuid_lookup(const struct avm_vcpu *vcpu, uint32_t leaf,
                      uint32_t subleaf, uint32_t out[4]) {
  for (uint32_t i = 0; i < vcpu->cpuid_nent; i++) {
    const struct kvm_cpuid_entry2 *x = &vcpu->cpuid[i];
    if (x->function != leaf)
      continue;
    if ((x->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) && x->index != subleaf)
      continue;
    out[0] = x->eax;
    out[1] = x->ebx;
    out[2] = x->ecx;
    out[3] = x->edx;
    return;
  }
  out[0] = 0;
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;
}
