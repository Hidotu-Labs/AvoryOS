#ifndef AVM_SVM_H
#define AVM_SVM_H

#include "avm_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* AMD SVM constants used by phase 1.  Source: AMD64 APM Volume 2.
 *
 * The manual text has not been checked (see NOTES.md).  The boot probe is the
 * check for the VMCB offsets: a wrong offset fails VMRUN or the first exit. */

/* CPUID 8000_0001h ECX bit 2: SVM supported. */
#define CPUID_80000001_ECX_SVM (1u << 2)
/* CPUID 8000_000Ah EDX bit 3: next-RIP save, needed to skip an intercepted
 * instruction without decoding it. */
#define CPUID_8000000A_EDX_NRIPS (1u << 3)

/* Writes the special registers the guest has, from the VMCB, into s. */
void svm_sregs_out(struct avm_vcpu *vcpu, struct kvm_sregs *s);

/* Runs CPUID on the host.  Used for the host's own feature bits, never for a
 * guest's CPUID, which comes from the vCPU's table (avm_cpuid.c). */
static inline void svm_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a,
                             uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(subleaf));
}

/* Extended feature MSRs. */
#define MSR_EFER 0xC0000080u
#define EFER_SVME (1ull << 12)
#define EFER_SCE (1ull << 0)
#define EFER_LME (1ull << 8)
#define EFER_LMA (1ull << 10)
#define EFER_NXE (1ull << 11)
/* The EFER bits the guest or the VMM may set.  LMA is read-only: the CPU sets
 * it from LME and CR0.PG. */
#define EFER_WRITABLE (EFER_SCE | EFER_LME | EFER_NXE)
#define CR0_PG (1ull << 31)

/* Guest-visible MSRs.  Their values live in the VMCB (see svm_msr_get). */
#define MSR_IA32_SYSENTER_CS 0x174u
#define MSR_IA32_SYSENTER_ESP 0x175u
#define MSR_IA32_SYSENTER_EIP 0x176u
#define MSR_IA32_CR_PAT 0x277u
#define MSR_STAR 0xC0000081u
#define MSR_LSTAR 0xC0000082u
#define MSR_CSTAR 0xC0000083u
#define MSR_SFMASK 0xC0000084u
#define MSR_FS_BASE 0xC0000100u
#define MSR_GS_BASE 0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

/* VM_CR: firmware-controlled SVM enable.  SVMDIS blocks SVM when set. */
#define MSR_VM_CR 0xC0010114u
#define VM_CR_SVMDIS (1ull << 4)
#define VM_CR_LOCK (1ull << 3)

/* Host state save area.  VMRUN saves host state here, so each CPU needs its
 * own 4 KB page before it runs a guest. */
#define MSR_VM_HSAVE_PA 0xC0010117u

/* VMCB control area. */
#define VMCB_INTERCEPT_MISC1 0x00C
#define VMCB_INTERCEPT_MISC2 0x010
#define VMCB_IOPM_BASE_PA 0x040
#define VMCB_MSRPM_BASE_PA 0x048
#define VMCB_GUEST_ASID 0x058
#define VMCB_TLB_CONTROL 0x05C
#define VMCB_EXITCODE 0x070
#define VMCB_EXITINFO1 0x078
#define VMCB_EXITINFO2 0x080
#define VMCB_NP_ENABLE 0x090
#define VMCB_EVENTINJ 0x0A8
#define VMCB_INTERCEPT_EXC 0x008  /* intercept exceptions, bit n = vector n */
#define VMCB_N_CR3 0x0B0
#define VMCB_CLEAN_BITS 0x0C0
#define VMCB_NRIP 0x0C8

/* VMCB state save area (starts at 0x400). */
#define VMCB_SAVE 0x400
#define VMCB_SAVE_ES 0x400
#define VMCB_SAVE_CS 0x410
#define VMCB_SAVE_SS 0x420
#define VMCB_SAVE_DS 0x430
#define VMCB_SAVE_FS 0x440
#define VMCB_SAVE_GS 0x450
#define VMCB_SAVE_GDTR 0x460
#define VMCB_SAVE_LDTR 0x470
#define VMCB_SAVE_IDTR 0x480
#define VMCB_SAVE_TR 0x490
#define VMCB_SAVE_CPL 0x4CB
#define VMCB_SAVE_EFER 0x4D0
#define VMCB_SAVE_CR4 0x548
#define VMCB_SAVE_CR3 0x550
#define VMCB_SAVE_CR0 0x558
#define VMCB_SAVE_DR7 0x560
#define VMCB_SAVE_DR6 0x568
#define VMCB_SAVE_RFLAGS 0x570
#define VMCB_SAVE_RIP 0x578
#define VMCB_SAVE_RSP 0x5D8
#define VMCB_SAVE_RAX 0x5F8
#define VMCB_SAVE_G_PAT 0x668
/* SYSCALL, SYSENTER and KernelGSBase: VMLOAD and VMSAVE move these, not
 * VMRUN.  The offsets follow the AMD manual's VMCB state save area. */
