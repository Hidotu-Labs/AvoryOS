/* LinuxKPI device core: identity, driver data, devres and the printk helpers.
 * See linux/device.h for the contract. */

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sysfs.h>

/* DRM minor devnodes (linuxkpi/src/drm_file.c): a class devnode name of the
 * form "dri/cardN" or "dri/renderD128" becomes a native devfs node. */
int linuxkpi_drm_devnode_register(struct device *dev, const char *path);
void linuxkpi_drm_devnode_unregister(const char *path);

static LIST_HEAD(created_devices);

/* ── identity / driver data ─────────────────────────────────────────────── */

void *dev_get_drvdata(const struct device *dev) {
  return dev ? dev->driver_data : NULL;
}

/* Backing functions for the _Generic dev_fwnode() macro in property.h. */
const struct fwnode_handle *__dev_fwnode_const(const struct device *dev) {
  return dev ? dev->fwnode : NULL;
}

struct fwnode_handle *__dev_fwnode(struct device *dev) {
  return dev ? dev->fwnode : NULL;
}

void dev_set_drvdata(struct device *dev, void *data) {
  if (dev)
    dev->driver_data = data;
}

void device_initialize(struct device *dev) {
  INIT_LIST_HEAD(&dev->devres_head);
  INIT_LIST_HEAD(&dev->kpi_list);
}

int device_add(struct device *dev) {
  if (!dev)
    return -EINVAL;

  /* The device hierarchy feeds kobj.parent, which imported code passes to
   * sysfs_create_link() (DRM's MODESET-only controlD compat link reads it and
   * turns a NULL parent into -EINVAL, failing drm_dev_register). */
  dev->kobj.parent = dev->parent ? &dev->parent->kobj : NULL;
  if (!dev->kobj.name)
    dev->kobj.name = dev_name(dev);

  /* Dynamic sysfs: class devices get /sys/class/<class>/<name>, then their
   * attribute groups are materialized with real show/store callbacks. */
  if (dev->class) {
    void *class_dir = dev->class->kpi_dir;

    if (!class_dir && dev->class->name)
      class_dir = asc_sysfs_class_dir(dev->class->name);
    if (class_dir)
      dev->kobj.sd = (struct kernfs_node *)asc_sysfs_device_dir(
          class_dir, dev_name(dev));
    device_add_groups(dev, dev->groups);
  }

  /* devtmpfs equivalent for character devices: a class devnode() names the
   * /dev path (DRM uses "dri/cardN" and "dri/renderD128").  Other classes
   * have no devnode yet, so this stays a no-op for them. */
  if (dev->class && dev->class->devnode && dev->devt) {
    char *node_name = dev->class->devnode(dev, NULL);

    if (node_name) {
      if (strncmp(node_name, "dri/", 4) == 0)
        linuxkpi_drm_devnode_register(dev, node_name);
      kfree(node_name);
    }
  }
  return 0;
}

int device_register(struct device *dev) {
  if (!dev)
    return -EINVAL;
  if (!dev->devres_head.next)
    device_initialize(dev);
  return device_add(dev);
}

void device_del(struct device *dev) {
  if (!dev)
    return;

  /* Remove the dynamic sysfs directory (and its attributes), then the
   * devtmpfs node, then let device_is_registered() turn false. */
  if (dev->kobj.sd && dev->class) {
    void *class_dir = dev->class->kpi_dir;

    if (!class_dir && dev->class->name)
      class_dir = asc_sysfs_class_dir(dev->class->name);
    if (class_dir)
      asc_sysfs_remove(class_dir, dev_name(dev));
    dev->kobj.sd = NULL;
  }
  if (dev->class && dev->class->devnode && dev->devt) {
    char *node_name = dev->class->devnode(dev, NULL);

    if (node_name) {
      if (strncmp(node_name, "dri/", 4) == 0)
        linuxkpi_drm_devnode_unregister(node_name);
      kfree(node_name);
    }
  }
  dev->kobj.name = NULL;
}

void device_unregister(struct device *dev) {
  if (!dev)
    return;
  devres_release_all(dev);
  device_del(dev);
  /* Devices created by __kpi_device_create() are on the global list; manual
   * unregister+kfree (tests) must unlink them so the list never dangles. */
  if (dev->kpi_list.next && dev->kpi_list.next != &dev->kpi_list)
    list_del(&dev->kpi_list);
  if (dev->release)
    dev->release(dev);
}

struct device *get_device(struct device *dev) { return dev; }

void put_device(struct device *dev) { (void)dev; }

