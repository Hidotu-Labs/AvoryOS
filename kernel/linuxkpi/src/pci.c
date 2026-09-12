/* Minimal Linux PCI API for LinuxKPI (Phase 4 C4).
 *
 * One `struct pci_dev` wrapper is built per device the native PCI enumerator
 * found (kernel/src/drivers/pci); the bridge in linuxkpi/native_pci.h hides
 * the native types.  Implemented here: config access, capability lookup,
 * resource decoding from BARs, region bookkeeping, BAR mapping, device
 * enable/master, and a direct probe/remove driver registry.
 *
 * Deliberately not modeled yet (recorded in docs/linuxkpi-gaps.md):
 *   - pci_dev reference counting: `pci_get_device()`-style wrappers live for
 *     the whole kernel lifetime; pci_dev_get/put are no-ops.
 *   - the generic driver core: pci_register_driver() probes directly through
 *     the id table (no driver_register(), no deferred probe, no dynids).
 *   - real I/O-port address translation: I/O BARs expose their raw port
 *     address and request_region() keeps only a conflict registry.
 *   - ROM size probing: pci_map_rom() only maps an already-decoded BAR.
 *   - MSI/MSI-X allocation (CONFIG_PCI_MSI is set for struct layout only).
 *
 * The root bus/device scaffolding exists so `pci_dev.bus`, `dev.archdata`,
 * and the `kobj.parent` chain DRM's sysfs links expect are all non-NULL. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_pci.h>

/* ── bus / root scaffolding ─────────────────────────────────────────────── */

struct bus_type pci_bus_type = {
    .name = "pci",
};

static struct pci_bus kpi_root_bus;
static bool kpi_pci_scanned;

/* Wrappers in native enumeration order.  They are never freed; see the file
 * header. */
static struct pci_dev **kpi_devs;
static int kpi_dev_count;

/* Registered LinuxKPI PCI drivers. */
struct kpi_pci_driver {
  struct pci_driver *drv;
  struct list_head node;
};

static LIST_HEAD(kpi_pci_drivers);

/* ── regions (conflict registry; native port I/O is raw in/out) ─────────── */

struct resource ioport_resource = {
    .name = "PCI/ISA PnP",
    .start = 0,
    .end = 0xFFFF,
    .flags = IORESOURCE_IO,
};

static struct resource *kpi_pci_region_parent(unsigned long flags) {
  return (flags & IORESOURCE_IO) ? &ioport_resource : &iomem_resource;
}

struct resource *__request_region(struct resource *parent,
                                  resource_size_t start, resource_size_t n,
                                  const char *name, int flags) {
  struct resource *res;
  resource_size_t end;

  if (!parent || !n)
    return NULL;
  end = start + n - 1;
  if (end < start)
    return NULL;

  for (res = parent->child; res; res = res->sibling) {
    if (res->end >= start && res->start <= end)
      return NULL; /* conflict */
  }

  res = kzalloc(sizeof(*res), GFP_KERNEL);
  if (!res)
    return NULL;

  res->name = name;
  res->start = start;
  res->end = end;
  res->flags = (unsigned long)flags;
  res->parent = parent;
  res->sibling = parent->child;
  parent->child = res;
  return res;
}

void __release_region(struct resource *parent, resource_size_t start,
                      resource_size_t n) {
  struct resource *res, *prev = NULL;
  resource_size_t end;

  if (!parent || !n)
    return;
  end = start + n - 1;

  for (res = parent->child; res; prev = res, res = res->sibling) {
    if (res->start == start && res->end == end) {
      if (prev)
        prev->sibling = res->sibling;
      else
        parent->child = res->sibling;
      kfree(res);
      return;
    }
  }
}

static void kpi_devm_release_region_action(void *data) {
  struct resource *res = data;

  if (res)
    __release_region(res->parent, res->start, res->end - res->start + 1);
}

