/* GPU-specific Raphael submission visibility compatibility policy.
 * Uses the imported driver's existing, budgeted EMIT_MEM_SYNC operation.
 * This is a switchable rendering candidate, not a reset or a CPU DMA barrier.
 */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include <linux/moduleparam.h>
#include <linuxkpi/log.h>

static bool kpi_amdgpu_ib_mem_sync = true;
module_param(kpi_amdgpu_ib_mem_sync, bool, 0);
MODULE_PARM_DESC(kpi_amdgpu_ib_mem_sync,
                "Request full GPU cache synchronization for Raphael GPUVM submissions");

extern int linuxkpi_original_amdgpu_ib_schedule(struct amdgpu_ring *,
                                              unsigned int, struct amdgpu_ib *,
                                              struct amdgpu_job *,
                                              struct dma_fence **);

int amdgpu_ib_schedule(struct amdgpu_ring *ring, unsigned int num_ibs,
                       struct amdgpu_ib *ibs, struct amdgpu_job *job,
                       struct dma_fence **f)
{
  struct amdgpu_device *adev = ring->adev;
  uint32_t flags;
  int ret;
  if (!kpi_amdgpu_ib_mem_sync || !num_ibs || !job || !job->vm ||
      adev->no_hw_access || adev->pdev->vendor != 0x1002 ||
      adev->pdev->device != 0x164e || !(adev->flags & AMD_IS_APU) ||
      !amdgpu_passthrough(adev) || amdgpu_sriov_vf(adev) ||
      adev->ip_versions[GC_HWIP][0] != IP_VERSION(10, 3, 6) ||
      (ring->funcs->type != AMDGPU_RING_TYPE_GFX &&
       ring->funcs->type != AMDGPU_RING_TYPE_COMPUTE) ||
      !ring->funcs->emit_mem_sync)
    return linuxkpi_original_amdgpu_ib_schedule(ring, num_ibs, ibs, job, f);

  /* The pinned IB scheduler reserves emit_frame_size, which already includes
   * the family's 8-dword ACQUIRE_MEM operation. Set its existing request bit;
   * do not append packets outside that accounting or rewrite userspace IBs.
   * gfx_v10_0's callback flushes/invalidates GL2/GLM/GL1/GLV/GLK/GLI.
   * Restoring flags preserves retry and caller ownership on every exit.
   */
  flags = ibs[0].flags;
  ibs[0].flags = flags | AMDGPU_IB_FLAG_EMIT_MEM_SYNC;
  static unsigned int logged_types;
  unsigned int bit = 1U << ring->funcs->type;
  if (!(__atomic_fetch_or(&logged_types, bit, __ATOMIC_RELAXED) & bit))
    klogf("[INFO] LinuxKPI: Raphael GPU cache sync enabled ring=%s type=%u\n",
          ring->name, ring->funcs->type);
  ret = linuxkpi_original_amdgpu_ib_schedule(ring, num_ibs, ibs, job, f);
  ibs[0].flags = flags;
  return ret;
}
#endif
