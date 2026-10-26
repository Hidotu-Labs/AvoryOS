#include "mm/page_ops.h"
#include "cpu/features.h"
#include "console/klog.h"

/* Default to safe ERMS implementation before dynamic dispatch probe */
void (*page_zero)(void *page) = page_zero_erms;
void (*page_copy)(void *dst, const void *src) = page_copy_erms;
void (*page_zero_batch)(void **pages, size_t count) = page_zero_nt_batch;

static page_ops_engine_t active_engine = PAGE_OPS_ENGINE_ERMS;

page_ops_engine_t page_ops_get_engine(void) {
  return active_engine;
}

const char *page_ops_engine_name(page_ops_engine_t engine) {
  switch (engine) {
  case PAGE_OPS_ENGINE_NT:
    return "MOVNTI Non-Temporal (64-bit WC Cache Bypass, FPU-Safe)";
  case PAGE_OPS_ENGINE_AVX512:
    return "AVX-512 Non-Temporal (512-bit ZMM / VMOVNTDQ)";
  case PAGE_OPS_ENGINE_AVX2:
    return "AVX2 Non-Temporal (256-bit YMM / VMOVNTDQ)";
  case PAGE_OPS_ENGINE_ERMS:
  default:
    return "ERMS L1-Resident Fast Path (REP STOSQ / REP MOVSQ)";
  }
}

void page_ops_init(void) {
  /*
   * Synchronous single-page operations (demand-zero write faults and CoW page
   * breaks in vmm_fault.c):
   * When a thread takes a write page fault, it is about to write into that page
   * immediately. ERMS (rep stosq / rep movsq) runs at full L1 cache speed
   * (~104 cycles per page) without write-combining buffer draining or sfence stalls,
   * leaving the zeroed page hot in L1 cache for the faulting thread.
   */
  if (cpu_has_erms()) {
    page_zero = page_zero_erms;
    page_copy = page_copy_erms;
    active_engine = PAGE_OPS_ENGINE_ERMS;
  } else {
    page_zero = page_zero_nt;
    page_copy = page_copy_nt;
    active_engine = PAGE_OPS_ENGINE_NT;
  }

  /*
   * Background bulk refill operations (pmm_zero_pool_refill on idle CPUs):
   * Idle CPUs pre-zero 32-page (128 KB) batches in the background. Using non-temporal
   * streaming stores (AVX2 / AVX-512 vmovntdq) bypasses the CPU caches completely,
   * avoiding cache pollution and ensuring userland process working sets in L1/L2/L3
   * are never evicted by background zeroing.
   */
  if (cpu_has_avx512()) {
    page_zero_batch = page_zero_avx512_batch;
  } else if (cpu_has_avx2()) {
    page_zero_batch = page_zero_avx2_batch;
  } else {
    page_zero_batch = page_zero_nt_batch;
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Page Operations Engine: ");
  klog_puts(page_ops_engine_name(active_engine));
  klog_puts(" (Sync) | AVX2/NT Streaming (Batch Pool)\n");
}

void page_zero_huge(void *page) {
  uint8_t *ptr = (uint8_t *)page;
  for (size_t i = 0; i < 512; i++) {
    page_zero(ptr + (i * 4096));
  }
}

void page_copy_huge(void *dst, const void *src) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < 512; i++) {
    page_copy(d + (i * 4096), s + (i * 4096));
  }
}
