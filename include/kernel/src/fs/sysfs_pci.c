#include "fs/sysfs_pci.h"
#include "console/klog.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "fs/ramfs.h"
#include "lib/string.h"
#include "mm/heap.h"

#define PCI_SYSFS_MAX_DEVICES PCI_MAX_DEVICES

typedef int (*sysfs_show_t)(void *ctx, char *buf, size_t size);
typedef int (*sysfs_store_t)(void *ctx, const char *buf, size_t size);

struct sysfs_attribute {
  sysfs_show_t show;
  sysfs_store_t store;
  void *ctx;
};

enum pci_attr_kind {
  PCI_ATTR_VENDOR,
  PCI_ATTR_DEVICE,
  PCI_ATTR_SUBSYSTEM_VENDOR,
  PCI_ATTR_SUBSYSTEM_DEVICE,
  PCI_ATTR_REVISION,
  PCI_ATTR_CLASS,
  PCI_ATTR_IRQ,
  PCI_ATTR_RESOURCE,
  PCI_ATTR_MODALIAS,
  PCI_ATTR_UEVENT,
  PCI_ATTR_ENABLE,
  PCI_ATTR_DRIVER_OVERRIDE
};

struct pci_attr_context {
  struct pci_device *pci;
  enum pci_attr_kind kind;
};

struct driver_attr_context {
  char driver_name[DRIVER_OVERRIDE_LEN];
  bool bind;
};

static vfs_node_t *pci_devices_dir;
static vfs_node_t *pci_drivers_dir;
static vfs_node_t *pci_device_nodes[PCI_SYSFS_MAX_DEVICES];

static vfs_node_t *pci_mkdir(vfs_node_t *parent, const char *name) {
  vfs_node_t *existing = parent ? vfs_finddir(parent, (char *)name) : NULL;
  if (existing && (existing->flags & FS_TYPE_MASK) == FS_DIRECTORY)
    return existing;
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node)
    return NULL;
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->flags = FS_DIRECTORY | FS_PERSISTENT;
  node->mask = 0555;
  ramfs_mount_on(node);
  ramfs_mount_node(parent, node);
  vfs_dentry_invalidate(parent, name);
  return node;
}

static int pci_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (!node || !node->ptr || !buf || !size)
    return -1;
  const char *target = (const char *)node->ptr;
  uint32_t len = (uint32_t)strlen(target);
  if (len > size)
    len = size;
  memcpy(buf, target, len);
  return (int)len;
}

static void pci_symlink(vfs_node_t *parent, const char *name,
                        const char *target) {
  if (!parent || vfs_finddir(parent, (char *)name))
    return;
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node)
    return;
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->flags = FS_SYMLINK | FS_PERSISTENT;
  node->mask = 0777;
  node->readlink = pci_readlink;
  char *owned_target = kmalloc(strlen(target) + 1);
  if (!owned_target) {
    kfree(node);
    return;
  }
  strcpy(owned_target, target);
  node->ptr = (vfs_node_t *)owned_target;
  ramfs_mount_node(parent, node);
  vfs_dentry_invalidate(parent, name);
}

static uint32_t attr_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  struct sysfs_attribute *attr = node ? node->device : NULL;
  if (!attr || !attr->show || !buffer)
    return 0;
  char value[1024];
  int len = attr->show(attr->ctx, value, sizeof(value));
  if (len <= 0 || offset >= (uint32_t)len)
    return 0;
  uint32_t available = (uint32_t)len - offset;
  if (size > available)
    size = available;
  memcpy(buffer, value + offset, size);
  return size;
}

static uint32_t attr_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                           uint8_t *buffer) {
  struct sysfs_attribute *attr = node ? node->device : NULL;
  if (!attr || !attr->store || !buffer || offset != 0 || size > 127)
    return 0;
  uint32_t written = size;
  char value[128];
  memcpy(value, buffer, size);
  value[size] = '\0';
  while (size && (value[size - 1] == '\n' || value[size - 1] == '\r' ||
                  value[size - 1] == ' ' || value[size - 1] == '\t'))
    value[--size] = '\0';
  return attr->store(attr->ctx, value, size) == 0 ? written : 0;
}

static uint32_t config_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  struct pci_device *pci = node ? node->device : NULL;
  if (!pci || !buffer || offset >= node->length)
    return 0;
  if (size > node->length - offset)
    size = node->length - offset;
  for (uint32_t i = 0; i < size; i++) {
    uint32_t pos = offset + i;
    uint32_t value = pci_config_read32(pci->bus, pci->slot, pci->func,
                                       (uint16_t)(pos & ~3U));
    buffer[i] = (uint8_t)(value >> ((pos & 3U) * 8));
  }
  return size;
}

