/* Minimal Linux PCI API for LinuxKPI (Phase 4 C4; extended by Phase 5 C1).
 *
 * One `struct pci_dev` wrapper is built per device the native PCI enumerator
 * found (kernel/src/drivers/pci); the bridge in linuxkpi/native_pci.h hides
 * the native types.  Implemented here: config access, capability lookup,
 * resource decoding from BARs, region bookkeeping, BAR mapping, device
 * enable/master, a direct probe/remove driver registry, config state
 * save/restore, ROM BAR sizing + mapping, and the PCIe link helpers.
 *
 * Deliberately not modeled yet (recorded in docs/linuxkpi-gaps.md):
 *   - pci_dev reference counting: `pci_get_device()`-style wrappers live for
 *     the whole kernel lifetime; pci_dev_get/put are no-ops.
 *   - the generic driver core: pci_register_driver() probes directly through
 *     the id table (no driver_register(), no deferred probe, no dynids).
 *   - real I/O-port address translation: I/O BARs expose their raw port
 *     address and request_region() keeps only a conflict registry.
 *   - saved state beyond config space 0x00..0x3F: upstream also saves
 *     PCIe/PCIx/LTR/DPC/AER/PTM/VC capability state; ROM BAR assignment is a
 *     shim-local low-MMIO scanner instead of pci_assign_resource()/bridge
 *     windows, and there is no IORESOURCE_ROM_SHADOW handling.
 *   - MSI/MSI-X allocation (arrives with Phase 5 C2).
 *
 * The root bus/device scaffolding exists so `pci_dev.bus`, `dev.archdata`,
 * and the `kobj.parent` chain DRM's sysfs links expect are all non-NULL. */

#include <linux/delay.h>

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_irq.h>
#include <linuxkpi/native_pci.h>
#include <linuxkpi/native_sysfs.h>

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
  /* Attach the wrapper to the native PCI sysfs directory so driver dev_groups
   * (and sysfs_create_file on this device) materialize under
   * /sys/bus/pci/devices/<bdf>. */
  {
    char path[64];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s",
             pci_name(pdev));
    pdev->dev.kobj.sd = (struct kernfs_node *)asc_sysfs_dir_by_path(path);
  }
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
    return;
  }

  /* Driver-level sysfs groups appear on a successful bind and disappear on
   * unbind, like the upstream driver core. */
  device_add_groups(&pdev->dev, drv->driver.dev_groups);
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
      device_remove_groups(&pdev->dev, drv->driver.dev_groups);
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

/* ── power management (inert bookkeeping, Phase 5 C5) ───────────────────── */

/* No PM core exists (CONFIG_PM unset): pci_set_power_state() records the
 * requested state so drivers see a consistent current_state, but performs no
 * hardware transition.  pci_choose_state() answers D3hot for any request and
 * pci_wake_from_d3() accepts the enable flag without touching the PMCSR. */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state) {
  if (!dev)
    return -EINVAL;
  dev->current_state = state;
  return 0;
}

pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state) {
  (void)dev;
  (void)state;
  return PCI_D3hot;
}

int pci_wake_from_d3(struct pci_dev *dev, bool enable) {
  (void)enable;
  if (!dev)
    return -EINVAL;
  return 0;
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

/* ── MSI/MSI-X vector allocation (Phase 5 C2) ───────────────────────────── */

#define KPI_PCI_IRQ_MAX_VECTORS 32

struct kpi_pci_irq_vec {
  void *native;
  unsigned int index;
};

struct kpi_pci_irq_state {
  bool used;
  void *native;
  unsigned int count;
  unsigned int irq[KPI_PCI_IRQ_MAX_VECTORS];
  struct kpi_pci_irq_vec vecs[KPI_PCI_IRQ_MAX_VECTORS];
};

/* One state slot per wrapper; wrappers live for the kernel's lifetime. */
static struct kpi_pci_irq_state kpi_pci_irqs[256];

static void kpi_pci_irq_mask_cb(void *data, int masked) {
  struct kpi_pci_irq_vec *v = data;

  linuxkpi_native_pci_irq_mask(v->native, v->index, masked);
}

int pci_msi_vec_count(struct pci_dev *dev) {
  if (!dev || !dev->msi_cap)
    return 0;
  return 1; /* the native MSI path supports one vector */
}

int pci_msix_vec_count(struct pci_dev *dev) {
  u16 flags = 0;

  if (!dev || !dev->msix_cap)
    return 0;
  if (pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &flags) !=
      PCIBIOS_SUCCESSFUL)
    return 0;
  return (int)(flags & PCI_MSIX_FLAGS_QSIZE) + 1;
}

