#include "test_uaccess.h"
#include "../../console/klog.h"
#include "../../lib/string.h"
#include "../../lib/tsc.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

extern long strncpy_from_user(char *dst, const char *src, long count);
extern long strnlen_user(const char *src, long maxlen);
extern unsigned long copy_from_user(void *to, const void *from, unsigned long n);
extern unsigned long copy_to_user(void *to, const void *from, unsigned long n);

/* One user-mapped page to test against.  The routines reject addresses above
 * the user-space limit, so kernel stack buffers cannot be used as fake user
 * pointers the way they used to be here. */
#define UACCESS_TEST_VA 0x0000000040000000ULL

static int g_uaccess_fail = 0;

#define UACCESS_ASSERT(expr)                                                   \
  do {                                                                         \
    if (!(expr)) {                                                             \
      klog_puts(KLOG_CLR_RED "[UACCESS TEST] FAIL: " #expr KLOG_CLR_RESET       \
                             "\n");                                            \
      g_uaccess_fail = 1;                                                      \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

void test_uaccess(void) {
  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET
                          " Running Fast Word-at-a-time uaccess Test Suite...\n");
  g_uaccess_fail = 0;

  uint64_t *pml4 = vmm_get_active_pml4();
  void *phys = NULL;
  uint8_t *kva;
  bool mapped = false;
  char dst_buf[512];

  UACCESS_ASSERT(pml4 != NULL);
  phys = pmm_alloc_page();
  UACCESS_ASSERT(phys != NULL);
  UACCESS_ASSERT(vmm_map_page(pml4, UACCESS_TEST_VA, (uint64_t)phys,
                              PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                                  PAGE_FLAG_RW));
  mapped = true;
  vmm_flush_tlb(UACCESS_TEST_VA);

  kva = (uint8_t *)((uint64_t)phys + pmm_get_hhdm_offset());

  // 1. Correctness across alignments (0..7) and string lengths
  for (int len = 0; len <= 128; len++) {
    for (int align = 0; align < 8; align++) {
      uint8_t *user_src = kva + align;
      const char *src_va = (const char *)(UACCESS_TEST_VA + (uint64_t)align);

      memset(kva, 0, PAGE_SIZE);
      memset(dst_buf, 0xBB, sizeof(dst_buf));

      for (int i = 0; i < len; i++) {
        user_src[i] = 'A' + (i % 26);
      }
      user_src[len] = '\0';

      long copied = strncpy_from_user(dst_buf, src_va, sizeof(dst_buf));
      UACCESS_ASSERT(copied == len);
      UACCESS_ASSERT(dst_buf[len] == '\0');
      if (len > 0) {
        UACCESS_ASSERT(memcmp(dst_buf, user_src, len) == 0);
      }

      long slen = strnlen_user(src_va, sizeof(dst_buf));
      UACCESS_ASSERT(slen == (len + 1));
    }
  }
  klog_puts("  [UACCESS] strncpy_from_user & strnlen_user alignment/length verified (1032 cases)\n");

  // 2. Truncation when count < strlen
  memset(kva, 0, 64);
  memcpy(kva + 64, "0123456789abcdef0123456789abcdef", 33);
  memset(dst_buf, 0, sizeof(dst_buf));
  long trunc_res =
      strncpy_from_user(dst_buf, (const char *)(UACCESS_TEST_VA + 64), 10);
  UACCESS_ASSERT(trunc_res == 10);
  UACCESS_ASSERT(memcmp(dst_buf, "0123456789", 10) == 0);
  klog_puts("  [UACCESS] String truncation bounds verified\n");

  // 3. Exception bounds check (-EFAULT on out of range)
  long fault_res = strncpy_from_user(dst_buf, (const char *)0xFFFF800000000000ULL, 64);
  UACCESS_ASSERT(fault_res == -14); // -EFAULT
  klog_puts("  [UACCESS] Canonical user space bounds protection verified\n");

  // 4. Performance Benchmark: 10,000 iterations copying a 128-byte path
  memset(kva + 256, 'x', 127);
  kva[256 + 127] = '\0';
  char bench_dst[128];

  const int iterations = 10000;
  uint64_t t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    strncpy_from_user(bench_dst, (const char *)(UACCESS_TEST_VA + 256),
                      sizeof(bench_dst));
  }
  uint64_t t1 = rdtsc_fence();
  uint64_t total_cycles = t1 - t0;
  uint64_t avg_cycles = total_cycles / iterations;

  klog_puts("  [UACCESS] 128-byte path copy benchmark (10,000 iterations):\n");
  klog_puts("            Average copy time: ");
  klog_uint64(avg_cycles);
  klog_puts(" cycles/path (");
  klog_uint64((avg_cycles * 1000) / 128);
  klog_puts(" millicycles/byte)\n");

cleanup:
  if (mapped) {
    vmm_unmap_page(pml4, UACCESS_TEST_VA);
    vmm_free_empty_tables(pml4, UACCESS_TEST_VA);
    vmm_flush_tlb(UACCESS_TEST_VA);
  }
  if (phys)
    pmm_free_page(phys);

  if (g_uaccess_fail) {
    klog_puts(KLOG_CLR_RED "[FAIL]" KLOG_CLR_RESET
                            " Fast Word-at-a-time uaccess tests FAILED!\n\n");
    return;
  }

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET
                            " Fast Word-at-a-time uaccess tests PASSED successfully!\n\n");
}