static vfs_node_t *pci_attr(vfs_node_t *parent, const char *name,
                            uint16_t mode, sysfs_show_t show,
                            sysfs_store_t store, void *ctx) {
  if (!parent || vfs_finddir(parent, (char *)name))
    return NULL;
  vfs_node_t *node = kmalloc(sizeof(*node));
  struct sysfs_attribute *attr = kmalloc(sizeof(*attr));
  if (!node || !attr) {
    kfree(node);
    kfree(attr);
    return NULL;
  }
  vfs_node_init(node);
  strncpy(node->name, name, sizeof(node->name) - 1);
  node->flags = FS_FILE | FS_PERSISTENT;
  node->mask = mode;
  node->read = show ? attr_read : NULL;
  node->write = store ? attr_write : NULL;
  attr->show = show;
  attr->store = store;
  attr->ctx = ctx;
  node->device = attr;
  ramfs_mount_node(parent, node);
  vfs_dentry_invalidate(parent, name);
  return node;
}

static struct pci_device *find_pci_name(const char *name) {
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->kernel_device &&
        strcmp(pci->kernel_device->name, name) == 0)
      return pci;
  }
  return NULL;
}

static uint64_t bar_start(struct pci_device *pci, unsigned bar) {
  uint32_t raw = pci->bar[bar];
  return raw & ((raw & 1) ? ~0x3ULL : ~0xfULL);
}

static int pci_show(void *opaque, char *buf, size_t size) {
  struct pci_attr_context *ctx = opaque;
  struct pci_device *pci = ctx ? ctx->pci : NULL;
  if (!pci)
    return -1;
  switch (ctx->kind) {
  case PCI_ATTR_VENDOR:
    return snprintf(buf, size, "0x%04x\n", pci->vendor_id);
  case PCI_ATTR_DEVICE:
    return snprintf(buf, size, "0x%04x\n", pci->device_id);
  case PCI_ATTR_SUBSYSTEM_VENDOR: {
    uint32_t value = pci_config_read32(pci->bus, pci->slot, pci->func, 0x2c);
    return snprintf(buf, size, "0x%04x\n", value & 0xffff);
  }
  case PCI_ATTR_SUBSYSTEM_DEVICE: {
    uint32_t value = pci_config_read32(pci->bus, pci->slot, pci->func, 0x2c);
    return snprintf(buf, size, "0x%04x\n", value >> 16);
  }
  case PCI_ATTR_REVISION:
    return snprintf(buf, size, "0x%02x\n",
                    pci_config_read32(pci->bus, pci->slot, pci->func, 0x08) & 0xff);
  case PCI_ATTR_CLASS:
    return snprintf(buf, size, "0x%02x%02x%02x\n", pci->class_code,
                    pci->subclass, pci->prog_if);
  case PCI_ATTR_IRQ:
    return snprintf(buf, size, "%u\n", pci->irq_line);
  case PCI_ATTR_MODALIAS:
    return snprintf(buf, size,
                    "pci:v0000%04Xd0000%04Xsv00000000sd00000000bc%02Xsc%02Xi%02X\n",
                    pci->vendor_id, pci->device_id, pci->class_code,
                    pci->subclass, pci->prog_if);
  case PCI_ATTR_UEVENT:
    return snprintf(buf, size,
                    "DRIVER=%s\nPCI_CLASS=%02X%02X%02X\nPCI_ID=%04X:%04X\n"
                    "MODALIAS=pci:v0000%04Xd0000%04Xsv00000000sd00000000bc%02Xsc%02Xi%02X\n",
                    pci->kernel_device && pci->kernel_device->driver
                        ? pci->kernel_device->driver->name
                        : "",
                    pci->class_code, pci->subclass, pci->prog_if,
                    pci->vendor_id, pci->device_id, pci->vendor_id,
                    pci->device_id, pci->class_code, pci->subclass,
                    pci->prog_if);
  case PCI_ATTR_ENABLE: {
    uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    return snprintf(buf, size, "%u\n", (command & 3) != 0);
  }
  case PCI_ATTR_DRIVER_OVERRIDE:
    return snprintf(buf, size, "%s\n",
                    pci->kernel_device ? pci->kernel_device->driver_override
                                       : "");
  case PCI_ATTR_RESOURCE: {
    size_t used = 0;
    for (unsigned bar = 0; bar < 6 && used < size; bar++) {
      uint64_t start = pci->bar[bar] ? bar_start(pci, bar) : 0;
      uint64_t end = start ? start + 0xfff : 0;
      uint64_t flags = pci->bar[bar] ? ((pci->bar[bar] & 1) ? 0x100 : 0x200) : 0;
      int n = snprintf(buf + used, size - used,
                       "%016llx %016llx %016llx\n",
                       (unsigned long long)start, (unsigned long long)end,
                       (unsigned long long)flags);
      if (n < 0)
        break;
      used += (size_t)n;
    }
    return (int)used;
  }
  }
  return -1;
}

