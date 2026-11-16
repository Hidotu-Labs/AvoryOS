#include "drivers/manager/device.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/heap.h"

static struct device *root_node;
static struct bus_type *bus_list;
static spinlock_t core_lock = SPINLOCK_INIT;
#define DM_PM_DEVICE_MAX 512
static struct device *pm_devices[DM_PM_DEVICE_MAX];
static size_t pm_device_count;
static bool pm_transition;
static bool pm_suspended;
/* Serializes driver-list mutation and driver unbind with suspend snapshots. */
static bool driver_transition;
/* In-flight device destruction keeps the tree stable while suspend scans it. */
static size_t active_mutations;

static struct pci_device *pm_pci_device(struct device *dev) {
  if (!dev || dev->bus != asc_pci_bus_type())
    return NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = asc_pci_get_device(i);
    if (pci && pci->kernel_device == dev)
      return pci;
  }
  return NULL;
}

static bool pm_pci_unbound_endpoint(struct device *dev) {
  return dev && dev->bus == asc_pci_bus_type() &&
         dev->pci_class != 0x06 && !dev->pm_managed_by_core;
}

/* Unbound PCI endpoints have no driver state to restore. Quiesce the function
 * at the bus boundary so it cannot issue DMA or interrupts during S3. If the
 * function implements PCI PM, also put it in D3hot. */
static int pm_pci_suspend_unbound(struct device *dev) {
  struct pci_device *pci = pm_pci_device(dev);
  if (!pci)
    return -95;

  uint16_t command =
      pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  uint8_t pm_cap = asc_pci_find_capability(pci, PCI_CAP_ID_PM);
  uint16_t pmcsr = pm_cap
                       ? pci_config_read16(pci->bus, pci->slot, pci->func,
                                           (uint16_t)pm_cap + 4)
                       : 0;
  dev->pm_pci_command = command;
  dev->pm_pci_pm_cap = pm_cap;
  dev->pm_pci_pmcsr = pmcsr;

  /* Disable I/O, MMIO, and bus mastering before changing the power state.
   * INTx disable also prevents a legacy interrupt from waking the sleeping
   * guest through an unowned function. */
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     (command & ~0x0007U) | (1U << 10));
  if (pm_cap) {
    uint16_t d3 = (pmcsr & ~(0x3U | (1U << 15))) | 0x3U;
    pci_config_write16(pci->bus, pci->slot, pci->func,
                       (uint16_t)pm_cap + 4, d3);
    for (int i = 0; i < 10000; i++)
      io_wait();
    uint16_t state = pci_config_read16(pci->bus, pci->slot, pci->func,
                                       (uint16_t)pm_cap + 4) & 0x3U;
    if (state != 0x3U) {
      /* Some emulated/legacy endpoints expose PMCSR but ignore D3hot. The
       * command register already blocks their DMA and I/O/MMIO decode. */
      klog_puts("[PM] PCI endpoint did not enter D3hot; bus mastering remains disabled for ");
      klog_puts(dev->name ? dev->name : "?");
      klog_puts("\n");
    }
  }
  dev->pm_pci_suspended = true;
  return 0;
}

static int pm_pci_resume_unbound(struct device *dev) {
  struct pci_device *pci = pm_pci_device(dev);
  if (!pci || !dev->pm_pci_suspended)
    return -1;
  if (dev->pm_pci_pm_cap) {
    /* Restore the prior power and PME-enable state. PME status is W1C, so
     * write it as zero rather than replaying a possibly set status bit. */
    uint16_t pmcsr = dev->pm_pci_pmcsr & ~(1U << 15);
    pci_config_write16(pci->bus, pci->slot, pci->func,
                       (uint16_t)dev->pm_pci_pm_cap + 4, pmcsr);
    for (int i = 0; i < 10000; i++)
      io_wait();
  }
  if (!pci_restore_bars(pci))
    return -5;
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     dev->pm_pci_command);
  dev->pm_pci_suspended = false;
  return 0;
}

static int pm_resume_one(struct device *dev) {
  if (dev->pm_pci_suspended)
    return pm_pci_resume_unbound(dev);
  return dev->driver && dev->driver->resume ? dev->driver->resume(dev) : -95;
}

static bool pm_snapshot_tree(struct device *parent) {
  for (struct device *dev = parent->first_child; dev; dev = dev->next_sibling) {
    if (pm_device_count == DM_PM_DEVICE_MAX)
      return false;
    device_get(dev);
    pm_devices[pm_device_count++] = dev;
    if (!pm_snapshot_tree(dev))
      return false;
  }
  return true;
}

