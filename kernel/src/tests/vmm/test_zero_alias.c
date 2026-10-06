// Regression test: present anonymous data must never read back zero.
//
// Incident: a long-running threaded process (gdbus/XFCE session) died with
// SIGSEGV inside musl mallocng's alloc_meta -> get_random_secret, reading a
// NULL libc.auxv. That path runs only when malloc_context.init_done reads 0,
// yet the process had allocated successfully for its whole lifetime — and
// both zeroed fields sit on the same 4 KiB ld-musl BSS page while every
// neighboring mapping stayed intact. A live, dirty, anonymous data page was
// observed as zero: the fault-around demand-zero installer must never
// replace (or disturb) a present data mapping when filling neighboring
// holes, and map-if-unmapped must refuse present pages.
//
// This test drives the real helpers (vmm_page_present_nolock and
// vmm_map_page_if_unmapped) on a scratch address space exactly the way the
// fault path does: data on even pages (sentinels mimic an initialized libc:
// a nonzero flag word plus a nonzero pointer word), holes on odd pages,
// zero-fill the holes, then verify every data PTE bit-identical and every
// sentinel word intact. Deterministic: no timing or cross-CPU racing
// involved, so a FAIL here means the installer itself is broken.
#include "../../console/klog.h"
#include "../../mm/pmm.h"
#include "../../mm/vma.h"
#include "../../mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static int g_zero_alias_fail = 0;

#define ZERO_ALIAS_ASSERT(expr)                                                \
  do {                                                                         \
    if (!(expr)) {                                                             \
      klog_puts(KLOG_CLR_RED "[ZERO ALIAS TEST] FAIL: " #expr KLOG_CLR_RESET  \
                             "\n");                                            \
      g_zero_alias_fail = 1;                                                   \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

void test_zero_alias(void) {
  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET
                           " Running Zero-Fill Neighbor Alias Test...\n");
  g_zero_alias_fail = 0;

  uint64_t *pml4 = vmm_create_pml4();
  ZERO_ALIAS_ASSERT(pml4 != NULL);

  struct vma_list vmas;
  vma_list_init(&vmas);

  const uint64_t base_vaddr = 0x0000000000400000ULL;
  const size_t num_pages = 64;
  const uint64_t end_vaddr = base_vaddr + (num_pages * 4096);

  int vma_rc =
      vma_add(&vmas, base_vaddr, end_vaddr, 3 /* RW */,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, NULL, 0);
  ZERO_ALIAS_ASSERT(vma_rc == 0);

  uint64_t zp = pmm_get_zero_page_phys();
  ZERO_ALIAS_ASSERT(zp != 0);

  // Even pages: real frames with sentinel content (init-like flag + pointer).
  // Odd pages: holes, mirroring not-yet-touched BSS neighbors.
  uint64_t frames[64] = {0};
  for (size_t i = 0; i < num_pages; i += 2) {
    void *phys = pmm_alloc();
    ZERO_ALIAS_ASSERT(phys != NULL);
    frames[i] = (uint64_t)phys;
    bool ok = vmm_map_page(pml4, base_vaddr + (i * 4096), (uint64_t)phys,
                           PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW);
    ZERO_ALIAS_ASSERT(ok);
    uint64_t *k = (uint64_t *)PHYS_TO_VIRT((uint64_t)phys);
    k[0] = 1;                    // init_done-like: must never read back 0
    k[1] = 0x0000400000A2AC0ULL; // auxv-like nonzero pointer
    for (size_t w = 2; w < 512; w++)
      k[w] = 0xA5A5000000000000ULL | ((uint64_t)i << 32) | (uint64_t)w;
  }

  // Walk to the PT exactly like test_vmm_clone (same base: L4[0]/L3[0]/L2[2]).
  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  uint64_t *p_pdp = (uint64_t *)PHYS_TO_VIRT(pml4_virt[0] & PAGE_MASK);
  uint64_t *p_pd = (uint64_t *)PHYS_TO_VIRT(p_pdp[0] & PAGE_MASK);
  uint64_t *p_pt = (uint64_t *)PHYS_TO_VIRT(p_pd[2] & PAGE_MASK);

  uint64_t pte_snap[64];
  for (size_t i = 0; i < num_pages; i++) {
    uint64_t va = base_vaddr + (i * 4096);
    if (i & 1) {
      ZERO_ALIAS_ASSERT(p_pt[i] == 0);
      ZERO_ALIAS_ASSERT(!vmm_page_present_nolock(pml4, va));
    } else {
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_FLAG_PRESENT) != 0);
      ZERO_ALIAS_ASSERT(vmm_page_present_nolock(pml4, va));
      pte_snap[i] = p_pt[i];
    }
  }

  const uint64_t zflags =
      PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_COW;

  // Present data pages must be refused (never replaced).
  for (size_t i = 0; i < num_pages; i += 2) {
    uint64_t va = base_vaddr + (i * 4096);
    ZERO_ALIAS_ASSERT(!vmm_map_page_if_unmapped(pml4, va, zp, zflags));
  }

  // Fill the holes the way the fault-around path does.
  for (size_t i = 1; i < num_pages; i += 2) {
    uint64_t va = base_vaddr + (i * 4096);
    ZERO_ALIAS_ASSERT(vmm_map_page_if_unmapped(pml4, va, zp, zflags));
  }

  // Re-derive the PT pointer (same pattern as the clone test) and verify:
  // every data PTE bit-identical, every sentinel intact, holes now alias
  // the shared zero page read-only.
  pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  p_pdp = (uint64_t *)PHYS_TO_VIRT(pml4_virt[0] & PAGE_MASK);
  p_pd = (uint64_t *)PHYS_TO_VIRT(p_pdp[0] & PAGE_MASK);
  p_pt = (uint64_t *)PHYS_TO_VIRT(p_pd[2] & PAGE_MASK);
  for (size_t i = 0; i < num_pages; i++) {
    uint64_t va = base_vaddr + (i * 4096);
    if (i & 1) {
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_MASK) == (zp & PAGE_MASK));
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_FLAG_PRESENT) != 0);
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_FLAG_USER) != 0);
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_FLAG_RW) == 0);
      ZERO_ALIAS_ASSERT((p_pt[i] & PAGE_FLAG_COW) != 0);
      ZERO_ALIAS_ASSERT(vmm_page_present_nolock(pml4, va));
    } else {
      ZERO_ALIAS_ASSERT(p_pt[i] == pte_snap[i]);
      uint64_t *k =
          (uint64_t *)PHYS_TO_VIRT(frames[i]);
      ZERO_ALIAS_ASSERT(k[0] == 1);
      ZERO_ALIAS_ASSERT(k[1] == 0x0000400000A2AC0ULL);
      for (size_t w = 2; w < 512; w++)
        ZERO_ALIAS_ASSERT(k[w] ==
                          (0xA5A5000000000000ULL | ((uint64_t)i << 32) |
                           (uint64_t)w));
    }
  }
  klog_puts("  [VMM] 32 data pages stable across 32 neighboring zero fills; "
            "present pages refused replacement\n");

cleanup:
  // Data frames drop their single reference here; shared zero-page holes
  // are a decref no-op by design (pmm guards the zero page).
  vmm_free_user_pages_vma((uint64_t)pml4, &vmas);
  vma_list_destroy(&vmas);

  if (g_zero_alias_fail) {
    klog_puts(KLOG_CLR_RED "[FAIL]" KLOG_CLR_RESET
                             " Zero-Fill Neighbor Alias test FAILED!\n\n");
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET
                             " Zero-Fill Neighbor Alias test PASSED!\n\n");
}
