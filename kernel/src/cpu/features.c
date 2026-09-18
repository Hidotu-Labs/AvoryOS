#include "features.h"
#include "msr.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IA32_PAT_MSR 0x277

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
                         uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  uint32_t a = 0, b = 0, c = 0, d = 0;
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(leaf), "c"(subleaf)
                   : "memory");
  if (eax) *eax = a;
  if (ebx) *ebx = b;
  if (ecx) *ecx = c;
  if (edx) *edx = d;
}

/* Cached CPUID feature bits.  CPUID is serializing and, under KVM, a VM exit;
 * the predicates below are called from hot paths - context switches
 * (fsgsbase), uaccess (smap), the TLB shootdown path (pcid/invpcid) - so they
 * must not re-execute it.  cpu_features_init() probes once per CPU; a caller
 * that arrives even earlier triggers the same one-shot probe. */
static uint32_t feat_max_leaf;
static uint32_t feat_1_ecx;
static uint32_t feat_7_0_ebx;
static bool feat_probed;

static void features_probe(void) {
  cpuid(0, 0, &feat_max_leaf, NULL, NULL, NULL);
  cpuid(1, 0, NULL, NULL, &feat_1_ecx, NULL);
  if (feat_max_leaf >= 7)
    cpuid(7, 0, NULL, &feat_7_0_ebx, NULL, NULL);
  __atomic_store_n(&feat_probed, true, __ATOMIC_RELEASE);
}

static inline void features_ensure_probed(void) {
  if (!__atomic_load_n(&feat_probed, __ATOMIC_ACQUIRE))
    features_probe();
}

bool cpu_has_pcid(void) {
  features_ensure_probed();
  return (feat_1_ecx & (1U << 17)) != 0; // CPUID.01H:ECX.PCID[bit 17]
}

bool cpu_has_invpcid(void) {
  features_ensure_probed();
  if (feat_max_leaf < 7)
    return false;
  return (feat_7_0_ebx & (1U << 10)) != 0; // CPUID.07H:EBX.INVPCID[bit 10]
}


bool cpu_has_fsgsbase(void) {
  features_ensure_probed();
  if (feat_max_leaf < 7)
    return false;
  return (feat_7_0_ebx & (1U << 0)) != 0; // CPUID.07H:EBX.FSGSBASE[bit 0]
}

bool cpu_has_smep(void) {
  features_ensure_probed();
  if (feat_max_leaf < 7)
    return false;
  return (feat_7_0_ebx & (1U << 7)) != 0; // CPUID.07H:EBX.SMEP[bit 7]
}

bool cpu_has_smap(void) {
  features_ensure_probed();
  if (feat_max_leaf < 7)
    return false;
  return (feat_7_0_ebx & (1U << 20)) != 0; // CPUID.07H:EBX.SMAP[bit 20]
}

bool cpu_has_xsave(void) {
  features_ensure_probed();
  return (feat_1_ecx & (1U << 26)) != 0; // CPUID.01H:ECX.XSAVE[bit 26]
}

bool cpu_has_avx(void) {
  features_ensure_probed();
  return (feat_1_ecx & (1U << 28)) != 0; // CPUID.01H:ECX.AVX[bit 28]
}

static inline void __attribute__((unused)) xsetbv(uint32_t index, uint64_t value) {
  uint32_t eax = (uint32_t)value;
  uint32_t edx = (uint32_t)(value >> 32);
  __asm__ volatile("xsetbv" : : "c"(index), "a"(eax), "d"(edx) : "memory");
}

// Set up PAT (Page Attribute Table) to define memory types.
// We configure PA7 to be Write-Combining (01h).
static void cpu_pat_init(void) {
  uint64_t pat = rdmsr(IA32_PAT_MSR);
  // Default PAT: 0x0007040600070406 (PA0:WB, PA1:WT, PA2:UC-, PA3:UC, PA4:WB, PA5:WT, PA6:UC-, PA7:UC)
  // We want to set PA7 (bits 56-63) to 0x01 (WC).
  pat &= ~(0xFFULL << 56);
  pat |= (0x01ULL << 56);
  wrmsr(IA32_PAT_MSR, pat);
}

bool cpu_has_xsave_flag = false;
bool cpu_has_smap_flag = false;

// Enable SSE/SSE2, PCID, FSGSBASE, AVX/XSAVE, SMEP and SMAP for long mode.
void cpu_features_init(void) {
  /* Publish the cached CPUID bits before any of the predicates below runs;
   * the lazy probe makes this redundant but keeps the "probed on every CPU
   * before anything else" property obvious. */
  features_ensure_probed();

  uint64_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1ULL << 2); // EM — no x87 emulation
  cr0 |= (1ULL << 1);  // MP — monitor coprocessor (with TS, matches PC behavior)
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9);  // OSFXSR — allow FXSAVE/FXRSTOR + SSE in user mode
  cr4 |= (1ULL << 10); // OSXMMEXCPT — #XF for unmasked SIMD exceptions

  // Enable FSGSBASE (wrfsbase / rdfsbase) if supported by the CPU
  if (cpu_has_fsgsbase()) {
    cr4 |= (1ULL << 16); // CR4.FSGSBASE (bit 16)
  }

  // Enable PCID (Process-Context Identifiers) if supported by the CPU
  if (cpu_has_pcid()) {
    cr4 |= (1ULL << 17); // CR4.PCIDE (bit 17)
  }

  // Enable OSXSAVE if supported by CPU (required for AVX / AVX2 / XSAVE)
  if (cpu_has_xsave()) {
    cr4 |= (1ULL << 18); // CR4.OSXSAVE (bit 18)
    cpu_has_xsave_flag = true;
  }

  // Enable SMEP: supervisor mode may not execute user-mapped pages.
  if (cpu_has_smep()) {
    cr4 |= (1ULL << 20); // CR4.SMEP (bit 20)
  }

  // Enable SMAP: supervisor mode may not access user-mapped pages unless
  // RFLAGS.AC is set.  Publish the flag before CR4 changes so any uaccess
  // path that starts between the store and the CR4 write already knows to
  // issue stac.
  if (cpu_has_smap()) {
    cpu_has_smap_flag = true;
    cr4 |= (1ULL << 21); // CR4.SMAP (bit 21)
  }

  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  // Initialize XCR0 with x87, SSE, and AVX state components
  if (cpu_has_xsave()) {
    uint64_t xcr0 = 1ULL | 2ULL; // x87 (bit 0) | SSE (bit 1)
    if (cpu_has_avx()) {
      xcr0 |= 4ULL; // AVX (bit 2)
    }
    xsetbv(0, xcr0);
  }

  __asm__ volatile("fninit");
  cpu_pat_init();
}

