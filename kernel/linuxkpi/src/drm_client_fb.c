/* Lightweight drm_client -> native fb_global console bridge.
 *
 * Implements drm_client_setup() using the already-compiled drm_client /
 * drm_client_modeset core instead of Linux fbcon/vt.  When a KMS driver such as
 * amdgpu or bochs disables UEFI/BIOS GOP scanout during probe, this allocates
 * a dumb framebuffer, commits the preferred mode, and mirrors cached-backbuffer
 * dirty spans into it using WC streaming writes while KD_TEXT owns the display.
 */

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>

#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <linuxkpi/drm_console.h>
#include <linuxkpi/log.h>

#include "../../linux/drivers/gpu/drm/drm_internal.h"

struct kpi_drm_console {
  struct drm_client_dev client;
  struct drm_client_buffer *buffer;
  struct iosys_map map;
  u32 width;
  u32 height;
  u32 pitch;
  bool active;
};

static struct kpi_drm_console *kpi_console_client;
/* Console writes may run with IRQs disabled. Protect the selected client and
 * drain its in-flight blits before releasing or remapping the buffer. DRM
 * serializes the client callbacks. Never call a DRM callback under this lock. */
static DEFINE_SPINLOCK(kpi_console_lock);

static inline u8 *kpi_drm_console_dst(const struct kpi_drm_console *con) {
  return con->map.is_iomem ? (u8 __force *)con->map.vaddr_iomem
                           : (u8 *)con->map.vaddr;
}

static void kpi_drm_console_blit(struct kpi_drm_console *con,
                                 const void *src_base, u32 fw, u32 fh,
                                 u32 fpitch, u32 src_y_off, u32 dst_x,
                                 u32 dst_y, u32 w, u32 h) {
  u8 *dst_base;
  u32 max_w, max_h;
  size_t row_bytes;

  if (!con || !con->buffer || iosys_map_is_null(&con->map))
    return;
  if (!src_base || !fw || !fh || !fpitch || !w || !h)
    return;
  if (src_y_off >= fh)
    return;
  if (h > fh - src_y_off)
    h = fh - src_y_off;

  max_w = fw < con->width ? fw : con->width;
  max_h = fh < con->height ? fh : con->height;
  if (dst_x >= max_w || dst_y >= max_h)
    return;
  if (w > max_w - dst_x)
    w = max_w - dst_x;
  if (h > max_h - dst_y)
    h = max_h - dst_y;
  if (!w || !h)
    return;

  dst_base = kpi_drm_console_dst(con);
  if (dst_x == 0 && w == max_w && fpitch == con->pitch) {
    linuxkpi_memcpy_to_wc(
        dst_base + (size_t)dst_y * con->pitch,
        (const u8 *)src_base + (size_t)src_y_off * fpitch, (size_t)h * fpitch);
  } else {
    row_bytes = (size_t)w * 4;
    for (u32 r = 0; r < h; r++) {
      size_t src_off = (size_t)(src_y_off + r) * fpitch + (size_t)dst_x * 4;
      size_t dst_off = (size_t)(dst_y + r) * con->pitch + (size_t)dst_x * 4;
      linuxkpi_memcpy_to_wc(dst_base + dst_off,
                            (const u8 *)src_base + src_off, row_bytes);
    }
  }
}

static void kpi_drm_console_sync_full(struct kpi_drm_console *con) {
  unsigned int fw = 0, fh = 0, fpitch = 0, y_origin = 0;
  _Bool is_gfx = 0;
  const void *src =
      linuxkpi_native_fb_snapshot(&fw, &fh, &fpitch, &y_origin, &is_gfx);

  if (is_gfx || !src || !fw || !fh || !fpitch)
    return;
  if (__atomic_load_n(&con->client.dev->master, __ATOMIC_RELAXED) != NULL)
    return;

  if (fw < con->width) {
    u32 rows = fh < con->height ? fh : con->height;
    size_t tail_bytes = (size_t)(con->width - fw) * 4;
    for (u32 r = 0; r < rows; r++)
      iosys_map_memset(&con->map, (size_t)r * con->pitch + (size_t)fw * 4, 0,
                       tail_bytes);
  }
  if (fh < con->height) {
    iosys_map_memset(&con->map, (size_t)fh * con->pitch, 0,
                     (size_t)(con->height - fh) * con->pitch);
  }

  if (y_origin >= fh)
    y_origin = 0;
  u32 top_rows = fh - y_origin;
  kpi_drm_console_blit(con, src, fw, fh, fpitch, y_origin, 0, 0, fw, top_rows);
  if (y_origin > 0) {
    kpi_drm_console_blit(con, src, fw, fh, fpitch, 0, 0, top_rows, fw,
                         y_origin);
  }
  wmb();
}