static unsigned int kpi_pci_flags_to_modes(unsigned int flags) {
  unsigned int modes = 0;

  if (flags & PCI_IRQ_LEGACY)
    modes |= LINUXKPI_IRQ_MODE_INTX;
  if (flags & PCI_IRQ_MSI)
    modes |= LINUXKPI_IRQ_MODE_MSI;
  if (flags & PCI_IRQ_MSIX)
    modes |= LINUXKPI_IRQ_MODE_MSIX;
  return modes;
}

int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
                                   unsigned int max_vecs, unsigned int flags,
                                   struct irq_affinity *affd) {
  struct kpi_pci_irq_state *st;
  unsigned int modes, i;
  int idx, count;

  (void)affd;
  if (!dev || min_vecs == 0 || max_vecs < min_vecs)
    return -EINVAL;
  idx = kpi_pci_index_of(dev);
  if (idx < 0 ||
      idx >= (int)(sizeof(kpi_pci_irqs) / sizeof(kpi_pci_irqs[0])))
    return -EINVAL;
  st = &kpi_pci_irqs[idx];
  if (st->used)
    return -EBUSY;

  modes = kpi_pci_flags_to_modes(flags);
  if (!modes)
    return -EINVAL;
  count = (int)min_vecs;
  if (count > KPI_PCI_IRQ_MAX_VECTORS)
    count = KPI_PCI_IRQ_MAX_VECTORS;

  st->native =
      linuxkpi_native_pci_irq_request(dev->sysdata, (unsigned int)count, modes);
  if (!st->native)
    return -ENOSPC;

  st->used = true;
  st->count = (unsigned int)count;
  for (i = 0; i < (unsigned int)count; i++) {
    int irq = linuxkpi_native_pci_irq_vector(st->native, i);

    if (irq < 0) {
      for (unsigned int j = 0; j < i; j++)
        linuxkpi_irq_unregister_mask(st->irq[j]);
      linuxkpi_native_pci_irq_release(st->native);
      memset(st, 0, sizeof(*st));
      return -ENOSPC;
    }
    st->irq[i] = (unsigned int)irq;
    st->vecs[i].native = st->native;
    st->vecs[i].index = i;
    linuxkpi_irq_register_mask(st->irq[i], kpi_pci_irq_mask_cb, &st->vecs[i]);
  }
  return count;
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
                          unsigned int max_vecs, unsigned int flags) {
  return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs, flags, NULL);
}

int pci_irq_vector(struct pci_dev *dev, unsigned int nr) {
  struct kpi_pci_irq_state *st;
  int idx;

  if (!dev)
    return -EINVAL;
  idx = kpi_pci_index_of(dev);
  if (idx < 0 ||
      idx >= (int)(sizeof(kpi_pci_irqs) / sizeof(kpi_pci_irqs[0])))
    return -EINVAL;
  st = &kpi_pci_irqs[idx];
  if (!st->used || nr >= st->count)
    return -EINVAL;
  return (int)st->irq[nr];
}

const struct cpumask *pci_irq_get_affinity(struct pci_dev *pdev, int vec) {
  (void)pdev;
  (void)vec;
  return NULL;
}

