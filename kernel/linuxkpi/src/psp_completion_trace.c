/* Observe PSP completion at its existing HDP invalidation boundary. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include <linuxkpi/log.h>

void linuxkpi_psp_invalidate_hdp(struct amdgpu_device *adev,
                               struct amdgpu_ring *ring)
{
  struct psp_context *psp = &adev->psp;
  unsigned expected, observed;

  /* Not compiled with the scoped macro: this calls the original HDP helper
   * exactly once. No additional register reads/writes or polling delays.
   */
  amdgpu_device_invalidate_hdp(adev, ring);
  if (ring || adev->accel_working || !psp->fence_buf || !psp->cmd_buf_mem)
    return;
  expected = (unsigned)atomic_read(&psp->fence_value);
  if (!expected || expected > 128)
    return;
  observed = READ_ONCE(*(uint32_t *)psp->fence_buf);
  if (observed != expected)
    return;

  /* The PSP command caller holds psp->mutex; this is the same fence equality
   * used by the imported completion loop. That loop stops after this call,
   * so at most one record is produced per command, never per pending poll.
   * Completion can arrive between this sample and the imported loop's check;
   * a missing record alone therefore does not prove missing completion.
   * A matching fence is not a successful status or a hardware-stability claim.
   */
  klogf("[INFO] LinuxKPI: PSP fence observed cmd=%x expected=%u observed=%u "
        "status=%x fw_gpu=%llx\n", psp->cmd_buf_mem->cmd_id, expected,
        observed, psp->cmd_buf_mem->resp.status,
        ((unsigned long long)psp->cmd_buf_mem->resp.fw_addr_hi << 32) |
        psp->cmd_buf_mem->resp.fw_addr_lo);
}
#endif
