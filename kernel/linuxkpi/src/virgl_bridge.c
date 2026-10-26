/* Native virgl needs Linux sync-file interop and a reserved render minor. */
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/file.h>
#include <linux/err.h>
#include <linux/xarray.h>
#include <linux/slab.h>
extern struct xarray drm_minors_xa;
int linuxkpi_virgl_render_reserve(void) {
    u32 minor;
    int ret = xa_alloc(&drm_minors_xa, &minor, NULL, XA_LIMIT(128, 191), GFP_KERNEL);
    return ret < 0 ? ret : (int)minor;
}
void linuxkpi_virgl_render_release(unsigned int minor) {
    xa_erase(&drm_minors_xa, minor);
}
int linuxkpi_virgl_fence_wait(int fd) {
    struct dma_fence *fence = sync_file_get_fence(fd);
    if (!fence) return -EINVAL;
    long ret = dma_fence_wait_timeout(fence, true, 10000);
    int status = ret > 0 ? dma_fence_get_status(fence) : 0;
    dma_fence_put(fence);
    return ret < 0 ? ret : ret == 0 ? -ETIMEDOUT : status < 0 ? status : 0;
}
struct virgl_native_fence {
    struct dma_fence base;
    spinlock_t lock;
};
static const char *virgl_fence_driver(struct dma_fence *fence) {
    (void)fence;
    return "virtio_gpu";
}
static const char *virgl_fence_timeline(struct dma_fence *fence) {
    (void)fence;
    return "virgl-host";
}
static const struct dma_fence_ops virgl_fence_ops = {
    .get_driver_name = virgl_fence_driver,
    .get_timeline_name = virgl_fence_timeline,
};
void *linuxkpi_virgl_fence_create(void) {
    struct virgl_native_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (!f) return NULL;
    spin_lock_init(&f->lock);
    /* Independent contexts allow completion replies to arrive out of order. */
    dma_fence_init(&f->base, &virgl_fence_ops, &f->lock, dma_fence_context_alloc(1), 1);
    return &f->base;
}
void linuxkpi_virgl_fence_get(void *fence) { dma_fence_get(fence); }
void linuxkpi_virgl_fence_put(void *fence) { dma_fence_put(fence); }
void linuxkpi_virgl_fence_complete(void *opaque, int error) {
    struct dma_fence *fence = opaque;
    if (error) dma_fence_set_error(fence, error);
    dma_fence_signal(fence);
    dma_fence_put(fence); /* Drop the submitted job's reference. */
}
int linuxkpi_virgl_fence_export_pending(void *opaque) {
    struct dma_fence *fence = opaque;
    struct sync_file *sync = sync_file_create(fence);
    if (!sync) return -ENOMEM;
    int fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) { fput(sync->file); return fd; }
    fd_install(fd, sync->file);
    return fd;
}
int linuxkpi_virgl_fence_export(void) {
    struct dma_fence *fence = dma_fence_get_stub();
    int ret = linuxkpi_virgl_fence_export_pending(fence);
    dma_fence_put(fence);
    return ret;
}