int dev_set_name(struct device *dev, const char *fmt, ...) {
  va_list ap;

  if (!dev)
    return -EINVAL;

  va_start(ap, fmt);
  vsnprintf(dev->kpi_name, sizeof(dev->kpi_name), fmt, ap);
  va_end(ap);
  dev->init_name = dev->kpi_name;
  return 0;
}

int __kpi_dev_printk(const char *level, const struct device *dev,
                     const char *fmt, ...) {
  char buf[192];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (level && level[0] == '\001' && level[1])
    level += 2;
  klogf("(%s) %s\n", dev_name(dev), buf);
  return n;
}

/* ── classes / device creation ──────────────────────────────────────────── */

struct class *class_create(const char *name) {
  struct class *cls = kmalloc(sizeof(*cls), GFP_KERNEL);

  if (cls) {
    cls->name = name;
    cls->kpi_dir = name ? asc_sysfs_class_dir(name) : NULL;
  }
  return cls;
}

void class_destroy(const struct class *cls) {
  if (cls)
    kfree((void *)cls);
}

static struct device *kpi_device_create_va(const struct class *cls,
                                           struct device *parent, dev_t devt,
                                           void *drvdata,
                                           const struct attribute_group **groups,
                                           const char *fmt, va_list ap) {
  struct device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);

  if (!dev)
    return ERR_PTR(-ENOMEM);

  device_initialize(dev);
  dev->class = (struct class *)cls;
  dev->parent = parent;
  dev->devt = devt;
  dev->driver_data = drvdata;
  dev->groups = groups;

  vsnprintf(dev->kpi_name, sizeof(dev->kpi_name), fmt, ap);
  dev->init_name = dev->kpi_name;

  list_add_tail(&dev->kpi_list, &created_devices);
  device_register(dev);
  return dev;
}

struct device *__kpi_device_create(const struct class *cls,
                                   struct device *parent, dev_t devt,
                                   void *drvdata, const char *fmt, ...) {
  struct device *dev;
  va_list ap;

  va_start(ap, fmt);
  dev = kpi_device_create_va(cls, parent, devt, drvdata, NULL, fmt, ap);
  va_end(ap);
  return dev;
}

struct device *device_create_with_groups(const struct class *cls,
                                         struct device *parent, dev_t devt,
                                         void *drvdata,
                                         const struct attribute_group **groups,
                                         const char *fmt, ...) {
  struct device *dev;
  va_list ap;

  va_start(ap, fmt);
  dev = kpi_device_create_va(cls, parent, devt, drvdata, groups, fmt, ap);
  va_end(ap);
  return dev;
}

void __kpi_device_destroy(const struct class *cls, dev_t devt) {
  struct device *dev, *tmp;

  (void)cls;
  list_for_each_entry_safe(dev, tmp, &created_devices, kpi_list) {
    if (dev->devt == devt) {
      device_unregister(dev);
      kfree(dev);
      return;
    }
  }
}

/* ── devres ─────────────────────────────────────────────────────────────── */

struct devres_node {
  struct list_head entry;
  void *data;
  void (*release)(void *);
};

/* devres_alloc()/devres_add() internal API used by imported helpers (e.g.
 * devm_kasprintf_strarray in string_helpers.c).  The record wraps the
 * caller's data and is released together with the device. */
struct devres_alloc_rec {
  struct list_head list;
  struct device *dev;
  dr_release_t release;
  char data[];
};

static LIST_HEAD(devres_alloc_list);

void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp) {
  struct devres_alloc_rec *rec = kmalloc(sizeof(*rec) + size, gfp);

  if (!rec)
    return NULL;
  rec->dev = NULL;
  rec->release = release;
  return rec->data;
}

void devres_add(struct device *dev, void *res) {
  struct devres_alloc_rec *rec =
      container_of(res, struct devres_alloc_rec, data);

  rec->dev = dev;
  list_add_tail(&rec->list, &devres_alloc_list);
}

void devres_free(void *res) {
  struct devres_alloc_rec *rec;

  if (!res)
    return;
  rec = container_of(res, struct devres_alloc_rec, data);
  if (rec->dev)
    list_del(&rec->list);
  kfree(rec);
}

static void devres_ensure(struct device *dev) {
  if (!dev->devres_head.next)
    INIT_LIST_HEAD(&dev->devres_head);
}

static void __kpi_kfree_action(void *p) { kfree(p); }

int devm_add_action(struct device *dev, void (*action)(void *), void *data) {
  struct devres_node *node;

  if (!dev)
    return -EINVAL;

  node = kmalloc(sizeof(*node), GFP_KERNEL);
  if (!node)
    return -ENOMEM;

  node->data = data;
  node->release = action;
  devres_ensure(dev);
  list_add_tail(&node->entry, &dev->devres_head);
  return 0;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
                             void *data) {
  int ret = devm_add_action(dev, action, data);

  if (ret && action)
    action(data);
  return ret;
}

