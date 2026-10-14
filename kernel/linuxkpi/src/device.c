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
  spin_lock_init(&dev->devres_lock);
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
  dr_release_t resource_release;
  struct device *owner;
  enum { DEVRES_ACTION, DEVRES_RESOURCE, DEVRES_GROUP_OPEN,
         DEVRES_GROUP_CLOSE } kind;
  bool selected;
};

/* devres_alloc()/devres_add() internal API used by imported helpers (e.g.
 * devm_kasprintf_strarray in string_helpers.c).  The record wraps the
 * caller's data and is released together with the device. */
struct devres_alloc_rec {
  struct devres_node node;
  char data[] __aligned(ARCH_KMALLOC_MINALIGN);
};

struct devres_group {
  struct devres_node open, close;
  void *id;
};

static void devres_ensure(struct device *dev) {
  /* Legacy zeroed wrappers initialize before publication. Normal devices
   * initialize this list and lock together in device_initialize(). */
  if (!dev->devres_head.next) {
    INIT_LIST_HEAD(&dev->devres_head);
    spin_lock_init(&dev->devres_lock);
  }
}

void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp) {
  struct devres_alloc_rec *rec;

  if (size > (size_t)-1 - sizeof(*rec))
    return NULL;
  rec = kzalloc(sizeof(*rec) + size, gfp);

  if (!rec)
    return NULL;
  INIT_LIST_HEAD(&rec->node.entry);
  rec->node.kind = DEVRES_RESOURCE;
  rec->node.data = rec->data;
  rec->node.resource_release = release;
  return rec->data;
}

void devres_add(struct device *dev, void *res) {
  struct devres_alloc_rec *rec =
      container_of(res, struct devres_alloc_rec, data);

  unsigned long flags;
  devres_ensure(dev);
  spin_lock_irqsave(&dev->devres_lock, flags);
  BUG_ON(!list_empty(&rec->node.entry));
  rec->node.owner = dev;
  list_add_tail(&rec->node.entry, &dev->devres_head);
  spin_unlock_irqrestore(&dev->devres_lock, flags);
}

void devres_free(void *res) {
  struct devres_alloc_rec *rec;

  if (!res)
    return;
  rec = container_of(res, struct devres_alloc_rec, data);
  /* Attached resources belong to the device and cannot be freed here. */
  BUG_ON(!list_empty(&rec->node.entry));
  kfree(rec);
}

static void __kpi_kfree_action(void *p) { kfree(p); }

int devm_add_action(struct device *dev, void (*action)(void *), void *data) {
  struct devres_node *node;
  unsigned long flags;

  if (!dev)
    return -EINVAL;

  node = kzalloc(sizeof(*node), GFP_KERNEL);
  if (!node)
    return -ENOMEM;

  node->data = data;
  node->release = action;
  devres_ensure(dev);
  spin_lock_irqsave(&dev->devres_lock, flags);
  list_add_tail(&node->entry, &dev->devres_head);
  spin_unlock_irqrestore(&dev->devres_lock, flags);
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
  struct devres_node *found = NULL;
  unsigned long flags;

  if (!dev || !dev->devres_head.next)
    return;

  spin_lock_irqsave(&dev->devres_lock, flags);
  list_for_each_entry_reverse(node, &dev->devres_head, entry) {
    if (node->kind == DEVRES_ACTION && node->release == action &&
        node->data == data) {
      list_del_init(&node->entry);
      found = node;
      break;
    }
  }
  spin_unlock_irqrestore(&dev->devres_lock, flags);
  kfree(found);
}

/* ── devres groups ──────────────────────────────────────────────────────── */

void *devres_open_group(struct device *dev, void *id, gfp_t gfp) {
  struct devres_group *group;
  unsigned long flags;

  if (!dev)
    return NULL;
  group = kzalloc(sizeof(*group), gfp);
  if (!group)
    return NULL;
  INIT_LIST_HEAD(&group->open.entry);
  INIT_LIST_HEAD(&group->close.entry);
  group->open.kind = DEVRES_GROUP_OPEN;
  group->close.kind = DEVRES_GROUP_CLOSE;
  group->open.data = group->close.data = group;
  group->id = id ? id : group;
  devres_ensure(dev);
  spin_lock_irqsave(&dev->devres_lock, flags);
  list_add_tail(&group->open.entry, &dev->devres_head);
  spin_unlock_irqrestore(&dev->devres_lock, flags);
  return group->id;
}

