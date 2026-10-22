/* Phase 1 — memory, synchronization and container library tests.
 *
 * Compiled with the real Linux headers.  Exercises the native-backed slab,
 * spinlock and RCU overlays through upstream libraries: xarray, idr, refcount
 * and siphash.
 */

#include <linux/refcount.h>
#include <linux/siphash.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#include <linuxkpi/log.h>

static bool test_slab(void) {
  unsigned char *p = kzalloc(64, GFP_KERNEL);
  if (!p)
    return false;

  for (int i = 0; i < 64; i++) {
    if (p[i] != 0) {
      kfree(p);
      return false;
    }
    p[i] = (unsigned char)i;
  }

  unsigned char *q = kmemdup(p, 64, GFP_KERNEL);
  if (!q) {
    kfree(p);
    return false;
  }
  bool ok = true;
  for (int i = 0; i < 64; i++) {
    if (q[i] != (unsigned char)i)
      ok = false;
  }
  kfree(q);
  kfree(p);

  struct kmem_cache *c = kmem_cache_create("phase1", 96, 16, 0, NULL);
  if (!c)
    return false;

  void *o1 = kmem_cache_alloc(c, GFP_KERNEL);
  unsigned char *o2 = kmem_cache_zalloc(c, GFP_KERNEL);
  if (!o1 || !o2) {
    ok = false;
  } else {
    if (ksize(o1) != 96)
      ok = false;
    for (int i = 0; i < 96; i++) {
      if (o2[i] != 0)
        ok = false;
    }
  }

  kfree(o1);
  kmem_cache_free(c, o2);
  kmem_cache_destroy(c);
  return ok;
}

static bool test_spinlock(void) {
  DEFINE_SPINLOCK(lock);
  unsigned long flags;

  spin_lock_irqsave(&lock, flags);
  bool held = spin_is_locked(&lock);
  bool try_fails_while_held = !spin_trylock(&lock);
  spin_unlock_irqrestore(&lock, flags);

  bool try_succeeds_free = spin_trylock(&lock);
  if (try_succeeds_free)
    spin_unlock(&lock);

  return held && try_fails_while_held && try_succeeds_free;
}

static bool test_xarray(void) {
  DEFINE_XARRAY(xa);
  void *p = kmalloc(16, GFP_KERNEL);
  if (!p)
    return false;

  bool ok = true;

  if (xa_store(&xa, 5, p, GFP_KERNEL) != NULL)
    ok = false;
  if (xa_load(&xa, 5) != p)
    ok = false;
  if (xa_load(&xa, 6) != NULL)
    ok = false;
  if (xa_erase(&xa, 5) != p)
    ok = false;
  if (xa_load(&xa, 5) != NULL)
    ok = false;

  /* xa_destroy() drops the internal node through call_rcu(); this checks the
   * stopgap callback path at least once. */
  if (xa_store(&xa, 2, p, GFP_KERNEL) != NULL)
    ok = false;
  xa_destroy(&xa);

  kfree(p);
  return ok;
}

static bool test_refcount(void) {
  refcount_t r;

  refcount_set(&r, 1);
  refcount_inc(&r);
  if (refcount_read(&r) != 2)
    return false;
  if (refcount_dec_and_test(&r))
    return false;
  return refcount_dec_and_test(&r);
}

static bool test_siphash(void) {
  siphash_key_t key = {
      .key = {0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL}};
  u64 a = siphash_1u64(0x123456789abcdef0ULL, &key);
  u64 b = siphash_1u64(0x123456789abcdef0ULL, &key);
  u64 c = siphash_1u64(0x123456789abcdef1ULL, &key);

  return a == b && a != c;
}

void linuxkpi_test_phase1_mem(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"slab", test_slab},         {"spinlock", test_spinlock},
      {"xarray", test_xarray},     {"refcount", test_refcount},
      {"siphash", test_siphash},
  };

  klog_puts("[LINUXKPI] Phase 1 memory/sync self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s (upstream) correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s (upstream) wrong result\n", tests[i].name);
  }
}
