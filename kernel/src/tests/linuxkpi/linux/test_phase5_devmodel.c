/* Phase 5 C5 — synthetic amdgpu-shaped PCI driver + PCI sysfs self-test.
 *
 * Builds the driver shape the P6a compile needs on top of the C1/C2 PCI and
 * IRQ surfaces:
 *
 *   1. a struct pci_driver with an id table binding QEMU's EDU, a
 *      .driver.dev_groups array and a full 6.6-layout .driver.pm dev_pm_ops
 *      (the callback set amdgpu_pm_ops initializes),
 *   2. runtime-PM calls from stock <linux/pm_runtime.h> compiling to the
 *      !CONFIG_PM no-ops,
 *   3. pci_set_power_state()/pci_choose_state()/pci_wake_from_d3() inert
 *      semantics,
 *   4. driver dev_groups materializing under /sys/bus/pci/devices/<bdf> and
 *      disappearing on unbind, plus a direct sysfs_create_file() on the PCI
 *      device kobject.
 *
 * Without EDU the suite logs [SKIP] and never fails. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_vfs.h>

#define P5D_VENDOR 0x1234
#define P5D_DEVICE 0x11e8
#define P5D_NAME "p5c5-synthetic"

static int p5d_failures;
static int p5d_probes;
static int p5d_removes;
static int p5d_runtime_calls;
static int p5d_power_ok;
static struct pci_dev *p5d_probed;

static void p5d_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: devmodel %s\n", what);
}

static void p5d_fail(const char *what, long v) {
  p5d_failures++;
  klogf("[FAIL] LinuxKPI: devmodel %s (%ld)\n", what, v);
}

/* ── PM callbacks (amdgpu-shaped dev_pm_ops) ────────────────────────────── */

