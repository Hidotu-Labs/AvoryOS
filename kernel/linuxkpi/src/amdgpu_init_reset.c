/* Scoped Raphael passthrough initialization recovery. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linuxkpi/log.h>

/* MODE2 alone failed earlier trials. Combined with PSP ring replacement,
 * physical X-close and Ctrl-C/reopen pairs initialized successfully.
 * Keep a disable switch and do not extend this policy to other devices.
 */
static bool kpi_amdgpu_mode2_reset = true;
module_param(kpi_amdgpu_mode2_reset, bool, 0);
MODULE_PARM_DESC(kpi_amdgpu_mode2_reset,
                "Request Raphael passthrough MODE2 reset on every probe");

static bool linuxkpi_raphael_mode2_requested(struct amdgpu_device *adev)
{
  return kpi_amdgpu_mode2_reset &&
         adev->pdev->vendor == 0x1002 && adev->pdev->device == 0x164e &&
         (adev->flags & AMD_IS_APU) && amdgpu_passthrough(adev) &&
         !amdgpu_sriov_vf(adev) && !adev->gmc.xgmi.num_physical_nodes &&
         adev->ip_versions[MP0_HWIP][0] == IP_VERSION(13, 0, 5) &&
         adev->ip_versions[MP1_HWIP][0] == IP_VERSION(13, 0, 5);
}

bool linuxkpi_amdgpu_need_reset_on_init(struct amdgpu_device *adev)
{
  if (linuxkpi_raphael_mode2_requested(adev)) {
    /* Upstream init temporarily selects AUTO, which resolves to MODE2 for
     * MP1 13.0.5. Use its existing pre-post reset boundary, not a PSP retry.
     * Do not infer cold/dirty state from an undocumented PSP status value.
     */
    klogf("[INFO] LinuxKPI: Raphael MODE2 init reset requested\n");
    return true;
  }
  return adev->asic_funcs->need_reset_on_init(adev);
}

int linuxkpi_amdgpu_asic_reset(struct amdgpu_device *adev)
{
  bool trace = linuxkpi_raphael_mode2_requested(adev) &&
               amdgpu_reset_method == AMD_RESET_METHOD_NONE;
  int ret;
  if (trace)
    klogf("[INFO] LinuxKPI: Raphael MODE2 init reset enter\n");
  /* nv's reset callback owns bus-master handling, PCI save/restore and SMU
   * reset. Preserve its result, with no fallback, retry or bridge reset.
   */
  ret = adev->asic_funcs->reset(adev);
  /* The pinned nv MODE2 implementation polls memsize but doesn't turn
   * exhausted readiness polling into an error. For this scoped path, check
   * the same readiness register once after it returns; never report reset
   * success while the device still reads as absent. Preserve SMU errors.
   */
  if (trace && !ret && adev->nbio.funcs->get_memsize(adev) == 0xffffffff)
    ret = -ETIME;
  if (trace)
    klogf("[INFO] LinuxKPI: Raphael MODE2 init reset exit ret=%d\n", ret);
  return ret;
}
#endif
