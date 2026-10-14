/* Compatibility for the pinned import's kernel-doorbell BO lifetime. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include "amdgpu_doorbell.h"
#include "amdgpu_ttm.h"

extern void linuxkpi_original_amdgpu_ttm_fini(struct amdgpu_device *adev);

void amdgpu_ttm_fini(struct amdgpu_device *adev)
{
  /* GMC sw_fini runs after the other IP consumers have torn down. The
   * imported device_fini_sw releases doorbells later, after TTM managers
   * have disappeared. Release this pinned BO before destroying its manager;
   * upstream free_kernel clears both the owned BO and its CPU mapping, so
   * the later doorbell_fini is harmless. Also covers partial TTM init after
   * doorbell allocation, since mman.initialized is set at device init.
   * Do not move this to device_fini_sw entry: IP teardown still uses TTM.
   */
  if (adev->mman.initialized)
    amdgpu_doorbell_fini(adev);
  linuxkpi_original_amdgpu_ttm_fini(adev);
}
#endif