struct resource *__devm_request_region(struct device *dev,
                                       struct resource *parent,
                                       resource_size_t start,
                                       resource_size_t n,
                                       const char *name) {
  struct resource *res;

  if (!dev)
    return NULL;
  res = __request_region(parent, start, n, name, 0);
  if (!res)
    return NULL;
  if (devm_add_action(dev, kpi_devm_release_region_action, res)) {
    __release_region(parent, start, n);
    return NULL;
  }
  return res;
}

void __devm_release_region(struct device *dev, struct resource *parent,
                           resource_size_t start, resource_size_t n) {
  (void)dev;
  __release_region(parent, start, n);
}

/* ── wrapper construction ───────────────────────────────────────────────── */

static void kpi_pci_dev_release(struct device *dev) {
  /* Wrappers are permanent; release only exists so device_unregister() would
   * have something to call if the lifetime model changes. */
  (void)dev;
}

static void kpi_pci_root_init(void) {
  memset(&kpi_root_bus, 0, sizeof(kpi_root_bus));
  kpi_root_bus.number = 0;
  INIT_LIST_HEAD(&kpi_root_bus.devices);
  INIT_LIST_HEAD(&kpi_root_bus.resources);

  device_initialize(&kpi_root_bus.dev);
  dev_set_name(&kpi_root_bus.dev, "pci0000:00");
  kpi_root_bus.dev.bus = &pci_bus_type;
  kpi_root_bus.dev.release = kpi_pci_dev_release;
  /* Not device_add()ed (the native sysfs tree owns /sys/pci), but the kobject
   * must be named/non-NULL for child parents and sysfs links. */
  kpi_root_bus.dev.kobj.name = kpi_root_bus.dev.init_name;
}

static struct pci_dev *kpi_pci_dev_new(void *handle) {
  struct linuxkpi_pci_info info;
  struct pci_dev *pdev;
  int i;

  linuxkpi_pci_native_snapshot(handle, &info);
  pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
  if (!pdev)
    return NULL;

  pdev->sysdata = handle;
  pdev->bus = &kpi_root_bus;
  pdev->devfn = (unsigned int)((info.slot << 3) | (info.func & 0x7));
  pdev->vendor = info.vendor_id;
  pdev->device = info.device_id;
  pdev->subsystem_vendor = info.subsystem_vendor;
  pdev->subsystem_device = info.subsystem_device;
  pdev->class = ((unsigned int)info.class_code << 16) |
                ((unsigned int)info.subclass << 8) | info.prog_if;
  pdev->revision = info.revision;
  pdev->hdr_type = (u8)(info.header_type & 0x7F);
  pdev->multifunction = (info.header_type & 0x80) ? 1 : 0;
  pdev->irq = info.irq_line;
  pdev->pin = info.irq_pin;
  pdev->current_state = PCI_D0;
  pdev->error_state = pci_channel_io_normal;
  atomic_set(&pdev->enable_cnt, 0);
  spin_lock_init(&pdev->pcie_cap_lock);

  pdev->dev.bus = &pci_bus_type;
  pdev->dev.parent = &kpi_root_bus.dev;
  pdev->dev.release = kpi_pci_dev_release;
  pdev->dev.dma_mask = 0xFFFFFFFFULL;
  pdev->dev.coherent_dma_mask = 0xFFFFFFFFULL;
  dev_set_name(&pdev->dev, "0000:%02x:%02x.%x", info.bus, info.slot,
               info.func);
  device_register(&pdev->dev);

  /* Decode BARs into struct resource; 64-bit BARs consume two raw dwords. */
  for (i = 0; i < 6; i++) {
    struct resource *res = &pdev->resource[i];
    u32 raw = info.bar_raw[i];

    if (!raw || raw == 0xFFFFFFFFu)
      continue;

    if (raw & 1u) {
      res->flags = IORESOURCE_IO;
    } else {
      res->flags = IORESOURCE_MEM;
      if (((raw >> 1) & 3u) == 2u)
        res->flags |= IORESOURCE_MEM_64;
      if (raw & 8u)
        res->flags |= IORESOURCE_PREFETCH;
    }
    res->start = (resource_size_t)info.bar_phys[i];
    res->end = info.bar_size[i] ? res->start + info.bar_size[i] - 1
                                : res->start;
    if (((raw >> 1) & 3u) == 2u)
      i++; /* skip the high dword of a 64-bit BAR */
  }

