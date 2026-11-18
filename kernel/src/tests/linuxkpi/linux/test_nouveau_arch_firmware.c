/* Real imported GSP/FMC and ACR/GR/SEC2 firmware loader tests across
 * Turing/Ampere/Ada/Blackwell and Maxwell/Pascal without requiring GPU hardware. */
#include <linux/version.h>
#include <linuxkpi/log.h>
#if defined(AVORYOS_NOUVEAU_COMPAT) && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#include <core/device.h>
#include <core/firmware.h>
#include <nvfw/fw.h>
#include <subdev/gsp.h>
#include "priv.h"
#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linuxkpi/probe_diag.h>

static bool kpi_nouveau_arch_fw_test;
module_param(kpi_nouveau_arch_fw_test, bool, 0);

static int test_gsp_chip(const char *chip_name, bool use_fmc) {
  struct nvkm_device_chip chip = { .name = chip_name };
  struct device parent = { .init_name = "nouveau-arch-fw-test" };
  struct nvkm_device *device = kzalloc(sizeof(*device), GFP_KERNEL);
  struct nvkm_gsp *gsp = kzalloc(sizeof(*gsp), GFP_KERNEL);
  struct nvkm_gsp_fwif fwif = { .ver = "570.144" };
  struct linuxkpi_firmware_usage before, after;
  unsigned long maps;
  int ret = -ENOMEM;

  if (!device || !gsp)
    goto out;

  device->chip = &chip;
  device->dev = &parent;
  device->cfgopt = "NvGspRm=1";
  gsp->subdev.device = device;
  strcpy(gsp->subdev.name, "gsp-arch-test");

  linuxkpi_firmware_usage(&before);
  maps = linuxkpi_vmap_live_areas();

  ret = use_fmc ? gh100_gsp_load(gsp, 0, &fwif)
                : tu102_gsp_load(gsp, 1, &fwif);
  if (ret) {
    klogf("[FAIL] LinuxKPI: nouveau %s 570.144 load failed (%d)\n", chip_name, ret);
    goto out_dtor;
  }

  if (!gsp->fws.rm || !gsp->fws.bl ||
      (use_fmc ? (!gsp->fws.fmc || gsp->fws.fmc->size < 4 ||
                  memcmp(gsp->fws.fmc->data, "\x7f" "ELF", 4) != 0)
               : (!gsp->fws.booter.load || !gsp->fws.booter.unload))) {
    klogf("[FAIL] LinuxKPI: nouveau %s 570.144 incomplete firmware set\n", chip_name);
    ret = -EINVAL;
    goto out_dtor;
  }

  nvkm_gsp_dtor_fws(gsp);
  linuxkpi_firmware_usage(&after);
  if (memcmp(&before, &after, sizeof(before)) ||
      maps != linuxkpi_vmap_live_areas()) {
    klogf("[FAIL] LinuxKPI: nouveau %s 570.144 leaked firmware/vmap\n", chip_name);
    ret = -EBUSY;
    goto out;
  }

  klogf("[  OK  ] LinuxKPI: nouveau %s 570.144 %s firmware load/unload\n",
        chip_name, use_fmc ? "FMC" : "booter");
  ret = 0;
  goto out;

out_dtor:
  nvkm_gsp_dtor_fws(gsp);
out:
  kfree(gsp);
  kfree(device);
  return ret;
}

static int test_legacy_fw(const char *chip_name, const char *fwname, bool check_bin_hdr) {
  struct nvkm_device_chip chip = { .name = chip_name };
  struct device parent = { .init_name = "nouveau-legacy-fw-test" };
  struct nvkm_device *device = kzalloc(sizeof(*device), GFP_KERNEL);
  struct nvkm_subdev *subdev = kzalloc(sizeof(*subdev), GFP_KERNEL);
  const struct firmware *fw = NULL;
  int ret = -ENOMEM;

  if (!device || !subdev)
    goto out;

  device->chip = &chip;
  device->dev = &parent;
  subdev->device = device;
  strcpy(subdev->name, "legacy-fw-test");

  ret = nvkm_firmware_get(subdev, fwname, 0, &fw);
  if (ret || !fw || !fw->size) {
    klogf("[FAIL] LinuxKPI: nouveau %s/%s load failed (%d)\n",
          chip_name, fwname, ret);
    ret = ret ? ret : -EINVAL;
    goto out;
  }

  if (check_bin_hdr) {
    const struct nvfw_bin_hdr *hdr = nvfw_bin_hdr(subdev, fw->data);
    if (!hdr || hdr->bin_magic != 0x10de || hdr->bin_size > fw->size) {
      klogf("[FAIL] LinuxKPI: nouveau %s/%s invalid nvfw_bin_hdr\n",
            chip_name, fwname);
      ret = -EINVAL;
      goto out_put;
    }
  }

  klogf("[  OK  ] LinuxKPI: nouveau %s/%s (%zu bytes)\n",
        chip_name, fwname, fw->size);
  ret = 0;

out_put:
  nvkm_firmware_put(fw);
out:
  kfree(subdev);
  kfree(device);
  return ret;
}

void linuxkpi_test_nouveau_arch_firmware(void) {
  static const struct { const char *chip; bool fmc; } gsp_cases[] = {
    { "TU102", false },
    { "TU116", false },
    { "GA102", false },
    { "AD106", false },
    { "GB202", true },
  };
  static const struct { const char *chip; const char *fw; bool bin_hdr; } legacy_cases[] = {
    { "GM200", "acr/ucode_load", false },
    { "GM200", "acr/ucode_unload", false },
    { "GM200", "acr/bl", false },
    { "GM200", "gr/fecs_data", false },
    { "GM206", "acr/ucode_load", false },
    { "GP102", "acr/ucode_load", true },
    { "GP102", "acr/ucode_unload", true },
    { "GP102", "acr/bl", true },
    { "GP102", "acr/unload_bl", true },
    { "GP102", "nvdec/scrubber", true },
    { "GP102", "sec2/image", false },
    { "GP102", "sec2/sig", false },
    { "GP102", "sec2/desc", false },
    { "GP108", "gr/fecs_data", false },
  };
  int failures = 0;

  if (!kpi_nouveau_arch_fw_test)
    return;

  for (unsigned int i = 0; i < ARRAY_SIZE(gsp_cases); i++) {
    if (test_gsp_chip(gsp_cases[i].chip, gsp_cases[i].fmc))
      failures++;
  }
  for (unsigned int i = 0; i < ARRAY_SIZE(legacy_cases); i++) {
    if (test_legacy_fw(legacy_cases[i].chip, legacy_cases[i].fw, legacy_cases[i].bin_hdr))
      failures++;
  }

  if (failures == 0)
    klog_puts("[  OK  ] LinuxKPI: nouveau multi-arch firmware suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: nouveau multi-arch firmware suite had %d failure(s)\n", failures);
}
#else
void linuxkpi_test_nouveau_arch_firmware(void) { }
#endif