/* Caller holds devres_lock. NULL selects the latest still-open group. */
static struct devres_group *devres_find_group(struct device *dev, void *id) {
  struct devres_node *node;
  list_for_each_entry_reverse(node, &dev->devres_head, entry) {
    struct devres_group *group;
    if (node->kind != DEVRES_GROUP_OPEN)
      continue;
    group = node->data;
    if ((id && group->id == id) ||
        (!id && list_empty(&group->close.entry)))
      return group;
  }
  return NULL;
}

void devres_close_group(struct device *dev, void *id) {
  struct devres_group *group;
  unsigned long flags;
  if (!dev || !dev->devres_head.next)
    return;
  spin_lock_irqsave(&dev->devres_lock, flags);
  group = devres_find_group(dev, id);
  if (group && list_empty(&group->close.entry))
    list_add_tail(&group->close.entry, &dev->devres_head);
  else
    WARN_ON(1);
  spin_unlock_irqrestore(&dev->devres_lock, flags);
}

/* All nodes are already detached. Callbacks may sleep, remove actions, or
 * register another resource without taking devres_lock recursively. */
static void devres_release_nodes(struct list_head *todo) {
  while (!list_empty(todo)) {
    struct devres_node *node = list_last_entry(todo, struct devres_node, entry);
    list_del_init(&node->entry);
    if (node->kind == DEVRES_GROUP_CLOSE)
      continue;
    if (node->kind == DEVRES_GROUP_OPEN) {
      kfree(node->data);
      continue;
    }
    if (node->kind == DEVRES_RESOURCE) {
      if (node->resource_release)
        node->resource_release(node->owner, node->data);
    } else if (node->release) {
      node->release(node->data);
    }
    /* Raw records embed their node as the first member. */
    kfree(node);
  }
}

void devres_release_group(struct device *dev, void *id) {
  struct devres_group *group;
  struct devres_node *node;
  struct list_head *pos, *end;
  unsigned long flags;
  LIST_HEAD(todo);
  if (!dev || !dev->devres_head.next)
    return;
  spin_lock_irqsave(&dev->devres_lock, flags);
  group = devres_find_group(dev, id);
  if (!group) {
    if (!list_empty(&dev->devres_head))
      WARN_ON(1);
    spin_unlock_irqrestore(&dev->devres_lock, flags);
    return;
  }
  end = list_empty(&group->close.entry) ? &dev->devres_head
                                      : group->close.entry.next;
  for (pos = &group->open.entry; pos != end; pos = pos->next)
    list_entry(pos, struct devres_node, entry)->selected = true;
  /* Remove group markers only when both boundaries are inside this range
   * (or an open group has no closing boundary yet). Crossing groups survive.
   */
  for (pos = &group->open.entry; pos != end;) {
    struct list_head *next = pos->next;
    bool take = true;
    node = list_entry(pos, struct devres_node, entry);
    if (node->kind == DEVRES_GROUP_OPEN || node->kind == DEVRES_GROUP_CLOSE) {
      struct devres_group *g = node->data;
      struct devres_node *pair = node->kind == DEVRES_GROUP_OPEN ? &g->close
                                                               : &g->open;
      take = pair->selected || list_empty(&pair->entry);
    }
    if (take)
      list_move_tail(pos, &todo);
    pos = next;
  }
  list_for_each_entry(node, &dev->devres_head, entry)
    node->selected = false;
  list_for_each_entry(node, &todo, entry)
    node->selected = false;
  spin_unlock_irqrestore(&dev->devres_lock, flags);
  devres_release_nodes(&todo);
}

void devres_release_all(struct device *dev) {
  unsigned long flags;
  LIST_HEAD(todo);
  if (!dev || !dev->devres_head.next)
    return;
  spin_lock_irqsave(&dev->devres_lock, flags);
  list_splice_init(&dev->devres_head, &todo);
  spin_unlock_irqrestore(&dev->devres_lock, flags);
  devres_release_nodes(&todo);
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
  struct kpi_devm_irq *found = NULL;
  struct devres_node *removed = NULL;
  unsigned long flags;

  if (!dev || !dev->devres_head.next)
    return;
  spin_lock_irqsave(&dev->devres_lock, flags);
  list_for_each_entry_safe(node, tmp, &dev->devres_head, entry) {
    struct kpi_devm_irq *w;

    if (node->kind != DEVRES_ACTION || node->release != kpi_devm_irq_release)
      continue;
    w = node->data;
    if (!w || w->irq != irq || w->dev_id != dev_id)
      continue;
    list_del_init(&node->entry);
    removed = node;
    found = w;
    break;
  }
  spin_unlock_irqrestore(&dev->devres_lock, flags);
  if (found) {
    kfree(removed);
    kpi_devm_irq_release(found);
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
  devm_remove_action(dev, __kpi_kfree_action, (void *)p);
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