void devm_remove_action(struct device *dev, void (*action)(void *),
                        void *data) {
  struct devres_node *node;

  if (!dev || !dev->devres_head.next)
    return;

  list_for_each_entry(node, &dev->devres_head, entry) {
    if (node->release == action && node->data == data) {
      list_del(&node->entry);
      kfree(node);
      return;
    }
  }
}

/* ── devres groups ──────────────────────────────────────────────────────── */

void *devres_open_group(struct device *dev, void *id, gfp_t gfp) {
  (void)gfp;

  if (!dev)
    return NULL;
  devres_ensure(dev);
  dev->kpi_group_node_mark =
      dev->devres_head.prev == &dev->devres_head
          ? NULL
          : (void *)dev->devres_head.prev;
  dev->kpi_group_alloc_mark =
      list_empty(&devres_alloc_list) ? NULL : (void *)devres_alloc_list.prev;
  return id ? id : dev;
}

void devres_close_group(struct device *dev, void *id) {
  (void)dev;
  (void)id;
}

void devres_release_group(struct device *dev, void *id) {
  struct devres_node *node;
  struct devres_alloc_rec *rec;
  struct list_head *pos;
  struct list_head *start;

  (void)id;
  if (!dev || !dev->devres_head.next)
    return;

  start = dev->kpi_group_node_mark
              ? (struct list_head *)dev->kpi_group_node_mark
              : &dev->devres_head;
  for (pos = dev->devres_head.prev; pos != &dev->devres_head && pos != start;) {
    struct list_head *prev = pos->prev;

    node = list_entry(pos, struct devres_node, entry);
    list_del(pos);
    if (node->release)
      node->release(node->data);
    kfree(node);
    pos = prev;
  }

  start = dev->kpi_group_alloc_mark
              ? (struct list_head *)dev->kpi_group_alloc_mark
              : &devres_alloc_list;
  for (pos = devres_alloc_list.prev; pos != &devres_alloc_list && pos != start;) {
    struct list_head *prev = pos->prev;

    rec = list_entry(pos, struct devres_alloc_rec, list);
    if (rec->dev == dev) {
      list_del(pos);
      if (rec->release)
        rec->release(dev, rec->data);
      kfree(rec);
    }
    pos = prev;
  }

  dev->kpi_group_node_mark = NULL;
  dev->kpi_group_alloc_mark = NULL;
}

void devres_release_all(struct device *dev) {
  struct devres_alloc_rec *rec, *rtmp;

  /* Devres resources are released in reverse registration order (LIFO), so
   * later actions can still use earlier ones. */
  while (dev && dev->devres_head.next && !list_empty(&dev->devres_head)) {
    struct devres_node *node =
        list_last_entry(&dev->devres_head, struct devres_node, entry);

    list_del(&node->entry);
    if (node->release)
      node->release(node->data);
    kfree(node);
  }

  list_for_each_entry_safe(rec, rtmp, &devres_alloc_list, list) {
    if (rec->dev == dev) {
      list_del(&rec->list);
      if (rec->release)
        rec->release(dev, rec->data);
      kfree(rec);
    }
  }
}

/* ── devm_request_irq / devm_free_irq ───────────────────────────────────── */

struct kpi_devm_irq {
  unsigned int irq;
  void *dev_id;
};

static void kpi_devm_irq_release(void *data) {
  struct kpi_devm_irq *w = data;

  free_irq(w->irq, w->dev_id);
  kfree(w);
}

int devm_request_irq(struct device *dev, unsigned int irq,
                     irqreturn_t (*handler)(int, void *),
                     unsigned long irqflags, const char *devname,
                     void *dev_id) {
  struct kpi_devm_irq *w;
  int ret;

  if (!dev)
    return -EINVAL;
  ret = request_irq(irq, handler, irqflags, devname, dev_id);
  if (ret)
    return ret;

  w = kmalloc(sizeof(*w), GFP_KERNEL);
  if (!w) {
    free_irq(irq, dev_id);
    return -ENOMEM;
  }
  w->irq = irq;
  w->dev_id = dev_id;
  if (devm_add_action(dev, kpi_devm_irq_release, w)) {
    free_irq(irq, dev_id);
    kfree(w);
    return -ENOMEM;
  }
  return 0;
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id) {
  struct devres_node *node, *tmp;

  if (!dev || !dev->devres_head.next)
    return;
  list_for_each_entry_safe(node, tmp, &dev->devres_head, entry) {
    struct kpi_devm_irq *w;

    if (node->release != kpi_devm_irq_release)
      continue;
    w = node->data;
    if (!w || w->irq != irq || w->dev_id != dev_id)
      continue;
    list_del(&node->entry);
    kfree(node);
    free_irq(irq, dev_id);
    kfree(w);
    return;
  }
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) {
  void *p = kmalloc(size, gfp);

  if (!p)
    return NULL;
  if (devm_add_action(dev, __kpi_kfree_action, p)) {
    kfree(p);
    return NULL;
  }
  return p;
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp) {
  return devm_kmalloc(dev, size, gfp | __GFP_ZERO);
}

