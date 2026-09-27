#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

void cpu_features_init(void);
bool cpu_has_pcid(void);
bool cpu_has_invpcid(void);
bool cpu_has_xsave(void);
bool cpu_has_avx(void);
bool cpu_has_fsgsbase(void);
bool cpu_has_smep(void);
bool cpu_has_smap(void);
extern bool cpu_has_xsave_flag;

/* True once CR4.SMAP has been enabled on the BSP.  stac/clac are #UD on CPUs
 * that do not enumerate SMAP, so every stac/clac site gates on this flag. */
extern bool cpu_has_smap_flag;

/* Coarse-grained user access window.  The syscall entry path sets AC for the
 * whole dispatch (see syscall_entry.asm) because most legacy syscalls here
 * still touch user memory directly; these helpers cover the remaining
 * exception-context user writes (signal delivery).  stac/clac are no-ops on
 * CPUs without SMAP. */
static inline void user_access_begin(void) {
  if (cpu_has_smap_flag)
    __asm__ volatile("stac" ::: "memory");
}

static inline void user_access_end(void) {
  if (cpu_has_smap_flag)
    __asm__ volatile("clac" ::: "memory");
}

/* Current RFLAGS.AC state (bit 18).  Used by the SMEP/SMAP test to prove the
 * uaccess paths restore the caller's AC state. */
static inline bool ac_flag_set(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0" : "=r"(flags));
  return (flags & (1ULL << 18)) != 0;
}

static inline void wrfsbase(uint64_t val) {
  __asm__ volatile("wrfsbase %0" : : "r"(val) : "memory");
}

static inline uint64_t rdfsbase(void) {
  uint64_t val;
  __asm__ volatile("rdfsbase %0" : "=r"(val));
  return val;
}

#endif