  pdev->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
  pdev->msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
  pdev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
  if (pdev->pcie_cap) {
    u16 flags = 0;

    pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &flags);
    pdev->pcie_flags_reg = flags;
  }
  return pdev;
}

void linuxkpi_pci_scan(void) {
  int native_count, i;

  if (kpi_pci_scanned)
    return;
  kpi_pci_scanned = true;

  kpi_pci_root_init();

  native_count = linuxkpi_pci_native_count();
  if (native_count <= 0) {
    klog_puts("[KERNEL] LinuxKPI: PCI scan found no devices\n");
    return;
  }

  kpi_devs = kzalloc(sizeof(*kpi_devs) * (size_t)native_count, GFP_KERNEL);
  if (!kpi_devs) {
    klog_puts("[WARN] LinuxKPI: PCI wrapper array allocation failed\n");
    return;
  }

  for (i = 0; i < native_count; i++) {
    void *handle = linuxkpi_pci_native_get(i);
    struct pci_dev *pdev;

    if (!handle)
      continue;
    pdev = kpi_pci_dev_new(handle);
    if (pdev)
      kpi_devs[kpi_dev_count++] = pdev;
  }

  klogf("[KERNEL] LinuxKPI: PCI %d wrapper(s) over %d native device(s)\n",
        kpi_dev_count, native_count);
}

/* ── driver registry ────────────────────────────────────────────────────── */

static bool kpi_pci_match_one(const struct pci_device_id *id,
                              const struct pci_dev *pdev) {
  if (id->vendor != PCI_ANY_ID && id->vendor != pdev->vendor)
    return false;
  if (id->device != PCI_ANY_ID && id->device != pdev->device)
    return false;
  if (id->subvendor != PCI_ANY_ID &&
      id->subvendor != pdev->subsystem_vendor)
    return false;
  if (id->subdevice != PCI_ANY_ID &&
      id->subdevice != pdev->subsystem_device)
    return false;
  if ((pdev->class ^ id->class) & id->class_mask)
    return false;
  return true;
}

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *dev) {
  if (!ids || !dev)
    return NULL;

  while (ids->vendor || ids->device || ids->subvendor || ids->subdevice ||
         ids->class_mask) {
    if (kpi_pci_match_one(ids, dev))
      return ids;
    ids++;
  }
  return NULL;
}

static void kpi_pci_probe_device(struct pci_driver *drv,
                                 struct pci_dev *pdev) {
  const struct pci_device_id *id;
  int ret;

  if (!pdev || pdev->driver)
    return;
  id = pci_match_id(drv->id_table, pdev);
  if (!id)
    return;

  pdev->driver = drv;
  pdev->dev.driver = &drv->driver;
  if (!drv->probe)
    return;

  ret = drv->probe(pdev, id);
  if (ret) {
    klogf("[KERNEL] LinuxKPI: pci %s probe %s failed: %d\n", drv->name,
          pci_name(pdev), ret);
    pdev->driver = NULL;
    pdev->dev.driver = NULL;
  }
}

int __pci_register_driver(struct pci_driver *drv, struct module *owner,
                          const char *mod_name) {
  struct kpi_pci_driver *node;
  int i;

  (void)owner;
  (void)mod_name;
  if (!drv || !drv->name)
    return -EINVAL;

  if (!drv->driver.name) {
    drv->driver.name = drv->name;
    drv->driver.bus = &pci_bus_type;
  }

  node = kzalloc(sizeof(*node), GFP_KERNEL);
  if (!node)
    return -ENOMEM;
  node->drv = drv;
  list_add_tail(&node->node, &kpi_pci_drivers);

  for (i = 0; i < kpi_dev_count; i++)
    kpi_pci_probe_device(drv, kpi_devs[i]);
  return 0;
}