static int p5d_pm_prepare(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static void p5d_pm_complete(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
}

static int p5d_pm_suspend(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static int p5d_pm_resume(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static int p5d_pm_runtime_suspend(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static int p5d_pm_runtime_resume(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static int p5d_pm_runtime_idle(struct device *dev) {
  (void)dev;
  p5d_runtime_calls++;
  return 0;
}

static const struct dev_pm_ops p5d_pm_ops = {
    .prepare = p5d_pm_prepare,
    .complete = p5d_pm_complete,
    .suspend = p5d_pm_suspend,
    .resume = p5d_pm_resume,
    .freeze = p5d_pm_suspend,
    .thaw = p5d_pm_resume,
    .poweroff = p5d_pm_suspend,
    .restore = p5d_pm_resume,
    .suspend_noirq = p5d_pm_suspend,
    .resume_noirq = p5d_pm_resume,
    .freeze_noirq = p5d_pm_suspend,
    .thaw_noirq = p5d_pm_resume,
    .poweroff_noirq = p5d_pm_suspend,
    .restore_noirq = p5d_pm_resume,
    .runtime_suspend = p5d_pm_runtime_suspend,
    .runtime_resume = p5d_pm_runtime_resume,
    .runtime_idle = p5d_pm_runtime_idle,
};

/* ── attributes ─────────────────────────────────────────────────────────── */

static ssize_t p5d_attr_show(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "p5d-pci\n");
}
static DEVICE_ATTR_RO(p5d_attr);

static ssize_t p5d_direct_show(struct device *dev,
                               struct device_attribute *attr, char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "p5d-direct\n");
}
static DEVICE_ATTR_RO(p5d_direct);

static struct attribute *p5d_attrs[] = {
    &dev_attr_p5d_attr.attr,
    NULL,
};

static const struct attribute_group p5d_group = {
    .attrs = p5d_attrs,
};

static const struct attribute_group *p5d_groups[] = {
    &p5d_group,
    NULL,
};

/* ── driver ─────────────────────────────────────────────────────────────── */

static int p5d_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
  pm_message_t msg = {.event = 0};

  (void)id;
  p5d_probes++;
  p5d_probed = pdev;

  /* Stock pm_runtime.h no-ops must compile and link against the overlay. */
  pm_runtime_enable(&pdev->dev);
  pm_runtime_get_sync(&pdev->dev);
  pm_runtime_mark_last_busy(&pdev->dev);
  pm_runtime_put_autosuspend(&pdev->dev);
  pm_runtime_put_sync(&pdev->dev);
  pm_runtime_disable(&pdev->dev);

  if (pci_set_power_state(pdev, PCI_D3hot) == 0 &&
      pdev->current_state == PCI_D3hot &&
      pci_set_power_state(pdev, PCI_D0) == 0 &&
      pci_choose_state(pdev, msg) == PCI_D3hot &&
      pci_wake_from_d3(pdev, true) == 0)
    p5d_power_ok = 1;
  return 0;
}

static void p5d_remove(struct pci_dev *pdev) {
  p5d_removes++;
  pm_runtime_get_sync(&pdev->dev);
  pm_runtime_mark_last_busy(&pdev->dev);
  pm_runtime_put_autosuspend(&pdev->dev);
}

static const struct pci_device_id p5d_ids[] = {
    {PCI_DEVICE(P5D_VENDOR, P5D_DEVICE)},
    {},
};

static struct pci_driver p5d_driver = {
    .name = P5D_NAME,
    .id_table = p5d_ids,
    .probe = p5d_probe,
    .remove = p5d_remove,
    .driver =
        {
            .name = P5D_NAME,
            .dev_groups = p5d_groups,
            .pm = &p5d_pm_ops,
        },
};

/* ── helpers ────────────────────────────────────────────────────────────── */

static int p5d_read_path(const char *path, char *buf, int buflen) {
  void *node = asc_vfs_kernel_open(path);
  int n;

  if (!node)
    return -1;
  memset(buf, 0, (size_t)buflen);
  n = (int)asc_vfs_kernel_read(node, 0, (unsigned int)buflen - 1,
                               (unsigned char *)buf);
  asc_vfs_kernel_close(node);
  return n;
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_devmodel(void) {
  struct pci_dev *edu;
  char path[80], buf[48];
  int ret;

  p5d_failures = 0;
  p5d_probes = 0;
  p5d_removes = 0;
  p5d_runtime_calls = 0;
  p5d_power_ok = 0;
  p5d_probed = NULL;
  klog_puts("[LINUXKPI] Phase 5 devmodel self-test\n");

  edu = pci_get_device(P5D_VENDOR, P5D_DEVICE, NULL);
  if (!edu) {
    klog_puts("[SKIP] LinuxKPI: devmodel suite (no edu device)\n");
    return;
  }

  ret = pci_register_driver(&p5d_driver);
  if (ret != 0) {
    p5d_fail("pci_register_driver", ret);
    pci_dev_put(edu);
    return;
  }

  if (p5d_probes == 1 && p5d_probed == edu)
    p5d_ok("amdgpu-shaped driver probed the EDU device");
  else
    p5d_fail("probe", p5d_probes);

  if (p5d_driver.driver.pm == &p5d_pm_ops && p5d_runtime_calls >= 0)
    p5d_ok("6.6 dev_pm_ops + pm_runtime_* no-ops compile and link");
  else
    p5d_fail("dev_pm_ops", 0);

  if (p5d_power_ok)
    p5d_ok("pci_set_power_state/choose_state/wake_from_d3");
  else
    p5d_fail("pci power state", 0);

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/p5d_attr",
           pci_name(edu));
  ret = p5d_read_path(path, buf, sizeof(buf));
  if (ret == 8 && strcmp(buf, "p5d-pci\n") == 0)
    p5d_ok("driver dev_groups appear under /sys/bus/pci/devices/<bdf>");
  else
    p5d_fail("driver dev_groups", ret);

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/p5d_direct",
           pci_name(edu));
  ret = sysfs_create_file(&edu->dev.kobj, &dev_attr_p5d_direct.attr);
  if (ret == 0 && p5d_read_path(path, buf, sizeof(buf)) == 11 &&
      strcmp(buf, "p5d-direct\n") == 0)
    p5d_ok("sysfs_create_file on a PCI device kobject");
  else
    p5d_fail("sysfs_create_file pci", ret);

  sysfs_remove_file(&edu->dev.kobj, &dev_attr_p5d_direct.attr);
  if (asc_vfs_kernel_open(path) == NULL)
    p5d_ok("sysfs_remove_file on a PCI device kobject");
  else
    p5d_fail("sysfs_remove_file pci", 0);

  pci_unregister_driver(&p5d_driver);

  if (p5d_removes == 1)
    p5d_ok("driver remove called on unregister");
  else
    p5d_fail("remove", p5d_removes);

  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/p5d_attr",
           pci_name(edu));
  if (asc_vfs_kernel_open(path) == NULL)
    p5d_ok("dev_groups removed on unbind");
  else
    p5d_fail("dev_groups unbind removal", 0);

  pci_dev_put(edu);

  if (p5d_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: devmodel suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: devmodel suite had %d failure(s)\n", p5d_failures);
}
