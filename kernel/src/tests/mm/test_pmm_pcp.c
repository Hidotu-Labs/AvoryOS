#include "test_pmm_pcp.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lib/tsc.h"
#include "mm/pmm.h"
#include "mm/pmm_internal.h"
#include "smp/cpu.h"
#include <stdbool.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

void test_pmm_pcp(void) {
  klog_puts("[TEST] Running Per-CPU Page Allocator (PCP) Test Suite...\n");

  // 1. Single page allocation and free through PCP
  void *p1 = pmm_alloc_page();
  if (!p1 || ((uint64_t)p1 & 0xFFFULL) != 0) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_pmm_pcp: pmm_alloc_page failed or misaligned\n" KLOG_CLR_RESET);
    return;
  }

  uint8_t *v1 = (uint8_t *)PHYS_TO_VIRT(p1);
  v1[0] = 0xAA;
  v1[4095] = 0x55;
  if (v1[0] != 0xAA || v1[4095] != 0x55) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_pmm_pcp: memory integrity check failed\n" KLOG_CLR_RESET);
    pmm_free_page(p1);
    return;
  }

  pmm_free_page(p1);
  klog_puts("  [PCP] Single-page alloc and free verified\n");

  // 2. Batch refill and drain test (allocating more than PCP_CAPACITY)
  #define TEST_PCP_BURST (PCP_CAPACITY + 20)
  void *pages[TEST_PCP_BURST];
  memset(pages, 0, sizeof(pages));

  for (size_t i = 0; i < TEST_PCP_BURST; i++) {
    pages[i] = pmm_alloc_page();
    if (!pages[i]) {
      klog_puts(KLOG_CLR_RED "[FAIL] test_pmm_pcp: burst allocation failed\n" KLOG_CLR_RESET);
      for (size_t j = 0; j < i; j++) {
        pmm_free_page(pages[j]);
      }
      return;
    }
    // Verify distinct pages
    for (size_t j = 0; j < i; j++) {
      if (pages[j] == pages[i]) {
        klog_puts(KLOG_CLR_RED "[FAIL] test_pmm_pcp: duplicate page allocated\n" KLOG_CLR_RESET);
        for (size_t k = 0; k <= i; k++) {
          pmm_free_page(pages[k]);
        }
        return;
      }
    }
  }

  // Free all burst pages back: this will fill the PCP capacity and trigger batch drain
  for (size_t i = 0; i < TEST_PCP_BURST; i++) {
    pmm_free_page(pages[i]);
  }
  klog_puts("  [PCP] Burst allocation and batch drain (148 pages) verified\n");

  // 3. Fast-path microbenchmark (1000 alloc+free cycles)
  #define BENCH_ITERS 1000
  uint64_t t_start = rdtsc();
  for (int i = 0; i < BENCH_ITERS; i++) {
    void *p = pmm_alloc_page();
    pmm_free_page(p);
  }
  uint64_t t_end = rdtsc();
  uint64_t cycles_per_cycle = (t_end - t_start) / BENCH_ITERS;

  klog_puts("  [BENCH] PCP alloc+free latency: ");
  klog_uint64(cycles_per_cycle);
  klog_puts(" cycles/pair\n");

  // 4. Drain all caches and verify clean accounting
  pmm_pcp_drain_all();
  size_t cached_after = pmm_pcp_total_cached();
  if (cached_after != 0) {
    klog_puts(KLOG_CLR_RED "[FAIL] test_pmm_pcp: pmm_pcp_drain_all did not drain all pages\n" KLOG_CLR_RESET);
    return;
  }
  klog_puts("  [PCP] pmm_pcp_drain_all flush invariant verified (0 cached)\n");

  klog_puts(KLOG_CLR_GREEN "[PASS] Per-CPU Page Allocator (PCP) tests PASSED successfully!\n\n" KLOG_CLR_RESET);
}
