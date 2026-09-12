/* Phase 1 — remaining upstream library imports: crc32, xxhash, idr, kfifo and
 * the cheap math helpers.  Compiled with the real Linux headers. */

#include <linux/crc32.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/rational.h>
#include <linux/reciprocal_div.h>
#include <linux/slab.h>
#include <linux/xxhash.h>

#include <linuxkpi/log.h>

/* ── crc32 ──────────────────────────────────────────────────────────────── */

static bool test_crc32(void) {
  static const unsigned char check[] = "123456789";

  /* CRC-32/ISO-HDLC and CRC-32C check values ("123456789"): the kernel
   * functions are raw (caller supplies init/final xor), so invert here. */
  u32 le = crc32_le(~0u, check, sizeof(check) - 1) ^ ~0u;
  u32 c = __crc32c_le(~0u, check, sizeof(check) - 1) ^ ~0u;

  return le == 0xCBF43926u && c == 0xE3069283u;
}

/* ── xxhash ─────────────────────────────────────────────────────────────── */

static bool test_xxhash(void) {
  static const void *empty = "";

  /* Canonical XXH32/XXH64 vectors for the empty input with seed 0. */
  return xxh32(empty, 0, 0) == 0x02CC5D05u &&
         xxh64(empty, 0, 0) == 0xEF46DB3751D8E999ULL &&
         xxh32("abc", 3, 0) == xxh32("abc", 3, 0);
}

/* ── idr ────────────────────────────────────────────────────────────────── */

static bool test_idr(void) {
  DEFINE_IDR(idr);
  bool ok;

  int a = idr_alloc(&idr, (void *)0x1000, 0, 0, GFP_KERNEL);
  int b = idr_alloc(&idr, (void *)0x2000, 0, 0, GFP_KERNEL);
  if (a < 0 || b < 0 || a == b)
    return false;

  ok = idr_find(&idr, a) == (void *)0x1000 &&
       idr_find(&idr, b) == (void *)0x2000 &&
       idr_find(&idr, b + 1000) == NULL;

  idr_remove(&idr, a);
  ok = ok && idr_find(&idr, a) == NULL;

  int seen = 0;
  int id;
  void *entry;
  idr_for_each_entry(&idr, entry, id) {
    if (id == b && entry == (void *)0x2000)
      seen++;
  }
  ok = ok && seen == 1;

  idr_destroy(&idr);
  return ok;
}

/* ── kfifo ──────────────────────────────────────────────────────────────── */

static bool test_kfifo(void) {
  DECLARE_KFIFO(fifo, unsigned char, 16);
  unsigned char seq[16];
  unsigned char out[8];
  bool ok;

  for (int i = 0; i < 16; i++)
    seq[i] = (unsigned char)i;

  INIT_KFIFO(fifo);

  /* Basic roundtrip. */
  unsigned int n = kfifo_in(&fifo, seq, 6);
  ok = (n == 6) && (kfifo_len(&fifo) == 6);
  n = kfifo_out(&fifo, out, 6);
  ok = ok && (n == 6) && kfifo_is_empty(&fifo);
  for (int i = 0; i < 6; i++)
    ok = ok && out[i] == (unsigned char)i;

  /* Wrap: fill 12, drain 8, push 4 through the end, drain everything and
   * check the bytes come back in order. */
  kfifo_reset(&fifo);
  kfifo_in(&fifo, seq, 12);
  kfifo_out(&fifo, out, 8);
  kfifo_in(&fifo, seq + 12, 4);
  n = kfifo_out(&fifo, out, 8);
  ok = ok && n == 8;
  for (int i = 0; i < 8; i++)
    ok = ok && out[i] == (unsigned char)(8 + i) && kfifo_is_empty(&fifo);

  return ok;
}

/* ── cheap math helpers ─────────────────────────────────────────────────── */

static bool test_rational(void) {
  unsigned long n = 0, d = 0;

  rational_best_approximation(16, 10, 1UL << 20, 1UL << 20, &n, &d);
  if (n != 8 || d != 5)
    return false;

  rational_best_approximation(1, 3, 100, 100, &n, &d);
  return n == 1 && d == 3;
}

static bool test_reciprocal_div(void) {
  struct reciprocal_value rv = reciprocal_value(10);

  for (u32 v = 0; v < 100000; v += 7) {
    if (reciprocal_divide(v, rv) != v / 10)
      return false;
  }
  return true;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase1_imports(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"crc32", test_crc32},
      {"xxhash", test_xxhash},
      {"idr", test_idr},
      {"kfifo", test_kfifo},
      {"rational", test_rational},
      {"reciprocal_div", test_reciprocal_div},
  };

  klog_puts("[LINUXKPI] Phase 1 imports self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
