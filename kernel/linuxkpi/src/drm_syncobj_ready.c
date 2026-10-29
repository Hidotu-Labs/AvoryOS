#include <linux/dma-fence-chain.h>
#include <linux/uaccess.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>

int linuxkpi_original_drm_syncobj_timeline_wait_ioctl(struct drm_device *dev,
    void *data, struct drm_file *file_private);

int drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_private) {
    struct drm_syncobj_timeline_wait *args = data;
    const unsigned int supported = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
        DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
        DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
    if (args->count_handles != 1 || (args->flags & ~supported) ||
        !drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE))
        goto fallback;
    u32 handle;
    u64 point;
    if (copy_from_user(&handle, u64_to_user_ptr(args->handles), sizeof(handle)))
        goto fallback;
    struct drm_syncobj *syncobj = drm_syncobj_find(file_private, handle);
    if (!syncobj)
        goto fallback;
    if (copy_from_user(&point, u64_to_user_ptr(args->points), sizeof(point))) {
        drm_syncobj_put(syncobj);
        goto fallback;
    }
    struct dma_fence *fence = drm_syncobj_fence_get(syncobj);
    drm_syncobj_put(syncobj);
    if (!fence)
        goto fallback;
    int result = dma_fence_chain_find_seqno(&fence, point);
    bool ready = !result && (!fence || (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ||
                             dma_fence_is_signaled(fence));
    dma_fence_put(fence);
    if (!ready)
        goto fallback;
    args->first_signaled = 0;
    return 0;
fallback:
    return linuxkpi_original_drm_syncobj_timeline_wait_ioctl(dev, data, file_private);
}
