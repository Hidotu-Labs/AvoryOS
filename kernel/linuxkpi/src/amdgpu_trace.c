/* Opt-in resource tracing around selected upstream entry points. No waits,
 * allocations or data changes: record borrowed identities while the caller
 * still owns its objects. Mode 3 caps and retains 1024 resource events. */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include "amdgpu_gem.h"
#include "amdgpu_object.h"
#include "amdgpu_vm.h"
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/mm.h>
#include <drm/ttm/ttm_tt.h>
#include <linuxkpi/gpu_diag.h>
#include <linuxkpi/log.h>

extern int linuxkpi_original_amdgpu_gem_va_ioctl(struct drm_device *, void *, struct drm_file *);
extern int linuxkpi_original_amdgpu_vm_bo_update(struct amdgpu_device *, struct amdgpu_bo_va *, bool);
extern int linuxkpi_original_amdgpu_fence_emit(struct amdgpu_ring *, struct dma_fence **, struct amdgpu_job *, unsigned);
extern int linuxkpi_original_dma_fence_signal(struct dma_fence *);
extern void linuxkpi_original_dma_resv_add_fence(struct dma_resv *, struct dma_fence *, enum dma_resv_usage);

extern int linuxkpi_original_psp_wait_for(struct psp_context *, uint32_t,
                                         uint32_t, uint32_t, bool);

/* The upstream hw_init path converts PSP failures to -EINVAL. Preserve the
 * original wait result, but expose the failed handshake before that conversion.
 * One status read after failure; never retry commands or write PSP registers. */
int psp_wait_for(struct psp_context *psp, uint32_t reg_index,
                  uint32_t reg_val, uint32_t mask, bool check_changed) {
  int ret = linuxkpi_original_psp_wait_for(psp, reg_index, reg_val, mask,
                                          check_changed);
  if (ret) {
    struct amdgpu_device *adev = psp->adev;
    uint32_t observed = RREG32(reg_index);
    klogf("[ERROR] LinuxKPI: PSP wait failed reg=%x expected=%x mask=%x "
          "observed-after-failure=%x changed=%u ret=%d\n",
          reg_index, reg_val, mask, observed, check_changed, ret);
  }
  return ret;
}

int linuxkpi_psp_ring_create(struct psp_context *psp,
                            enum psp_ring_type ring_type) {
  int ret;
  /* Read software-owned setup only. No additional register access, commands,
   * buffer changes, retries, or alteration of the family's return value. */
  klogf("[INFO] LinuxKPI: PSP ring-create enter type=%u gpu=%llx size=%u "
        "timeout_us=%u\n", (unsigned)ring_type,
        (unsigned long long)psp->km_ring.ring_mem_mc_addr,
        psp->km_ring.ring_size, psp->adev->usec_timeout);
  ret = psp->funcs->ring_create(psp, ring_type);
  klogf("[INFO] LinuxKPI: PSP ring-create exit ret=%d\n", ret);
  return ret;
}

int amdgpu_gem_va_ioctl(struct drm_device *dev, void *data, struct drm_file *file) {
  struct drm_amdgpu_gem_va *args = data;
  /* This is the DRM core's kernel argument copy, not an unchecked user ptr. */
  bool trace = gpu_diag_resource_enabled();
  u64 handle_op = ((u64)args->handle << 32) | args->operation;
  u64 address = args->va_address, size = args->map_size, flags = args->flags;
  u64 offset = args->offset_in_bo;
  int ret = linuxkpi_original_amdgpu_gem_va_ioctl(dev, data, file);
  if (trace) {
    gpu_diag_record(GPU_DIAG_VM_BIND, file, handle_op, address, size, flags, ret);
    gpu_diag_record(GPU_DIAG_VM_OFFSET, file, handle_op >> 32, offset, address, size, ret);
  }
  return ret;
}

int amdgpu_vm_bo_update(struct amdgpu_device *adev, struct amdgpu_bo_va *va, bool clear) {
  /* As upstream requires, the caller reserves the BO/VM before updating. */
  bool trace = gpu_diag_resource_enabled();
  struct amdgpu_bo *bo = va->base.bo;
  if (trace && bo) {
    struct ttm_tt *tt = bo->tbo.ttm;
    gpu_diag_record(GPU_DIAG_BO_BACKING, bo, bo->tbo.base.size,
                    bo->tbo.resource ? bo->tbo.resource->mem_type : ~0U,
                    (uintptr_t)bo->tbo.base.resv, (uintptr_t)&bo->tbo, 0);
    if (tt && tt->pages && tt->dma_address && tt->num_pages) {
      /* Only boundary pages: full BO page walks would perturb workloads. */
      unsigned long indexes[2] = {0, tt->num_pages - 1};
      for (unsigned i = 0; i < 2; i++) {
        unsigned long n = indexes[i];
        struct page *page = tt->pages[n];
        if (page)
          gpu_diag_record(GPU_DIAG_PAGE_BIND, bo, n, page_to_phys(page),
                          tt->dma_address[n], atomic_read(&page->_refcount), 0);
        if (indexes[0] == indexes[1])
          break;
      }
    }
  }
  int ret = linuxkpi_original_amdgpu_vm_bo_update(adev, va, clear);
  if (trace)
    gpu_diag_record(GPU_DIAG_VM_UPDATE, bo, (uintptr_t)va->base.vm,
                    clear, (uintptr_t)va->last_pt_update, 0, ret);
  return ret;
}

int amdgpu_fence_emit(struct amdgpu_ring *ring, struct dma_fence **f,
                     struct amdgpu_job *job, unsigned flags) {
  int ret = linuxkpi_original_amdgpu_fence_emit(ring, f, job, flags);
  if (!ret && f && *f && gpu_diag_resource_enabled())
    gpu_diag_record(GPU_DIAG_FENCE_EMIT, *f, (*f)->context, (*f)->seqno,
                    (uintptr_t)ring, flags, 0);
  return ret;
}

int dma_fence_signal(struct dma_fence *fence) {
  bool trace = fence && gpu_diag_resource_enabled();
  u64 context = trace ? fence->context : 0, seqno = trace ? fence->seqno : 0;
  if (trace)
    gpu_diag_record(GPU_DIAG_FENCE_SIGNAL, fence, context, seqno, 0, 0, 0);
  int ret = linuxkpi_original_dma_fence_signal(fence);
  /* Callbacks may release references. Never dereference after signaling. */
  if (trace)
    gpu_diag_record(GPU_DIAG_FENCE_SIGNAL, fence, context, seqno, 1, 0, ret);
  return ret;
}

void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
                        enum dma_resv_usage usage) {
  bool trace = gpu_diag_resource_enabled();
  u64 context = trace ? fence->context : 0, seqno = trace ? fence->seqno : 0;
  linuxkpi_original_dma_resv_add_fence(obj, fence, usage);
  if (trace)
    gpu_diag_record(GPU_DIAG_RESV_FENCE, obj, (uintptr_t)fence, context, seqno, usage, 0);
}
#endif
