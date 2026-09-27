#include "pcid.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../lock/spinlock.h"
#include "../smp/cpu.h"
#include <stdbool.h>
#include <stdint.h>
#include "../lib/string.h"

static spinlock_t pcid_lock = SPINLOCK_INIT;
static uint64_t pcid_bitmap[PCID_COUNT / 64];
static uint16_t next_hint = PCID_MIN;
static bool pcid_manager_ready = false;

void pcid_init(void) {
  spinlock_acquire(&pcid_lock);
  memset(pcid_bitmap, 0, sizeof(pcid_bitmap));

  // PCID 0 is permanently reserved for kernel/default address space
  pcid_bitmap[0] |= 1ULL;
  next_hint = PCID_MIN;
  pcid_manager_ready = true;
  spinlock_release(&pcid_lock);

  if (cpu_has_pcid()) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                             " PCID Hardware Tagging Subsystem initialized (4096 PCIDs).\n");
    if (cpu_has_invpcid()) {
      klog_puts("     INVPCID instruction supported.\n");
    }
  } else {
    klog_puts(KLOG_CLR_YELLOW "[ WARN ]" KLOG_CLR_RESET
                              " CPU does not support PCID; running with legacy CR3 flushes.\n");
  }
}

uint16_t pcid_alloc(void) {
  if (!cpu_has_pcid() || !pcid_manager_ready)
    return PCID_KERNEL;

  spinlock_acquire(&pcid_lock);

  // First check from next_hint to PCID_MAX
  for (uint16_t id = next_hint; id <= PCID_MAX; id++) {
    size_t word = id / 64;
    size_t bit = id % 64;
    if (!(pcid_bitmap[word] & (1ULL << bit))) {
      pcid_bitmap[word] |= (1ULL << bit);
      next_hint = (id < PCID_MAX) ? (id + 1) : PCID_MIN;
      spinlock_release(&pcid_lock);
      return id;
    }
  }

  // Wrap around from PCID_MIN to next_hint
  for (uint16_t id = PCID_MIN; id < next_hint; id++) {
    size_t word = id / 64;
    size_t bit = id % 64;
    if (!(pcid_bitmap[word] & (1ULL << bit))) {
      pcid_bitmap[word] |= (1ULL << bit);
      next_hint = (id < PCID_MAX) ? (id + 1) : PCID_MIN;
      spinlock_release(&pcid_lock);
      return id;
    }
  }

  spinlock_release(&pcid_lock);
  // All PCIDs allocated; fallback to kernel/shared PCID (will flush CR3 on switch)
  return PCID_KERNEL;
}

void pcid_free(uint16_t pcid) {
  if (pcid == PCID_KERNEL || pcid > PCID_MAX)
    return;

  spinlock_acquire(&pcid_lock);
  size_t word = pcid / 64;
  size_t bit = pcid % 64;
  pcid_bitmap[word] &= ~(1ULL << bit);
  if (pcid < next_hint)
    next_hint = pcid;
  spinlock_release(&pcid_lock);

  /*
   * Releasing a PCID must leave no core able to reuse it with stale
   * translations, because pcid_alloc() hands ids straight back out.
   *
   * Clearing each CPU's bookkeeping bit is what achieves that: the scheduler
   * only reloads CR3 with CR3_NOFLUSH when the bit is set (sched/sched.c), so
   * the next switch into a recycled PCID is a flushing load.  Note that
   * pcid_flush_context() below can only ever reach this core - INVPCID is a
   * local instruction - so the bitmap clears, not the INVPCID, are what make
   * remote reuse safe.
   */
  uint32_t count = cpu_get_count();
  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (cpu) {
      cpu_pcid_invalidate(cpu, pcid);
    }
  }

  // If INVPCID is available, invalidate the single context locally
  if (cpu_has_invpcid()) {
    pcid_flush_context(pcid);
  }
}

/*
 * The active_pcids_bmp words are shared: a CPU clearing a bit for a PCID it is
 * not running (pcid_free, or a shootdown for a space that is not loaded here)
 * races the owner setting it when it switches in.  Both operations are therefore
 * locked RMWs.  This matters now that a single-address shootdown clears one bit
 * instead of flushing everything: a lost update would leave the bit set, and the
 * scheduler would then reload CR3 with CR3_NOFLUSH and keep using a stale
 * translation forever.
 */
bool cpu_pcid_is_cached(struct cpu_info *cpu, uint16_t pcid) {
  if (!cpu || pcid == PCID_KERNEL || pcid > PCID_MAX)
    return false;
  size_t word = pcid / 64;
  size_t bit = pcid % 64;
  return (__atomic_load_n(&cpu->active_pcids_bmp[word], __ATOMIC_ACQUIRE) &
          (1ULL << bit)) != 0;
}

void cpu_pcid_mark_cached(struct cpu_info *cpu, uint16_t pcid) {
  if (!cpu || pcid == PCID_KERNEL || pcid > PCID_MAX)
    return;
  size_t word = pcid / 64;
  size_t bit = pcid % 64;
  __atomic_or_fetch(&cpu->active_pcids_bmp[word], 1ULL << bit, __ATOMIC_ACQ_REL);
}

void cpu_pcid_invalidate(struct cpu_info *cpu, uint16_t pcid) {
  if (!cpu || pcid == PCID_KERNEL || pcid > PCID_MAX)
    return;
  size_t word = pcid / 64;
  size_t bit = pcid % 64;
  __atomic_and_fetch(&cpu->active_pcids_bmp[word], ~(1ULL << bit),
                     __ATOMIC_ACQ_REL);
}

void cpu_pcid_invalidate_all(struct cpu_info *cpu) {
  if (!cpu)
    return;
  memset(cpu->active_pcids_bmp, 0, sizeof(cpu->active_pcids_bmp));
}

void pcid_flush_context(uint16_t pcid) {
  if (cpu_has_invpcid()) {
    struct invpcid_desc desc = {.pcid = pcid, .rsvd = 0, .addr = 0};
    invpcid(INVPCID_TYPE_SINGLE_CTXT, &desc);
  }
}

void pcid_flush_all(void) {
  if (cpu_has_invpcid()) {
    struct invpcid_desc desc = {0};
    invpcid(INVPCID_TYPE_ALL_NON_GLOBAL, &desc);
  } else {
    // Non-INVPCID full flush: reload CR3 with NOFLUSH=0
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= ~CR3_NOFLUSH;
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
  }
}