static void pm_drop_snapshot(void) {
  for (size_t i = 0; i < pm_device_count; i++) {
    device_put(pm_devices[i]);
    pm_devices[i] = NULL;
  }
  pm_device_count = 0;
}

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
  if (pm_transition || pm_suspended || driver_transition || active_mutations) {
    spinlock_release(&core_lock);
    kfree((void *)dev->name);
    kfree(dev);
    return NULL;
  }
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

static int dm_unbind_device_impl(struct device *dev,
                                 bool during_driver_transition,
                                 bool during_device_destroy) {
  if (!dev)
    return -1;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || dev->pm_suspended ||
      (driver_transition && !during_driver_transition)) {
    spinlock_release(&core_lock);
    return -16;
  }
  spinlock_acquire(&dev->lock);
  struct driver *drv = dev->driver;
  if (dev->state == DEVICE_PROBING ||
      (dev->state == DEVICE_REMOVING && !during_device_destroy) ||
      dev->state == DEVICE_DEAD) {
    spinlock_release(&dev->lock);
    spinlock_release(&core_lock);
    return -16;
  }
  if (!drv) {
    spinlock_release(&dev->lock);
    spinlock_release(&core_lock);
    return 0;
  }
  dev->driver = NULL;
  active_mutations++;
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);

  if (drv->remove)
    drv->remove(dev);

  spinlock_acquire(&dev->lock);
  dev->driver_data = NULL;
  if (dev->state != DEVICE_DEAD)
    dev->state = DEVICE_UNBOUND;
  spinlock_release(&dev->lock);
  spinlock_acquire(&core_lock);
  active_mutations--;
  spinlock_release(&core_lock);
  return 0;
}

int dm_unbind_device(struct device *dev) {
  return dm_unbind_device_impl(dev, false, false);
}

int device_destroy(struct device *dev) {
  if (!dev || dev == root_node)
    return -1;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition || active_mutations ||
      dev->pm_suspended) {
    spinlock_release(&core_lock);
    return -16;
  }
  spinlock_acquire(&dev->lock);
  if (dev->first_child || dev->state == DEVICE_PROBING ||
      dev->state == DEVICE_REMOVING ||
      dev->state == DEVICE_DEAD ||
      dev->external_driver) {
    spinlock_release(&dev->lock);
    spinlock_release(&core_lock);
    return -1;
  }
  device_state_t previous_state = dev->state;
  dev->state = DEVICE_REMOVING;
  active_mutations++;
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);
  int unbind_result = dm_unbind_device_impl(dev, false, true);
  if (unbind_result) {
    spinlock_acquire(&dev->lock);
    dev->state = previous_state;
    spinlock_release(&dev->lock);
    spinlock_acquire(&core_lock);
    active_mutations--;
    spinlock_release(&core_lock);
    return unbind_result;
  }

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
  active_mutations--;
  spinlock_release(&core_lock);

  spinlock_acquire(&dev->lock);
  dev->state = DEVICE_DEAD;
  spinlock_release(&dev->lock);
  device_put(dev);
  return 0;
}

bool device_add_resource(struct device *dev, resource_type_t type,
                         const char *name, uint64_t start, uint64_t end) {
  if (!dev || !name)
    return false;
  if (end < start)
    end = start;
  char *owned_name = dm_strdup(name);
  if (!owned_name)
    return false;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition) {
    spinlock_release(&core_lock);
    kfree(owned_name);
    return false;
  }
  spinlock_acquire(&dev->lock);
  if (dev->resource_count >= MAX_RESOURCES ||
      dev->state == DEVICE_REMOVING || dev->state == DEVICE_DEAD) {
    spinlock_release(&dev->lock);
    spinlock_release(&core_lock);
    kfree(owned_name);
    return false;
  }
  struct resource *res = &dev->resources[dev->resource_count++];
  res->type = type;
  res->name = owned_name;
  res->start = start;
  res->end = end;
  res->flags = 0;
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);
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
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition) {
    spinlock_release(&core_lock);
    return -16;
  }
  spinlock_acquire(&dev->lock);
  if (dev->state != DEVICE_UNBOUND || dev->driver || dev->external_driver) {
    spinlock_release(&dev->lock);
    spinlock_release(&core_lock);
    return -1;
  }
  dev->state = DEVICE_PROBING;
  dev->driver = drv;
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);

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
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition || active_mutations) {
    spinlock_release(&core_lock);
    return;
  }
  for (struct driver *it = drv->bus->drivers; it; it = it->next) {
    if (strcmp(it->name, drv->name) == 0) {
      spinlock_release(&core_lock);
      return;
    }
  }
  drv->next = drv->bus->drivers;
  drv->bus->drivers = drv;
  active_mutations++;
  spinlock_release(&core_lock);
  if (drv->kind == DRIVER_KERNEL)
    for (struct device *dev = drv->bus->devices; dev;
         dev = dev->next_bus_device)
      dm_probe_device(dev);
  spinlock_acquire(&core_lock);
  active_mutations--;
  spinlock_release(&core_lock);
}