void pci_unregister_driver(struct pci_driver *drv) {
  struct kpi_pci_driver *node, *tmp;
  int i;

  if (!drv)
    return;

  list_for_each_entry_safe(node, tmp, &kpi_pci_drivers, node) {
    if (node->drv != drv)
      continue;

    for (i = 0; i < kpi_dev_count; i++) {
      struct pci_dev *pdev = kpi_devs[i];

      if (!pdev || pdev->driver != drv)
        continue;
      if (drv->remove)
        drv->remove(pdev);
      pdev->driver = NULL;
      pdev->dev.driver = NULL;
    }

    list_del(&node->node);
    kfree(node);
  }
}

struct pci_driver *pci_dev_driver(const struct pci_dev *dev) {
  return dev ? dev->driver : NULL;
}

/* ── configuration space / capabilities ─────────────────────────────────── */

static bool kpi_pci_cfg_ok(const struct pci_dev *dev, int where, int width) {
  return dev && dev->sysdata && where >= 0 && where + width <= 4096;
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val) {
  if (!val || !kpi_pci_cfg_ok(dev, where, 1))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  *val = linuxkpi_pci_config_read8(dev->sysdata, (unsigned short)where);
  return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val) {
  if (!val || !kpi_pci_cfg_ok(dev, where, 2))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  *val = linuxkpi_pci_config_read16(dev->sysdata, (unsigned short)where);
  return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val) {
  if (!val || !kpi_pci_cfg_ok(dev, where, 4))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  *val = linuxkpi_pci_config_read32(dev->sysdata, (unsigned short)where);
  return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val) {
  if (!kpi_pci_cfg_ok(dev, where, 1))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  linuxkpi_pci_config_write8(dev->sysdata, (unsigned short)where, val);
  return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_word(const struct pci_dev *dev, int where, u16 val) {
  if (!kpi_pci_cfg_ok(dev, where, 2))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  linuxkpi_pci_config_write16(dev->sysdata, (unsigned short)where, val);
  return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) {
  if (!kpi_pci_cfg_ok(dev, where, 4))
    return PCIBIOS_BAD_REGISTER_NUMBER;
  linuxkpi_pci_config_write32(dev->sysdata, (unsigned short)where, val);
  return PCIBIOS_SUCCESSFUL;
}

u8 pci_find_capability(struct pci_dev *dev, int cap) {
  if (!dev || !dev->sysdata)
    return 0;
  return linuxkpi_pci_find_capability(dev->sysdata, (u8)cap);
}

u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap) {
  int ttl = 48;
  u16 ent;

  if (!dev)
    return 0;
  while (ttl--) {
    if (pci_read_config_word(dev, pos, &ent) != PCIBIOS_SUCCESSFUL)
      break;
    if ((ent & 0xFF) == cap)
      return pos;
    pos = (u8)((ent >> 8) & 0xFF);
    if (!pos)
      break;
  }
  return 0;
}

u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 pos, int cap) {
  int ttl = 48;
  u32 header;

  if (!dev || !dev->sysdata)
    return 0;
  if (pos < 0x100)
    pos = 0x100;

  while (ttl-- && pos >= 0x100) {
    if (pci_read_config_dword(dev, pos, &header) != PCIBIOS_SUCCESSFUL)
      return 0;
    if (header == 0 || header == 0xFFFFFFFFu)
      return 0;
    if ((header & 0xFFFF) == (u32)cap)
      return pos;
    pos = (u16)((header >> 16) & 0xFFF);
  }
  return 0;
}

