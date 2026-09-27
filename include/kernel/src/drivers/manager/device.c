#include "drivers/manager/device.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/heap.h"

static struct device *root_node;
static struct bus_type *bus_list;
static spinlock_t core_lock = SPINLOCK_INIT;

static char *dm_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  char *copy = kmalloc(len + 1);
  if (copy)
    memcpy(copy, s, len + 1);
  return copy;
}

static struct device *find_child(struct device *parent, const char *name) {
  if (!parent || !name)
    return NULL;
  for (struct device *child = parent->first_child; child;
       child = child->next_sibling) {
    if (strcmp(child->name, name) == 0)
      return child;
  }
  return NULL;
}

void dm_init(void) {
  root_node = kmalloc(sizeof(*root_node));
  if (!root_node)
    return;
  memset(root_node, 0, sizeof(*root_node));
  root_node->name = dm_strdup("");
  root_node->state = DEVICE_UNBOUND;
  root_node->refcount = 1;
  spinlock_init(&root_node->lock);
  bus_list = NULL;
  device_create(root_node, "sys");
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
               " Device core initialized.\n");
}

struct device *dm_root(void) { return root_node; }

int dm_register_bus(struct bus_type *bus) {
  if (!bus || !bus->name || !bus->name[0])
    return -1;
  spinlock_acquire(&core_lock);
  for (struct bus_type *it = bus_list; it; it = it->next) {
    if (it == bus || strcmp(it->name, bus->name) == 0) {
      spinlock_release(&core_lock);
      return it == bus ? 0 : -1;
    }
  }
  bus->devices = NULL;
  bus->drivers = NULL;
  spinlock_init(&bus->lock);
  bus->next = bus_list;
  bus_list = bus;
  spinlock_release(&core_lock);
  return 0;
}

struct bus_type *dm_find_bus(const char *name) {
  if (!name)
    return NULL;
  spinlock_acquire(&core_lock);
  for (struct bus_type *bus = bus_list; bus; bus = bus->next) {
    if (strcmp(bus->name, name) == 0) {
      spinlock_release(&core_lock);
      return bus;
    }
  }
  spinlock_release(&core_lock);
  return NULL;
}

struct device *device_create_on_bus(struct bus_type *bus,
                                    struct device *parent,
                                    const char *name) {
  if (!name || !name[0])
    return NULL;
  struct device *dev = kmalloc(sizeof(*dev));
  if (!dev)
    return NULL;
  memset(dev, 0, sizeof(*dev));
  dev->name = dm_strdup(name);
  if (!dev->name) {
    kfree(dev);
    return NULL;
  }
  dev->parent = parent;
  dev->bus = bus;
  dev->state = DEVICE_UNBOUND;
  dev->refcount = 1;
  spinlock_init(&dev->lock);

  spinlock_acquire(&core_lock);
  if (parent) {
    if (!parent->first_child) {
      parent->first_child = dev;
    } else {
      struct device *tail = parent->first_child;
      while (tail->next_sibling)
        tail = tail->next_sibling;
      tail->next_sibling = dev;
    }
  }
  if (bus) {
    dev->next_bus_device = bus->devices;
    bus->devices = dev;
  }
  spinlock_release(&core_lock);
  return dev;
}

struct device *device_create(struct device *parent, const char *name) {
  return device_create_on_bus(NULL, parent, name);
}

void device_get(struct device *dev) {
  if (dev)
    __atomic_add_fetch(&dev->refcount, 1, __ATOMIC_RELAXED);
}

