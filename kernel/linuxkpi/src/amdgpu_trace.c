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
#include <linuxkpi/native_sched.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
typedef struct amdgpu_fence kpi_fence_arg_t;
typedef uint32_t kpi_psp_wait_flags_t;
#define KPI_VCN_STATE(adev) ((adev)->vcn.inst[0])
#define KPI_TASK_PID(info) ((info)->task.pid)
#else
typedef struct amdgpu_job kpi_fence_arg_t;
typedef bool kpi_psp_wait_flags_t;
#define KPI_VCN_STATE(adev) ((adev)->vcn)
#define KPI_TASK_PID(info) ((info)->pid)
#endif

extern int linuxkpi_original_amdgpu_gem_va_ioctl(struct drm_device *, void *, struct drm_file *);
extern int linuxkpi_original_amdgpu_vm_bo_update(struct amdgpu_device *, struct amdgpu_bo_va *, bool);
extern int linuxkpi_original_amdgpu_fence_emit(struct amdgpu_ring *, struct dma_fence **, kpi_fence_arg_t *, unsigned);
extern int linuxkpi_original_dma_fence_signal(struct dma_fence *);
extern void linuxkpi_original_dma_resv_add_fence(struct dma_resv *, struct dma_fence *, enum dma_resv_usage);

extern int linuxkpi_original_amdgpu_device_ip_set_powergating_state(
    void *, enum amd_ip_block_type, enum amd_powergating_state);
extern int linuxkpi_original_amdgpu_vcn_dec_ring_test_ring(struct amdgpu_ring *);

/* Only observe the existing Raphael PF startup path. In particular, do not
 * sample new media MMIO registers while their accessibility is uncertain. */
static bool vcn_startup_trace_device(struct amdgpu_device *adev) {
  return adev && adev->pdev && !adev->no_hw_access &&
         adev->pdev->vendor == 0x1002 && adev->pdev->device == 0x164e &&
         (adev->flags & AMD_IS_APU) && amdgpu_passthrough(adev) &&
         !amdgpu_sriov_vf(adev);
}

static unsigned vcn_power_trace_count;
/* Decoded submissions are any fence emit on the VCN decode ring. The
 * instrumented first N reveal whether playback touches the ring at all:
 * without this, decoded-frame idleness and "decode never started" look
 * identical in the power gating trace. */
static unsigned vcn_dec_submit_count;
/* Decode-completion latency: ring context + monotonic stamp captured at the
 * last VCN dec fence emit, matched on every dma_fence_signal by dma_fence
 * context. Frames that fail to complete quickly are exactly the thing the
 * submission counter cannot see, and the only kernel-side signature a
 * "VCN decode problem" could still leave. */
static uint64_t vcn_dec_emit_ctx;
static uint64_t vcn_dec_emit_t;
static unsigned vcn_dec_done_logged;

static unsigned vcn_dec_ring_fences(const struct amdgpu_device *adev) {
  if (adev->vcn.num_vcn_inst == 0)
    return 0;
  return amdgpu_fence_count_emitted(
      (struct amdgpu_ring *)&adev->vcn.inst[0].ring_dec);
}

int amdgpu_device_ip_set_powergating_state(
    void *dev, enum amd_ip_block_type type, enum amd_powergating_state state) {
  struct amdgpu_device *adev = dev;
  bool trace = type == AMD_IP_BLOCK_TYPE_VCN &&
               vcn_startup_trace_device(adev) &&
               __atomic_fetch_add(&vcn_power_trace_count, 1, __ATOMIC_RELAXED) < 256;
  unsigned before = trace ? KPI_VCN_STATE(adev).cur_state : 0;
  int ret = linuxkpi_original_amdgpu_device_ip_set_powergating_state(dev, type, state);
  if (trace)
    klogf("[INFO] LinuxKPI: VCN power t=%llums request=%u before=%u after=%u "
          "jobs=%u decfence=%u decsub=%u dpm=%u indirect=%u pg_flags=%llx ret=%d\n",
          linuxkpi_monotonic_ms(), (unsigned)state, before,
          (unsigned)KPI_VCN_STATE(adev).cur_state,
          atomic_read(&KPI_VCN_STATE(adev).total_submission_cnt),
          vcn_dec_ring_fences(adev), vcn_dec_submit_count,
          adev->pm.dpm_enabled, KPI_VCN_STATE(adev).indirect_sram,
          (unsigned long long)adev->pg_flags, ret);
  return ret;
}