u16 pci_find_ext_capability(struct pci_dev *dev, int cap) {
  return pci_find_next_ext_capability(dev, 0, cap);
}

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val) {
  if (!val)
    return -EINVAL;
  *val = 0;
  if (!dev || !dev->pcie_cap || pos < 0)
    return -EINVAL;
  if (pci_read_config_word(dev, dev->pcie_cap + pos, val) !=
      PCIBIOS_SUCCESSFUL)
    return -EINVAL;
  return 0;
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val) {
  if (!val)
    return -EINVAL;
  *val = 0;
  if (!dev || !dev->pcie_cap || pos < 0)
    return -EINVAL;
  if (pci_read_config_dword(dev, dev->pcie_cap + pos, val) !=
      PCIBIOS_SUCCESSFUL)
    return -EINVAL;
  return 0;
}

int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val) {
  if (!dev || !dev->pcie_cap || pos < 0)
    return -EINVAL;
  if (pci_write_config_word(dev, dev->pcie_cap + pos, val) !=
      PCIBIOS_SUCCESSFUL)
    return -EINVAL;
  return 0;
}

int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val) {
  if (!dev || !dev->pcie_cap || pos < 0)
    return -EINVAL;
  if (pci_write_config_dword(dev, dev->pcie_cap + pos, val) !=
      PCIBIOS_SUCCESSFUL)
    return -EINVAL;
  return 0;
}

int pcie_capability_clear_and_set_word_unlocked(struct pci_dev *dev, int pos,
                                                u16 clear, u16 set) {
  u16 val;
  int ret;

  ret = pcie_capability_read_word(dev, pos, &val);
  if (ret)
    return ret;
  val &= ~clear;
  val |= set;
  return pcie_capability_write_word(dev, pos, val);
}

int pcie_capability_clear_and_set_word_locked(struct pci_dev *dev, int pos,
                                              u16 clear, u16 set) {
  unsigned long flags;
  int ret;

  if (!dev)
    return -EINVAL;
  spin_lock_irqsave(&dev->pcie_cap_lock, flags);
  ret = pcie_capability_clear_and_set_word_unlocked(dev, pos, clear, set);
  spin_unlock_irqrestore(&dev->pcie_cap_lock, flags);
  return ret;
}

int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos,
                                        u32 clear, u32 set) {
  u32 val;
  int ret;

  ret = pcie_capability_read_dword(dev, pos, &val);
  if (ret)
    return ret;
  val &= ~clear;
  val |= set;
  return pcie_capability_write_dword(dev, pos, val);
}

/* ── enable / bus master ────────────────────────────────────────────────── */

static bool kpi_pci_has_flag(const struct pci_dev *dev, unsigned long flag) {
  int i;

  for (i = 0; i < PCI_STD_NUM_BARS; i++) {
    if (dev->resource[i].flags & flag)
      return true;
  }
  return false;
}

static int kpi_pci_do_enable(struct pci_dev *dev, bool want_io, bool want_mem) {
  u16 set = 0, clear = 0;

  if (!dev)
    return -EINVAL;

  if (want_io)
    set |= PCI_COMMAND_IO;
  else
    clear |= PCI_COMMAND_IO;
  if (want_mem)
    set |= PCI_COMMAND_MEMORY;
  else
    clear |= PCI_COMMAND_MEMORY;

  if (atomic_read(&dev->enable_cnt) == 0)
    linuxkpi_pci_command_update(dev->sysdata, set, clear);

  atomic_inc(&dev->enable_cnt);
  dev->current_state = PCI_D0;
  return 0;
}

int pci_enable_device(struct pci_dev *dev) {
  if (!dev)
    return -EINVAL;
  return kpi_pci_do_enable(dev, kpi_pci_has_flag(dev, IORESOURCE_IO),
                           kpi_pci_has_flag(dev, IORESOURCE_MEM));
}

int pci_enable_device_io(struct pci_dev *dev) {
  return kpi_pci_do_enable(dev, true, false);
}

int pci_enable_device_mem(struct pci_dev *dev) {
  return kpi_pci_do_enable(dev, false, true);
}

