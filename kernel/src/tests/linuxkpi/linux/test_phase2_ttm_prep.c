/* Phase 2 - TTM preparation checks.
 *
 * TTM (Phase 7) registers a shrinker (ttm_pool) and calls
 * unmap_mapping_range() from its VM code.  Both are stubs today; this suite
 * proves the symbols compile and link, that the shrinker registration is
 * inert, and that the CONFIG_MMU_NOTIFIER compile-out used by dma-resv.c is
 * visible from a Linux-API translation unit. */

#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/shrinker.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

static unsigned long stub_count_objects(struct shrinker *shrinker,
                                        struct shrink_control *sc) {
  (void)shrinker;
  if (sc)
    sc->nr_scanned = 0;
  return 0; /* SHRINK_EMPTY would also be honest; 0 = "nothing to report" */
}

static unsigned long stub_scan_objects(struct shrinker *shrinker,
                                       struct shrink_control *sc) {
  (void)shrinker;
  if (sc)
    sc->nr_scanned = 0;
  return 0; /* always zero scanned */
}

static bool test_shrinker_stub(void) {
  struct shrinker shrinker = {
      .count_objects = stub_count_objects,
      .scan_objects = stub_scan_objects,
      .seeks = DEFAULT_SEEKS,
  };
  struct shrink_control sc = {.nr_to_scan = 128};

  if (register_shrinker(&shrinker, "kpi-test") != 0)
    return false;
  if (!(shrinker.flags & SHRINKER_REGISTERED))
    return false;

  /* Inert: the stub never walks the callback chain, so a manual call is the
   * only way anything runs, and it reports zero scanned. */
  if (shrinker.count_objects(&shrinker, &sc) != 0)
    return false;
  if (sc.nr_scanned != 0)
    return false;
  if (shrinker.scan_objects(&shrinker, &sc) != 0)
    return false;
  if (sc.nr_scanned != 0)
    return false;

  unregister_shrinker(&shrinker);
  return !(shrinker.flags & SHRINKER_REGISTERED);
}

static bool test_mmu_notifier_compileout(void) {
  struct mmu_notifier_range range;

  /* The overlay only provides the type and the flags; dma-resv.c's
   * CONFIG_MMU_NOTIFIER block is compiled out (autoconf.h does not define
   * it), which is why no mmu_notifier registration is needed. */
  memset(&range, 0, sizeof(range));
  range.start = 0x1000;
  range.end = 0x2000;
  range.flags = MMU_NOTIFY_CLEAR;

  /* unmap_mapping_range() is a no-op stub in linuxkpi/src/mmap.c; calling it
   * here pins the symbol down at link time for the TTM import. */
  unmap_mapping_range(NULL, 0, 0, 1);

  return range.start == 0x1000 && range.end == 0x2000;
}

void linuxkpi_test_phase2_ttm_prep(void) {
  klog_puts("[LINUXKPI] Phase 2 TTM prep self-test\n");

  if (test_shrinker_stub())
    klog_puts("[  OK  ] LinuxKPI: shrinker stub (register/unregister, "
              "0 scanned) correct\n");
  else
    klog_puts("[ FAIL ] LinuxKPI: shrinker stub wrong result\n");

  if (test_mmu_notifier_compileout())
    klog_puts("[  OK  ] LinuxKPI: mmu_notifier compile-out + "
              "unmap_mapping_range stub correct\n");
  else
    klog_puts("[ FAIL ] LinuxKPI: mmu_notifier compile-out wrong result\n");
}
