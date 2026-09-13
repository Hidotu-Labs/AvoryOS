/* Phase 5 C5 — dynamic sysfs + devres completion self-test.
 *
 * Covers the synthetic device path end to end through the native sysfs tree
 * (asc_vfs_kernel_open/read/write):
 *
 *   1. device_create_with_groups() materializes a real class device dir and
 *      its attribute groups: RO/RW attributes, is_visible() skipping, and a
 *      named subgroup in its own directory,
 *   2. binary attributes: full and partial reads at offsets, a write, and
 *      EOF behavior, all with bin_attr size bounds,
 *   3. removal: device_remove_groups() unlinks the files (and releases their
 *      per-attribute contexts), then device_add_groups() re-creates them,
 *   4. devres: registrations release in reverse order on device unregister,
 *      devm_kasprintf()/devm_kmalloc_array() contents, and the
 *      devm_ioremap_resource() invalid-resource path,
 *   5. a create/destroy loop with a PMM baseline (context and node release).
 *
 * The PCI-side synthetic driver lives in test_phase5_devmodel.c. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>
#include <linuxkpi/native_vfs.h>

#define P5S_CLASS_NAME "p5c5"
#define P5S_DEV_NAME "dev0"
#define P5S_DEV_DIR "/sys/class/" P5S_CLASS_NAME "/" P5S_DEV_NAME
#define P5S_LOOP 64
#define P5S_BIN_SIZE 256

static int p5s_failures;
static int p5s_rw_value;

static void p5s_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: sysfs %s\n", what);
}

static void p5s_fail(const char *what, long v) {
  p5s_failures++;
  klogf("[FAIL] LinuxKPI: sysfs %s (%ld)\n", what, v);
}

/* ── attributes ─────────────────────────────────────────────────────────── */

static ssize_t p5s_ro_show(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "p5c5-ro\n");
}
static DEVICE_ATTR_RO(p5s_ro);

static ssize_t p5s_rw_show(struct device *dev, struct device_attribute *attr,
                           char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "value=%d\n", p5s_rw_value);
}

static ssize_t p5s_rw_store(struct device *dev, struct device_attribute *attr,
                            const char *buf, size_t count) {
  (void)dev;
  (void)attr;
  p5s_rw_value = (int)(buf[0] - '0');
  return (ssize_t)count;
}
static DEVICE_ATTR_RW(p5s_rw);

static ssize_t p5s_hidden_show(struct device *dev,
                               struct device_attribute *attr, char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "hidden\n");
}
static DEVICE_ATTR_RO(p5s_hidden);

static ssize_t p5s_sub_show(struct device *dev, struct device_attribute *attr,
                            char *buf) {
  (void)dev;
  (void)attr;
  return sysfs_emit(buf, "p5c5-sub\n");
}
static DEVICE_ATTR_RO(p5s_sub);

static umode_t p5s_is_visible(struct kobject *kobj, struct attribute *attr,
                              int n) {
  (void)kobj;
  (void)n;
  if (attr == &dev_attr_p5s_hidden.attr)
    return 0;
  return attr->mode;
}

static struct attribute *p5s_attrs[] = {
    &dev_attr_p5s_ro.attr,
    &dev_attr_p5s_rw.attr,
    &dev_attr_p5s_hidden.attr,
    NULL,
};

static struct attribute *p5s_sub_attrs[] = {
    &dev_attr_p5s_sub.attr,
    NULL,
};

static const struct attribute_group p5s_sub_group = {
    .name = "sub",
    .attrs = p5s_sub_attrs,
};

static const struct attribute_group p5s_group = {
    .attrs = p5s_attrs,
    .is_visible = p5s_is_visible,
    .groups = (const struct attribute_group *[]){&p5s_sub_group, NULL},
};

static u8 p5s_bin_data[P5S_BIN_SIZE];

static ssize_t p5s_bin_read(struct file *file, struct kobject *kobj,
                            struct bin_attribute *attr, char *buf, loff_t off,
                            size_t count) {
  (void)file;
  (void)kobj;
  (void)attr;
  if (off >= P5S_BIN_SIZE)
    return 0;
  if (off + (loff_t)count > P5S_BIN_SIZE)
    count = P5S_BIN_SIZE - (size_t)off;
  memcpy(buf, p5s_bin_data + off, count);
  return (ssize_t)count;
}

static ssize_t p5s_bin_write(struct file *file, struct kobject *kobj,
                             struct bin_attribute *attr, char *buf, loff_t off,
                             size_t count) {
  (void)file;
  (void)kobj;
  (void)attr;
  if (off >= P5S_BIN_SIZE)
    return 0;
  if (off + (loff_t)count > P5S_BIN_SIZE)
    count = P5S_BIN_SIZE - (size_t)off;
  memcpy(p5s_bin_data + off, buf, count);
  return (ssize_t)count;
}