int pci_reenable_device(struct pci_dev *dev) {
  if (!dev)
    return -EINVAL;
  pci_disable_device(dev);
  return pci_enable_device(dev);
}

void pci_disable_device(struct pci_dev *dev) {
  if (!dev)
    return;
  if (atomic_dec_and_test(&dev->enable_cnt)) {
    linuxkpi_pci_set_bus_mastering(dev->sysdata, 0);
    linuxkpi_pci_command_update(dev->sysdata, 0,
                                PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
  }
}

static void kpi_pcim_disable_action(void *data) {
  pci_disable_device((struct pci_dev *)data);
}

int pcim_enable_device(struct pci_dev *pdev) {
  int ret;

  if (!pdev)
    return -EINVAL;
  if (pdev->is_managed)
    return 0;

  ret = devm_add_action(&pdev->dev, kpi_pcim_disable_action, pdev);
  if (ret)
    return ret;
  ret = pci_enable_device(pdev);
  if (ret)
    return ret;
  pdev->is_managed = 1;
  return 0;
}

void pcim_pin_device(struct pci_dev *pdev) { (void)pdev; }

void pci_set_master(struct pci_dev *dev) {
  if (!dev)
    return;
  linuxkpi_pci_command_update(dev->sysdata, PCI_COMMAND_MASTER, 0);
  dev->is_busmaster = 1;
}

void pci_clear_master(struct pci_dev *dev) {
  if (!dev)
    return;
  linuxkpi_pci_command_update(dev->sysdata, 0, PCI_COMMAND_MASTER);
  dev->is_busmaster = 0;
}

/* ── region requests ────────────────────────────────────────────────────── */

int pci_request_region(struct pci_dev *pdev, int bar, const char *res_name) {
  struct resource *res;
  struct resource *parent, *child;
  resource_size_t size;

  if (!pdev || bar < 0 || bar >= PCI_STD_NUM_BARS)
    return -EINVAL;
  res = &pdev->resource[bar];
  if (!res->flags)
    return -EINVAL;
  if (res->flags & IORESOURCE_BUSY)
    return -EBUSY;

  size = resource_size(res);
  parent = kpi_pci_region_parent(res->flags);
  child = __request_region(parent, res->start, size, res_name, 0);
  if (!child)
    return -EBUSY;
  res->flags |= IORESOURCE_BUSY;

  if (pdev->is_managed) {
    if (devm_add_action(&pdev->dev, kpi_devm_release_region_action, child)) {
      __release_region(parent, res->start, size);
      res->flags &= ~IORESOURCE_BUSY;
      return -ENOMEM;
    }
  }
  return 0;
}

void pci_release_region(struct pci_dev *pdev, int bar) {
  struct resource *res;

  if (!pdev || bar < 0 || bar >= PCI_STD_NUM_BARS)
    return;
  res = &pdev->resource[bar];
  if (!(res->flags & IORESOURCE_BUSY))
    return;
  __release_region(kpi_pci_region_parent(res->flags), res->start,
                   resource_size(res));
  res->flags &= ~IORESOURCE_BUSY;
}

static int kpi_pci_request_selected(struct pci_dev *pdev, int bars,
                                    const char *name) {
  int i, ret;

  for (i = 0; i < PCI_STD_NUM_BARS; i++) {
    if (!(bars & (1 << i)))
      continue;
    ret = pci_request_region(pdev, i, name);
    if (ret) {
      for (i--; i >= 0; i--) {
        if (bars & (1 << i))
          pci_release_region(pdev, i);
      }
      return ret;
    }
  }
  return 0;
}

static void kpi_pci_release_selected(struct pci_dev *pdev, int bars) {
  int i;

  for (i = 0; i < PCI_STD_NUM_BARS; i++) {
    if (bars & (1 << i))
      pci_release_region(pdev, i);
  }
}

static int kpi_pci_all_bars(const struct pci_dev *pdev) {
  int bars = 0, i;

  for (i = 0; i < PCI_STD_NUM_BARS; i++) {
    if (pdev->resource[i].flags)
      bars |= 1 << i;
  }
  return bars;
}

int pci_select_bars(struct pci_dev *dev, unsigned long flags) {
  int bars = 0, i;

  if (!dev)
    return 0;
  for (i = 0; i < PCI_STD_NUM_BARS; i++) {
    if (pci_resource_flags(dev, i) & flags)
      bars |= 1 << i;
  }
  return bars;
}

int pci_request_regions(struct pci_dev *pdev, const char *res_name) {
  if (!pdev)
    return -EINVAL;
  return kpi_pci_request_selected(pdev, kpi_pci_all_bars(pdev), res_name);
}

int pci_request_regions_exclusive(struct pci_dev *pdev, const char *res_name) {
  return pci_request_regions(pdev, res_name);
}

void pci_release_regions(struct pci_dev *pdev) {
  if (!pdev)
    return;
  kpi_pci_release_selected(pdev, kpi_pci_all_bars(pdev));
}

int pci_request_selected_regions(struct pci_dev *pdev, int bars,
                                 const char *res_name) {
  return kpi_pci_request_selected(pdev, bars, res_name);
}

int pci_request_selected_regions_exclusive(struct pci_dev *pdev, int bars,
                                           const char *res_name) {
  return kpi_pci_request_selected(pdev, bars, res_name);
}

void pci_release_selected_regions(struct pci_dev *pdev, int bars) {
  if (!pdev)
    return;
  kpi_pci_release_selected(pdev, bars);
}

/* ── BAR mapping ────────────────────────────────────────────────────────── */

static void __iomem *kpi_pci_iomap(struct pci_dev *dev, int bar,
                                   unsigned long offset, unsigned long maxlen,
                                   bool wc) {
  resource_size_t start, len;
  unsigned long flags;

  if (!dev || bar < 0 || bar >= PCI_STD_NUM_BARS)
    return NULL;
  if (!(dev->resource[bar].flags))
    return NULL;

  len = pci_resource_len(dev, bar);
  flags = pci_resource_flags(dev, bar);
  if (!len || offset > len)
    return NULL;
  if (!maxlen || maxlen > len - offset)
    maxlen = (unsigned long)(len - offset);

  start = pci_resource_start(dev, bar) + offset;
  if (flags & IORESOURCE_IO)
    return (void __iomem *)(uintptr_t)start; /* raw port address */
  if (flags & IORESOURCE_MEM)
    return wc ? ioremap_wc(start, maxlen) : ioremap(start, maxlen);
  return NULL;
}

void __iomem *pci_iomap_range(struct pci_dev *dev, int bar,
                              unsigned long offset, unsigned long maxlen) {
  return kpi_pci_iomap(dev, bar, offset, maxlen, false);
}

void __iomem *pci_iomap_wc_range(struct pci_dev *dev, int bar,
                                 unsigned long offset, unsigned long maxlen) {
  return kpi_pci_iomap(dev, bar, offset, maxlen, true);
}

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long max) {
  return kpi_pci_iomap(dev, bar, 0, max, false);
}