int dm_unregister_driver(struct driver *drv) {
  if (!drv || !drv->bus)
    return -1;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition || active_mutations) {
    spinlock_release(&core_lock);
    return -16;
  }
  for (struct device *dev = drv->bus->devices; dev;
       dev = dev->next_bus_device) {
    spinlock_acquire(&dev->lock);
    bool busy = dev->state == DEVICE_PROBING ||
                dev->state == DEVICE_REMOVING;
    spinlock_release(&dev->lock);
    if (busy) {
      spinlock_release(&core_lock);
      return -16;
    }
  }
  driver_transition = true;
  spinlock_release(&core_lock);
  for (struct device *dev = drv->bus->devices; dev;
       dev = dev->next_bus_device) {
    if (dev->driver == drv)
      (void)dm_unbind_device_impl(dev, true, false);
  }
  spinlock_acquire(&core_lock);
  struct driver **link = &drv->bus->drivers;
  while (*link && *link != drv)
    link = &(*link)->next;
  if (*link == drv)
    *link = drv->next;
  driver_transition = false;
  spinlock_release(&core_lock);
  drv->next = NULL;
  return 0;
}

bool dm_claim_external_device(struct device *dev, const char *driver_name) {
  bool claimed = false;
  if (!dev || !driver_name || !driver_name[0])
    return false;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition) {
    spinlock_release(&core_lock);
    return false;
  }
  spinlock_acquire(&dev->lock);
  if (dev->state == DEVICE_UNBOUND && !dev->driver && !dev->external_driver) {
    dev->external_driver = driver_name;
    dev->state = DEVICE_BOUND_EXTERNAL;
    claimed = true;
  }
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);
  return claimed;
}