void pci_free_irq_vectors(struct pci_dev *dev) {
  struct kpi_pci_irq_state *st;
  unsigned int i;
  int idx;

  if (!dev)
    return;
  idx = kpi_pci_index_of(dev);
  if (idx < 0 ||
      idx >= (int)(sizeof(kpi_pci_irqs) / sizeof(kpi_pci_irqs[0])))
    return;
  st = &kpi_pci_irqs[idx];
  if (!st->used)
    return;
  for (i = 0; i < st->count; i++)
    linuxkpi_irq_unregister_mask(st->irq[i]);
  linuxkpi_native_pci_irq_release(st->native);
  memset(st, 0, sizeof(*st));
}

/* ── configuration state save/restore (Phase 5 C1) ──────────────────────── */

/* Standard config space only (16 dwords); upstream also saves PCIe/PCIx/
 * LTR/DPC/AER/PTM/VC capability state, which this shim does not model. */
struct pci_saved_state {
  u32 config_space[16];
};

int pci_save_state(struct pci_dev *dev) {
  int i;

  if (!dev)
    return -EINVAL;
  for (i = 0; i < 16; i++)
    pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]);
  dev->state_saved = true;
  return 0;
}

void pci_restore_state(struct pci_dev *dev) {
  int i;

  if (!dev || !dev->state_saved)
    return;
  for (i = 0; i < 16; i++)
    pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]);
  dev->state_saved = false;
}

struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev) {
  struct pci_saved_state *state;

  if (!dev || !dev->state_saved)
    return NULL;
  state = kzalloc(sizeof(*state), GFP_KERNEL);
  if (!state)
    return NULL;
  memcpy(state->config_space, dev->saved_config_space,
         sizeof(state->config_space));
  return state;
}

int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state) {
  if (!dev)
    return -EINVAL;
  dev->state_saved = false;
  if (!state)
    return 0;
  memcpy(dev->saved_config_space, state->config_space,
         sizeof(state->config_space));
  dev->state_saved = true;
  return 0;
}

int pci_load_and_free_saved_state(struct pci_dev *dev,
                                  struct pci_saved_state **state) {
  int ret;

  if (!state)
    return -EINVAL;
  ret = pci_load_saved_state(dev, *state);
  kfree(*state);
  *state = NULL;
  return ret;
}

/* ── PCIe link helpers (Phase 5 C1) ─────────────────────────────────────── */

enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev) {
  u32 lnkcap2 = 0, lnkcap = 0;

  if (!dev || !pci_is_pcie(dev))
    return PCI_SPEED_UNKNOWN;

  /* Link Capabilities 2 (PCIe r3.0+) reports the full supported-speed
   * vector; fall back to the 2.5/5.0 GT/s field in Link Capabilities. */
  if (pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2) == 0 &&
      lnkcap2) {
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_64_0GB)
      return PCIE_SPEED_64_0GT;
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_32_0GB)
      return PCIE_SPEED_32_0GT;
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_16_0GB)
      return PCIE_SPEED_16_0GT;
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_8_0GB)
      return PCIE_SPEED_8_0GT;
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_5_0GB)
      return PCIE_SPEED_5_0GT;
    if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_2_5GB)
      return PCIE_SPEED_2_5GT;
    return PCI_SPEED_UNKNOWN;
  }

  if (pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap) != 0 ||
      !lnkcap)
    return PCI_SPEED_UNKNOWN;

  switch (lnkcap & PCI_EXP_LNKCAP_SLS) {
  case PCI_EXP_LNKCAP_SLS_2_5GB:
    return PCIE_SPEED_2_5GT;
  case PCI_EXP_LNKCAP_SLS_5_0GB:
    return PCIE_SPEED_5_0GT;
  case PCI_EXP_LNKCAP_SLS_8_0GB:
    return PCIE_SPEED_8_0GT;
  case PCI_EXP_LNKCAP_SLS_16_0GB:
    return PCIE_SPEED_16_0GT;
  case PCI_EXP_LNKCAP_SLS_32_0GB:
    return PCIE_SPEED_32_0GT;
  case PCI_EXP_LNKCAP_SLS_64_0GB:
    return PCIE_SPEED_64_0GT;
  default:
    return PCI_SPEED_UNKNOWN;
  }
}

enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev) {
  u32 lnkcap = 0;

  if (!dev || !pci_is_pcie(dev))
    return PCIE_LNK_WIDTH_UNKNOWN;
  if (pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap) != 0 ||
      !lnkcap)
    return PCIE_LNK_WIDTH_UNKNOWN;
  lnkcap = (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4;
  return lnkcap ? (enum pcie_link_width)lnkcap : PCIE_LNK_WIDTH_UNKNOWN;
}

void pcie_print_link_status(struct pci_dev *dev) {
  u16 lnksta = 0;

  if (!dev || !pci_is_pcie(dev))
    return;
  pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
  dev_info(&dev->dev,
           "PCIe link: current x%u (speed code %u), capable x%d (speed %d)\n",
           (unsigned int)((lnksta & PCI_EXP_LNKSTA_NLW) >> 4),
           (unsigned int)(lnksta & PCI_EXP_LNKSTA_CLS),
           (int)pcie_get_width_cap(dev), (int)pcie_get_speed_cap(dev));
}

int pci_wait_for_pending_transaction(struct pci_dev *dev) {
  u16 devsta = 0;
  int i;

  if (!dev || !pci_is_pcie(dev))
    return 1; /* nothing to wait for */
  for (i = 0; i < 100; i++) {
    if (pcie_capability_read_word(dev, PCI_EXP_DEVSTA, &devsta) != 0)
      return 0;
    if (!(devsta & PCI_EXP_DEVSTA_TRPND))
      return 1;
    msleep(1);
  }
  return 0; /* bounded 100 ms wait */
}

bool pci_device_is_present(struct pci_dev *pdev) {
  u32 id = 0;

  if (!pdev || pci_dev_is_disconnected(pdev))
    return false;
  if (pci_read_config_dword(pdev, PCI_VENDOR_ID, &id) != PCIBIOS_SUCCESSFUL)
    return false;
  return (id & 0xFFFF) != 0xFFFF;
}

void pci_release_resource(struct pci_dev *dev, int resno) {
  struct resource *res;

  if (!dev || resno < 0 || resno >= DEVICE_COUNT_RESOURCE)
    return;
  res = &dev->resource[resno];
  if (res->parent) {
    __release_region(res->parent, res->start, resource_size(res));
    res->parent = NULL;
  }
  res->start = 0;
  res->end = 0;
  res->flags = 0;
}

/* ── ROM (size probe + map) ─────────────────────────────────────────────── */

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

/* Firmware (OVMF under QEMU, and the same for VFIO boots) commonly leaves
 * expansion ROM BARs unassigned (address bits all-ones).  Upstream assigns
 * one from the bridge windows with pci_assign_resource(); this shim scans the
 * low 32-bit MMIO hole downwards, avoiding every decoded BAR.  Reserved
 * CPU/IOAPIC/HPET space starts at 0xFEC00000.  Documented in gaps.md. */
#define KPI_PCI_MMIO_HOLE_START 0x80000000ULL
#define KPI_PCI_MMIO_HOLE_END 0xFEC00000ULL

static bool kpi_pci_mmio_range_free(resource_size_t start, size_t len,
                                    const struct pci_dev *self) {
  int i, j;

  for (i = 0; i < kpi_dev_count; i++) {
    const struct pci_dev *pdev = kpi_devs[i];

    for (j = 0; j < DEVICE_COUNT_RESOURCE; j++) {
      const struct resource *r = &pdev->resource[j];
      resource_size_t rstart, rend;

      if (pdev == self && j == PCI_ROM_RESOURCE)
        continue;
      if (!(r->flags & IORESOURCE_MEM) || !resource_size(r))
        continue;
      rstart = r->start;
      rend = r->end + 1;
      if (rstart < start + len && start < rend)
        return false;
    }
  }
  return true;
}

