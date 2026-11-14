/* Bus registration and synchronous binding for LinuxKPI managed buses.
 * PCI and platform retain their existing binding paths. */
#include <linux/driver.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

struct kpi_bus {
  struct list_head node, devices, drivers;
  const struct bus_type *type;
};
struct kpi_driver {
  struct list_head node;
  struct device_driver *driver;
};

static LIST_HEAD(buses);
static DEFINE_MUTEX(bus_lock);

static struct kpi_bus *find_bus(const struct bus_type *type) {
  struct kpi_bus *bus;
  list_for_each_entry(bus, &buses, node)
    if (bus->type == type)
      return bus;
  return NULL;
}

int bus_register(const struct bus_type *type) {
  if (!type || !type->name || !type->match)
    return -EINVAL;
  struct kpi_bus *bus = kzalloc(sizeof(*bus), GFP_KERNEL);
  if (!bus)
    return -ENOMEM;
  bus->type = type;
  INIT_LIST_HEAD(&bus->devices);
  INIT_LIST_HEAD(&bus->drivers);
  mutex_lock(&bus_lock);
  if (find_bus(type)) {
    mutex_unlock(&bus_lock);
    kfree(bus);
    return -EEXIST;
  }
  list_add_tail(&bus->node, &buses);
  mutex_unlock(&bus_lock);
  return 0;
}

void bus_unregister(const struct bus_type *type) {
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(type);
  if (bus && list_empty(&bus->devices) && list_empty(&bus->drivers)) {
    list_del(&bus->node);
    kfree(bus);
  }
  mutex_unlock(&bus_lock);
}

static void try_bind(struct device *dev, struct device_driver *driver) {
  if (dev->driver || dev->bus->match(dev, driver) <= 0)
    return;
  dev->driver = driver;
  if (driver->probe && driver->probe(dev)) {
    devres_release_all(dev);
    dev->driver = NULL;
    return;
  }
  device_add_groups(dev, driver->dev_groups);
}

static void unbind(struct device *dev) {
  struct device_driver *driver = dev->driver;
  if (!driver)
    return;
  device_remove_groups(dev, driver->dev_groups);
  if (driver->remove)
    driver->remove(dev);
  devres_release_all(dev);
  dev->driver = NULL;
}

int linuxkpi_bus_add_device(struct device *dev) {
  if (!dev->bus || !dev->bus->match)
    return 0;
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(dev->bus);
  if (!bus) {
    mutex_unlock(&bus_lock);
    return -ENODEV;
  }
  list_add_tail(&dev->kpi_bus_node, &bus->devices);
  device_add_groups(dev, dev->bus->dev_groups);
  struct kpi_driver *driver;
  list_for_each_entry(driver, &bus->drivers, node)
    try_bind(dev, driver->driver);
  mutex_unlock(&bus_lock);
  return 0;
}

void linuxkpi_bus_remove_device(struct device *dev) {
  if (!dev->bus || !dev->bus->match)
    return;
  mutex_lock(&bus_lock);
  if (!list_empty(&dev->kpi_bus_node)) {
    unbind(dev);
    list_del_init(&dev->kpi_bus_node);
  }
  mutex_unlock(&bus_lock);
}

int driver_register(struct device_driver *driver) {
  if (!driver || !driver->bus || !driver->bus->match)
    return 0;
  struct kpi_driver *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
  if (!entry)
    return -ENOMEM;
  entry->driver = driver;
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(driver->bus);
  if (!bus) {
    mutex_unlock(&bus_lock);
    kfree(entry);
    return -ENODEV;
  }
  struct kpi_driver *registered;
  list_for_each_entry(registered, &bus->drivers, node) {
    if (registered->driver == driver) {
      mutex_unlock(&bus_lock);
      kfree(entry);
      return -EEXIST;
    }
  }
  list_add_tail(&entry->node, &bus->drivers);
  struct device *dev;
  list_for_each_entry(dev, &bus->devices, kpi_bus_node)
    try_bind(dev, driver);
  mutex_unlock(&bus_lock);
  return 0;
}

void driver_unregister(struct device_driver *driver) {
  if (!driver || !driver->bus || !driver->bus->match)
    return;
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(driver->bus);
  if (bus) {
    struct kpi_driver *entry, *next;
    list_for_each_entry_safe(entry, next, &bus->drivers, node) {
      if (entry->driver != driver)
        continue;
      struct device *dev;
      list_for_each_entry(dev, &bus->devices, kpi_bus_node)
        if (dev->driver == driver)
          unbind(dev);
      list_del(&entry->node);
      kfree(entry);
      break;
    }
  }
  mutex_unlock(&bus_lock);
}

int driver_attach(struct device_driver *driver) {
  if (!driver || !driver->bus || !driver->bus->match)
    return 0;
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(driver->bus);
  if (bus) {
    struct device *dev;
    list_for_each_entry(dev, &bus->devices, kpi_bus_node)
      try_bind(dev, driver);
  }
  mutex_unlock(&bus_lock);
  return bus ? 0 : -ENODEV;
}

int device_bind_driver(struct device *dev) {
  if (!dev || !dev->driver)
    return -EINVAL;
  return device_add_groups(dev, dev->driver->dev_groups);
}

void device_release_driver(struct device *dev) {
  if (!dev)
    return;
  mutex_lock(&bus_lock);
  unbind(dev);
  mutex_unlock(&bus_lock);
}

static struct device *bus_find_device(const struct bus_type *type,
                                      struct device *start,
                                      const char *name,
                                      const struct fwnode_handle *fwnode) {
  struct device *result = NULL;
  mutex_lock(&bus_lock);
  struct kpi_bus *bus = find_bus(type);
  if (bus) {
    struct device *dev;
    bool search = !start;
    list_for_each_entry(dev, &bus->devices, kpi_bus_node) {
      if (!search) {
        search = dev == start;
        continue;
      }
      if (name ? !strcmp(dev_name(dev), name) : dev->fwnode == fwnode) {
        result = get_device(dev);
        break;
      }
    }
  }
  mutex_unlock(&bus_lock);
  return result;
}

struct device *bus_find_device_by_name(const struct bus_type *bus,
                                        struct device *start, const char *name) {
  return name ? bus_find_device(bus, start, name, NULL) : NULL;
}

struct device *bus_find_device_by_fwnode(const struct bus_type *bus,
                                          const struct fwnode_handle *fwnode) {
  return fwnode ? bus_find_device(bus, NULL, NULL, fwnode) : NULL;
}
