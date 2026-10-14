#ifndef AVORYOS_PSP_RING_TRACE_H
#define AVORYOS_PSP_RING_TRACE_H

/* Forced into amdgpu_psp.c only. Keep the imported callback protocol intact
 * while recording the command boundary before hardware access can fail. */
#include "amdgpu.h"

int linuxkpi_psp_ring_create(struct psp_context *psp,
                            enum psp_ring_type ring_type);
int linuxkpi_psp_reinit_ring_create(struct psp_context *psp,
                                   enum psp_ring_type ring_type);

#undef psp_ring_create
#define psp_ring_create(psp, type) linuxkpi_psp_reinit_ring_create((psp), (type))

void linuxkpi_psp_ring_set_wptr(struct psp_context *psp, uint32_t value);
#undef psp_ring_set_wptr
#define psp_ring_set_wptr(psp, value) linuxkpi_psp_ring_set_wptr((psp), (value))

struct amdgpu_ring;
void linuxkpi_psp_invalidate_hdp(struct amdgpu_device *adev,
                               struct amdgpu_ring *ring);
#define amdgpu_device_invalidate_hdp(adev, ring) \
  linuxkpi_psp_invalidate_hdp((adev), (ring))

#endif