static resource_size_t kpi_pci_assign_rom_address(const struct pci_dev *self,
                                                  size_t len) {
  resource_size_t candidate;

  if (!len)
    return 0;
  candidate = ((resource_size_t)KPI_PCI_MMIO_HOLE_END - len) &
              ~((resource_size_t)len - 1);
  while (candidate >= (resource_size_t)KPI_PCI_MMIO_HOLE_START) {
    if (kpi_pci_mmio_range_free(candidate, len, self))
      return candidate;
    if (candidate < (resource_size_t)KPI_PCI_MMIO_HOLE_START + len)
      break;
    candidate -= len;
  }
  return 0;
}

void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size) {
  struct resource *res;
  u32 orig = 0, probe = 0;
  resource_size_t start;
  size_t len;
  void __iomem *rom;
  bool was_enabled;

  if (!pdev || !size)
    return NULL;
  *size = 0;

  if (pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &orig) !=
      PCIBIOS_SUCCESSFUL)
    return NULL;
  if (orig == 0xFFFFFFFFu)
    return NULL;

  /* Size the ROM BAR: save the original value, write all-ones, read the
   * size mask back, then restore.  This is the same sequence upstream's
   * pci_read_bases() uses for expansion ROMs.  The BAR may have been
   * disabled but its address bits assigned, which is the common case. */
  pci_write_config_dword(pdev, PCI_ROM_ADDRESS, ~PCI_ROM_ADDRESS_ENABLE);
  pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &probe);
  pci_write_config_dword(pdev, PCI_ROM_ADDRESS, orig);
  if (!probe || probe == 0xFFFFFFFFu)
    return NULL;

  probe &= PCI_ROM_ADDRESS_MASK;
  len = (u32)~probe + 1;
  if (!len)
    return NULL;

  /* An unassigned BAR reads back all-ones in every address bit above the
   * size mask after the sizing write (e.g. 0xFFFF0000 for a 64K ROM), and
   * OVMF leaves expansion ROM BARs in exactly that state.  Both a zero
   * address and that all-ones pattern mean "assign one". */
  start = (resource_size_t)(orig & PCI_ROM_ADDRESS_MASK);
  if (start == 0 || start == (resource_size_t)probe ||
      start == (resource_size_t)PCI_ROM_ADDRESS_MASK) {
    start = kpi_pci_assign_rom_address(pdev, len);
    if (!start)
      return NULL;
    orig = (orig & ~PCI_ROM_ADDRESS_MASK) | (u32)start;
    pci_write_config_dword(pdev, PCI_ROM_ADDRESS, orig);
  }

  res = &pdev->resource[PCI_ROM_RESOURCE];
  res->start = start;
  res->end = start + len - 1;
  res->flags = IORESOURCE_MEM;
  was_enabled = (orig & PCI_ROM_ADDRESS_ENABLE) != 0;
  if (was_enabled)
    res->flags |= IORESOURCE_ROM_ENABLE;

  if (pci_enable_rom(pdev))
    return NULL;

  rom = ioremap(start, len);
  if (!rom) {
    if (!was_enabled)
      pci_disable_rom(pdev);
    return NULL;
  }

  *size = len;
  return rom;
}

void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom) {
  struct resource *res;

  if (rom)
    iounmap(rom);
  if (!pdev)
    return;
  res = &pdev->resource[PCI_ROM_RESOURCE];
  /* IORESOURCE_ROM_ENABLE records that the ROM was already enabled before
   * pci_map_rom(); only disable when this call enabled it. */
  if (!(res->flags & IORESOURCE_ROM_ENABLE))
    pci_disable_rom(pdev);
}