static struct bin_attribute p5s_bin = {
    .attr = {.name = "p5bin", .mode = 0644},
    .size = P5S_BIN_SIZE,
    .read = p5s_bin_read,
    .write = p5s_bin_write,
};

static struct bin_attribute *p5s_bin_attrs[] = {&p5s_bin, NULL};

static const struct attribute_group p5s_bin_group = {
    .bin_attrs = p5s_bin_attrs,
};

static const struct attribute_group *p5s_groups[] = {
    &p5s_group,
    &p5s_bin_group,
    NULL,
};

/* ── kernel-side path helpers (native VFS bridge) ───────────────────────── */

static int p5s_read_path(const char *path, char *buf, int buflen) {
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

static int p5s_write_path(const char *path, const char *s) {
  void *node = asc_vfs_kernel_open(path);
  int n;

  if (!node)
    return -1;
  n = (int)asc_vfs_kernel_write(node, 0, (unsigned int)strlen(s),
                                (unsigned char *)s);
  asc_vfs_kernel_close(node);
  return n;
}

/* ── devres ordering ────────────────────────────────────────────────────── */

static int p5s_devres_order[4];
static int p5s_devres_count;

static void p5s_devres_release(void *data) {
  p5s_devres_order[p5s_devres_count++] = (int)(long)data;
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_sysfs(void) {
  struct class *cls;
  struct device *dev;
  unsigned long pmm_before;
  char buf[64];
  int i, ret;

  p5s_failures = 0;
  p5s_rw_value = 0;
  for (i = 0; i < P5S_BIN_SIZE; i++)
    p5s_bin_data[i] = (u8)(i ^ 0x5a);
  klog_puts("[LINUXKPI] Phase 5 sysfs/devres self-test\n");

  cls = class_create(P5S_CLASS_NAME);
  if (!cls) {
    p5s_fail("class_create", 0);
    return;
  }

  dev = device_create_with_groups(cls, NULL, 0, NULL, p5s_groups, P5S_DEV_NAME);
  if (IS_ERR_OR_NULL(dev)) {
    p5s_fail("device_create_with_groups", dev ? PTR_ERR(dev) : -ENOMEM);
    class_destroy(cls);
    return;
  }

  /* 1: text attributes through the native sysfs path. */
  ret = p5s_read_path(P5S_DEV_DIR "/p5s_ro", buf, sizeof(buf));
  if (ret == 8 && strcmp(buf, "p5c5-ro\n") == 0)
    p5s_ok("RO device attribute read");
  else
    p5s_fail("RO attribute", ret);

  ret = p5s_write_path(P5S_DEV_DIR "/p5s_rw", "7");
  if (ret == 1) {
    ret = p5s_read_path(P5S_DEV_DIR "/p5s_rw", buf, sizeof(buf));
    if (p5s_rw_value == 7 && ret > 0 && strcmp(buf, "value=7\n") == 0)
      p5s_ok("RW device attribute store + read back");
    else
      p5s_fail("RW attribute readback", p5s_rw_value);
  } else {
    p5s_fail("RW attribute store", ret);
  }

  if (asc_vfs_kernel_open(P5S_DEV_DIR "/p5s_hidden") == NULL)
    p5s_ok("is_visible() skips the hidden attribute");
  else
    p5s_fail("hidden attribute", 0);

  ret = p5s_read_path(P5S_DEV_DIR "/sub/p5s_sub", buf, sizeof(buf));
  if (ret == 9 && strcmp(buf, "p5c5-sub\n") == 0)
    p5s_ok("named subgroup materialized in its own directory");
  else
    p5s_fail("named subgroup", ret);

  /* 2: binary attribute with offset handling. */
  {
    void *node = asc_vfs_kernel_open(P5S_DEV_DIR "/p5bin");
    u8 b[P5S_BIN_SIZE];

    if (!node) {
      p5s_fail("bin attribute open", 0);
    } else {
      memset(b, 0, sizeof(b));
      ret = (int)asc_vfs_kernel_read(node, 0, 16, b);
      if (ret == 16 && b[0] == 0x5a && b[15] == (u8)(15 ^ 0x5a))
        p5s_ok("binary attribute full/partial read");
      else
        p5s_fail("bin partial read", ret);

      memset(b, 0, sizeof(b));
      ret = (int)asc_vfs_kernel_read(node, P5S_BIN_SIZE - 10, 64, b);
      if (ret == 10 && b[0] == (u8)((P5S_BIN_SIZE - 10) ^ 0x5a))
        p5s_ok("binary attribute EOF clamp");
      else
        p5s_fail("bin EOF clamp", ret);

      ret = (int)asc_vfs_kernel_read(node, P5S_BIN_SIZE, 16, b);
      if (ret == 0)
        p5s_ok("binary attribute read at or past EOF");
      else
        p5s_fail("bin EOF", ret);

      memset(b, 0, sizeof(b));
      for (i = 0; i < 8; i++)
        b[i] = (u8)(0xa0 + i);
      ret = (int)asc_vfs_kernel_write(node, 64, 8, b);
      if (ret == 8 && p5s_bin_data[64] == 0xa0 && p5s_bin_data[71] == 0xa7)
        p5s_ok("binary attribute partial write");
      else
        p5s_fail("bin write", ret);
      asc_vfs_kernel_close(node);
    }
  }

  /* 3: remove and re-create the groups; files and contexts go away. */
  device_remove_groups(dev, p5s_groups);
  if (asc_vfs_kernel_open(P5S_DEV_DIR "/p5s_ro") == NULL &&
      asc_vfs_kernel_open(P5S_DEV_DIR "/sub") == NULL)
    p5s_ok("device_remove_groups unlinks the files");
  else
    p5s_fail("group removal", 0);

  device_add_groups(dev, p5s_groups);
  ret = p5s_read_path(P5S_DEV_DIR "/p5s_ro", buf, sizeof(buf));
  if (ret == 8 && strcmp(buf, "p5c5-ro\n") == 0)
    p5s_ok("device_add_groups re-creates the files");
  else
    p5s_fail("group re-create", ret);

  /* 4: devres ordering, helpers and invalid ioremap resource. */
  {
    struct device fake;

    memset(&fake, 0, sizeof(fake));
    device_initialize(&fake);
    p5s_devres_count = 0;
    devm_add_action(&fake, p5s_devres_release, (void *)1L);
    devm_add_action(&fake, p5s_devres_release, (void *)2L);
    devm_add_action(&fake, p5s_devres_release, (void *)3L);

    {
      char *s = devm_kasprintf(&fake, GFP_KERNEL, "p5c5-%d", 7);
      u8 *arr = devm_kmalloc_array(&fake, 4, 16, GFP_KERNEL);

      if (s && strcmp(s, "p5c5-7") == 0 && arr != NULL)
        p5s_ok("devm_kasprintf + devm_kmalloc_array");
      else
        p5s_fail("devm helpers", s ? 0 : -ENOMEM);
    }

    {
      struct resource res;
      void __iomem *map;

      memset(&res, 0, sizeof(res));
      res.flags = IORESOURCE_MEM | IORESOURCE_UNSET;
      map = devm_ioremap_resource(&fake, &res);
      if (IS_ERR(map) && PTR_ERR(map) == -EINVAL)
        p5s_ok("devm_ioremap_resource rejects an unset resource");
      else
        p5s_fail("devm_ioremap_resource invalid", map ? 0 : -ENOMEM);
    }

    devres_release_all(&fake);
    if (p5s_devres_count == 3 && p5s_devres_order[0] == 3 &&
        p5s_devres_order[1] == 2 && p5s_devres_order[2] == 1)
      p5s_ok("devres releases in reverse order");
    else
      p5s_fail("devres order", p5s_devres_count);
  }

  /* 5: whole-device lifecycle loop with a PMM baseline. */
  klog_puts("[INFO] LinuxKPI: sysfs lifecycle start\n");
  device_unregister(dev);
  kfree(dev);
  pmm_before = asc_pmm_get_free_pages();
  for (i = 0; i < P5S_LOOP; i++) {
    dev = device_create_with_groups(cls, NULL, 0, NULL, p5s_groups,
                                    P5S_DEV_NAME);
    if (IS_ERR_OR_NULL(dev)) {
      p5s_fail("loop create", dev ? PTR_ERR(dev) : -ENOMEM);
      break;
    }
    device_remove_groups(dev, p5s_groups);
    device_unregister(dev);
    kfree(dev);
    if ((i & 7) == 7)
      klogf("[INFO] LinuxKPI: sysfs lifecycle %d\n", i + 1);
  }
  if (i == P5S_LOOP)
    p5s_ok("64 create/group-remove/destroy cycles");
  if (asc_pmm_get_free_pages() + 2 >= pmm_before)
    p5s_ok("sysfs lifecycle PMM stable");
  else
    klogf("[FAIL] LinuxKPI: sysfs PMM baseline=%lu final=%lu\n", pmm_before,
          asc_pmm_get_free_pages());

  if (asc_vfs_kernel_open(P5S_DEV_DIR "/p5s_ro") == NULL)
    p5s_ok("device_del removed the class device directory");
  else
    p5s_fail("device dir removal", 0);

  class_destroy(cls);

  if (p5s_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: sysfs suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: sysfs suite had %d failure(s)\n", p5s_failures);
}
