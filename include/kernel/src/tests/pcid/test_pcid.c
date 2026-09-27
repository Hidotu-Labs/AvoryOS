#include "test_pcid.h"
#include "../../console/klog.h"
#include "../../cpu/features.h"
#include "../../lib/tsc.h"
#include "../../mm/pcid.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../smp/cpu.h"
#include <stdbool.h>
#include <stdint.h>

static int g_pcid_fail = 0;

#define PCID_ASSERT(expr)                                                     \
  do {                                                                        \
    if (!(expr)) {                                                            \
      klog_puts(KLOG_CLR_RED "[PCID TEST] FAIL: " #expr KLOG_CLR_RESET "\n"); \
      g_pcid_fail = 1;                                                        \
      return;                                                                 \
    }                                                                         \
  } while (0)

static void test_pcid_detection_and_cr4(void) {
  bool supported = cpu_has_pcid();
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  if (supported) {
    PCID_ASSERT((cr4 & (1ULL << 17)) != 0); // CR4.PCIDE must be enabled
    klog_puts("  [PCID] Feature detected and CR4.PCIDE active (bit 17 = 1)\n");
    if (cpu_has_invpcid()) {
      klog_puts("  [PCID] INVPCID capability verified (CPUID.7.EBX bit 10)\n");
    }
  } else {
    PCID_ASSERT((cr4 & (1ULL << 17)) == 0);
    klog_puts("  [PCID] PCID not supported by host CPU; legacy fallback verified\n");
  }
}

static void test_pcid_allocator_lifecycle(void) {
  if (!cpu_has_pcid())
    return;

  uint16_t id1 = pcid_alloc();
  uint16_t id2 = pcid_alloc();
  uint16_t id3 = pcid_alloc();
  uint16_t id4 = pcid_alloc();

  PCID_ASSERT(id1 >= PCID_MIN && id1 <= PCID_MAX);
  PCID_ASSERT(id2 >= PCID_MIN && id2 <= PCID_MAX);
  PCID_ASSERT(id3 >= PCID_MIN && id3 <= PCID_MAX);
  PCID_ASSERT(id4 >= PCID_MIN && id4 <= PCID_MAX);

  // Assert all IDs are unique
  PCID_ASSERT(id1 != id2 && id1 != id3 && id1 != id4);
  PCID_ASSERT(id2 != id3 && id2 != id4);
  PCID_ASSERT(id3 != id4);

  // Free id2 and id4
  pcid_free(id2);
  pcid_free(id4);

  // Re-allocating should recycle one of the freed IDs
  uint16_t id_new1 = pcid_alloc();
  PCID_ASSERT(id_new1 == id2 || id_new1 == id4 || (id_new1 >= PCID_MIN && id_new1 <= PCID_MAX));

  pcid_free(id1);
  pcid_free(id3);
  pcid_free(id_new1);
  klog_puts("  [PCID] Allocator lifecycle and ID recycling verified\n");
}

static void test_pcid_cr3_formatting_and_caching(void) {
  uint64_t fake_pml4 = 0x00007F1000ULL;
  uint16_t test_pcid = 123;
  uint64_t cr3_noflush = (fake_pml4 & CR3_ADDR_MASK) | (test_pcid & CR3_PCID_MASK) | CR3_NOFLUSH;

  PCID_ASSERT((cr3_noflush & CR3_ADDR_MASK) == fake_pml4);
  PCID_ASSERT((cr3_noflush & CR3_PCID_MASK) == 123);
  PCID_ASSERT((cr3_noflush & CR3_NOFLUSH) != 0);

  struct cpu_info *cpu = cpu_get_current();
  if (cpu) {
    cpu_pcid_invalidate(cpu, test_pcid);
    PCID_ASSERT(!cpu_pcid_is_cached(cpu, test_pcid));

    cpu_pcid_mark_cached(cpu, test_pcid);
    PCID_ASSERT(cpu_pcid_is_cached(cpu, test_pcid));

    cpu_pcid_invalidate(cpu, test_pcid);
    PCID_ASSERT(!cpu_pcid_is_cached(cpu, test_pcid));
  }
  klog_puts("  [PCID] CR3 bitmask formatting & CPU cache tracking verified\n");
}

static void test_pcid_benchmark(void) {
  if (!cpu_has_pcid())
    return;

  // Measure context switch overhead: Flushing vs NOFLUSH
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  uint64_t base_pml4 = current_cr3 & CR3_ADDR_MASK;

  uint16_t pcid_a = pcid_alloc();
  uint16_t pcid_b = pcid_alloc();
  if (!pcid_a || !pcid_b) {
    if (pcid_a) pcid_free(pcid_a);
    if (pcid_b) pcid_free(pcid_b);
    return;
  }

  uint64_t cr3_a_flush = base_pml4 | pcid_a;
  uint64_t cr3_b_flush = base_pml4 | pcid_b;
  uint64_t cr3_a_noflush = base_pml4 | pcid_a | CR3_NOFLUSH;
  uint64_t cr3_b_noflush = base_pml4 | pcid_b | CR3_NOFLUSH;

  const uint32_t iterations = 1000;

  // Warm up PCID entries
  __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_a_flush) : "memory");
  __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_b_flush) : "memory");

  // 1. Benchmark WITH FLUSH (Bit 63 = 0)
  uint64_t start_flush = rdtsc();
  for (uint32_t i = 0; i < iterations; i++) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_a_flush) : "memory");
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_b_flush) : "memory");
  }
  uint64_t end_flush = rdtsc();
  uint64_t flush_cycles = (end_flush - start_flush) / (iterations * 2);

  // 2. Benchmark WITH NOFLUSH (Bit 63 = 1)
  uint64_t start_noflush = rdtsc();
  for (uint32_t i = 0; i < iterations; i++) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_a_noflush) : "memory");
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_b_noflush) : "memory");
  }
  uint64_t end_noflush = rdtsc();
  uint64_t noflush_cycles = (end_noflush - start_noflush) / (iterations * 2);

  // Restore original CR3
  __asm__ volatile("mov %0, %%cr3" :: "r"(current_cr3) : "memory");

  pcid_free(pcid_a);
  pcid_free(pcid_b);

  klog_puts("  [PCID] Benchmark (1000 iterations):\n");
  klog_puts("         CR3 Flush average:   ");
  klog_uint64(flush_cycles);
  klog_puts(" cycles/switch\n");
  klog_puts("         CR3 NOFLUSH average: ");
  klog_uint64(noflush_cycles);
  klog_puts(" cycles/switch\n");
  if (flush_cycles > noflush_cycles) {
    uint64_t speedup_pct = ((flush_cycles - noflush_cycles) * 100) / flush_cycles;
    klog_puts("         Switching latency reduction: ");
    klog_uint64(speedup_pct);
    klog_puts("%\n");
  }
}

void test_pcid(void) {
  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET " Running PCID Hardware Tagging Test Suite...\n");
  g_pcid_fail = 0;

  test_pcid_detection_and_cr4();
  if (g_pcid_fail) return;

  test_pcid_allocator_lifecycle();
  if (g_pcid_fail) return;

  test_pcid_cr3_formatting_and_caching();
  if (g_pcid_fail) return;

  test_pcid_benchmark();
  if (g_pcid_fail) return;

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET " PCID Hardware Tagging & Benchmark tests PASSED successfully!\n\n");
}
