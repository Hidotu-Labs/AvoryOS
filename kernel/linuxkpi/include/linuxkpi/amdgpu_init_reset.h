#ifndef AVORYOS_AMDGPU_INIT_RESET_H
#define AVORYOS_AMDGPU_INIT_RESET_H
/* Scoped to amdgpu_device.c; the imported source remains unchanged. */
#include "amdgpu.h"
bool linuxkpi_amdgpu_need_reset_on_init(struct amdgpu_device *adev);
int linuxkpi_amdgpu_asic_reset(struct amdgpu_device *adev);
#undef amdgpu_asic_need_reset_on_init
#define amdgpu_asic_need_reset_on_init(adev) linuxkpi_amdgpu_need_reset_on_init(adev)
#undef amdgpu_asic_reset
#define amdgpu_asic_reset(adev) linuxkpi_amdgpu_asic_reset(adev)
#endif