void dm_release_external_device(struct device *dev, const char *driver_name) {
  if (!dev || !driver_name)
    return;
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition) {
    spinlock_release(&core_lock);
    return;
  }
  spinlock_acquire(&dev->lock);
  if (dev->external_driver && strcmp(dev->external_driver, driver_name) == 0) {
    dev->external_driver = NULL;
    if (dev->state == DEVICE_BOUND_EXTERNAL)
      dev->state = DEVICE_UNBOUND;
  }
  spinlock_release(&dev->lock);
  spinlock_release(&core_lock);
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
  } else if (dev->external_driver) {
    console_puts(" [external: ");
    console_puts(dev->external_driver);
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

int dm_suspend_devices(void) {
  spinlock_acquire(&core_lock);
  if (pm_transition || pm_suspended || driver_transition || active_mutations) {
    spinlock_release(&core_lock);
    return -16;
  }
  pm_transition = true;
  pm_device_count = 0;
  bool snapshot_ok = root_node && pm_snapshot_tree(root_node);
  bool devices_ready = snapshot_ok;
  int preflight_error = snapshot_ok ? -16 : -28;
  struct device *unsupported_device = NULL;
  for (size_t i = 0; devices_ready && i < pm_device_count; i++) {
    struct device *dev = pm_devices[i];
    spinlock_acquire(&dev->lock);
    device_state_t state = dev->state;
    bool has_pci_core_owner = pm_pci_unbound_endpoint(dev);
    bool unsupported_unbound_hardware =
        state == DEVICE_UNBOUND && (dev->bus || dev->resource_count != 0) &&
        !dev->pm_managed_by_core && !has_pci_core_owner;
    spinlock_release(&dev->lock);
    if (state == DEVICE_PROBING || state == DEVICE_REMOVING ||
        state == DEVICE_DEAD) {
      devices_ready = false;
      preflight_error = -16;
      break;
    }
    // A hardware device outside the bound-driver state machine has no owner
    // to save or restore its registers. Treat it as unsupported for S3.
    if (unsupported_unbound_hardware) {
      devices_ready = false;
      preflight_error = -95;
      unsupported_device = dev;
      break;
    }
  }
  if (!devices_ready) {
    pm_transition = false;
    spinlock_release(&core_lock);
    if (unsupported_device) {
      klog_puts("[PM] S3 refused: unbound hardware device ");
      klog_puts(unsupported_device->name ? unsupported_device->name : "?");
      klog_puts(" has no suspend owner.\n");
    }
    pm_drop_snapshot();
    return preflight_error;
  }
  spinlock_release(&core_lock);

  // The device tree snapshot is preorder: reverse traversal suspends children
  // before parents, as required when a parent provides a device's resources.
  for (size_t pos = pm_device_count; pos > 0; pos--) {
    struct device *dev = pm_devices[pos - 1];
    if (dev->state == DEVICE_UNBOUND && pm_pci_unbound_endpoint(dev)) {
      int result = pm_pci_suspend_unbound(dev);
      if (result) {
        klog_puts("[PM] S3 refused: PCI core could not suspend unbound ");
        klog_puts(dev->name ? dev->name : "?");
        klog_puts(" (error ");
        klog_uint64((uint64_t)(-(int64_t)result));
        klog_puts(").\n");
        for (size_t j = pos; j < pm_device_count; j++) {
          struct device *done = pm_devices[j];
          if (done->pm_suspended) {
            (void)pm_resume_one(done);
            done->pm_suspended = false;
          }
        }
        spinlock_acquire(&core_lock);
        pm_transition = false;
        spinlock_release(&core_lock);
        pm_drop_snapshot();
        return result;
      }
      dev->pm_suspended = true;
      klog_puts("[PM] PCI core suspended unbound endpoint ");
      klog_puts(dev->name ? dev->name : "?");
      klog_puts("\n");
      continue;
    }
    if (dev->state != DEVICE_BOUND_KERNEL &&
        dev->state != DEVICE_BOUND_USER &&
        dev->state != DEVICE_BOUND_EXTERNAL)
      continue;
    struct driver *drv = dev->driver;
    if (!drv || !drv->suspend || !drv->resume) {
      klog_puts("[PM] S3 refused: device ");
      klog_puts(dev->name ? dev->name : "?");
      klog_puts(" driver ");
      klog_puts(drv && drv->name ? drv->name : "?");
      klog_puts(" has no complete suspend/resume callbacks.\n");
      // Never enter S3 with a bound device that cannot restore its state.
      for (size_t j = pos; j < pm_device_count; j++) {
        struct device *done = pm_devices[j];
        if (done->pm_suspended) {
          (void)pm_resume_one(done);
          done->pm_suspended = false;
        }
      }
      spinlock_acquire(&core_lock);
      pm_transition = false;
      spinlock_release(&core_lock);
      pm_drop_snapshot();
      return -95;
    }
    int result = drv->suspend(dev);
    if (result) {
      klog_puts("[PM] S3 refused: device suspend failed for ");
      klog_puts(dev->name ? dev->name : "?");
      klog_puts(" (error ");
      klog_uint64((uint64_t)(-(int64_t)result));
      klog_puts(").\n");
      for (size_t j = pos; j < pm_device_count; j++) {
        struct device *done = pm_devices[j];
        if (done->pm_suspended) {
          (void)pm_resume_one(done);
          done->pm_suspended = false;
        }
      }
      spinlock_acquire(&core_lock);
      pm_transition = false;
      spinlock_release(&core_lock);
      pm_drop_snapshot();
      return result;
    }
    dev->pm_suspended = true;
  }

  spinlock_acquire(&core_lock);
  pm_suspended = true;
  pm_transition = false;
  spinlock_release(&core_lock);
  return 0;
}

int dm_resume_devices(void) {
  spinlock_acquire(&core_lock);
  if (pm_transition || !pm_suspended) {
    spinlock_release(&core_lock);
    return -22;
  }
  pm_transition = true;
  size_t count = pm_device_count;
  spinlock_release(&core_lock);

  // Preorder resumes parents before children so shared resources are ready.
  int first_error = 0;
  /* Restore marked devices ahead of slow sibling callbacks (for example,
   * bring the display back before xHCI synchronously re-enumerates USB). Keep
   * the parent-before-child rule: an early device waits for every suspended
   * ancestor to finish first. */
  for (size_t i = 0; i < count; i++) {
    struct device *dev = pm_devices[i];
    if (!dev->pm_suspended || !dev->driver ||
        !dev->driver->resume_early || !dev->driver->resume)
      continue;
    bool parents_ready = true;
    for (struct device *parent = dev->parent; parent; parent = parent->parent) {
      if (parent->pm_suspended) {
        parents_ready = false;
        break;
      }
    }
    if (!parents_ready)
      continue;
    int result = pm_resume_one(dev);
    if (result && !first_error)
      first_error = result;
    dev->pm_suspended = false;
  }
  for (size_t i = 0; i < count; i++) {
    struct device *dev = pm_devices[i];
    if (!dev->pm_suspended)
      continue;
    int result = pm_resume_one(dev);
    if (result && !first_error)
      first_error = result;
    dev->pm_suspended = false;
  }
  pm_drop_snapshot();

  spinlock_acquire(&core_lock);
  pm_suspended = false;
  pm_transition = false;
  spinlock_release(&core_lock);
  return first_error;
}
