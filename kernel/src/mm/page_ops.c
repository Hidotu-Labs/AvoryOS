#include "mm/page_ops.h"
#include "cpu/features.h"
#include "console/klog.h"

/* Default to safe ERMS implementation before dynamic dispatch probe */
void (*page_zero)(void *page) = page_zero_erms;
void (*page_copy)(void *dst, const void *src) = page_copy_erms;

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
    return "ERMS / String Operations (REP STOSQ / REP MOVSQ)";
  }
}

void page_ops_init(void) {
  /*
   * Kernel page faults, CoW, and demand-zero operations execute in interrupt/
   * exception context on behalf of user threads. Vector operations (AVX-512/AVX2)
   * in this path clobber user YMM/ZMM registers and execute VZEROUPPER, destroying
   * userland process state.
   *
   * We use 64-bit MOVNTI non-temporal stores and PREFETCHNTA: this achieves
   * full L1/L2/L3 cache bypass and eliminates RFO overhead (preserving the 32MB L3
   * cache on Zen 4 / Ryzen 5 7600) while using only general-purpose registers,
   * making it 100% interrupt-safe, exception-safe, and zero-FPU-overhead.
   */
  page_zero = page_zero_nt;
  page_copy = page_copy_nt;
  active_engine = PAGE_OPS_ENGINE_NT;

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Page Operations Engine: ");
  klog_puts(page_ops_engine_name(active_engine));
  klog_puts("\n");
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
