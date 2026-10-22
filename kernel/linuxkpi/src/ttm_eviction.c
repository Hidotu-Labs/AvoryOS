/* SPDX-License-Identifier: GPL-2.0-only */
/* LinuxKPI TTM eviction compatibility entry point.
 * The pinned import tests ret after the LRU loop without initializing it.
 * Empty managers must still drain their move fence and report real errors.
 * Keep imported sources unchanged; the build renames their entry point.
 * This follows the imported eviction algorithm with a defined initial result.
 */
#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_resource.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
/* Newer upstream eviction no longer has the empty-manager bug. */
int linuxkpi_original_ttm_resource_manager_evict_all(
    struct ttm_device *bdev, struct ttm_resource_manager *man);
int ttm_resource_manager_evict_all(struct ttm_device *bdev,
                                  struct ttm_resource_manager *man) {
  return linuxkpi_original_ttm_resource_manager_evict_all(bdev, man);
}
#else

int ttm_resource_manager_evict_all(struct ttm_device *bdev,
				   struct ttm_resource_manager *man)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false,
		.force_alloc = true
	};
	struct dma_fence *fence;
	int ret = 0;
	unsigned i;

	/*
	 * Can't use standard list traversal since we're unlocking.
	 */

	spin_lock(&bdev->lru_lock);
	for (i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		while (!list_empty(&man->lru[i])) {
			spin_unlock(&bdev->lru_lock);
			ret = ttm_mem_evict_first(bdev, man, NULL, &ctx,
						  NULL);
			if (ret)
				return ret;
			spin_lock(&bdev->lru_lock);
		}
	}
	spin_unlock(&bdev->lru_lock);

	if (ret && ret != -ENOENT)
		return ret;

	spin_lock(&man->move_lock);
	fence = dma_fence_get(man->move);
	spin_unlock(&man->move_lock);

	if (fence) {
		ret = dma_fence_wait(fence, false);
		dma_fence_put(fence);
		if (ret)
			return ret;
	}

	return 0;
}
#endif
