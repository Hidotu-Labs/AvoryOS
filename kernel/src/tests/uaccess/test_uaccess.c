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
  UACCESS_ASSERT(copy_from_user(dst_buf, (const void *)0xFFFF800000000000ULL, 8) == 8);
  UACCESS_ASSERT(copy_to_user((void *)0xFFFF800000000000ULL, dst_buf, 8) == 8);
  extern unsigned long clear_user(void *to, unsigned long n);
  UACCESS_ASSERT(clear_user((void *)0xFFFF800000000000ULL, 8) == 8);
  klog_puts("  [UACCESS] Canonical user space bounds protection verified\n");

  // 4. copy_from_user correctness across sizes 0..128 and alignments 0..7
  for (int len = 0; len <= 128; len++) {
    for (int align = 0; align < 8; align++) {
      uint8_t *user_src = kva + align;
      const void *src_va = (const void *)(UACCESS_TEST_VA + (uint64_t)align);
      for (int i = 0; i < len; i++) {
        user_src[i] = (uint8_t)((i + align) ^ 0x5A);
      }
      memset(dst_buf, 0xFF, sizeof(dst_buf));
      unsigned long uncopied = copy_from_user(dst_buf, src_va, (unsigned long)len);
      UACCESS_ASSERT(uncopied == 0);
      if (len > 0) {
        UACCESS_ASSERT(memcmp(dst_buf, user_src, len) == 0);
      }
    }
  }
  klog_puts("  [UACCESS] copy_from_user correctness across all sizes/alignments verified (1032 cases)\n");

  // 5. copy_to_user correctness across sizes 0..128 and alignments 0..7
  char src_buf[512];
  for (int len = 0; len <= 128; len++) {
    for (int align = 0; align < 8; align++) {
      uint8_t *user_dst = kva + align;
      void *dst_va = (void *)(UACCESS_TEST_VA + (uint64_t)align);
      memset(kva, 0, PAGE_SIZE);
      for (int i = 0; i < len; i++) {
        src_buf[i] = (uint8_t)((i * 3 + align) ^ 0xA5);
      }
      unsigned long uncopied = copy_to_user(dst_va, src_buf, (unsigned long)len);
      UACCESS_ASSERT(uncopied == 0);
      if (len > 0) {
        UACCESS_ASSERT(memcmp(user_dst, src_buf, len) == 0);
      }
    }
  }
  klog_puts("  [UACCESS] copy_to_user correctness across all sizes/alignments verified (1032 cases)\n");

  // 6. clear_user correctness across sizes 0..128 and alignments 0..7
  for (int len = 0; len <= 128; len++) {
    for (int align = 0; align < 8; align++) {
      uint8_t *user_dst = kva + align;
      void *dst_va = (void *)(UACCESS_TEST_VA + (uint64_t)align);
      memset(kva, 0xCC, PAGE_SIZE);
      unsigned long uncopied = clear_user(dst_va, (unsigned long)len);
      UACCESS_ASSERT(uncopied == 0);
      for (int i = 0; i < len; i++) {
        UACCESS_ASSERT(user_dst[i] == 0);
      }
      UACCESS_ASSERT(user_dst[len] == 0xCC);
    }
  }
  klog_puts("  [UACCESS] clear_user correctness across all sizes/alignments verified (1032 cases)\n");

  // 7. Performance Benchmarks: 10,000 iterations for small copy sizes
  const int iterations = 10000;
  uint32_t val32 = 0x12345678;
  uint64_t val64 = 0x1122334455667788ULL;
  struct { uint64_t a, b; } val128 = { 0x1111, 0x2222 };
  uint8_t buf64[64];

  // 4-byte copy_from_user
  uint64_t t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_from_user(&val32, (const void *)UACCESS_TEST_VA, sizeof(val32));
  }
  uint64_t t1 = rdtsc_fence();
  uint64_t cycles_cfu4 = (t1 - t0) / iterations;

  // 4-byte copy_to_user
  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_to_user((void *)UACCESS_TEST_VA, &val32, sizeof(val32));
  }
  t1 = rdtsc_fence();
  uint64_t cycles_ctu4 = (t1 - t0) / iterations;

  // 8-byte copy_from_user
  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_from_user(&val64, (const void *)UACCESS_TEST_VA, sizeof(val64));
  }
  t1 = rdtsc_fence();
  uint64_t cycles_cfu8 = (t1 - t0) / iterations;

  // 8-byte copy_to_user
  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_to_user((void *)UACCESS_TEST_VA, &val64, sizeof(val64));
  }
  t1 = rdtsc_fence();
  uint64_t cycles_ctu8 = (t1 - t0) / iterations;

  // 16-byte copy_from_user
  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_from_user(&val128, (const void *)UACCESS_TEST_VA, sizeof(val128));
  }
  t1 = rdtsc_fence();
  uint64_t cycles_cfu16 = (t1 - t0) / iterations;

  // 64-byte copy_from_user
  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    copy_from_user(buf64, (const void *)UACCESS_TEST_VA, sizeof(buf64));
  }
  t1 = rdtsc_fence();
  uint64_t cycles_cfu64 = (t1 - t0) / iterations;

  klog_puts("  [UACCESS BENCH] copy_from_user:\n");
  klogf("            4-byte:  %llu cycles/op\n", (unsigned long long)cycles_cfu4);
  klogf("            8-byte:  %llu cycles/op\n", (unsigned long long)cycles_cfu8);
  klogf("            16-byte: %llu cycles/op\n", (unsigned long long)cycles_cfu16);
  klogf("            64-byte: %llu cycles/op\n", (unsigned long long)cycles_cfu64);

  klog_puts("  [UACCESS BENCH] copy_to_user:\n");
  klogf("            4-byte:  %llu cycles/op\n", (unsigned long long)cycles_ctu4);
  klogf("            8-byte:  %llu cycles/op\n", (unsigned long long)cycles_ctu8);

  // 8. Path string copy benchmark (128 bytes)
  memset(kva + 256, 'x', 127);
  kva[256 + 127] = '\0';
  char bench_dst[128];

  t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    strncpy_from_user(bench_dst, (const char *)(UACCESS_TEST_VA + 256),
                      sizeof(bench_dst));
  }
  t1 = rdtsc_fence();
  uint64_t total_cycles = t1 - t0;
  uint64_t avg_cycles = total_cycles / iterations;

  klogf("  [UACCESS BENCH] 128-byte path strncpy: %llu cycles/path\n",
        (unsigned long long)avg_cycles);

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