static int pci_store(void *opaque, const char *buf, size_t size) {
  struct pci_attr_context *ctx = opaque;
  struct pci_device *pci = ctx ? ctx->pci : NULL;
  if (!pci || !pci->kernel_device)
    return -1;
  if (ctx->kind == PCI_ATTR_DRIVER_OVERRIDE) {
    if (size >= DRIVER_OVERRIDE_LEN)
      return -1;
    memcpy(pci->kernel_device->driver_override, buf, size);
    pci->kernel_device->driver_override[size] = '\0';
    return 0;
  }
  if (ctx->kind == PCI_ATTR_ENABLE && size == 1 &&
      (buf[0] == '0' || buf[0] == '1')) {
    uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    command = buf[0] == '1' ? (command | 3) : (command & ~3U);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command);
    return 0;
  }
  return -1;
}

static vfs_node_t *driver_dir(struct driver *driver) {
  return driver && pci_drivers_dir
             ? vfs_finddir(pci_drivers_dir, (char *)driver->name)
             : NULL;
}

static vfs_node_t *device_dir(struct pci_device *pci) {
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    if (pci_get_device(i) == pci)
      return pci_device_nodes[i];
  }
  return NULL;
}

static void remove_driver_links(struct pci_device *pci, struct driver *driver) {
  vfs_node_t *ddir = device_dir(pci);
  vfs_node_t *drvdir = driver_dir(driver);
  if (ddir && ddir->unlink)
    vfs_unlink(ddir, "driver");
  if (drvdir && drvdir->unlink)
    vfs_unlink(drvdir, (char *)pci->kernel_device->name);
}

static void add_driver_links(struct pci_device *pci, struct driver *driver) {
  vfs_node_t *ddir = device_dir(pci);
  vfs_node_t *drvdir = driver_dir(driver);
  if (!ddir || !drvdir)
    return;
  char target[160];
  snprintf(target, sizeof(target), "../../../bus/pci/drivers/%s", driver->name);
  pci_symlink(ddir, "driver", target);
  snprintf(target, sizeof(target),
           "../../../../devices/pci0000:00/%s", pci->kernel_device->name);
  pci_symlink(drvdir, pci->kernel_device->name, target);
}

static int driver_store(void *opaque, const char *buf, size_t size) {
  struct driver_attr_context *ctx = opaque;
  if (!ctx || !size)
    return -1;
  struct driver *driver =
      dm_find_driver(pci_bus_type(), ctx->driver_name);
  if (!driver)
    return -1;
  struct pci_device *pci = find_pci_name(buf);
  if (!pci || !pci->kernel_device)
    return -1;
  if (ctx->bind) {
    if (dm_bind_device(pci->kernel_device, driver) != 0)
      return -1;
    add_driver_links(pci, driver);
    return 0;
  }
  if (pci->kernel_device->driver != driver)
    return -1;
  remove_driver_links(pci, driver);
  return dm_unbind_device(pci->kernel_device);
}

static struct pci_attr_context *new_pci_context(struct pci_device *pci,
                                                 enum pci_attr_kind kind) {
  struct pci_attr_context *ctx = kmalloc(sizeof(*ctx));
  if (ctx) {
    ctx->pci = pci;
    ctx->kind = kind;
  }
  return ctx;
}

static void add_pci_attribute(vfs_node_t *dir, const char *name, uint16_t mode,
                              struct pci_device *pci,
                              enum pci_attr_kind kind, bool writable) {
  struct pci_attr_context *ctx = new_pci_context(pci, kind);
  if (ctx)
    pci_attr(dir, name, mode, pci_show, writable ? pci_store : NULL, ctx);
}

static bool add_driver_directory(struct driver *driver, void *unused) {
  (void)unused;
  vfs_node_t *dir = pci_mkdir(pci_drivers_dir, driver->name);
  if (!dir)
    return true;
  struct driver_attr_context *bind = kmalloc(sizeof(*bind));
  struct driver_attr_context *unbind = kmalloc(sizeof(*unbind));
  if (bind) {
    strncpy(bind->driver_name, driver->name, sizeof(bind->driver_name) - 1);
    bind->driver_name[sizeof(bind->driver_name) - 1] = '\0';
    bind->bind = true;
    pci_attr(dir, "bind", 0200, NULL, driver_store, bind);
  }
  if (unbind) {
    strncpy(unbind->driver_name, driver->name, sizeof(unbind->driver_name) - 1);
    unbind->driver_name[sizeof(unbind->driver_name) - 1] = '\0';
    unbind->bind = false;
    pci_attr(dir, "unbind", 0200, NULL, driver_store, unbind);
  }
  if (driver->kind == DRIVER_KERNEL)
    pci_symlink(dir, "module", "../../../../module/ascent_builtin");
  return true;
}

