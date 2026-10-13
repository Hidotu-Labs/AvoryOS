/* Exercise actual legacy cursor state without requiring a physical monitor.
 * Boot tests run before userspace opens DRM, so temporarily replacing the
 * upload callback cannot interfere with a desktop client. */
#include "drivers/gpu/drm/drm.h"
#include "console/klog.h"
#include "mm/heap.h"
extern drm_cursor_fn_t g_ascent_drm_cursor_fn;
extern drm_commit_damage_fn_t g_ascent_drm_commit_damage_fn;

static void external_scanout(struct drm_device *dev,
                             const struct drm_clip_rect *clips,
                             uint32_t count, uint32_t crtc) {
  (void)dev; (void)clips; (void)count; (void)crtc;
}

static uint32_t seen_crtc, seen_pixel;
static void record_cursor(uint32_t crtc_id, struct drm_gem_object *gem,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           int32_t x, int32_t y, int32_t hot_x, int32_t hot_y,
                           uint32_t flags) {
  (void)width; (void)height; (void)pitch; (void)x; (void)y;
  (void)hot_x; (void)hot_y; (void)flags;
  seen_crtc = crtc_id;
  seen_pixel = gem ? *(uint32_t *)gem->virt_addr : 0;
}

void ascentdrm_cursor_selftest(void) {
  struct drm_device *dev = kcalloc(1, sizeof(*dev));
  struct drm_file *file = kcalloc(1, sizeof(*file));
  struct drm_crtc crtcs[2] = {0};
  struct drm_plane planes[2] = {0};
  struct drm_gem_object *gem = NULL;
  uint32_t handles[2] = {0};
  bool ok = false;
  drm_cursor_fn_t saved = g_ascent_drm_cursor_fn;
  drm_commit_damage_fn_t saved_scanout = g_ascent_drm_commit_damage_fn;
  if (!dev || !file) goto done;
  spinlock_init(&dev->lock);
  spinlock_init(&file->lock);
  INIT_LIST_HEAD(&dev->kms_objects);
  INIT_LIST_HEAD(&dev->gem_objects);
  dev->next_gem_handle = 1;
  file->dev = dev;
  file->next_handle = 1;
  for (uint32_t i = 0; i < 2; i++) {
    crtcs[i].base.type = DRM_MODE_OBJECT_CRTC;
    crtcs[i].base.id = 17 + i;
    crtcs[i].cursor = &planes[i];
    list_add_tail(&crtcs[i].base.list, &dev->kms_objects);
  }
  g_ascent_drm_cursor_fn = record_cursor;
  struct drm_mode_cursor2 unavailable = {
    .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtcs[0].base.id,
    .width = 16, .height = 16, .handle = 999,
  };
  g_ascent_drm_commit_damage_fn = external_scanout;
  g_ascent_drm_cursor_fn = NULL;
  if (ascentdrm_ioctl_legacy_cursor(file, dev, &unavailable) != -6)
    goto cleanup;
  g_ascent_drm_cursor_fn = record_cursor;
  for (uint32_t i = 0; i < 2; i++) {
    gem = ascentdrm_gem_object_create(dev, 4096);
    if (!gem) goto cleanup;
    *(uint32_t *)gem->virt_addr = 0xff00ff00 + i;
    handles[i] = ascentdrm_file_gem_register(file, gem);
    if (!handles[i]) { ascentdrm_gem_object_free(dev, gem); goto cleanup; }
    ascentdrm_gem_put_last(gem); /* transfer creator ref to file */
    struct drm_mode_cursor2 cur = {
      .flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
      .crtc_id = crtcs[i].base.id, .width = 16, .height = 16,
      .handle = handles[i], .x = 40 + i, .y = 50 + i,
      .hot_x = 2, .hot_y = 3,
    };
    if (ascentdrm_ioctl_legacy_cursor(file, dev, &cur) ||
        seen_crtc != cur.crtc_id || seen_pixel != 0xff00ff00 + i)
      goto cleanup;
    ascentdrm_file_gem_release(file, handles[i]);
    handles[i] = 0;
    /* Closing the handle must leave the displayed cursor buffer alive. */
    if (gem->refcount != 1 || planes[i].fb->gem_obj != gem)
      goto cleanup;
    cur.flags = DRM_MODE_CURSOR_MOVE;
    cur.x += 10;
    if (ascentdrm_ioctl_legacy_cursor(file, dev, &cur) ||
        seen_crtc != cur.crtc_id || seen_pixel != 0xff00ff00 + i)
      goto cleanup;
    cur.crtc_id = 9999;
    if (ascentdrm_ioctl_legacy_cursor(file, dev, &cur) != -2)
      goto cleanup;
    cur.crtc_id = crtcs[i].base.id;
    cur.flags = 0;
    if (ascentdrm_ioctl_legacy_cursor(file, dev, &cur) != -22)
      goto cleanup;
  }
  if (planes[0].fb == planes[1].fb ||
      *(uint32_t *)planes[0].fb->gem_obj->virt_addr != 0xff00ff00)
    goto cleanup;
  struct drm_mode_cursor2 hide = {
    .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtcs[1].base.id,
  };
  if (ascentdrm_ioctl_legacy_cursor(file, dev, &hide) ||
      planes[1].fb || !planes[0].fb || seen_crtc != hide.crtc_id || seen_pixel)
    goto cleanup;
  ok = true;
cleanup:
  for (uint32_t i = 0; i < 2; i++)
    if (handles[i]) ascentdrm_file_gem_release(file, handles[i]);
  ascentdrm_cursor_reset(dev);
  if (!list_empty(&dev->gem_objects)) ok = false;
done:
  g_ascent_drm_cursor_fn = saved;
  g_ascent_drm_commit_damage_fn = saved_scanout;
  kfree(file);
  kfree(dev);
  klog_puts(ok ? "[  OK  ] DRM cursor: CRTC isolation, closed-handle lifetime, move/hide and teardown\n"
                : "[FAIL] DRM cursor: plane lifetime/CRTC test\n");
}
