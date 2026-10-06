#include "test_page_ops.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lib/tsc.h"
#include "mm/page_ops.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/vmm_internal.h"
#include <stdint.h>
#include <stdbool.h>

void test_page_ops(void) {
  klog_puts("[TEST] Running Hardware-Accelerated Page Operations Test Suite...\n");

  void *p1_phys = pmm_alloc_page();
  void *p2_phys = pmm_alloc_page();
  if (!p1_phys || !p2_phys) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_page_ops: failed to allocate test pages\n" KLOG_CLR_RESET);
    if (p1_phys) pmm_free_page(p1_phys);
    if (p2_phys) pmm_free_page(p2_phys);
    return;
  }

  uint8_t *page1 = (uint8_t *)PHYS_TO_VIRT((uint64_t)p1_phys);
  uint8_t *page2 = (uint8_t *)PHYS_TO_VIRT((uint64_t)p2_phys);

  // 1. Correctness: page_zero_nt
  memset(page1, 0x5A, 4096);
  page_zero_nt(page1);

  bool zero_ok = true;
  for (size_t i = 0; i < 4096; i++) {
    if (page1[i] != 0) {
      zero_ok = false;
      break;
    }
  }

  if (!zero_ok) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_page_ops: page_zero_nt failed to zero entire 4KB page\n" KLOG_CLR_RESET);
    pmm_free_page(p1_phys);
    pmm_free_page(p2_phys);
    return;
  }

  // Correctness: page_zero_erms
  memset(page1, 0xA5, 4096);
  page_zero_erms(page1);
  for (size_t i = 0; i < 4096; i++) {
    if (page1[i] != 0) {
      zero_ok = false;
      break;
    }
  }
  if (!zero_ok) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_page_ops: page_zero_erms failed to zero entire 4KB page\n" KLOG_CLR_RESET);
    pmm_free_page(p1_phys);
    pmm_free_page(p2_phys);
    return;
  }
  klog_puts("  [PAGE_OPS] page_zero (NT & ERMS) 4KB byte-level verification passed\n");

  // 2. Correctness: page_copy_nt
  for (size_t i = 0; i < 4096; i++) {
    page1[i] = (uint8_t)(i * 37 + 13);
  }
  memset(page2, 0, 4096);
  page_copy_nt(page2, page1);

  bool copy_ok = true;
  for (size_t i = 0; i < 4096; i++) {
    if (page2[i] != page1[i]) {
      copy_ok = false;
      break;
    }
  }

  if (!copy_ok) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_page_ops: page_copy_nt content mismatch\n" KLOG_CLR_RESET);
    pmm_free_page(p1_phys);
    pmm_free_page(p2_phys);
    return;
  }

  // Correctness: page_copy_erms
  memset(page2, 0, 4096);
  page_copy_erms(page2, page1);
  for (size_t i = 0; i < 4096; i++) {
    if (page2[i] != page1[i]) {
      copy_ok = false;
      break;
    }
  }
  if (!copy_ok) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_page_ops: page_copy_erms content mismatch\n" KLOG_CLR_RESET);
    pmm_free_page(p1_phys);
    pmm_free_page(p2_phys);
    return;
  }
  klog_puts("  [PAGE_OPS] page_copy (NT & ERMS) 4KB pattern integrity verified\n");

  // 3. Micro-benchmark (1,000 iterations each)
  const uint32_t iters = 1000;
  uint64_t t0, t1;

  // Benchmark page_zero_nt
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    page_zero_nt(page1);
  }
  t1 = rdtsc();
  uint64_t zero_nt_cycles = (t1 - t0) / iters;

  // Benchmark page_zero_erms
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    page_zero_erms(page1);
  }
  t1 = rdtsc();
  uint64_t zero_erms_cycles = (t1 - t0) / iters;

  // Benchmark standard memset
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    memset(page1, 0, 4096);
  }
  t1 = rdtsc();
  uint64_t memset_cycles = (t1 - t0) / iters;

  // Benchmark page_copy_nt
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    page_copy_nt(page2, page1);
  }
  t1 = rdtsc();
  uint64_t copy_nt_cycles = (t1 - t0) / iters;

  // Benchmark page_copy_erms
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    page_copy_erms(page2, page1);
  }
  t1 = rdtsc();
  uint64_t copy_erms_cycles = (t1 - t0) / iters;

  // Benchmark standard memcpy
  t0 = rdtsc();
  for (uint32_t i = 0; i < iters; i++) {
    memcpy(page2, page1, 4096);
  }
  t1 = rdtsc();
  uint64_t memcpy_cycles = (t1 - t0) / iters;

  klog_puts("  [BENCH] page_zero_nt: ");
  klog_uint64(zero_nt_cycles);
  klog_puts(" cycles | erms: ");
  klog_uint64(zero_erms_cycles);
  klog_puts(" cycles | memset: ");
  klog_uint64(memset_cycles);
  klog_puts(" cycles\n");

  klog_puts("  [BENCH] page_copy_nt: ");
  klog_uint64(copy_nt_cycles);
  klog_puts(" cycles | erms: ");
  klog_uint64(copy_erms_cycles);
  klog_puts(" cycles | memcpy: ");
  klog_uint64(memcpy_cycles);
  klog_puts(" cycles\n");

  pmm_free_page(p1_phys);
  pmm_free_page(p2_phys);

  klog_puts(KLOG_CLR_GREEN "[PASS] Hardware-Accelerated Page Operations tests PASSED successfully!\n" KLOG_CLR_RESET);
}
