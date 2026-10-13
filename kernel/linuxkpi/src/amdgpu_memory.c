/* AvoryOS AMD userspace allocation policy.
 *
 * The native DRM entry point receives the DRM core's kernel-side
 * argument copy. The imported implementation is compiled under a distinct
 * symbol, so its source stays pristine and still performs flag validation,
 * object creation, fenced initialization and handle publication.
 */
#ifdef AVORYOS_AMDGPU_MEMORY_POLICY
#include "amdgpu.h"
#include "amdgpu_gem.h"

extern int linuxkpi_amdgpu_gem_create_ioctl(struct drm_device *dev, void *data,
                                          struct drm_file *file);

int amdgpu_gem_create_ioctl(struct drm_device *dev, void *data,
                            struct drm_file *file) {
    struct amdgpu_device *adev = drm_to_adev(dev);
    union drm_amdgpu_gem_create *args = data;
    u64 requested_flags = args->in.domain_flags;
    int ret;

    /* Fresh userspace VRAM must not reveal its previous allocation's pixels.
     * amdgpu attaches the clear fence before publishing the new handle.
     * Existing objects, PRIME imports and internal allocations are unaffected.
     * GTT uses TTM's existing zero-allocation path. Preserve the driver's
     * behavior while buffer functions are unavailable during bring-up.
     */
    if ((args->in.domains & AMDGPU_GEM_DOMAIN_VRAM) &&
        adev->mman.buffer_funcs_enabled)
        args->in.domain_flags |= AMDGPU_GEM_CREATE_VRAM_CLEARED;

    ret = linuxkpi_amdgpu_gem_create_ioctl(dev, data, file);
    /* Only the handle occupies the output union; preserve the caller's
     * unused input flags in the rest of the returned argument copy. */
    args->in.domain_flags = requested_flags;
    return ret;
}
#endif