int amdgpu_vcn_dec_ring_test_ring(struct amdgpu_ring *ring) {
  struct amdgpu_device *adev = ring->adev;
  bool trace = vcn_startup_trace_device(adev);
  if (trace)
    klogf("[INFO] LinuxKPI: VCN decode test enter me=%u gpu=%llx bytes=%u "
          "doorbell=%u index=%u wptr=%llu power=%u timeout_us=%u\n",
          ring->me, (unsigned long long)ring->gpu_addr, ring->ring_size,
          ring->use_doorbell, ring->doorbell_index,
          (unsigned long long)ring->wptr, (unsigned)KPI_VCN_STATE(adev).cur_state,
          adev->usec_timeout);
  int ret = linuxkpi_original_amdgpu_vcn_dec_ring_test_ring(ring);
  if (trace)
    klogf("[INFO] LinuxKPI: VCN decode test exit me=%u wptr=%llu "
          "power=%u ret=%d\n", ring->me, (unsigned long long)ring->wptr,
          (unsigned)KPI_VCN_STATE(adev).cur_state, ret);
  return ret;
}

/* Only gmc_v10_0.c and amdgpu_job.c redirect their failure-report lookup
 * here. Preserve the upstream task lookup and then retain the opt-in capture. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
struct amdgpu_task_info *linuxkpi_amdgpu_vm_fault_info_pasid(
    struct amdgpu_device *adev, u32 pasid) {
  struct amdgpu_task_info *info = amdgpu_vm_get_task_info_pasid(adev, pasid);
  gpu_diag_vm_failure(adev, pasid, info ? info->tgid : 0, info ? KPI_TASK_PID(info) : 0);
  /* Return the upstream reference unchanged; its caller owns the put. */
  return info;
}
#else
void linuxkpi_amdgpu_vm_fault_info(struct amdgpu_device *adev, u32 pasid,
                                   struct amdgpu_task_info *info) {
  amdgpu_vm_get_task_info(adev, pasid, info);
  gpu_diag_vm_failure(adev, pasid, info->tgid, info->pid);
}
#endif

extern int linuxkpi_original_psp_wait_for(struct psp_context *, uint32_t,
                                         uint32_t, uint32_t, kpi_psp_wait_flags_t);

/* The upstream hw_init path converts PSP failures to -EINVAL. Preserve the
 * original wait result, but expose the failed handshake before that conversion.
 * One status read after failure; never retry commands or write PSP registers. */
int psp_wait_for(struct psp_context *psp, uint32_t reg_index,
                  uint32_t reg_val, uint32_t mask, kpi_psp_wait_flags_t check_changed) {
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
                     kpi_fence_arg_t *job, unsigned flags) {
  int ret = linuxkpi_original_amdgpu_fence_emit(ring, f, job, flags);
  /* VCN decoded-frame submissions: the first few go to the serial log so a
   * desktop run can prove whether playback reaches the decode ring, and the
   * running total is carried on every VCN power line (decsub=). */
  if (!ret && ring && ring->adev &&
      vcn_startup_trace_device(ring->adev) &&
      ring->funcs->type == AMDGPU_RING_TYPE_VCN_DEC) {
    unsigned n = __atomic_add_fetch(&vcn_dec_submit_count, 1, __ATOMIC_RELAXED);
    /* dma_fence context = adev->fence_context + ring->idx (this kernel). */
    vcn_dec_emit_ctx = ring->adev->fence_context + ring->idx;
    vcn_dec_emit_t = linuxkpi_monotonic_ms();
    if (n <= 32)
      klogf("[INFO] LinuxKPI: VCN dec submit t=%llums n=%u gpu=%llx wptr=%llu\n",
            (unsigned long long)vcn_dec_emit_t, n,
            (unsigned long long)ring->gpu_addr,
            (unsigned long long)ring->wptr);
  }
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
  if (fence && __atomic_load_n(&vcn_dec_emit_ctx, __ATOMIC_RELAXED) &&
      fence->context == __atomic_load_n(&vcn_dec_emit_ctx, __ATOMIC_RELAXED)) {
    unsigned long long now = linuxkpi_monotonic_ms();
    unsigned long long lat =
        now - __atomic_load_n(&vcn_dec_emit_t, __ATOMIC_RELAXED);
    /* Ring latency beyond ~3 frames means a decode hang; those are the
     * only reportable kernel signature of a genuine decoder fault. */
    if (lat < 120 && __atomic_load_n(&vcn_dec_done_logged, __ATOMIC_RELAXED) < 32)
      klogf("[INFO] LinuxKPI: VCN dec done t=%llums lat=%llums seq=%llu\n",
            now, lat, (unsigned long long)fence->seqno);
    else if (lat >= 120)
      klogf("[INFO] LinuxKPI: VCN dec done (SLOW) t=%llums lat=%llums seq=%llu\n",
            now, lat, (unsigned long long)fence->seqno);
    __atomic_fetch_add(&vcn_dec_done_logged, 1, __ATOMIC_RELAXED);
  }
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
