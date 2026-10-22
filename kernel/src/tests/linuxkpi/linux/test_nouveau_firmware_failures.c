/* Real imported GSP loader/destructor against intentionally incomplete files.
 * No GPU constructor, MMIO, DMA submission or firmware execution is involved. */
#include <linux/version.h>
#include <linuxkpi/log.h>
#if defined(AVORYOS_NOUVEAU_COMPAT) && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#include <core/device.h>
#include <subdev/gsp.h>
#include "priv.h"
#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linuxkpi/probe_diag.h>

static bool kpi_nouveau_failure_tests;
module_param(kpi_nouveau_failure_tests, bool, 0);
static const struct nvkm_device_chip test_chip = { .name = "AD106" };

void linuxkpi_test_nouveau_firmware_failures(void) {
  struct nvkm_device *device;
  struct nvkm_gsp *gsp;
  struct device parent = { .init_name = "nouveau-failure-test" };
  const char *versions[] = { "c4-gsp", "c4-bootloader", "c4-booter_load", "c4-booter_unload" };
  if (!kpi_nouveau_failure_tests)
    return;
  device = kzalloc(sizeof(*device), GFP_KERNEL);
  gsp = kzalloc(sizeof(*gsp), GFP_KERNEL);
  if (!device || !gsp) {
    klog_puts("[FAIL] LinuxKPI: nouveau firmware failure test allocation\n");
    goto done;
  }
  device->chip = &test_chip;
  device->dev = &parent;
  device->cfgopt = "NvGspRm=1";
  gsp->subdev.device = device;
  strcpy(gsp->subdev.name, "gsp-failure");
  for (unsigned int round = 0; round < 8; round++) {
    for (unsigned int stage = 0; stage < ARRAY_SIZE(versions); stage++) {
      struct nvkm_gsp_fwif fwif = { .ver = versions[stage] };
      struct linuxkpi_firmware_usage before, after;
      unsigned long maps = linuxkpi_vmap_live_areas();
      unsigned long loads = linuxkpi_firmware_load_count();
      int ret;
      linuxkpi_firmware_usage(&before);
      ret = tu102_gsp_load(gsp, 1, &fwif);
      linuxkpi_firmware_usage(&after);
      if (ret != -ENOENT || linuxkpi_firmware_load_count() - loads != stage ||
          gsp->fws.rm || gsp->fws.bl ||
          gsp->fws.booter.load || gsp->fws.booter.unload || gsp->fws.fmc ||
          memcmp(&before, &after, sizeof(before)) ||
          maps != linuxkpi_vmap_live_areas()) {
        klogf("[FAIL] LinuxKPI: nouveau missing-firmware unwind stage=%u round=%u ret=%d\n",
              stage, round, ret);
        nvkm_gsp_dtor_fws(gsp);
        goto done;
      }
      /* A second destructor must be harmless after the loader unwound. */
      nvkm_gsp_dtor_fws(gsp);
    }
  }
  klog_puts("[  OK  ] LinuxKPI: nouveau four missing-firmware stages x8 release all buffers and VM mappings\n");
done:
  kfree(gsp);
  kfree(device);
}
#else
void linuxkpi_test_nouveau_firmware_failures(void) { }
#endif
