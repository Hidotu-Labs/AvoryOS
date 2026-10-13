/* Cross-device PRIME sharing without requiring a passed-through GPU. */
#include "drivers/gpu/drm/drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "console/klog.h"
#include "mm/heap.h"
#include <linuxkpi/native_prime.h>
#include <linuxkpi/native_vfs.h>

extern struct drm_framebuffer *ascentdrm_framebuffer_create(
    struct drm_device *dev, struct drm_mode_fb_cmd *cmd);
extern void ascentdrm_framebuffer_free(struct drm_device *dev,
                                     struct drm_framebuffer *fb);

struct prime_test_device {
  struct drm_device dev;
  struct drm_file file;
};

static void prime_test_init(struct prime_test_device *t) {
  spinlock_init(&t->dev.lock);
  spinlock_init(&t->file.lock);
  INIT_LIST_HEAD(&t->dev.gem_objects);
  INIT_LIST_HEAD(&t->dev.kms_objects);
  t->dev.next_kms_id = 1;
  t->dev.next_gem_handle = 1;
  t->file.dev = &t->dev;
  t->file.next_handle = 1;
}

void ascentdrm_prime_selftest(void) {
#ifdef LINUXKPI_LINUX_IMPORTED
  struct prime_test_device *a = kcalloc(1, sizeof(*a));
  struct prime_test_device *b = kcalloc(1, sizeof(*b));
  struct drm_gem_object *obj = NULL, *imported;
  int fd = -1, exported = -1;
  uint32_t handle = 0, same = 0, foreign = 0, again = 0;
  bool ok = false;
  if (!a || !b)
    goto done;
  prime_test_init(a);
  prime_test_init(b);
  obj = ascentdrm_gem_object_create(&a->dev, 8192);
  if (!obj)
    goto done;
  handle = ascentdrm_file_gem_register(&a->file, obj);
  if (!handle) { ascentdrm_gem_object_free(&a->dev, obj); obj = NULL; goto done; }
  ascentdrm_gem_put_last(obj); /* transfer creator reference to the handle */
  ((uint32_t *)obj->virt_addr)[0] = 0xabcddcba;
  ((uint32_t *)obj->virt_addr)[1024] = 0x12344321;
  fd = ascentdrm_prime_export(obj, 0x80000 /* O_CLOEXEC */);
  if (fd < 0 || ascentdrm_prime_import(&a->file, fd, &same) || same != handle)
    goto done;
  if (ascentdrm_prime_import(&b->file, fd, &foreign) ||
      ascentdrm_prime_import(&b->file, fd, &again) || again != foreign)
    goto done;
  imported = ascentdrm_file_gem_lookup(&b->file, foreign);
  if (!imported || !imported->prime_buffer ||
      imported->get_page_phys(imported, 1) != obj->phys_addr + 4096 ||
      ((uint32_t *)imported->virt_addr)[1024] != 0x12344321)
    goto done;
  ((uint32_t *)imported->virt_addr)[0] = 0x55aa55aa;
  if (((uint32_t *)obj->virt_addr)[0] != 0x55aa55aa)
    goto done;
  /* The import must pin its exporter after both its handle and fd close. */
  ascentdrm_file_gem_release(&a->file, handle);
  handle = 0;
  asc_vfs_fd_close(fd);
  fd = -1;
  exported = ascentdrm_prime_export(imported, 0x80000);
  if (exported < 0 || ascentdrm_prime_import(&b->file, exported, &again) ||
      again != foreign || ((uint32_t *)imported->virt_addr)[0] != 0x55aa55aa)
    goto done;
  /* Xorg imports its output pixmap and uses legacy ADDFB, without first
   * preparing a VirtIO resource. Exercise that exact entry point. */
  if (virtio_gpu_is_initialized()) {
    struct drm_mode_fb_cmd cmd = {
      .width = 32, .height = 32, .pitch = 128,
      .bpp = 32, .depth = 32, .handle = imported->handle,
    };
    struct drm_framebuffer *fb = ascentdrm_framebuffer_create(&b->dev, &cmd);
    if (fb) {
      ascentdrm_framebuffer_free(&b->dev, fb);
      goto done;
    }
    cmd.depth = 24;
    fb = ascentdrm_framebuffer_create(&b->dev, &cmd);
    if (!fb)
      goto done;
    bool prepared = fb->gem_obj == imported && fb->pitch == cmd.pitch &&
                    !fb->backing_offset;
    ascentdrm_framebuffer_free(&b->dev, fb);
    if (!prepared)
      goto done;
  }
  if (ascentdrm_prime_import(&b->file, -1, &again) != -9)
    goto done;
  ok = true;
done:
  if (fd >= 0) asc_vfs_fd_close(fd);
  if (exported >= 0) asc_vfs_fd_close(exported);
  if (a && handle) ascentdrm_file_gem_release(&a->file, handle);
  if (b && foreign) ascentdrm_file_gem_release(&b->file, foreign);
  if (a && obj && !list_empty(&a->dev.gem_objects)) ok = false;
  if (b && !list_empty(&b->dev.gem_objects)) ok = false;
  kfree(b);
  kfree(a);
  klog_puts(ok ? "[  OK  ] PRIME: cross-device sharing, repeated import, re-export, legacy framebuffer and teardown\n"
                : "[FAIL] PRIME: cross-device sharing/lifetime test\n");
#endif
}
