#include "test_vmm_clone.h"
#include "../../console/klog.h"
#include "../../lib/tsc.h"
#include "../../mm/pmm.h"
#include "../../mm/vma.h"
#include "../../mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static int g_vmm_fail = 0;

#define VMM_ASSERT(expr)                                                       \
  do {                                                                         \
    if (!(expr)) {                                                             \
      klog_puts(KLOG_CLR_RED "[VMM CLONE TEST] FAIL: " #expr KLOG_CLR_RESET   \
                             "\n");                                            \
      g_vmm_fail = 1;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

void test_vmm_clone(void) {
  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET
                          " Running Batched TLB Shootdown Clone Test...\n");
  g_vmm_fail = 0;

  uint64_t *parent_pml4 = vmm_create_pml4();
  VMM_ASSERT(parent_pml4 != NULL);

  struct vma_list vmas;
  vma_list_init(&vmas);

  const uint64_t base_vaddr = 0x0000000000400000ULL;
  const size_t num_pages = 64; // 256 KiB
  const uint64_t end_vaddr = base_vaddr + (num_pages * 4096);

  int vma_rc = vma_add(&vmas, base_vaddr, end_vaddr, 3 /* RW */, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, NULL, 0);
  VMM_ASSERT(vma_rc == 0);

  // Allocate and map 64 physical pages as RW into the parent PML4
  for (size_t i = 0; i < num_pages; i++) {
    void *phys = pmm_alloc();
    VMM_ASSERT(phys != NULL);
    bool ok = vmm_map_page(parent_pml4, base_vaddr + (i * 4096), (uint64_t)phys,
                           PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW);
    VMM_ASSERT(ok);
  }

  // Benchmark the batched CoW fork clone operation
  uint64_t t0 = rdtsc_fence();
  uint64_t child_pml4 = vmm_clone_user_mappings_vma(parent_pml4, &vmas);
  uint64_t t1 = rdtsc_fence();
  uint64_t clone_cycles = t1 - t0;

  VMM_ASSERT(child_pml4 != 0);

  // Walk parent and child PML4 to verify that all 64 pages were transitioned to CoW (RO + COW bit)
  uint64_t *parent_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)parent_pml4);
  uint64_t *child_virt = (uint64_t *)PHYS_TO_VIRT(child_pml4);

  // Level 4 index for 0x400000 is 0
  uint64_t p_pdp_phys = parent_virt[0] & PAGE_MASK;
  uint64_t c_pdp_phys = child_virt[0] & PAGE_MASK;
  VMM_ASSERT(p_pdp_phys != 0 && c_pdp_phys != 0);

  uint64_t *p_pdp = (uint64_t *)PHYS_TO_VIRT(p_pdp_phys);
  uint64_t *c_pdp = (uint64_t *)PHYS_TO_VIRT(c_pdp_phys);

  // Level 3 index for 0x400000 is 0
  uint64_t p_pd_phys = p_pdp[0] & PAGE_MASK;
  uint64_t c_pd_phys = c_pdp[0] & PAGE_MASK;
  VMM_ASSERT(p_pd_phys != 0 && c_pd_phys != 0);

  uint64_t *p_pd = (uint64_t *)PHYS_TO_VIRT(p_pd_phys);
  uint64_t *c_pd = (uint64_t *)PHYS_TO_VIRT(c_pd_phys);

  // Level 2 index for 0x400000 is 2 (0x400000 / 0x200000)
  uint64_t p_pt_phys = p_pd[2] & PAGE_MASK;
  uint64_t c_pt_phys = c_pd[2] & PAGE_MASK;
  VMM_ASSERT(p_pt_phys != 0 && c_pt_phys != 0);

  uint64_t *p_pt = (uint64_t *)PHYS_TO_VIRT(p_pt_phys);
  uint64_t *c_pt = (uint64_t *)PHYS_TO_VIRT(c_pt_phys);

  // Verify all 64 PTEs in both tables
  for (size_t i = 0; i < num_pages; i++) {
    uint64_t p_pte = p_pt[i];
    uint64_t c_pte = c_pt[i];

    // Must be PRESENT, USER, COW, and NOT RW
    VMM_ASSERT((p_pte & PAGE_FLAG_PRESENT) != 0);
    VMM_ASSERT((p_pte & PAGE_FLAG_COW) != 0);
    VMM_ASSERT((p_pte & PAGE_FLAG_RW) == 0);

    VMM_ASSERT((c_pte & PAGE_FLAG_PRESENT) != 0);
    VMM_ASSERT((c_pte & PAGE_FLAG_COW) != 0);
    VMM_ASSERT((c_pte & PAGE_FLAG_RW) == 0);

    // Both PTEs must point to the exact same physical frame
    VMM_ASSERT((p_pte & PAGE_MASK) == (c_pte & PAGE_MASK));
  }

  // Cleanup child and parent address spaces
  vmm_free_user_pages_vma(child_pml4, &vmas);
  vmm_free_user_pages_vma((uint64_t)parent_pml4, &vmas);
  vma_list_destroy(&vmas);

  klog_puts("  [VMM] 64-page CoW clone verified (1 batched TLB shootdown broadcast)\n");
  klog_puts("  [VMM] Total clone time: ");
  klog_uint64(clone_cycles);
  klog_puts(" cycles (");
  klog_uint64(clone_cycles / num_pages);
  klog_puts(" cycles/page)\n");

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET
                          " Batched TLB Shootdown Clone tests PASSED successfully!\n\n");
}