void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long max) {
  return kpi_pci_iomap(dev, bar, 0, max, true);
}

void pci_iounmap(struct pci_dev *dev, void __iomem *addr) {
  (void)dev;
  /* I/O BAR mappings are raw port addresses, not VMAP addresses. */
  if (addr && (uintptr_t)addr > 0xFFFF)
    iounmap(addr);
}

/* ── device lookup ──────────────────────────────────────────────────────── */

static int kpi_pci_index_of(const struct pci_dev *dev) {
  int i;

  for (i = 0; i < kpi_dev_count; i++) {
    if (kpi_devs[i] == dev)
      return i;
  }
  return -1;
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
                               struct pci_dev *from) {
  int start = 0, i;

  if (from) {
    start = kpi_pci_index_of(from);
    if (start < 0)
      return NULL;
    start++;
  }

  for (i = start; i < kpi_dev_count; i++) {
    struct pci_dev *pdev = kpi_devs[i];

    if (vendor != PCI_ANY_ID && pdev->vendor != vendor)
      continue;
    if (device != PCI_ANY_ID && pdev->device != device)
      continue;
    return pdev;
  }
  return NULL;
}

struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device,
                               unsigned int ss_vendor, unsigned int ss_device,
                               struct pci_dev *from) {
  int start = 0, i;

  if (from) {
    start = kpi_pci_index_of(from);
    if (start < 0)
      return NULL;
    start++;
  }

  for (i = start; i < kpi_dev_count; i++) {
    struct pci_dev *pdev = kpi_devs[i];

    if (vendor != PCI_ANY_ID && pdev->vendor != vendor)
      continue;
    if (device != PCI_ANY_ID && pdev->device != device)
      continue;
    if (ss_vendor != PCI_ANY_ID && pdev->subsystem_vendor != ss_vendor)
      continue;
    if (ss_device != PCI_ANY_ID && pdev->subsystem_device != ss_device)
      continue;
    return pdev;
  }
  return NULL;
}

struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from) {
  int start = 0, i;

  if (from) {
    start = kpi_pci_index_of(from);
    if (start < 0)
      return NULL;
    start++;
  }

  for (i = start; i < kpi_dev_count; i++) {
    if ((kpi_devs[i]->class & 0xFFFFFF) == (class & 0xFFFFFF))
      return kpi_devs[i];
  }
  return NULL;
}

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
                                            unsigned int devfn) {
  int i;

  if (domain != 0)
    return NULL;
  for (i = 0; i < kpi_dev_count; i++) {
    struct pci_dev *pdev = kpi_devs[i];

    if (pdev->bus && pdev->bus->number == bus && pdev->devfn == devfn)
      return pdev;
  }
  return NULL;
}

struct pci_dev *pci_dev_get(struct pci_dev *dev) { return dev; }

void pci_dev_put(struct pci_dev *dev) { (void)dev; }

int pci_dev_present(const struct pci_device_id *ids) {
  int i;

  for (i = 0; i < kpi_dev_count; i++) {
    if (pci_match_id(ids, kpi_devs[i]))
      return 1;
  }
  return 0;
}

/* ── ROM (basic; size probing is a later phase) ─────────────────────────── */

int pci_enable_rom(struct pci_dev *pdev) {
  u32 rom_addr = 0;

  if (!pdev)
    return -EINVAL;
  if (pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &rom_addr) !=
      PCIBIOS_SUCCESSFUL)
    return -EIO;
  if (!rom_addr || rom_addr == 0xFFFFFFFFu)
    return -EIO;
  if (!(rom_addr & PCI_ROM_ADDRESS_ENABLE))
    pci_write_config_dword(pdev, PCI_ROM_ADDRESS,
                           rom_addr | PCI_ROM_ADDRESS_ENABLE);
  return 0;
}

void pci_disable_rom(struct pci_dev *pdev) {
  u32 rom_addr = 0;

  if (!pdev)
    return;
  if (pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &rom_addr) !=
      PCIBIOS_SUCCESSFUL)
    return;
  if (rom_addr & PCI_ROM_ADDRESS_ENABLE)
    pci_write_config_dword(pdev, PCI_ROM_ADDRESS,
                           rom_addr & ~PCI_ROM_ADDRESS_ENABLE);
}

void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) {
  struct resource *res;

  if (!pdev || !size)
    return NULL;
  *size = 0;

  res = &pdev->resource[PCI_ROM_RESOURCE];
  if (!(res->flags & IORESOURCE_MEM) || !resource_size(res))
    return NULL; /* no decoded ROM resource yet */

  if (pci_enable_rom(pdev))
    return NULL;
  *size = resource_size(res);
  return ioremap(res->start, *size);
}

void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) {
  if (rom)
    iounmap(rom);
  if (pdev)
    pci_disable_rom(pdev);
}
