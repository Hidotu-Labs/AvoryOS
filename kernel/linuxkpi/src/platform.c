/* Minimal platform bus for the DRM canaries.
 *
 * AvoryOS has no ACPI/OF enumeration, so the only platform devices are the
 * ones a driver registers itself (vgem/vkms/simpledrm use
 * platform_device_register_simple()).  This keeps two global lists and does
 * synchronous name matching: registering either side probes every match.
 * There is no deferred probe, no id auto-allocation, and no driver core
 * refcounting; each is recorded in docs/linuxkpi-gaps.md as it becomes
 * relevant. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

struct kpi_platform_device {
  struct platform_device pdev;
  struct list_head node;
};

struct kpi_platform_driver {
  struct platform_driver *drv;
  struct list_head node;
};

static LIST_HEAD(kpi_platform_devices);
static LIST_HEAD(kpi_platform_drivers);

static bool kpi_platform_match(struct platform_device *pdev,
                               struct platform_driver *drv) {
  const struct platform_device_id *id;

  if (drv->id_table) {
    for (id = drv->id_table; id->name[0]; id++) {
      if (pdev->name && strcmp(id->name, pdev->name) == 0)
        return true;
    }
    return false;
  }
  return pdev->name && drv->driver.name &&
         strcmp(pdev->name, drv->driver.name) == 0;
}

static void kpi_platform_probe_device(struct platform_device *pdev) {
  struct kpi_platform_driver *kd;

  list_for_each_entry(kd, &kpi_platform_drivers, node) {
    if (!pdev->dev.driver && kpi_platform_match(pdev, kd->drv)) {
      pdev->dev.driver = &kd->drv->driver;
      if (kd->drv->probe)
        kd->drv->probe(pdev);
    }
  }
}

static void kpi_platform_probe_driver(struct platform_driver *drv) {
  struct kpi_platform_device *kdev;

  list_for_each_entry(kdev, &kpi_platform_devices, node) {
    if (!kdev->pdev.dev.driver && kpi_platform_match(&kdev->pdev, drv)) {
      kdev->pdev.dev.driver = &drv->driver;
      if (drv->probe)
        drv->probe(&kdev->pdev);
    }
  }
}

struct platform_device *platform_device_register_full(
    const struct platform_device_info *pdevinfo) {
  struct kpi_platform_device *kdev;
  struct platform_device *pdev;

  if (!pdevinfo || !pdevinfo->name)
    return ERR_PTR(-EINVAL);

  kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
  if (!kdev)
    return ERR_PTR(-ENOMEM);
  pdev = &kdev->pdev;

  pdev->name = kstrdup(pdevinfo->name, GFP_KERNEL);
  pdev->id = pdevinfo->id;
  pdev->id_auto = false;
  pdev->num_resources = pdevinfo->num_res;
  if (pdevinfo->num_res && pdevinfo->res) {
    pdev->resource = kmemdup(pdevinfo->res,
                             sizeof(*pdevinfo->res) * pdevinfo->num_res,
                             GFP_KERNEL);
  }
  if (pdevinfo->data && pdevinfo->size_data) {
    pdev->dev.platform_data = kmemdup(pdevinfo->data, pdevinfo->size_data,
                                      GFP_KERNEL);
  }

  device_initialize(&pdev->dev);
  pdev->dev.parent = pdevinfo->parent;
  pdev->dev.platform_data = pdev->dev.platform_data;
  dev_set_name(&pdev->dev, "%s.%d", pdevinfo->name, pdevinfo->id);
  if (pdevinfo->dma_mask) {
    pdev->dev.dma_mask = pdevinfo->dma_mask;
    pdev->dev.coherent_dma_mask = pdevinfo->dma_mask;
  }
  device_register(&pdev->dev);

  list_add_tail(&kdev->node, &kpi_platform_devices);
  kpi_platform_probe_device(pdev);
  return pdev;
}

void platform_device_unregister(struct platform_device *pdev) {
  struct kpi_platform_device *kdev;
  struct kpi_platform_driver *kd;

  if (!pdev)
    return;

  list_for_each_entry(kd, &kpi_platform_drivers, node) {
    if (pdev->dev.driver == &kd->drv->driver) {
      if (kd->drv->remove)
        kd->drv->remove(pdev);
      pdev->dev.driver = NULL;
    }
  }

  kdev = container_of(pdev, struct kpi_platform_device, pdev);
  list_del(&kdev->node);
  device_unregister(&pdev->dev);
  kfree(pdev->resource);
  kfree(pdev->name);
  kfree(kdev);
}

int __platform_driver_register(struct platform_driver *drv,
                               struct module *owner) {
  struct kpi_platform_driver *kd;

  (void)owner;
  if (!drv)
    return -EINVAL;

  kd = kzalloc(sizeof(*kd), GFP_KERNEL);
  if (!kd)
    return -ENOMEM;
  kd->drv = drv;
  list_add_tail(&kd->node, &kpi_platform_drivers);
  kpi_platform_probe_driver(drv);
  return 0;
}

void platform_driver_unregister(struct platform_driver *drv) {
  struct kpi_platform_device *kdev;
  struct kpi_platform_driver *kd, *tmp;

  list_for_each_entry_safe(kd, tmp, &kpi_platform_drivers, node) {
    if (kd->drv != drv)
      continue;
    list_for_each_entry(kdev, &kpi_platform_devices, node) {
      if (kdev->pdev.dev.driver == &drv->driver) {
        if (drv->remove)
          drv->remove(&kdev->pdev);
        kdev->pdev.dev.driver = NULL;
      }
    }
    list_del(&kd->node);
    kfree(kd);
  }
}

struct resource *platform_get_resource(struct platform_device *pdev,
                                       unsigned int type,
                                       unsigned int num) {
  unsigned int i;

  for (i = 0; i < pdev->num_resources; i++) {
    if ((pdev->resource[i].flags & type) == type) {
      if (num-- == 0)
        return &pdev->resource[i];
    }
  }
  return NULL;
}

void __iomem *devm_platform_get_and_ioremap_resource(
    struct platform_device *pdev, unsigned int index, struct resource **res) {
  struct resource *r;

  if (!pdev)
    return ERR_PTR(-EINVAL);
  r = platform_get_resource(pdev, IORESOURCE_MEM, index);
  if (res)
    *res = r;
  return devm_ioremap_resource(&pdev->dev, r);
}

void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev,
                                             unsigned int index) {
  return devm_platform_get_and_ioremap_resource(pdev, index, NULL);
}

int platform_get_irq(struct platform_device *pdev, unsigned int num) {
  struct resource *r = platform_get_resource(pdev, IORESOURCE_IRQ, num);

  if (!r)
    return -ENXIO;
  return (int)r->start;
}

int platform_irq_count(struct platform_device *pdev) {
  int count = 0;
  unsigned int i;

  for (i = 0; i < pdev->num_resources; i++) {
    if (pdev->resource[i].flags & IORESOURCE_IRQ)
      count++;
  }
  return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#include <linux/device/faux.h>

/* Synchronous native equivalent of the resource-free faux device bus.
 * Devices own their devres and optional callbacks, just like platform
 * devices above. There is no asynchronous binding or native hotplug bus. */