#define VMCB_SAVE_STAR 0x600
#define VMCB_SAVE_LSTAR 0x608
#define VMCB_SAVE_CSTAR 0x610
#define VMCB_SAVE_SFMASK 0x618
#define VMCB_SAVE_KERNEL_GS_BASE 0x620
#define VMCB_SAVE_SYSENTER_CS 0x628
#define VMCB_SAVE_SYSENTER_ESP 0x630
#define VMCB_SAVE_SYSENTER_EIP 0x638

/* Intercept bits, misc1 at VMCB_INTERCEPT_MISC1. */
/* INTR is intercepted so a host timer interrupt forces an exit while the
 * guest runs, and the loop can deliver the guest's timers at that point. */
#define INTERCEPT_INTR (1u << 0)
/* NMI is intercepted so a kick (an NMI from another CPU) exits the guest even
 * when the guest has IF clear. */
#define INTERCEPT_NMI (1u << 1)
#define INTERCEPT_CPUID (1u << 18)
#define INTERCEPT_HLT (1u << 24)
#define INTERCEPT_IOIO (1u << 27)
#define INTERCEPT_MSR (1u << 28)
#define INTERCEPT_SHUTDOWN (1u << 31)
/* Intercept bit, misc2 at VMCB_INTERCEPT_MISC2.  VMRUN must be intercepted. */
#define INTERCEPT_VMRUN (1u << 0)
/* XSETBV changes XCR0, which is per CPU.  Intercepting it keeps a guest from
 * changing the host's XCR0, and the handler keeps the guest's value apart. */
#define INTERCEPT_XSETBV (1u << 13)

/* Exit codes, VMCB_EXITCODE. */
#define SVM_EXIT_INTR 0x060
#define SVM_EXIT_NMI 0x061
#define SVM_EXIT_CPUID 0x072
#define SVM_EXIT_HLT 0x078
#define SVM_EXIT_XSETBV 0x08D
#define SVM_EXIT_IOIO 0x07B
#define SVM_EXIT_MSR 0x07C
#define SVM_EXIT_SHUTDOWN 0x07F
#define SVM_EXIT_NPF 0x400
#define SVM_EXIT_EXCP_DB 0x041 /* exception 1 (#DB) */
#define EXCP_DB_INTERCEPT (1u << 1)
#define SVM_EXIT_INVALID ((uint64_t)-1)

/* EVENTINJ: a general-protection fault with an error code of zero. */
#define SVM_EVENTINJ_VALID (1ull << 31)
#define SVM_EVENTINJ_ERRCODE (1ull << 11)
#define SVM_EVENTINJ_EXCEPTION (3ull << 8)
#define SVM_EVENTINJ_GP (SVM_EVENTINJ_VALID | SVM_EVENTINJ_ERRCODE | SVM_EVENTINJ_EXCEPTION | 13)
#define SVM_EVENTINJ_UD (SVM_EVENTINJ_VALID | SVM_EVENTINJ_EXCEPTION | 6)

bool svm_supported(void);

/* Enables SVM on the calling CPU.  The caller must be bound to that CPU. */
int svm_cpu_enable(void);

/* Guest-visible MSRs whose value is in the VMCB: EFER, PAT, the SYSCALL and
 * SYSENTER MSRs, KernelGSBase, and the FS and GS bases.  The guest's RDMSR and
 * WRMSR and the VMM's KVM_GET_MSRS and KVM_SET_MSRS all use these, so they
 * always agree.  svm_msr_set validates the value: -EINVAL means #GP for the
 * guest. */
bool svm_msr_in_vmcb(uint32_t msr);
int svm_msr_get(struct avm_vcpu *vcpu, uint32_t msr, uint64_t *val);
int svm_msr_set(struct avm_vcpu *vcpu, uint32_t msr, uint64_t val);

/* Per-vCPU VMCB and permission maps.  Uses vcpu->vm->npt_root. */
int svm_vcpu_init(struct avm_vcpu *vcpu);
void svm_vcpu_destroy(struct avm_vcpu *vcpu);

/* Runs the vCPU until it exits to the VMM.  Fills vcpu->run and returns 0,
 * or a negative errno.  Internal exits (CPUID, MSR, physical interrupts) are
 * handled here and do not return. */
int svm_vcpu_run(struct avm_vcpu *vcpu);

#endif /* AVM_SVM_H */