void sysfs_pci_driver_registered(struct driver *driver) {
  if (driver && driver->bus == pci_bus_type() && pci_drivers_dir)
    add_driver_directory(driver, NULL);
}

static struct pci_device *pci_from_device(struct device *device) {
  if (!device)
    return NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->kernel_device == device)
      return pci;
  }
  return NULL;
}

void sysfs_pci_device_bound(struct device *device) {
  struct pci_device *pci = pci_from_device(device);
  if (pci && device->driver)
    add_driver_links(pci, device->driver);
}

void sysfs_pci_device_unbinding(struct device *device) {
  struct pci_device *pci = pci_from_device(device);
  if (pci && device->driver)
    remove_driver_links(pci, device->driver);
}


static void add_region_files(vfs_node_t *dir, struct pci_device *pci) {
  for (unsigned bar = 0; bar < 6; bar++) {
    char name[16];
    snprintf(name, sizeof(name), "resource%u", bar);
    vfs_node_t *node = pci_attr(dir, name, 0400, NULL, NULL, pci);
    if (node && pci->bar[bar])
      node->length = 0x1000;
  }
}

static void add_config_file(vfs_node_t *dir, struct pci_device *pci) {
  if (!dir || vfs_finddir(dir, "config"))
    return;
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node)
    return;
  vfs_node_init(node);
  strcpy(node->name, "config");
  node->flags = FS_FILE | FS_PERSISTENT;
  node->mask = 0400;
  node->length = 256;
  node->device = pci;
  node->read = config_read;
  ramfs_mount_node(dir, node);
  vfs_dentry_invalidate(dir, "config");
}

void sysfs_pci_init(vfs_node_t *pci_bus_dir, vfs_node_t *devices_root) {
  if (!pci_bus_dir || !devices_root)
    return;
  pci_devices_dir = pci_mkdir(pci_bus_dir, "devices");
  pci_drivers_dir = pci_mkdir(pci_bus_dir, "drivers");
  vfs_node_t *segment = pci_mkdir(devices_root, "pci0000:00");
  if (!pci_devices_dir || !pci_drivers_dir || !segment)
    return;

  dm_for_each_driver(pci_bus_type(), add_driver_directory, NULL);

  uint32_t count = pci_get_device_count();
  if (count > PCI_SYSFS_MAX_DEVICES)
    count = PCI_SYSFS_MAX_DEVICES;
  for (uint32_t i = 0; i < count; i++) {
    struct pci_device *pci = pci_get_device(i);
    if (!pci || !pci->kernel_device)
      continue;
    const char *bdf = pci->kernel_device->name;
    vfs_node_t *dir = pci_mkdir(segment, bdf);
    pci_device_nodes[i] = dir;
    if (!dir)
      continue;

    char target[160];
    snprintf(target, sizeof(target), "../../../devices/pci0000:00/%s", bdf);
    pci_symlink(pci_devices_dir, bdf, target);
    pci_symlink(dir, "subsystem", "../../../bus/pci");

    add_pci_attribute(dir, "vendor", 0444, pci, PCI_ATTR_VENDOR, false);
    add_pci_attribute(dir, "device", 0444, pci, PCI_ATTR_DEVICE, false);
    add_pci_attribute(dir, "subsystem_vendor", 0444, pci,
                      PCI_ATTR_SUBSYSTEM_VENDOR, false);
    add_pci_attribute(dir, "subsystem_device", 0444, pci,
                      PCI_ATTR_SUBSYSTEM_DEVICE, false);
    add_pci_attribute(dir, "revision", 0444, pci, PCI_ATTR_REVISION, false);
    add_pci_attribute(dir, "class", 0444, pci, PCI_ATTR_CLASS, false);
    add_pci_attribute(dir, "irq", 0444, pci, PCI_ATTR_IRQ, false);
    add_pci_attribute(dir, "resource", 0444, pci, PCI_ATTR_RESOURCE, false);
    add_pci_attribute(dir, "modalias", 0444, pci, PCI_ATTR_MODALIAS, false);
    add_pci_attribute(dir, "uevent", 0444, pci, PCI_ATTR_UEVENT, false);
    add_pci_attribute(dir, "enable", 0644, pci, PCI_ATTR_ENABLE, true);
    add_pci_attribute(dir, "driver_override", 0644, pci,
                      PCI_ATTR_DRIVER_OVERRIDE, true);
    add_config_file(dir, pci);
    add_region_files(dir, pci);

    if (pci->kernel_device->driver)
      add_driver_links(pci, pci->kernel_device->driver);
  }
}
