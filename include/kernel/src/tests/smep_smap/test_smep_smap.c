#include "test_smep_smap.h"
#include "../../console/klog.h"
#include "../../cpu/features.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

extern long smap_probe_read(const void *p);
extern long smap_probe_read_ac(const void *p);
extern long smep_probe_exec(void *p);
extern unsigned long copy_from_user(void *to, const void *from, unsigned long n);
extern unsigned long copy_to_user(void *to, const void *from, unsigned long n);

/* A throwaway user address: the boot address space has no user mappings yet,
 * and the test unmaps the page again on the way out. */
#define SMEP_SMAP_TEST_VA 0x0000000040000000ULL
#define SMEP_SMAP_MAGIC 0x1122334455667788ULL

static int g_smep_smap_fail = 0;

#define SMEP_SMAP_ASSERT(expr)                                                 \
  do {                                                                         \
    if (!(expr)) {                                                             \
      klog_puts(KLOG_CLR_RED "[SMEP/SMAP TEST] FAIL: " #expr KLOG_CLR_RESET    \
                             "\n");                                            \
      g_smep_smap_fail = 1;                                                    \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static uint64_t smep_smap_read_cr4(void) {
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  return cr4;
}

static void smep_smap_print_feature(const char *name, bool supported,
                                    bool enabled) {
  klog_puts("  [SMEP/SMAP] ");
  klog_puts(name);
  klog_puts(": ");
  if (!supported) {
    klog_puts("not supported by CPU (checks skipped)\n");
  } else if (enabled) {
    klog_puts(KLOG_CLR_GREEN "enabled" KLOG_CLR_RESET " in CR4\n");
  } else {
    klog_puts(KLOG_CLR_RED "supported but NOT enabled in CR4" KLOG_CLR_RESET
                           "\n");
  }
}

void test_smep_smap(void) {
  bool has_smep;
  bool has_smap;
  bool mapped = false;
  uint64_t cr4;
  uint64_t *pml4;
  void *phys = NULL;
  uint8_t *kva;
  uint64_t copy_src = 0;
  uint64_t copy_dst = 0xDEADBEEFCAFEF00DULL;

  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET
                          " Running SMEP/SMAP Test Suite...\n");
  g_smep_smap_fail = 0;

  has_smep = cpu_has_smep();
  has_smap = cpu_has_smap();
  cr4 = smep_smap_read_cr4();
  pml4 = vmm_get_active_pml4();

  smep_smap_print_feature("SMEP", has_smep, (cr4 & (1ULL << 20)) != 0);
  smep_smap_print_feature("SMAP", has_smap, (cr4 & (1ULL << 21)) != 0);
  SMEP_SMAP_ASSERT(cpu_has_smap_flag == has_smap);

  /* 1. Map one present, user-accessible, executable page. */
  SMEP_SMAP_ASSERT(pml4 != NULL);
  phys = pmm_alloc_page();
  SMEP_SMAP_ASSERT(phys != NULL);
  SMEP_SMAP_ASSERT(vmm_map_page(pml4, SMEP_SMAP_TEST_VA, (uint64_t)phys,
                                PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                                    PAGE_FLAG_RW));
  mapped = true;
  vmm_flush_tlb(SMEP_SMAP_TEST_VA);

  kva = (uint8_t *)((uint64_t)phys + pmm_get_hhdm_offset());
  memset(kva, 0, PAGE_SIZE);
  kva[0] = 0xC3; /* ret — what the SMEP probe tries to execute */
  *(uint64_t *)(kva + 8) = SMEP_SMAP_MAGIC;

  /* 2. The real uaccess helpers must work against a genuine user mapping and
   *    must not leave AC set for the caller afterwards. */
  SMEP_SMAP_ASSERT(copy_from_user(&copy_src,
                                  (const void *)(SMEP_SMAP_TEST_VA + 8),
                                  sizeof(copy_src)) == 0);
  SMEP_SMAP_ASSERT(copy_src == SMEP_SMAP_MAGIC);
  SMEP_SMAP_ASSERT(copy_to_user((void *)(SMEP_SMAP_TEST_VA + 16), &copy_dst,
                                sizeof(copy_dst)) == 0);
  SMEP_SMAP_ASSERT(*(uint64_t *)(kva + 16) == copy_dst);
  SMEP_SMAP_ASSERT(!ac_flag_set());
  klog_puts("  [SMEP/SMAP] copy_{to,from}_user against a user page verified\n");

  /* 3. SMAP: a raw supervisor read of the mapped user page must fault while AC
   *    is clear, and succeed once stac sets AC. */
  if (has_smap) {
    SMEP_SMAP_ASSERT((cr4 & (1ULL << 21)) != 0);
    SMEP_SMAP_ASSERT(smap_probe_read((const void *)(SMEP_SMAP_TEST_VA + 8)) ==
                     -14);
    SMEP_SMAP_ASSERT(!ac_flag_set());
    SMEP_SMAP_ASSERT(smap_probe_read_ac(
                         (const void *)(SMEP_SMAP_TEST_VA + 8)) ==
                     (long)SMEP_SMAP_MAGIC);
    SMEP_SMAP_ASSERT(!ac_flag_set());
    klog_puts("  [SMEP/SMAP] SMAP fault without AC and stac override "
              "verified\n");
  } else {
    SMEP_SMAP_ASSERT(smap_probe_read((const void *)(SMEP_SMAP_TEST_VA + 8)) ==
                     (long)SMEP_SMAP_MAGIC);
  }

  /* 4. SMEP: a kernel call into the user page must fault when SMEP is on.
   *    Without SMEP the page's `ret` executes and the probe returns 1. */
  if (has_smep) {
    SMEP_SMAP_ASSERT((cr4 & (1ULL << 20)) != 0);
    SMEP_SMAP_ASSERT(smep_probe_exec((void *)SMEP_SMAP_TEST_VA) == 0);
    SMEP_SMAP_ASSERT(!ac_flag_set());
    klog_puts("  [SMEP/SMAP] SMEP kernel-execution trap verified\n");
  } else {
    SMEP_SMAP_ASSERT(smep_probe_exec((void *)SMEP_SMAP_TEST_VA) == 1);
  }

cleanup:
  if (mapped) {
    vmm_unmap_page(pml4, SMEP_SMAP_TEST_VA);
    vmm_free_empty_tables(pml4, SMEP_SMAP_TEST_VA);
    vmm_flush_tlb(SMEP_SMAP_TEST_VA);
  }
  if (phys)
    pmm_free_page(phys);

  if (g_smep_smap_fail) {
    klog_puts(KLOG_CLR_RED "[FAIL]" KLOG_CLR_RESET
                            " SMEP/SMAP tests FAILED!\n\n");
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET
                            " SMEP/SMAP tests PASSED successfully!\n\n");
}