void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp) {
  size_t bytes;

  if (__builtin_mul_overflow(n, size, &bytes))
    return NULL;
  return devm_kmalloc(dev, bytes, gfp | __GFP_ZERO);
}

void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp) {
  void *p = devm_kmalloc(dev, len, gfp);

  if (p)
    memcpy(p, src, len);
  return p;
}

char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) {
  size_t len = strlen(s) + 1;
  char *p = devm_kmalloc(dev, len, gfp);

  if (p)
    memcpy(p, s, len);
  return p;
}

char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...) {
  size_t size = 128;
  char *buf;

  for (;;) {
    va_list ap;
    int n;

    buf = devm_kmalloc(dev, size, gfp);
    if (!buf)
      return NULL;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    if (n >= 0 && (size_t)n < size)
      return buf;
    devm_kfree(dev, buf);
    size = n >= 0 ? (size_t)n + 1 : size * 2;
    if (size > (1u << 20))
      return NULL;
  }
}

void devm_kfree(struct device *dev, const void *p) {
  struct devres_node *node;

  if (dev && dev->devres_head.next) {
    list_for_each_entry(node, &dev->devres_head, entry) {
      if (node->data == p && node->release == __kpi_kfree_action) {
        list_del(&node->entry);
        kfree(node);
        break;
      }
    }
  }
  kfree(p);
}

/* ── devm mappings ──────────────────────────────────────────────────────── */

static void __kpi_devm_iounmap_action(void *addr) {
  iounmap((volatile void __iomem *)addr);
}

static void __kpi_devm_memunmap_action(void *addr) {
  memunmap(addr);
}

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset,
                           resource_size_t size) {
  void __iomem *addr = ioremap(offset, size);

  if (!addr || IS_ERR(addr))
    return addr;
  if (devm_add_action(dev, __kpi_devm_iounmap_action, (void *)addr)) {
    iounmap(addr);
    return NULL;
  }
  return addr;
}

void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset,
                              resource_size_t size) {
  void __iomem *addr = ioremap_wc(offset, size);

  if (!addr || IS_ERR(addr))
    return addr;
  if (devm_add_action(dev, __kpi_devm_iounmap_action, (void *)addr)) {
    iounmap(addr);
    return NULL;
  }
  return addr;
}

void __iomem *devm_ioremap_resource(struct device *dev,
                                    const struct resource *res) {
  void __iomem *addr;

  if (!res || (res->flags & IORESOURCE_UNSET)) {
    if (dev)
      dev_err(dev, "invalid resource\n");
    return ERR_PTR(-EINVAL);
  }
  addr = devm_ioremap(dev, res->start, resource_size(res));
  if (!addr || IS_ERR(addr)) {
    if (dev)
      dev_err(dev, "ioremap failed\n");
    return addr ? addr : ERR_PTR(-ENOMEM);
  }
  return addr;
}

void __iomem *devm_ioremap_resource_wc(struct device *dev,
                                       const struct resource *res) {
  void __iomem *addr;

  if (!res || (res->flags & IORESOURCE_UNSET)) {
    if (dev)
      dev_err(dev, "invalid resource\n");
    return ERR_PTR(-EINVAL);
  }
  addr = devm_ioremap_wc(dev, res->start, resource_size(res));
  if (!addr || IS_ERR(addr)) {
    if (dev)
      dev_err(dev, "ioremap failed\n");
    return addr ? addr : ERR_PTR(-ENOMEM);
  }
  return addr;
}

void *devm_memremap(struct device *dev, resource_size_t offset, size_t size,
                    unsigned long flags) {
  void *addr = memremap(offset, size, flags);

  if (!addr || IS_ERR(addr))
    return addr;
  if (devm_add_action(dev, __kpi_devm_memunmap_action, addr)) {
    memunmap(addr);
    return NULL;
  }
  return addr;
}

void devm_memunmap(struct device *dev, void *addr) {
  devm_remove_action(dev, __kpi_devm_memunmap_action, addr);
  memunmap(addr);
}