void device_put(struct device *dev) {
  if (!dev || dev == root_node)
    return;
  if (__atomic_sub_fetch(&dev->refcount, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  for (size_t i = 0; i < dev->resource_count; i++)
    kfree((void *)dev->resources[i].name);
  kfree((void *)dev->name);
  kfree(dev);
}

int dm_unbind_device(struct device *dev) {
  if (!dev)
    return -1;
  spinlock_acquire(&dev->lock);
  struct driver *drv = dev->driver;
  if (!drv) {
    spinlock_release(&dev->lock);
    return 0;
  }
  dev->driver = NULL;
  spinlock_release(&dev->lock);

  if (drv->remove)
    drv->remove(dev);

  spinlock_acquire(&dev->lock);
  dev->driver_data = NULL;
  if (dev->state != DEVICE_DEAD)
    dev->state = DEVICE_UNBOUND;
  spinlock_release(&dev->lock);
  return 0;
}

int device_destroy(struct device *dev) {
  if (!dev || dev == root_node || dev->first_child)
    return -1;
  spinlock_acquire(&dev->lock);
  if (dev->state == DEVICE_REMOVING || dev->state == DEVICE_DEAD) {
    spinlock_release(&dev->lock);
    return -1;
  }
  dev->state = DEVICE_REMOVING;
  spinlock_release(&dev->lock);
  dm_unbind_device(dev);

  spinlock_acquire(&core_lock);
  if (dev->parent) {
    struct device **link = &dev->parent->first_child;
    while (*link && *link != dev)
      link = &(*link)->next_sibling;
    if (*link == dev)
      *link = dev->next_sibling;
  }
  if (dev->bus) {
    struct device **link = &dev->bus->devices;
    while (*link && *link != dev)
      link = &(*link)->next_bus_device;
    if (*link == dev)
      *link = dev->next_bus_device;
  }
  dev->parent = NULL;
  dev->next_sibling = NULL;
  dev->next_bus_device = NULL;
  spinlock_release(&core_lock);

  spinlock_acquire(&dev->lock);
  dev->state = DEVICE_DEAD;
  spinlock_release(&dev->lock);
  device_put(dev);
  return 0;
}

bool device_add_resource(struct device *dev, resource_type_t type,
                         const char *name, uint64_t start, uint64_t end) {
  if (!dev || !name || dev->resource_count >= MAX_RESOURCES)
    return false;
  if (end < start)
    end = start;
  char *owned_name = dm_strdup(name);
  if (!owned_name)
    return false;
  struct resource *res = &dev->resources[dev->resource_count++];
  res->type = type;
  res->name = owned_name;
  res->start = start;
  res->end = end;
  res->flags = 0;
  return true;
}

struct device *device_find_by_path(const char *path) {
  if (!root_node || !path || path[0] != '/')
    return NULL;
  if (path[1] == '\0')
    return root_node;
  struct device *current = root_node;
  const char *p = path + 1;
  char component[64];
  while (*p) {
    size_t len = 0;
    while (*p && *p != '/' && len + 1 < sizeof(component))
      component[len++] = *p++;
    component[len] = '\0';
    if (*p == '/')
      p++;
    current = find_child(current, component);
    if (!current)
      return NULL;
  }
  return current;
}

static bool id_matches(struct device *dev, const struct device_id *id) {
  switch (id->type) {
  case ID_ANY:
    return true;
  case ID_NAME:
    return id->name && strcmp(dev->name, id->name) == 0;
  case ID_PCI:
    if (id->pci.match_class)
      return dev->pci_class == id->pci.class &&
             dev->pci_subclass == id->pci.subclass &&
             (!id->pci.prog_if || dev->pci_prog_if == id->pci.prog_if);
    return dev->vendor_id == id->pci.vendor &&
           dev->device_id == id->pci.device;
  case ID_ACPI:
    return false;
  }
  return false;
}

bool dm_driver_matches(struct device *dev, struct driver *drv) {
  if (!dev || !drv || (drv->bus && dev->bus != drv->bus))
    return false;
  if (dev->driver_override[0])
    return strcmp(dev->driver_override, drv->name) == 0;
  if (dev->bus && dev->bus->match)
    return dev->bus->match(dev, drv);
  for (size_t i = 0; i < drv->id_count; i++) {
    if (id_matches(dev, &drv->ids[i]))
      return true;
  }
  return false;
}

int dm_bind_device(struct device *dev, struct driver *drv) {
  if (!dev || !drv || !drv->probe || !dm_driver_matches(dev, drv))
    return -1;
  spinlock_acquire(&dev->lock);
  if (dev->state != DEVICE_UNBOUND || dev->driver) {
    spinlock_release(&dev->lock);
    return -1;
  }
  dev->state = DEVICE_PROBING;
  dev->driver = drv;
  spinlock_release(&dev->lock);

  int result = drv->probe(dev);
  spinlock_acquire(&dev->lock);
  if (result == 0) {
    dev->driver = drv;
    dev->state = drv->kind == DRIVER_USER ? DEVICE_BOUND_USER
                                          : DEVICE_BOUND_KERNEL;
  } else {
    dev->driver_data = NULL;
    dev->driver = NULL;
    dev->state = DEVICE_UNBOUND;
  }
  spinlock_release(&dev->lock);

  return result;
}

struct driver *dm_find_driver(struct bus_type *bus, const char *name) {
  if (!bus || !name)
    return NULL;
  for (struct driver *drv = bus->drivers; drv; drv = drv->next) {
    if (strcmp(drv->name, name) == 0)
      return drv;
  }
  return NULL;
}

int dm_bind_device_named(struct device *dev, const char *driver_name) {
  struct driver *drv = dm_find_driver(dev ? dev->bus : NULL, driver_name);
  return drv ? dm_bind_device(dev, drv) : -1;
}

int dm_probe_device(struct device *dev) {
  if (!dev || !dev->bus || dev->state != DEVICE_UNBOUND)
    return -1;
  for (struct driver *drv = dev->bus->drivers; drv; drv = drv->next) {
    if (drv->kind != DRIVER_KERNEL)
      continue;
    if (dm_driver_matches(dev, drv) && dm_bind_device(dev, drv) == 0)
      return 0;
  }
  return -1;
}

void dm_register_driver(struct driver *drv) {
  if (!drv || !drv->name)
    return;
  if (!drv->bus)
    drv->bus = dm_find_bus("pci");
  if (!drv->bus)
    return;
  if (dm_find_driver(drv->bus, drv->name))
    return;

  spinlock_acquire(&core_lock);
  drv->next = drv->bus->drivers;
  drv->bus->drivers = drv;
  spinlock_release(&core_lock);
  if (drv->kind == DRIVER_KERNEL)
    for (struct device *dev = drv->bus->devices; dev;
         dev = dev->next_bus_device)
      dm_probe_device(dev);
}

int dm_unregister_driver(struct driver *drv) {
  if (!drv || !drv->bus)
    return -1;
  for (struct device *dev = drv->bus->devices; dev;
       dev = dev->next_bus_device) {
    if (dev->driver == drv)
      dm_unbind_device(dev);
  }
  spinlock_acquire(&core_lock);
  struct driver **link = &drv->bus->drivers;
  while (*link && *link != drv)
    link = &(*link)->next;
  if (*link == drv)
    *link = drv->next;
  spinlock_release(&core_lock);
  drv->next = NULL;
  return 0;
}

void dm_for_each_device(struct bus_type *bus, dm_device_iter_t fn, void *ctx) {
  if (!bus || !fn)
    return;
  for (struct device *dev = bus->devices; dev; dev = dev->next_bus_device) {
    if (!fn(dev, ctx))
      break;
  }
}

void dm_for_each_driver(struct bus_type *bus, dm_driver_iter_t fn, void *ctx) {
  if (!bus || !fn)
    return;
  for (struct driver *drv = bus->drivers; drv; drv = drv->next) {
    if (!fn(drv, ctx))
      break;
  }
}

static void dump_device(struct device *dev, int depth) {
  for (int i = 0; i < depth; i++)
    console_puts("  ");
  console_puts("- ");
  console_puts(dev->name);
  if (dev->driver) {
    console_puts(" [");
    console_puts(dev->driver->name);
    console_puts("]");
  }
  console_puts("\n");
  for (struct device *child = dev->first_child; child;
       child = child->next_sibling)
    dump_device(child, depth + 1);
}

void dm_dump_tree(void) {
  console_puts("Device Tree Hierarchy:\n");
  if (root_node)
    dump_device(root_node, 0);
}