static int kpi_drm_client_setup_modeset(struct kpi_drm_console *con) {
  struct drm_client_dev *client = &con->client;
  struct drm_device *dev = client->dev;
  struct drm_mode_set *modeset;
  u32 fb_w = 0, fb_h = 0;
  struct drm_client_buffer *buffer;
  struct iosys_map map;
  unsigned long flags;
  int ret;

  if (con->buffer) {
    spin_lock_irqsave(&kpi_console_lock, flags);
    con->active = false;
    spin_unlock_irqrestore(&kpi_console_lock, flags);
    if (!iosys_map_is_null(&con->map)) {
      drm_client_buffer_vunmap(con->buffer);
      iosys_map_clear(&con->map);
    }
    ret = drm_client_modeset_commit_locked(client);
    if (ret)
      return ret;
    ret = drm_client_buffer_vmap(con->buffer, &map);
    if (ret)
      return ret;
    con->map = map;
    kpi_drm_console_sync_full(con);
    spin_lock_irqsave(&kpi_console_lock, flags);
    con->active = true;
    spin_unlock_irqrestore(&kpi_console_lock, flags);
    return 0;
  }

  ret = drm_client_modeset_probe(client, 0, 0);
  if (ret)
    return ret;

  mutex_lock(&client->modeset_mutex);
  drm_client_for_each_modeset(modeset, client) {
    if (!modeset->mode)
      continue;
    if (modeset->x + modeset->mode->hdisplay > fb_w)
      fb_w = modeset->x + modeset->mode->hdisplay;
    if (modeset->y + modeset->mode->vdisplay > fb_h)
      fb_h = modeset->y + modeset->mode->vdisplay;
  }
  mutex_unlock(&client->modeset_mutex);

  if (!fb_w || !fb_h)
    return 0;

  buffer = drm_client_framebuffer_create(client, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (IS_ERR(buffer))
    return PTR_ERR(buffer);

  mutex_lock(&client->modeset_mutex);
  drm_client_for_each_modeset(modeset, client) {
    if (modeset->mode)
      modeset->fb = buffer->fb;
  }
  mutex_unlock(&client->modeset_mutex);

  /* Commit before vmap so TTM drivers (amdgpu/bochs) migrate and pin the
   * dumb buffer into its final VRAM scanout location before mapping it. */
  ret = drm_client_modeset_commit_locked(client);
  if (ret) {
    drm_client_framebuffer_delete(buffer);
    return ret;
  }

  ret = drm_client_buffer_vmap(buffer, &map);
  if (ret) {
    drm_client_framebuffer_delete(buffer);
    return ret;
  }

  con->buffer = buffer;
  con->map = map;
  con->width = fb_w;
  con->height = fb_h;
  con->pitch = buffer->pitch;

  kpi_drm_console_sync_full(con);
  spin_lock_irqsave(&kpi_console_lock, flags);
  con->active = true;
  if (!kpi_console_client ||
      (dev->driver && !strcmp(dev->driver->name, "amdgpu")))
    kpi_console_client = con;
  spin_unlock_irqrestore(&kpi_console_lock, flags);

  klogf("[OK] LinuxKPI: drm_client fb console ready for %s (%ux%u pitch=%u)\n",
        dev->driver ? dev->driver->name : "drm", fb_w, fb_h, buffer->pitch);
  return 0;
}

static int kpi_drm_client_update(struct drm_client_dev *client) {
  struct kpi_drm_console *con = container_of(client, struct kpi_drm_console, client);
  struct drm_device *dev = client->dev;
  int ret;

  /* Leave the framebuffer intact while a userspace master owns the display.
   * A hotplug error would suppress future client notifications. Lastclose
   * calls restore once the device is available again. */
  if (!drm_master_internal_acquire(dev))
    return 0;

  ret = kpi_drm_client_setup_modeset(con);
  drm_master_internal_release(dev);
  return ret;
}

static void kpi_drm_client_unregister(struct drm_client_dev *client) {
  struct kpi_drm_console *con = container_of(client, struct kpi_drm_console, client);
  unsigned long flags;

  spin_lock_irqsave(&kpi_console_lock, flags);
  con->active = false;
  if (kpi_console_client == con)
    kpi_console_client = NULL;
  spin_unlock_irqrestore(&kpi_console_lock, flags);

  if (con->buffer) {
    if (!iosys_map_is_null(&con->map)) {
      drm_client_buffer_vunmap(con->buffer);
      iosys_map_clear(&con->map);
    }
    drm_client_framebuffer_delete(con->buffer);
    con->buffer = NULL;
  }
  drm_client_release(client);
  kfree(con);
}

static const struct drm_client_funcs kpi_drm_client_funcs = {
    .owner = THIS_MODULE,
    .unregister = kpi_drm_client_unregister,
    .restore = kpi_drm_client_update,
    .hotplug = kpi_drm_client_update,
};

void drm_client_setup(struct drm_device *dev,
                      const struct drm_format_info *format) {
  struct kpi_drm_console *con;
  int ret;

  (void)format;
  if (!dev || !dev->driver || !dev->driver->dumb_create ||
      !drm_core_check_feature(dev, DRIVER_MODESET))
    return;

  if (dev->driver->name && !strcmp(dev->driver->name, "vkms"))
    return;

  con = kzalloc(sizeof(*con), GFP_KERNEL);
  if (!con)
    return;

  ret = drm_client_init(dev, &con->client, "avoryfb", &kpi_drm_client_funcs);
  if (ret) {
    kfree(con);
    return;
  }

  drm_client_register(&con->client);
}

void drm_client_setup_with_fourcc(struct drm_device *dev, u32 fourcc) {
  (void)fourcc;
  drm_client_setup(dev, NULL);
}

void drm_client_setup_with_color_mode(struct drm_device *dev,
                                      unsigned int color_mode) {
  (void)color_mode;
  drm_client_setup(dev, NULL);
}

static bool kpi_drm_console_can_blit(const struct kpi_drm_console *con) {
  _Bool is_gfx = 0;

  if (!con || !con->active)
    return false;

  (void)linuxkpi_native_fb_snapshot(NULL, NULL, NULL, NULL, &is_gfx);
  return !is_gfx &&
         __atomic_load_n(&con->client.dev->master, __ATOMIC_RELAXED) == NULL;
}

_Bool linuxkpi_drm_console_owns_scanout(void) {
  unsigned long flags;
  bool owns;

  spin_lock_irqsave(&kpi_console_lock, flags);
  owns = kpi_drm_console_can_blit(kpi_console_client);
  spin_unlock_irqrestore(&kpi_console_lock, flags);
  return owns;
}

void linuxkpi_drm_console_blit_rect(const void *src_base, unsigned int src_w,
                                    unsigned int src_h, unsigned int src_pitch,
                                    unsigned int src_y_off,
                                    unsigned int dst_x, unsigned int dst_y,
                                    unsigned int w, unsigned int h) {
  struct kpi_drm_console *con;
  unsigned long flags;

  spin_lock_irqsave(&kpi_console_lock, flags);
  con = kpi_console_client;
  if (kpi_drm_console_can_blit(con)) {
    kpi_drm_console_blit(con, src_base, src_w, src_h, src_pitch, src_y_off,
                        dst_x, dst_y, w, h);
    /* Drain the WC stores before the writer can unmap the aperture. */
    wmb();
  }
  spin_unlock_irqrestore(&kpi_console_lock, flags);
}
