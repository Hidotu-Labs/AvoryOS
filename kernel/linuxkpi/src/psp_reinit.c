/* Raphael PF passthrough ring replacement after an unclean VM exit. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include "soc15_common.h"
#include "asic_reg/mp/mp_13_0_2_offset.h"
#include <linux/moduleparam.h>
#include <linuxkpi/log.h>

/* Paired with the scoped init MODE2 reset; stop alone failed GFX recovery. */
static bool kpi_amdgpu_psp_reinit = true;
module_param(kpi_amdgpu_psp_reinit, bool, 0);
MODULE_PARM_DESC(kpi_amdgpu_psp_reinit,
                "Stop prior Raphael PF PSP rings before creating a new ring");

extern int linuxkpi_psp_ring_create(struct psp_context *, enum psp_ring_type);

int linuxkpi_psp_reinit_ring_create(struct psp_context *psp,
                                  enum psp_ring_type type)
{
  struct amdgpu_device *adev = psp->adev;
  int ret;
  if (kpi_amdgpu_psp_reinit && !adev->no_hw_access &&
      adev->pdev->vendor == 0x1002 && adev->pdev->device == 0x164e &&
      (adev->flags & AMD_IS_APU) && amdgpu_passthrough(adev) &&
      !amdgpu_sriov_vf(adev) && type == PSP_RING_TYPE__KM &&
      adev->ip_versions[MP0_HWIP][0] == IP_VERSION(13, 0, 5)) {
    uint32_t reg = SOC15_REG_OFFSET(MP0, 0, regMP0_SMN_C2PMSG_64);
    /* Use the existing family callback, not a copied reset implementation.
     * PF create doesn't stop old rings; orderly suspend does. After abrupt
     * VM exit the old CPU/software ownership is gone but PSP may retain it.
     * Do not decode 0x0115, suppress create errors or retry creation.
     */
    ret = psp_wait_for(psp, reg, 0x80000000, 0x80000000, false);
    if (ret)
      return ret;
    klogf("[INFO] LinuxKPI: PSP reinit stop enter prior=%x\n", RREG32(reg));
    if (!psp->funcs->ring_stop)
      return -EOPNOTSUPP;
    ret = psp->funcs->ring_stop(psp, type);
    /* The imported stop tests only the ready bit. Require a zero raw status
     * too, using the same success mask as ring creation, before proceeding.
     */
    if (!ret)
      ret = psp_wait_for(psp, reg, 0x80000000, 0x8000ffff, false);
    klogf("[INFO] LinuxKPI: PSP reinit stop exit response=%x ret=%d\n",
          RREG32(reg), ret);
    if (ret)
      return ret;
  }
  return linuxkpi_psp_ring_create(psp, type);
}
#endif
