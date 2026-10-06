#include "test_numa.h"
#include "console/klog.h"
#include "mm/numa.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include "mm/mempolicy.h"
#include "mm/numa_migrate.h"
#include "sched/sched.h"
#include "lib/string.h"

void test_numa(void) {
  klog_puts("[TEST] Running NUMA Subsystem Self-Test...\n");

  uint32_t count = numa_node_count();
  if (count < 1) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: invalid node count: " KLOG_CLR_RESET);
    klog_uint64(count);
    klog_puts("\n");
    return;
  }

  struct numa_node *n0 = numa_get_node(0);
  if (!n0 || !n0->online) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: Node 0 is offline!\n" KLOG_CLR_RESET);
    return;
  }

  // Distance checks
  if (numa_distance(0, 0) != NUMA_LOCAL_DISTANCE) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: local distance != 10\n" KLOG_CLR_RESET);
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    for (uint32_t j = 0; j < count; j++) {
      if (numa_distance(i, j) != numa_distance(j, i)) {
        klog_puts(KLOG_CLR_RED "[FAIL] test_numa: asymmetric distance\n" KLOG_CLR_RESET);
        return;
      }
    }
  }

  // Node-directed single page allocation
  void *p = pmm_alloc_page_node(0);
  if (!p) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: pmm_alloc_page_node(0) returned NULL\n" KLOG_CLR_RESET);
    return;
  }
  if (pmm_get_ref(p) != 1) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: pmm_get_ref(p) != 1\n" KLOG_CLR_RESET);
    pmm_free_page(p);
    return;
  }
  pmm_free_page(p);

  // Node-directed multi-page allocation
  void *p_block = pmm_alloc_pages_node(4, 0);
  if (!p_block) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: pmm_alloc_pages_node(4, 0) returned NULL\n" KLOG_CLR_RESET);
    return;
  }
  pmm_free_pages(p_block, 4);

  // Node-directed huge page allocation
  void *hp = pmm_alloc_huge_page_node(0);
  if (!hp) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: pmm_alloc_huge_page_node(0) returned NULL\n" KLOG_CLR_RESET);
    return;
  }
  if ((uint64_t)hp & 0x1FFFFFULL) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: huge page not 2MB aligned!\n" KLOG_CLR_RESET);
    pmm_free_pages(hp, 512);
    return;
  }
  pmm_free_pages(hp, 512);

  // CPU to node mapping
  int bsp_node = numa_cpu_to_node(0);
  if (bsp_node < 0 || bsp_node >= MAX_NUMA_NODES) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: invalid BSP node\n" KLOG_CLR_RESET);
    return;
  }

  // Memory policy engine test
  struct mempolicy pol;
  mempolicy_init(&pol);
  if (pol.mode != MPOL_DEFAULT) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: default policy != MPOL_DEFAULT\n" KLOG_CLR_RESET);
    return;
  }

  pol.mode = MPOL_PREFERRED;
  pol.preferred_node = 0;
  if (mempolicy_select_node(&pol, 0x1000) != 0) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: preferred node 0 did not select 0\n" KLOG_CLR_RESET);
    return;
  }

  pol.mode = MPOL_BIND;
  pol.nodemask = 1ULL; // Node 0 only
  if (mempolicy_select_node(&pol, 0x2000) != 0) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: bind node 0 did not select 0\n" KLOG_CLR_RESET);
    return;
  }

  pol.mode = MPOL_INTERLEAVE;
  pol.nodemask = (1ULL << 0);
  if (mempolicy_select_node(&pol, 0x3000) != 0) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: interleave single node did not select 0\n" KLOG_CLR_RESET);
    return;
  }

  // Nodemask allocation test
  void *p_mask = pmm_alloc_pages_nodemask(1, 0, 1ULL);
  if (!p_mask) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_numa: pmm_alloc_pages_nodemask failed\n" KLOG_CLR_RESET);
    return;
  }
  pmm_free_page(p_mask);

  // Page migration self-test on an anonymous page
  struct thread *cur = sched_get_current();
  if (cur && cur->mm) {
    uint64_t test_va = 0x50000000;
    void *phys_frame = pmm_alloc_page_node(0);
    if (phys_frame) {
      memset(PHYS_TO_VIRT((uint64_t)phys_frame), 0xAB, PAGE_SIZE);
      uint64_t cr3 = (cur->cr3 != 0) ? cur->cr3 : (uint64_t)vmm_get_active_pml4();

      vmm_map_page((uint64_t *)cr3, test_va, (uint64_t)phys_frame,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER);

      // Verify page is on Node 0
      uint64_t *pte_ptr = vmm_get_pte_ptr((uint64_t *)cr3, test_va);
      if (!pte_ptr || (*pte_ptr & PAGE_MASK) != (uint64_t)phys_frame) {
        klog_puts(KLOG_CLR_RED "[FAIL] test_numa: test page map verification failed\n" KLOG_CLR_RESET);
      } else {
        // Test migration to Node 0 (same node: must succeed as no-op)
        int mig_ret = numa_migrate_page(cur, test_va, 0);
        if (mig_ret != 0) {
          klog_puts(KLOG_CLR_RED "[FAIL] test_numa: numa_migrate_page(same node) failed\n" KLOG_CLR_RESET);
        }

        // Test migration to Node 1 if available
        if (count > 1) {
          mig_ret = numa_migrate_page(cur, test_va, 1);
          if (mig_ret == 0) {
            uint64_t *new_pte = vmm_get_pte_ptr((uint64_t *)cr3, test_va);
            if (new_pte) {
              uint64_t new_phys = *new_pte & PAGE_MASK;
              if (numa_phys_to_node(new_phys) != 1) {
                klog_puts(KLOG_CLR_RED "[FAIL] test_numa: migrated page not on Node 1\n" KLOG_CLR_RESET);
              }
              // Verify content was preserved
              uint8_t *vptr = (uint8_t *)PHYS_TO_VIRT(new_phys);
              if (vptr[0] != 0xAB || vptr[4095] != 0xAB) {
                klog_puts(KLOG_CLR_RED "[FAIL] test_numa: migrated page content corrupted!\n" KLOG_CLR_RESET);
              }
            }
          }
        }
      }

      // Unmap test page
      vmm_unmap_page((uint64_t *)cr3, test_va);
    }
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " test_numa: all checks passed (nodes=");
  klog_uint64(count);
  klog_puts(" mode=");
  klog_puts(numa_is_available() ? "NUMA" : "UMA");
  klog_puts(")\n");
}
