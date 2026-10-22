/* Nouveau C3: real imported range trees, GPUVA references and closed gate. */
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <drm/drm_gpuvm.h>
#endif
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/maple_tree.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linuxkpi/log.h>

extern bool linuxkpi_pci_driver_registered(const char *name);
extern bool linuxkpi_pci_nouveau_gate(void);
extern bool linuxkpi_pci_nouveau_probe_attempted(void);

static bool test_maple_ranges(void) {
  struct maple_tree tree;
  struct mutex lock;
  unsigned long tokens[96];
  bool ok = true;
  mutex_init(&lock);
  mt_init_flags(&tree, MT_FLAGS_LOCK_EXTERN);
  mt_set_external_lock(&tree, &lock);
  mutex_lock(&lock);
  /* Enough disjoint ranges to split nodes, then erase and coalesce gaps. */
  for (unsigned long i = 0; i < 96; i++) {
    unsigned long first = i * 32 + 8;
    MA_STATE(mas, &tree, first, first + 15);
    tokens[i] = i;
    if (mas_store_gfp(&mas, &tokens[i], GFP_KERNEL)) {
      ok = false;
      break;
    }
  }
  for (unsigned long i = 0; i < 96 && ok; i++) {
    unsigned long first = i * 32 + 8;
    MA_STATE(mas, &tree, first, first);
    if (mas_walk(&mas) != &tokens[i] || mas.index != first || mas.last != first + 15)
      ok = false;
    mas.index = first - 1;
    mas_reset(&mas);
    if (mas_walk(&mas))
      ok = false;
  }
  for (unsigned long i = 0; i < 96 && ok; i += 2) {
    unsigned long first = i * 32 + 8;
    MA_STATE(mas, &tree, first, first);
    if (mas_erase(&mas) != &tokens[i])
      ok = false;
  }
  for (unsigned long i = 0; i < 96 && ok; i++) {
    unsigned long first = i * 32 + 8;
    MA_STATE(mas, &tree, first, first);
    void *expected = (i & 1) ? &tokens[i] : NULL;
    if (mas_walk(&mas) != expected)
      ok = false;
  }
  __mt_destroy(&tree);
  mutex_unlock(&lock);
  synchronize_rcu();
  mutex_destroy(&lock);
  return ok;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static unsigned vm_frees, gem_frees;
static void test_vm_free(struct drm_gpuvm *vm) { (void)vm; vm_frees++; }
static void test_gem_free(struct drm_gem_object *obj) {
  drm_gem_object_release(obj);
  gem_frees++;
}
static const struct drm_gpuvm_ops vm_ops = { .vm_free = test_vm_free };
static const struct drm_gem_object_funcs gem_ops = { .free = test_gem_free };
static const struct drm_driver test_driver = {
  .driver_features = DRIVER_GEM,
  .name = "kpi-gpuvm-test", .desc = "GPUVM lifetime test",
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
  .date = "20261019",
#endif
};
static struct device test_parent;

static bool test_gpuva_lifetime(void) {
  struct drm_gpuvm vm = {0};
  struct drm_gem_object object = {0};
  struct drm_gpuva a = {0}, b = {0}, overlap = {0}, reserved = {0};
  bool linked_a = false, linked_b = false, ok = true;
  device_initialize(&test_parent);
  dev_set_name(&test_parent, "kpi-gpuvm-test");
  struct drm_device *dev = drm_dev_alloc(&test_driver, &test_parent);
  if (IS_ERR(dev))
    return false;
  vm_frees = gem_frees = 0;
  drm_gem_private_object_init(dev, &object, 4096);
  object.funcs = &gem_ops;
  drm_gpuvm_init(&vm, "kpi-gpuvm-test", 0, dev, &object,
                  0x1000, 0x10000, 0x1000, 0x1000, &vm_ops);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  a.va.addr = 0x2000; a.va.range = 0x1000;
  b.va.addr = 0x4000; b.va.range = 0x1000;
  overlap.va.addr = 0x2800; overlap.va.range = 0x1000;
  reserved.va.addr = 0x1000; reserved.va.range = 0x1000;
#else
  drm_gpuva_init(&a, 0x2000, 0x1000, NULL, 0);
  drm_gpuva_init(&b, 0x4000, 0x1000, NULL, 0);
  drm_gpuva_init(&overlap, 0x2800, 0x1000, NULL, 0);
  drm_gpuva_init(&reserved, 0x1000, 0x1000, NULL, 0);
#endif
  if (drm_gpuva_insert(&vm, &a)) ok = false; else linked_a = true;
  if (drm_gpuva_insert(&vm, &b)) ok = false; else linked_b = true;
  if (drm_gpuva_insert(&vm, &overlap) != -EEXIST) {
    if (overlap.vm) drm_gpuva_remove(&overlap);
    ok = false;
  }
  if (drm_gpuva_insert(&vm, &reserved) != -EINVAL) {
    if (reserved.vm) drm_gpuva_remove(&reserved);
    ok = false;
  }
  if (drm_gpuva_find_first(&vm, 0x2000, 0x1000) != &a ||
      drm_gpuva_find_first(&vm, 0x3000, 0x1000) ||
      drm_gpuvm_range_valid(&vm, ~0ULL - 7, 16))
    ok = false;
  drm_gpuvm_put(&vm); /* Mappings must retain the VM after the owner drops it. */
  if (linked_a) drm_gpuva_remove(&a);
  if (linked_b && vm_frees) ok = false;
  if (linked_b) drm_gpuva_remove(&b);
  if (vm_frees != 1 || gem_frees) ok = false;
  drm_gem_object_put(&object);
  if (gem_frees != 1) ok = false;
  drm_dev_put(dev);
  return ok;
}

#endif

void linuxkpi_test_nouveau_compat(void) {
  if (test_maple_ranges())
    klogf("[  OK  ] LinuxKPI: nouveau maple splits, gaps, erase and teardown\n");
  else
    klogf("[FAIL] LinuxKPI: nouveau maple range semantics\n");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
  if (test_gpuva_lifetime())
    klogf("[  OK  ] LinuxKPI: nouveau GPUVA overlap, reservations and VM/GEM lifetime\n");
  else
    klogf("[FAIL] LinuxKPI: nouveau GPUVA semantics/lifetime\n");
#else
  klogf("[SKIP] LinuxKPI: nouveau GPUVA test requires the selected 6.12 API\n");
#endif
  if (!linuxkpi_pci_driver_registered("nouveau")) {
    klogf("[SKIP] LinuxKPI: nouveau driver not linked\n");
  } else if (!linuxkpi_pci_nouveau_gate()) {
    if (linuxkpi_pci_nouveau_probe_attempted())
      klogf("[FAIL] LinuxKPI: nouveau probe ran with hardware gate closed\n");
    else
      klogf("[  OK  ] LinuxKPI: nouveau driver registered with hardware gate closed, zero probes\n");
  } else {
    klogf("[INFO] LinuxKPI: nouveau driver registered with explicit hardware opt-in\n");
  }
}
