/* Failed NVKM construction ownership at the native PCI probe boundary.
 * The imported sources stay pristine; engine/device/pci.c is compiled under
 * the original name and callers enter through this LinuxKPI adapter. */
#include <linux/version.h>
#ifdef AVORYOS_NOUVEAU_COMPAT
#include <core/pci.h>
#include <core/intr.h>
#include <linux/errno.h>
#include <linux/slab.h>

extern int linuxkpi_original_nvkm_device_pci_new(struct pci_dev *, const char *,
                                               const char *, struct nvkm_device **);

static u64 kpi_nvkm_handle(struct pci_dev *pdev) {
  return (u64)pci_domain_nr(pdev->bus) << 32 |
         (u64)pdev->bus->number << 16 |
         (u64)PCI_SLOT(pdev->devfn) << 8 | PCI_FUNC(pdev->devfn);
}

bool linuxkpi_nouveau_device_registered(struct pci_dev *pdev) {
  return nvkm_device_find(kpi_nvkm_handle(pdev)) != NULL;
}

int nvkm_device_pci_new(struct pci_dev *pdev, const char *cfg, const char *dbg,
                         struct nvkm_device **out) {
  int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  if (!out)
    return -EINVAL;
  *out = NULL;
#endif
  ret = linuxkpi_original_nvkm_device_pci_new(pdev, cfg, dbg, out);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  if (ret && *out) {
    struct nvkm_device *device = *out;
    /* The stock constructor publishes its partial device before subdevice
     * creation. Its caller returns immediately on failure, so reclaim that
     * object here with the real NVKM destructor, including PCI enable/IRQ
     * ownership. Never remove an older device on duplicate-handle failure. */
    if (nvkm_device_find(kpi_nvkm_handle(pdev)) == device) {
      if (!device->intr.intr.next) {
        /* Mapping/chipset failures precede the stock interrupt constructor. */
        mutex_init(&device->mutex);
        nvkm_intr_ctor(device);
      }
      nvkm_device_del(out);
    } else {
      kfree(container_of(device, struct nvkm_device_pci, device));
      *out = NULL;
      pci_disable_device(pdev);
    }
  }
#endif
  return ret;
}
#endif