struct kpi_faux_device {
  struct faux_device faux;
  const struct faux_device_ops *ops;
  struct list_head entry;
  bool probed;
};
static DEFINE_MUTEX(kpi_faux_lock);
static LIST_HEAD(kpi_faux_devices);
static struct device kpi_faux_root = { .init_name = "faux" };
static struct bus_type kpi_faux_bus = { .name = "faux" };

static void kpi_faux_release(struct device *dev) {
  kfree(container_of(dev, struct kpi_faux_device, faux.dev));
}

void faux_device_destroy(struct faux_device *dev) {
  struct kpi_faux_device *object;
  if (!dev)
    return;
  object = container_of(dev, struct kpi_faux_device, faux);
  mutex_lock(&kpi_faux_lock);
  list_del_init(&object->entry);
  mutex_unlock(&kpi_faux_lock);
  if (object->probed && object->ops && object->ops->remove)
    object->ops->remove(dev);
  device_unregister(&dev->dev);
}

struct faux_device *faux_device_create_with_groups(const char *name,
    struct device *parent, const struct faux_device_ops *ops,
    const struct attribute_group **groups) {
  struct kpi_faux_device *object, *other;
  int ret;
  if (!name || !*name)
    return NULL;
  object = kzalloc(sizeof(*object), GFP_KERNEL);
  if (!object)
    return NULL;
  object->ops = ops;
  device_initialize(&object->faux.dev);
  object->faux.dev.release = kpi_faux_release;
  object->faux.dev.parent = parent ? parent : &kpi_faux_root;
  object->faux.dev.bus = &kpi_faux_bus;
  object->faux.dev.groups = groups;
  if (strlen(name) >= sizeof(object->faux.dev.kpi_name)) {
    kfree(object);
    return NULL;
  }
  dev_set_name(&object->faux.dev, "%s", name);
  mutex_lock(&kpi_faux_lock);
  if (!kpi_faux_root.devres_head.next)
    device_register(&kpi_faux_root);
  list_for_each_entry(other, &kpi_faux_devices, entry) {
    if (!strcmp(dev_name(&other->faux.dev), name)) {
      mutex_unlock(&kpi_faux_lock);
      kfree(object);
      return NULL;
    }
  }
  list_add_tail(&object->entry, &kpi_faux_devices);
  mutex_unlock(&kpi_faux_lock);
  ret = device_add(&object->faux.dev);
  if (!ret && ops && ops->probe)
    ret = ops->probe(&object->faux);
  if (ret) {
    faux_device_destroy(&object->faux);
    return NULL;
  }
  object->probed = true;
  return &object->faux;
}

struct faux_device *faux_device_create(const char *name, struct device *parent,
                                      const struct faux_device_ops *ops) {
  return faux_device_create_with_groups(name, parent, ops, NULL);
}
#endif
