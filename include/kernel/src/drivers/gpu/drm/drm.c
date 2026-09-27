#include "drm.h"
#include "drivers/gpu/virtio_gpu/virtio_gpu.h"
#include "../../../apic/lapic_timer.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"

struct drm_device global_drm_dev;
static struct drm_stats drm_perf_stats;

static inline uint64_t drm_read_cycles(void) {
  uint32_t lo, hi;
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
  return ((uint64_t)hi << 32) | lo;
}

void drm_stats_snapshot(struct drm_stats *out) {
  if (!out)
    return;
  spinlock_acquire(&global_drm_dev.lock);
  *out = drm_perf_stats;
  spinlock_release(&global_drm_dev.lock);
}

/* ── External symbols ────────────────────────────────────────────────────── */
extern struct drm_gem_object *drm_gem_object_create(struct drm_device *dev,
                                                    size_t size);
extern void drm_gem_object_free(struct drm_device *dev,
                                struct drm_gem_object *obj);
extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev,
                                                     uint32_t handle);
extern void drm_kms_init(struct drm_device *dev);
extern struct drm_framebuffer *
drm_framebuffer_create(struct drm_device *dev, struct drm_mode_fb_cmd *cmd);
extern void drm_framebuffer_free(struct drm_device *dev,
                                 struct drm_framebuffer *fb);
extern void epoll_notify_event(struct vfs_node *node, uint32_t events);

static uint32_t drm_event_sequence = 1;

static void drm_fill_vblank_event(struct drm_event_vblank *ev,
                                  uint32_t crtc_id) {
  uint64_t ms = lapic_timer_get_ms();
  ev->tv_sec = (uint32_t)(ms / 1000);
  ev->tv_usec = (uint32_t)((ms % 1000) * 1000);
  ev->sequence = drm_event_sequence++;
  ev->crtc_id = crtc_id;
}

/* drm_prop.c */
extern int drm_ioctl_obj_getprops(struct drm_device *dev, uint64_t arg);
extern int drm_ioctl_getproperty(struct drm_device *dev, uint64_t arg);
extern int drm_ioctl_atomic(struct vfs_node *node, struct drm_file *file,
                            struct drm_device *dev, uint64_t arg);
extern struct drm_prop_blob *drm_blob_create(struct drm_device *dev,
                                             const void *data, uint32_t length);
extern struct drm_prop_blob *drm_blob_find(struct drm_device *dev, uint32_t id);
extern void drm_blob_destroy(struct drm_device *dev, uint32_t id);

/* drm_file.c */
extern struct drm_file *drm_file_alloc(struct drm_device *dev);
extern void drm_file_free(struct drm_file *file);
extern uint32_t drm_file_gem_register(struct drm_file *file,
                                      struct drm_gem_object *obj);
extern struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file,
                                                  uint32_t handle);
extern void drm_file_gem_release(struct drm_file *file, uint32_t handle);
extern void drm_file_send_event(struct drm_file *file,
                                struct drm_event_vblank *ev,
                                struct vfs_node *node);
extern int drm_prime_export(struct drm_gem_object *obj);
extern struct drm_gem_object *drm_prime_import(int prime_fd);

/* drm_fb.c */
extern int drm_ioctl_addfb2(struct drm_file *file, struct drm_device *dev,
                            uint64_t arg);

/* ── virtio-gpu hook pointers (NULL = use Limine software blit) ──────────── */
drm_commit_damage_fn_t g_drm_commit_damage_fn = NULL;

drm_pageflip_fn_t g_drm_pageflip_fn = NULL;

drm_cursor_fn_t g_drm_cursor_fn = NULL;

void drm_register_cursor_backend(drm_cursor_fn_t cursor) {
  g_drm_cursor_fn = cursor;
}

drm_create_dumb_fn_t g_drm_create_dumb_fn = NULL;

typedef void (*drm_set_fb_fn_t)(uint32_t crtc_id, struct drm_framebuffer *fb);
drm_set_fb_fn_t g_drm_set_fb_fn = NULL;

drm_get_modes_fn_t g_drm_get_modes_fn = NULL;

void drm_register_scanout_backend(drm_create_dumb_fn_t create_dumb,
                                  drm_commit_damage_fn_t commit_damage,
                                  drm_get_modes_fn_t get_modes,
                                  drm_pageflip_fn_t pageflip) {
  g_drm_create_dumb_fn = create_dumb;
  g_drm_commit_damage_fn = commit_damage;
  g_drm_get_modes_fn = get_modes;
  g_drm_pageflip_fn = pageflip;
}

/* ── Helper: get drm_file from node->device ──────────────────────────────── */
/*
 * The VFS node's device pointer is set to drm_file* on open (per-client node).
 * For the template node (before first open), device points to drm_device.
 * We distinguish by checking whether the pointer is the global device.
 */
static inline struct drm_file *node_to_file(struct vfs_node *node) {
  return (struct drm_file *)node->device;
}

static struct drm_mode_object *drm_mode_object_find(struct drm_device *dev,
                                                    uint32_t id) {
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->id == id)
      return obj;
  }
  return NULL;
}

/* ── Per-client open / close ─────────────────────────────────────────────── */

/*
 * drm_open is called by vfs_open() on the per-client clone node.
 * The clone's device pointer is already set to the drm_file by drm_dri_finddir.
 */
static void drm_open(struct vfs_node *node) {
  /* Per-client DRM nodes use normal VFS fd reference counting. */
  (void)node;
}

static void drm_close(struct vfs_node *node) {
  struct drm_file *file = node_to_file(node);
  if (file)
    drm_file_free(file);
  /* node itself is freed by vfs_close since it's non-persistent */
}

/* ── Display Commit (Software Blit) ─────────────────────────────────────── */
static bool drm_cursor_damage_rect(const struct drm_plane *cursor,
                                   struct drm_clip_rect *clip) {
  if (!cursor || !cursor->fb || !clip)
    return false;

  int64_t x1 = (int64_t)cursor->crtc_x - cursor->hotspot_x;
  int64_t y1 = (int64_t)cursor->crtc_y - cursor->hotspot_y;
  uint32_t width = cursor->crtc_w ? cursor->crtc_w : cursor->fb->width;
  uint32_t height = cursor->crtc_h ? cursor->crtc_h : cursor->fb->height;
  int64_t x2 = x1 + width;
  int64_t y2 = y1 + height;

  if (x2 <= 0 || y2 <= 0 || x1 >= UINT16_MAX || y1 >= UINT16_MAX)
    return false;
  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x2 > UINT16_MAX)
    x2 = UINT16_MAX;
  if (y2 > UINT16_MAX)
    y2 = UINT16_MAX;
  if (x2 <= x1 || y2 <= y1)
    return false;

  clip->x1 = (uint16_t)x1;
  clip->y1 = (uint16_t)y1;
  clip->x2 = (uint16_t)x2;
  clip->y2 = (uint16_t)y2;
  return true;
}

static bool drm_damage_intersects(const struct drm_clip_rect *a,
                                  const struct drm_clip_rect *b) {
  return a->x1 <= b->x2 && b->x1 <= a->x2 &&
         a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void drm_commit_damage(struct drm_device *dev,
                              const struct drm_clip_rect *clips,
                              uint32_t num_clips, uint32_t target_fb_id) {
  /* Native backends still feed the common commit counters. */
  if (g_drm_commit_damage_fn) {
    if (dev) {
      spinlock_acquire(&dev->lock);
      drm_perf_stats.commits++;
      drm_perf_stats.direct_scanout_commits++;
      if (clips && num_clips) drm_perf_stats.damage_commits++;
      else drm_perf_stats.full_commits++;
      spinlock_release(&dev->lock);
    }
    g_drm_commit_damage_fn(dev, clips, num_clips, target_fb_id);
    return;
  }
  if (!dev)
    return;
  spinlock_acquire(&dev->lock);
  uint64_t start_cycles = drm_read_cycles();
  uint64_t copied_bytes = 0;
  bool wrote_wc = false;
  bool saw_direct_scanout = false;

  drm_perf_stats.commits++;
  if (clips && num_clips)
    drm_perf_stats.damage_commits++;
  else
    drm_perf_stats.full_commits++;

  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type == DRM_MODE_OBJECT_CRTC) {
      struct drm_crtc *crtc = (struct drm_crtc *)obj;
      if ((!target_fb_id || (crtc->fb && crtc->fb->base.id == target_fb_id)) &&
          crtc->fb && crtc->fb->gem_obj && crtc->fb->gem_obj->virt_addr) {
        void *hw_fb = fb_get_base();
        if (hw_fb) {
          uint32_t width = fb_get_width();
          uint32_t height = fb_get_height();
          uint32_t hw_pitch = fb_get_pitch();
          if (width > crtc->fb->width)
            width = crtc->fb->width;
          if (height > crtc->fb->height)
            height = crtc->fb->height;
          uint32_t sw_pitch = crtc->fb->pitch;

          bool direct_scanout = crtc->fb->gem_obj->virt_addr == hw_fb &&
                                sw_pitch == hw_pitch;
          if (direct_scanout)
            saw_direct_scanout = true;

          if (!direct_scanout && clips && num_clips) {
            struct drm_damage_span { uint32_t x1, x2; };
            struct drm_damage_span spans[64];
            uint32_t cpp = crtc->fb->bpp / 8;
            if (!cpp) cpp = 4;
            uint32_t damage_y1 = height, damage_y2 = 0;
            for (uint32_t i = 0; i < num_clips; i++) {
              uint32_t y1 = clips[i].y1;
              uint32_t y2 = clips[i].y2;
              if (y1 > height) y1 = height;
              if (y2 > height) y2 = height;
              if (y2 <= y1) continue;
              if (y1 < damage_y1) damage_y1 = y1;
              if (y2 > damage_y2) damage_y2 = y2;
            }

            /* Build a union of the damage on each scanline.  Xorg can send
             * overlapping clips; copying each rectangle independently writes
             * those pixels to the WC scanout more than once. */
            for (uint32_t y = damage_y1; y < damage_y2; y++) {
              uint32_t span_count = 0;
              for (uint32_t i = 0; i < num_clips; i++) {
                if (y < clips[i].y1 || y >= clips[i].y2)
                  continue;
                uint32_t x1 = clips[i].x1;
                uint32_t x2 = clips[i].x2;
                if (x1 > width) x1 = width;
                if (x2 > width) x2 = width;
                if (x2 <= x1) continue;

                uint32_t pos = 0;
                while (pos < span_count && spans[pos].x2 < x1)
                  pos++;
                uint32_t end = pos;
                while (end < span_count && spans[end].x1 <= x2) {
                  if (spans[end].x1 < x1) x1 = spans[end].x1;
                  if (spans[end].x2 > x2) x2 = spans[end].x2;
                  end++;
                }

                if (end > pos) {
                  spans[pos].x1 = x1;
                  spans[pos].x2 = x2;
                  uint32_t remove = end - pos - 1;
                  for (uint32_t j = end; j < span_count; j++)
                    spans[j - remove] = spans[j];
                  span_count -= remove;
                } else if (span_count < 64) {
                  for (uint32_t j = span_count; j > pos; j--)
                    spans[j] = spans[j - 1];
                  spans[pos].x1 = x1;
                  spans[pos].x2 = x2;
                  span_count++;
                } else {
                  /* Pathological fragmentation: one bounding span still
                   * avoids a full-height framebuffer copy. */
                  uint32_t bx1 = x1, bx2 = x2;
                  for (uint32_t j = 0; j < span_count; j++) {
                    if (spans[j].x1 < bx1) bx1 = spans[j].x1;
                    if (spans[j].x2 > bx2) bx2 = spans[j].x2;
                  }
                  spans[0].x1 = bx1;
                  spans[0].x2 = bx2;
                  span_count = 1;
                }
              }

              for (uint32_t i = 0; i < span_count; i++) {
                size_t xoff = (size_t)spans[i].x1 * cpp;
                if (xoff >= hw_pitch || xoff >= sw_pitch) continue;
                size_t line_bytes = (size_t)(spans[i].x2 - spans[i].x1) * cpp;
                if (line_bytes > hw_pitch - xoff)
                  line_bytes = hw_pitch - xoff;
                if (line_bytes > sw_pitch - xoff)
                  line_bytes = sw_pitch - xoff;
                if (!line_bytes) continue;
                memcpy_to_wc((uint8_t *)hw_fb + (size_t)y * hw_pitch + xoff,
                             (uint8_t *)crtc->fb->gem_obj->virt_addr +
                                 (size_t)y * sw_pitch + xoff,
                             line_bytes);
                copied_bytes += line_bytes;
                wrote_wc = true;
              }
            }
          } else if (!direct_scanout && hw_pitch == sw_pitch) {
#if DRM_DEBUG_LOGGING
            klog_puts("[DRM] Blit: fast copy, size=");
            klog_uint64((size_t)height * hw_pitch);
            klog_puts("\n");

            /* Check if the first few pixels are non-zero to see if anything is
             * being rendered */
            uint32_t *pixels = (uint32_t *)crtc->fb->gem_obj->virt_addr;
            bool all_zero = true;
            for (int i = 0; i < 16; i++) {
              if (pixels[i] != 0) {
                all_zero = false;
                break;
              }
            }
            if (all_zero) {
              klog_puts("[DRM] Warning: first 16 pixels are zero\n");
            } else {
              klog_puts("[DRM] Info: first pixels are non-zero: ");
              klog_hex32(pixels[0]);
              klog_puts("\n");
            }
#endif

            memcpy_to_wc(hw_fb, crtc->fb->gem_obj->virt_addr,
                         (size_t)height * hw_pitch);
            copied_bytes += (uint64_t)height * hw_pitch;
            wrote_wc = true;
          } else if (!direct_scanout) {
            uint32_t copy_len = width * 4;
            if (copy_len > hw_pitch)
              copy_len = hw_pitch;
            if (copy_len > sw_pitch)
              copy_len = sw_pitch;

#if DRM_DEBUG_LOGGING
            klog_puts("[DRM] Blit: line copy (pitch mismatch), lines=");
            klog_uint64(height);
            klog_puts("\n");

            /* Check if the first few pixels are non-zero to see if anything is
             * being rendered */
            uint32_t *pixels = (uint32_t *)crtc->fb->gem_obj->virt_addr;
            bool all_zero = true;
            for (int i = 0; i < 16; i++) {
              if (pixels[i] != 0) {
                all_zero = false;
                break;
              }
            }
            if (all_zero) {
              klog_puts("[DRM] Warning: first 16 pixels are zero\n");
            } else {
              klog_puts("[DRM] Info: first pixels are non-zero: ");
              klog_hex32(pixels[0]);
              klog_puts("\n");
            }
#endif

            for (uint32_t y = 0; y < height; y++) {
              memcpy_to_wc(
                  (uint8_t *)hw_fb + y * hw_pitch,
                  (uint8_t *)crtc->fb->gem_obj->virt_addr + y * sw_pitch,
                  copy_len);
            }
            copied_bytes += (uint64_t)copy_len * height;
            wrote_wc = true;
          }

          struct drm_plane *cursor = crtc->cursor;
          if (!g_drm_cursor_fn && cursor && cursor->fb && cursor->fb->gem_obj &&
              cursor->fb->gem_obj->virt_addr && cursor->fb->bpp == 32 &&
              cursor->fb->gem_obj->cache_mode == DRM_GEM_CACHE_WB &&
              crtc->fb->bpp == 32 &&
              crtc->fb->gem_obj->cache_mode == DRM_GEM_CACHE_WB) {
            uint32_t src_x = cursor->src_x >> 16;
            uint32_t src_y = cursor->src_y >> 16;
            uint32_t src_w = cursor->src_w ? (cursor->src_w >> 16) : 0;
            uint32_t src_h = cursor->src_h ? (cursor->src_h >> 16) : 0;

            if (src_x >= cursor->fb->width || src_y >= cursor->fb->height)
              goto cursor_done;

            uint32_t max_w = cursor->fb->width - src_x;
            uint32_t max_h = cursor->fb->height - src_y;
            uint32_t cw =
                cursor->crtc_w ? cursor->crtc_w : (src_w ? src_w : max_w);
            uint32_t ch =
                cursor->crtc_h ? cursor->crtc_h : (src_h ? src_h : max_h);
            if (cw > max_w)
              cw = max_w;
            if (ch > max_h)
              ch = max_h;

            int32_t cx = cursor->crtc_x - cursor->hotspot_x;
            int32_t cy = cursor->crtc_y - cursor->hotspot_y;
            uint8_t *src_base = (uint8_t *)cursor->fb->gem_obj->virt_addr;
            uint8_t *primary_base =
                (uint8_t *)crtc->fb->gem_obj->virt_addr;
            uint8_t *dst_base = (uint8_t *)hw_fb;
            for (uint32_t sy = 0; sy < ch; sy++) {
              int32_t dy = cy + (int32_t)sy;
              if (dy < 0 || dy >= (int32_t)height ||
                  dy >= (int32_t)crtc->fb->height)
                continue;
              uint32_t *src =
                  (uint32_t *)(src_base + (src_y + sy) * cursor->fb->pitch) +
                  src_x;
              uint32_t *background =
                  (uint32_t *)(primary_base + (uint32_t)dy * sw_pitch);
              uint32_t *dst = (uint32_t *)(dst_base + (uint32_t)dy * hw_pitch);
              for (uint32_t sx = 0; sx < cw; sx++) {
                int32_t dx = cx + (int32_t)sx;
                if (dx < 0 || dx >= (int32_t)width ||
                    dx >= (int32_t)crtc->fb->width)
                  continue;
                uint32_t sp = src[sx];
                uint32_t a = sp >> 24;
                if (a == 0)
                  continue;
                if (a == 255) {
                  dst[dx] = sp;
                } else {
                  /* Never read the WC scanout. Blend over the WB primary. */
                  uint32_t dp = background[dx];
                  uint32_t sr = (sp >> 16) & 0xff;
                  uint32_t sg = (sp >> 8) & 0xff;
                  uint32_t sb = sp & 0xff;
                  uint32_t dr = (dp >> 16) & 0xff;
                  uint32_t dg = (dp >> 8) & 0xff;
                  uint32_t db = dp & 0xff;
                  uint32_t r = (sr * a + dr * (255 - a)) / 255;
                  uint32_t g = (sg * a + dg * (255 - a)) / 255;
                  uint32_t b = (sb * a + db * (255 - a)) / 255;
                  dst[dx] = 0xff000000 | (r << 16) | (g << 8) | b;
                }
                copied_bytes += sizeof(uint32_t);
                wrote_wc = true;
              }
            }
          cursor_done:;
          }
        }
      }
    }
  }

  if (wrote_wc) {
    __asm__ volatile("sfence" ::: "memory");
    uint64_t elapsed = drm_read_cycles() - start_cycles;
    drm_perf_stats.copy_batches++;
    drm_perf_stats.bytes_copied += copied_bytes;
    drm_perf_stats.copy_cycles += elapsed;
    if (elapsed > drm_perf_stats.max_copy_cycles)
      drm_perf_stats.max_copy_cycles = elapsed;
  } else if (!saw_direct_scanout) {
    drm_perf_stats.empty_commits++;
  }
  if (saw_direct_scanout)
    drm_perf_stats.direct_scanout_commits++;

  spinlock_release(&dev->lock);
}

static void drm_sync_cursor_backend(struct drm_device *dev, uint32_t flags) {
  if (!g_drm_cursor_fn || !dev) return;
  uint32_t crtc_id=0,width=0,height=0,pitch=0;int32_t x=0,y=0,hot_x=0,hot_y=0;
  struct drm_gem_object *gem=NULL;
  spinlock_acquire(&dev->lock);
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type != DRM_MODE_OBJECT_CRTC) continue;
    struct drm_crtc *crtc=(struct drm_crtc *)obj;struct drm_plane *cursor=crtc->cursor;
    crtc_id=crtc->base.id;
    if(cursor){x=cursor->crtc_x;y=cursor->crtc_y;hot_x=cursor->hotspot_x;hot_y=cursor->hotspot_y;
      width=cursor->crtc_w;height=cursor->crtc_h;if(cursor->fb){gem=cursor->fb->gem_obj;pitch=cursor->fb->pitch;if(!width)width=cursor->fb->width;if(!height)height=cursor->fb->height;}}
    break;
  }
  spinlock_release(&dev->lock);
  if(crtc_id)g_drm_cursor_fn(crtc_id,gem,width,height,pitch,x,y,hot_x,hot_y,flags);
}

static void drm_commit_cursor_damage(struct drm_device *dev,
                                     const struct drm_clip_rect *old_damage,
                                     bool have_old_damage,
                                     const struct drm_clip_rect *new_damage,
                                     bool have_new_damage) {
  if (g_drm_cursor_fn) return;
  struct drm_clip_rect clips[2];
  uint32_t count = 0;

  if (have_old_damage)
    clips[count++] = *old_damage;
  if (have_new_damage) {
    if (count && drm_damage_intersects(&clips[0], new_damage)) {
      if (new_damage->x1 < clips[0].x1) clips[0].x1 = new_damage->x1;
      if (new_damage->y1 < clips[0].y1) clips[0].y1 = new_damage->y1;
      if (new_damage->x2 > clips[0].x2) clips[0].x2 = new_damage->x2;
      if (new_damage->y2 > clips[0].y2) clips[0].y2 = new_damage->y2;
    } else {
      clips[count++] = *new_damage;
    }
  }

  if (count)
    drm_commit_damage(dev, clips, count, 0);
}

static void drm_commit_heads(struct drm_device *dev,const struct drm_clip_rect *clips,uint32_t count){uint32_t ids[16],n=0;spinlock_acquire(&dev->lock);struct drm_mode_object *o;list_for_each_entry(o,&dev->kms_objects,list)if(o->type==DRM_MODE_OBJECT_CRTC&&((struct drm_crtc*)o)->fb&&n<16)ids[n++]=o->id;spinlock_release(&dev->lock);for(uint32_t i=0;i<n;i++)drm_commit_damage(dev,clips,count,ids[i]);}

/* PAGE_FLIP has no damage payload. DIRTYFB may have described changes to a
 * back buffer before it became active, so consume that cached damage here.
 * A buffer that has never been scanned out remains a full upload: the host
 * has no valid contents for it yet. */
static void drm_commit_flipped_crtc(struct drm_device *dev, uint32_t crtc_id) {
  struct drm_clip_rect damage;
  bool have_damage = false;

  spinlock_acquire(&dev->lock);
  struct drm_mode_object *obj = drm_mode_object_find(dev, crtc_id);
  if (obj && obj->type == DRM_MODE_OBJECT_CRTC) {
    struct drm_crtc *crtc = (struct drm_crtc *)obj;
    struct drm_framebuffer *fb = crtc->fb;
    if (fb) {
      if (fb->scanout_valid && fb->pending_damage_valid) {
        damage = fb->pending_damage;
        have_damage = true;
      }
      fb->pending_damage_valid = 0;
      fb->scanout_valid = 1;
    }
  }
  spinlock_release(&dev->lock);

  drm_commit_damage(dev, have_damage ? &damage : NULL, have_damage ? 1 : 0,
                    crtc_id);
}

static void drm_commit(struct drm_device *dev) {
  drm_commit_heads(dev,NULL,0);
}

/* Store legacy damage on the framebuffer. If it is already visible, submit
 * it immediately; otherwise PAGE_FLIP will submit it when that buffer is
 * selected. */
static int drm_dirtyfb(struct drm_device *dev,
                       const struct drm_mode_fb_dirty_cmd *dirty) {
  if (!dirty)
    return -14;
  if (dirty->num_clips > 4096)
    return -22;

  struct drm_clip_rect damage = {0};
  bool have_damage = false;
  if (dirty->num_clips && dirty->clips_ptr) {
    const struct drm_clip_rect *clips =
        (const struct drm_clip_rect *)dirty->clips_ptr;
    uint32_t x1 = UINT16_MAX, y1 = UINT16_MAX, x2 = 0, y2 = 0;
    for (uint32_t i = 0; i < dirty->num_clips; i++) {
      if (clips[i].x2 <= clips[i].x1 || clips[i].y2 <= clips[i].y1)
        continue;
      if (clips[i].x1 < x1) x1 = clips[i].x1;
      if (clips[i].y1 < y1) y1 = clips[i].y1;
      if (clips[i].x2 > x2) x2 = clips[i].x2;
      if (clips[i].y2 > y2) y2 = clips[i].y2;
    }
    if (x1 < x2 && y1 < y2) {
      damage.x1 = (uint16_t)x1;
      damage.y1 = (uint16_t)y1;
      damage.x2 = (uint16_t)x2;
      damage.y2 = (uint16_t)y2;
      have_damage = true;
    }
  }

  bool active = false;
  spinlock_acquire(&dev->lock);
  struct drm_mode_object *obj = drm_mode_object_find(dev, dirty->fb_id);
  if (!obj || obj->type != DRM_MODE_OBJECT_FB) {
    spinlock_release(&dev->lock);
    return -2;
  }
  struct drm_framebuffer *fb = (struct drm_framebuffer *)obj;
  if (have_damage) {
    if (damage.x2 > fb->width) damage.x2 = fb->width;
    if (damage.y2 > fb->height) damage.y2 = fb->height;
    if (damage.x1 < damage.x2 && damage.y1 < damage.y2) {
      if (fb->pending_damage_valid) {
        if (damage.x1 < fb->pending_damage.x1) fb->pending_damage.x1 = damage.x1;
        if (damage.y1 < fb->pending_damage.y1) fb->pending_damage.y1 = damage.y1;
        if (damage.x2 > fb->pending_damage.x2) fb->pending_damage.x2 = damage.x2;
        if (damage.y2 > fb->pending_damage.y2) fb->pending_damage.y2 = damage.y2;
      } else {
        fb->pending_damage = damage;
        fb->pending_damage_valid = 1;
      }
    }
  }
  struct drm_mode_object *iter;
  list_for_each_entry(iter, &dev->kms_objects, list) {
    if (iter->type == DRM_MODE_OBJECT_CRTC &&
        ((struct drm_crtc *)iter)->fb == fb) {
      active = true;
      break;
    }
  }
  if (active) {
    if (have_damage)
      damage = fb->pending_damage;
    /* An empty or malformed DIRTYFB is conservatively a full update; do not
     * let a region from an earlier frame turn its following flip into a
     * partial upload. */
    fb->pending_damage_valid = 0;
    fb->scanout_valid = 1;
  }
  spinlock_release(&dev->lock);

  if (active)
    drm_commit_damage(dev, have_damage ? &damage : NULL, have_damage ? 1 : 0,
                      dirty->fb_id);
  return 0;
}

/* Consume damage recorded while applying the immediately preceding atomic
 * request.  If userspace did not provide FB_DAMAGE_CLIPS, preserve the
 * conservative full-frame fallback. */
static void drm_commit_atomic(struct drm_device *dev) {
  struct drm_clip_rect damage;
  bool have_damage = false;

  spinlock_acquire(&dev->lock);
  if (dev->pending_damage_valid) {
    damage = dev->pending_damage;
    dev->pending_damage_valid = 0;
    have_damage = true;
  }
  spinlock_release(&dev->lock);

  if (have_damage)
    drm_commit_heads(dev, &damage, 1);
  else
    drm_commit(dev);
}

/* ── Per-client open / close ─────────────────────────────────────────────── */

static int drm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  struct drm_file *file = node_to_file(node);
  struct drm_device *dev = file ? file->dev : NULL;
  if (!dev)
    return -9; /* EBADF */

#if DRM_DEBUG_LOGGING
  klog_debug_puts("[DRM] ioctl request=0x");
  klog_debug_hex32(request);
  klog_debug_puts("\n");
#endif

  switch (request) {

  /* ── Version / caps ──────────────────────────────────────────────── */
  case DRM_IOCTL_VERSION: {
    klog_debug_puts("[DRM] VERSION ioctl\n");
    struct drm_version *v = (struct drm_version *)arg;
    static const char ascent_name[] = "ascentdrm";
    const char *name = ascent_name;
    static const char date[] = "20260706";
    static const char desc[] = "AscentOS DRM/KMS";
    size_t name_len = v->name_len;
    size_t date_len = v->date_len;
    size_t desc_len = v->desc_len;

    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;

    if (v->name && name_len > 0) {
      size_t copy = (strlen(name) < name_len) ? strlen(name) : name_len;
      memcpy(v->name, name, copy);
      if (copy < name_len)
        v->name[copy] = '\0';
    }
    if (v->date && date_len > 0) {
      size_t copy = (sizeof(date) - 1 < date_len) ? sizeof(date) - 1 : date_len;
      memcpy(v->date, date, copy);
      if (copy < date_len)
        v->date[copy] = '\0';
    }
    if (v->desc && desc_len > 0) {
      size_t copy = (sizeof(desc) - 1 < desc_len) ? sizeof(desc) - 1 : desc_len;
      memcpy(v->desc, desc, copy);
      if (copy < desc_len)
        v->desc[copy] = '\0';
    }

    v->name_len = strlen(name);
    v->date_len = sizeof(date) - 1;
    v->desc_len = sizeof(desc) - 1;
    return 0;
  }
  case DRM_IOCTL_GET_UNIQUE: {
    klog_debug_puts("[DRM] GET_UNIQUE ioctl\n");
    struct drm_unique *u = (struct drm_unique *)arg;
    static const char busid[] = "platform:ascentdrm:0";
    size_t unique_len = u->unique_len;

    if (u->unique && unique_len > 0) {
      size_t copy =
          (sizeof(busid) - 1 < unique_len) ? sizeof(busid) - 1 : unique_len;
      memcpy(u->unique, busid, copy);
      if (copy < unique_len)
        u->unique[copy] = '\0';
    }

    u->unique_len = sizeof(busid) - 1;
    return 0;
  }
  case DRM_IOCTL_SET_VERSION: {
    klog_debug_puts("[DRM] SET_VERSION ioctl\n");
    struct drm_set_version *sv = (struct drm_set_version *)arg;
    sv->drm_di_major = 1;
    sv->drm_di_minor = 4;
    sv->drm_dd_major = 1;
    sv->drm_dd_minor = 0;
    return 0;
  }
  case DRM_IOCTL_GET_CAP: {
    struct drm_get_cap *cap = (struct drm_get_cap *)arg;
    klog_debug_puts("[DRM] GET_CAP capability=0x");
    klog_debug_hex64(cap->capability);
    klog_debug_puts("\n");
    switch (cap->capability) {
    case DRM_CAP_DUMB_BUFFER:
      cap->value = 1;
      break;
    case DRM_CAP_VBLANK_HIGH_CRTC:
      cap->value = 1;
      break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
      /* VirtIO GEM buffers are normal WB memory and map directly. */
      cap->value = 0;
      break;
    case DRM_CAP_PRIME:
      cap->value = 3;
      break; /* (IMPORT | EXPORT) */
    case DRM_CAP_TIMESTAMP_MONOTONIC:
      cap->value = 1;
      break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
      cap->value = 0;
      break;
    case 0x12:
      cap->value = 1;
      break; /* CRTC_IN_VBLANK_EVENT */
    case 0x13:
      cap->value = 0;
      break; /* SYNCOBJ not implemented */
    case 0x14:
      cap->value = 0;
      break; /* SYNCOBJ_TIMELINE not implemented */
    case 0x15:
      cap->value = 0;
      break; /* DRM_CAP_PAGE_FLIP_TARGET not implemented */
    case DRM_CAP_ADDFB2_MODIFIERS:
      cap->value = 0;
      break;
    case DRM_CAP_CURSOR_WIDTH:
      cap->value = 64;
      break;
    case DRM_CAP_CURSOR_HEIGHT:
      cap->value = 64;
      break;
    case DRM_CAP_ATOMIC:
      cap->value = 1;
      break;
    case 0x11:
      cap->value = 0;
      break; /* DRM_CAP_LESSOR (not supported) */
    default:
      cap->value = 0;
      break;
    }
    klog_debug_puts("[DRM] GET_CAP value=0x");
    klog_debug_hex64(cap->value);
    klog_debug_puts("\n");
    return 0;
  }
  case 0x4010640D: /* DRM_IOCTL_SET_CLIENT_CAP */ {
    struct drm_set_client_cap *cap = (struct drm_set_client_cap *)arg;
    klog_debug_puts("[DRM] SET_CLIENT_CAP cap=");
    klog_debug_uint64(cap->capability);
    klog_debug_puts(" val=");
    klog_debug_uint64(cap->value);
    klog_debug_puts("\n");
    uint32_t bit = 0;
    switch (cap->capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
      bit = DRM_FILE_CAP_STEREO_3D;
      break;
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
      bit = DRM_FILE_CAP_UNIVERSAL_PLANES;
      break;
    case DRM_CLIENT_CAP_ATOMIC:
      /* Linux DRM makes ATOMIC imply universal planes and aspect-ratio modes.
       */
      bit = DRM_FILE_CAP_ATOMIC | DRM_FILE_CAP_UNIVERSAL_PLANES |
            DRM_FILE_CAP_ASPECT_RATIO;
      break;
    case DRM_CLIENT_CAP_ASPECT_RATIO:
      bit = DRM_FILE_CAP_ASPECT_RATIO;
      break;
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
      /* No writeback connectors exist, but enabling visibility is harmless. */
      bit = DRM_FILE_CAP_WRITEBACK_CONNECTORS;
      break;
    case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
      bit = DRM_FILE_CAP_CURSOR_PLANE_HOTSPOT;
      break;
    default:
      klog_debug_puts("[DRM] SET_CLIENT_CAP unsupported\n");
      return -95; /* EOPNOTSUPP */
    }

    if (cap->value)
      file->client_caps |= bit;
    else
      file->client_caps &= ~bit;
    return 0;
  }
  case DRM_IOCTL_SET_MASTER:
    file->is_master = 1;
    return 0;
  case DRM_IOCTL_DROP_MASTER:
    file->is_master = 0;
    return 0;
  case 0x40046411: /* DRM_IOCTL_AUTH_MAGIC */
    return 0;
  case 0x80046402: /* DRM_IOCTL_GET_MAGIC */ {
    uint32_t *magic = (uint32_t *)arg;
    *magic = 0x1234; /* dummy magic */
    return 0;
  }
  case 0x80086406: /* DRM_IOCTL_GET_STATS */
    klog_debug_puts("[DRM] GET_STATS -> ENOTTY\n");
    return -25; /* ENOTTY */
  case DRM_IOCTL_MODE_CREATE_LEASE:
    klog_debug_puts("[DRM] MODE_CREATE_LEASE -> EINVAL\n");
    return -22; /* EINVAL: Leasing not supported on this driver version */

  /* ── Per-client GEM ──────────────────────────────────────────────── */
  case DRM_IOCTL_GEM_CREATE: {
    struct drm_gem_create *c = (struct drm_gem_create *)arg;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -12; /* ENOMEM */
    /* Register in global list AND per-client table */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h) {
      drm_gem_object_free(dev, obj);
      return -12;
    }
    obj->refcount--; /* transfer creator ownership to the handle */
    c->handle = local_h;
    klog_debug_puts("[DRM] GEM_CREATE size=");
    klog_debug_uint64(c->size);
    klog_debug_puts(" handle=");
    klog_debug_uint64(c->handle);
    klog_debug_puts(" phys=0x");
    klog_debug_hex64(obj->phys_addr);
    klog_debug_puts("\n");
    return 0;
  }
  case DRM_IOCTL_GEM_CLOSE: {
    struct drm_gem_free *f = (struct drm_gem_free *)arg;
    drm_file_gem_release(file, f->handle);
    return 0;
  }
  case DRM_IOCTL_GEM_FREE: {
    struct drm_gem_free *f = (struct drm_gem_free *)arg;
    drm_file_gem_release(file, f->handle);
    return 0;
  }

  /* ── Dumb buffers (use per-client handle table) ───────────────────── */
  case DRM_IOCTL_MODE_CREATE_DUMB: {
    struct drm_mode_create_dumb *c = (struct drm_mode_create_dumb *)arg;
    klog_debug_puts("[DRM] CREATE_DUMB in width=");
    klog_debug_uint64(c->width);
    klog_debug_puts(" height=");
    klog_debug_uint64(c->height);
    klog_debug_puts(" bpp=");
    klog_debug_uint64(c->bpp);
    klog_debug_puts(" flags=0x");
    klog_debug_hex32(c->flags);
    klog_debug_puts("\n");

    if (g_drm_create_dumb_fn) {
      struct drm_gem_object *obj = NULL;
      int rc = g_drm_create_dumb_fn(dev, c->width, c->height, c->bpp, &obj);
      if (rc != 0 || !obj) return rc ? rc : -12;
      uint32_t local_h = drm_file_gem_register(file, obj);
      if (!local_h) { drm_gem_object_free(dev, obj); return -12; }
      obj->refcount--; /* transfer creator ownership to the handle */
      c->pitch = c->width * (c->bpp / 8);
      c->size = obj->size;
      c->handle = local_h;
      return 0;
    }

    /* Dumb/render buffers stay WB. Only the fixed physical scanout is WC. */
    c->pitch = (c->width * (c->bpp / 8) + 63) & ~63;
    c->size = (uint64_t)c->pitch * c->height;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -12; /* ENOMEM */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h) {
      drm_gem_object_free(dev, obj);
      return -12;
    }
    obj->refcount--; /* transfer creator ownership to the handle */
    c->handle = local_h;
    klog_debug_puts("[DRM] CREATE_DUMB out handle=");
    klog_debug_uint64(c->handle);
    klog_debug_puts(" pitch=");
    klog_debug_uint64(c->pitch);
    klog_debug_puts(" size=");
    klog_debug_uint64(c->size);
    klog_debug_puts(" gem_phys=0x");
    klog_debug_hex64(obj->phys_addr);
    klog_debug_puts(" gem_virt=0x");
    klog_debug_hex64((uint64_t)obj->virt_addr);
    klog_debug_puts("\n");
    return 0;
  }
  case DRM_IOCTL_MODE_MAP_DUMB: {
    struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
    struct drm_gem_object *obj = drm_file_gem_lookup(file, m->handle);
    if (!obj)
      return -2; /* ENOENT */
    m->offset = obj->phys_addr | 0x1000000000000000ULL;
    return 0;
  }
  case DRM_IOCTL_MODE_DESTROY_DUMB: {
    uint32_t handle = *(uint32_t *)arg;
    drm_file_gem_release(file, handle);
    return 0;
  }

  /* ── KMS resource queries ────────────────────────────────────────── */
  case DRM_IOCTL_MODE_GETRESOURCES: {
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
    uint32_t fbs = 0, crtcs = 0, connectors = 0, encoders = 0;
    struct drm_mode_object *mobj;
    spinlock_acquire(&dev->lock);
    list_for_each_entry(mobj, &dev->kms_objects, list) {
      if (mobj->type == DRM_MODE_OBJECT_CRTC) {
        if (res->crtc_id_ptr && crtcs < res->count_crtcs)
          ((uint32_t *)res->crtc_id_ptr)[crtcs] = mobj->id;
        crtcs++;
      } else if (mobj->type == DRM_MODE_OBJECT_CONNECTOR) {
        if (res->connector_id_ptr && connectors < res->count_connectors) {
          ((uint32_t *)res->connector_id_ptr)[connectors] = mobj->id;
          klog_debug_puts("[DRM] GETRESOURCES: filling connector id=");
          klog_debug_uint64(mobj->id);
          klog_debug_puts(" at ptr=");
          klog_debug_hex64(res->connector_id_ptr + connectors * 4);
          klog_debug_puts("\n");
        }
        connectors++;
      } else if (mobj->type == DRM_MODE_OBJECT_ENCODER) {
        if (res->encoder_id_ptr && encoders < res->count_encoders)
          ((uint32_t *)res->encoder_id_ptr)[encoders] = mobj->id;
        encoders++;
      } else if (mobj->type == DRM_MODE_OBJECT_FB) {
        if (res->fb_id_ptr && fbs < res->count_fbs)
          ((uint32_t *)res->fb_id_ptr)[fbs] = mobj->id;
        fbs++;
      }
    }
    spinlock_release(&dev->lock);
    klog_debug_puts("[DRM] GETRESOURCES: fbs=");
    klog_debug_uint64(fbs);
    klog_debug_puts(" crtcs=");
    klog_debug_uint64(crtcs);
    klog_debug_puts(" connectors=");
    klog_debug_uint64(connectors);
    klog_debug_puts(" encoders=");
    klog_debug_uint64(encoders);
    klog_debug_puts("\n");

    res->count_fbs = fbs;
    res->count_crtcs = crtcs;
    res->count_connectors = connectors;
    res->count_encoders = encoders;
    res->min_width = 0;
    res->max_width = 8192;
    res->min_height = 0;
    res->max_height = 8192;
    return 0;
  }
  case DRM_IOCTL_MODE_GETPLANERESOURCES: {
    struct {
      uint64_t plane_id_ptr;
      uint32_t count_planes;
    } *res = (void *)arg;

    uint32_t planes = 0;
    struct drm_mode_object *mobj;

    klog_debug_puts("[DRM] GETPLANERESOURCES in count=");
    klog_debug_uint64(res->count_planes);
    klog_debug_puts(" ptr=0x");
    klog_debug_hex64(res->plane_id_ptr);
    klog_debug_puts("\n");

    spinlock_acquire(&dev->lock);

    list_for_each_entry(mobj, &dev->kms_objects, list) {
      if (mobj->type == DRM_MODE_OBJECT_PLANE) {
        uint64_t type_val = 999;
        drm_obj_get_prop(mobj, DRM_PROP_ID_TYPE, &type_val);

        /* The Limine framebuffer bridge has no independent hardware cursor:
         * drm_commit() emulates one by blending it into the scanout buffer.
         * Do not expose that internal plane through universal-plane discovery.
         *
         * Weston 14 with the Pixman renderer otherwise sees a cursor plane but
         * does not allocate GBM cursor BOs. It keeps the cursor view off the
         * primary plane while never enabling the KMS cursor plane, making the
         * cursor invisible. Reporting only the real primary plane makes the
         * compositor use its correct software-cursor fallback.
         *
         * Keep the object for legacy MODE_CURSOR ioctls and for a future DRM
         * backend with a genuinely independent cursor plane. */
        if ((uint32_t)type_val == DRM_PLANE_TYPE_CURSOR && !g_drm_cursor_fn)
          continue;

        klog_debug_puts("[DRM] GETPLANERESOURCES found plane index=");
        klog_debug_uint64(planes);
        klog_debug_puts(" id=");
        klog_debug_uint64(mobj->id);
        klog_debug_puts(" type=");
        klog_debug_uint64(type_val);
        klog_debug_puts("\n");

        if (res->plane_id_ptr && planes < res->count_planes) {
          ((uint32_t *)res->plane_id_ptr)[planes] = mobj->id;

          klog_debug_puts("[DRM] GETPLANERESOURCES wrote index=");
          klog_debug_uint64(planes);
          klog_debug_puts(" id=");
          klog_debug_uint64(mobj->id);
          klog_debug_puts("\n");
        }

        planes++;
      }
    }

    spinlock_release(&dev->lock);

    res->count_planes = planes;

    klog_debug_puts("[DRM] GETPLANERESOURCES out count=");
    klog_debug_uint64(planes);
    klog_debug_puts("\n");

    return 0;
  }
  case 0xC02064B6: { /* DRM_IOCTL_MODE_GETPLANE */
    struct {
      uint32_t plane_id;
      uint32_t crtc_id;
      uint32_t fb_id;
      uint32_t possible_crtcs;
      uint32_t gamma_size;
      uint32_t count_formats;
      uint64_t format_type_ptr;
    } *p = (void *)arg;

    klog_debug_puts("[DRM] GETPLANE in id=");
    klog_debug_uint64(p->plane_id);
    klog_debug_puts(" format_ptr=0x");
    klog_debug_hex64(p->format_type_ptr);
    klog_debug_puts(" count_in=");
    klog_debug_uint64(p->count_formats);
    klog_debug_puts("\n");

    spinlock_acquire(&dev->lock);

    struct drm_mode_object *mobj = drm_mode_object_find(dev, p->plane_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_PLANE) {
      spinlock_release(&dev->lock);
      klog_debug_puts("[DRM] GETPLANE failed: bad plane id\n");
      return -2; /* ENOENT */
    }

    struct drm_plane *plane = (struct drm_plane *)mobj;

    uint64_t type_val = 0;
    if (drm_obj_get_prop(&plane->base, DRM_PROP_ID_TYPE, &type_val) != 0)
      type_val = DRM_PLANE_TYPE_OVERLAY;

    uint64_t crtc_id = 0;
    uint64_t fb_id = 0;
    drm_obj_get_prop(&plane->base, DRM_PROP_ID_CRTC_ID, &crtc_id);
    drm_obj_get_prop(&plane->base, DRM_PROP_ID_FB_ID, &fb_id);

    p->crtc_id = (uint32_t)crtc_id;
    p->fb_id = (uint32_t)fb_id;
    p->possible_crtcs = plane->possible_crtcs;
    p->gamma_size = 0;

    uint32_t formats[2];

    if ((uint32_t)type_val == DRM_PLANE_TYPE_CURSOR) {
      formats[0] = 0x34325241; /* ARGB8888 */
      formats[1] = 0x34325258; /* XRGB8888 */
      p->count_formats = 2;
    } else {
      formats[0] = 0x34325258; /* XRGB8888 */
      formats[1] = 0x34325241; /* ARGB8888 */
      p->count_formats = 2;
    }

    if (p->format_type_ptr) {
      ((uint32_t *)p->format_type_ptr)[0] = formats[0];
      ((uint32_t *)p->format_type_ptr)[1] = formats[1];
    }

    klog_debug_puts("[DRM] GETPLANE out id=");
    klog_debug_uint64(p->plane_id);
    klog_debug_puts(" type=");
    klog_debug_uint64(type_val);
    klog_debug_puts(" possible_crtcs=0x");
    klog_debug_hex32(p->possible_crtcs);
    klog_debug_puts(" formats=");
    klog_debug_uint64(p->count_formats);
    klog_debug_puts("\n");

    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_GETCRTC: {
    struct drm_mode_get_crtc *c = (struct drm_mode_get_crtc *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, c->crtc_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_CRTC) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_crtc *crtc = (struct drm_crtc *)mobj;
    c->fb_id = crtc->fb ? crtc->fb->base.id : 0;
    c->x = 0;
    c->y = 0;
    c->gamma_size = 0;
    c->mode_valid = 1;
    c->mode.hdisplay = fb_get_width();
    c->mode.hsync_start = c->mode.hdisplay + 8;
    c->mode.hsync_end = c->mode.hdisplay + 16;
    c->mode.htotal = c->mode.hdisplay + 32;
    c->mode.vdisplay = fb_get_height();
    c->mode.vsync_start = c->mode.vdisplay + 4;
    c->mode.vsync_end = c->mode.vdisplay + 8;
    c->mode.vtotal = c->mode.vdisplay + 12;
    c->mode.vrefresh = 60;
    strcpy(c->mode.name, "Native");
    spinlock_release(&dev->lock);
    return 0;
  }

  case DRM_IOCTL_MODE_GETENCODER: {
    struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, e->encoder_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_ENCODER) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_encoder *enc = (struct drm_encoder *)mobj;
    e->encoder_type = enc->encoder_type;
    e->possible_crtcs = enc->possible_crtcs;
    e->possible_clones = 0;
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
      if (obj->type == DRM_MODE_OBJECT_CRTC &&
          ((struct drm_crtc *)obj)->scanout_id == enc->scanout_id) {
        e->crtc_id = obj->id;
        break;
      }
    }
    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_GETCONNECTOR: {
    struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
    klog_debug_puts("[DRM] GETCONNECTOR: arg=");
    klog_debug_hex64((uint64_t)arg);
    klog_debug_puts(" id_in_struct=");
    klog_debug_uint64(c->connector_id);
    klog_debug_puts("\n");
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, c->connector_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_CONNECTOR) {
      klog_debug_puts("[DRM] GETCONNECTOR: object not found or wrong type: id=");
      klog_debug_uint64(c->connector_id);
      klog_debug_puts("\n");
      spinlock_release(&dev->lock);
      return -2; /* ENOENT */
    }
    struct drm_connector *conn = (struct drm_connector *)mobj;
    c->connector_type = conn->connector_type;
    c->connector_type_id = conn->scanout_id + 1;
    c->connection = conn->connection_status;
    c->mm_width = 300;
    c->mm_height = 200;
    c->subpixel = 1;
    if (conn->encoder)
      c->encoder_id = conn->encoder->base.id;

    /* Expose connector properties — wlroots needs CRTC_ID on the connector */
    uint32_t prop_count = mobj->prop_count;
    if (c->props_ptr && c->count_props >= prop_count) {
      uint32_t *prop_ids = (uint32_t *)c->props_ptr;
      uint64_t *prop_vals = (uint64_t *)c->prop_values_ptr;
      for (uint32_t i = 0; i < prop_count; i++) {
        prop_ids[i] = mobj->props[i].prop_id;
        prop_vals[i] = mobj->props[i].value;
      }
    }
    c->count_props = prop_count;

    uint32_t mode_capacity = c->count_modes;
    c->count_modes = 1;
    if (g_drm_get_modes_fn) {
      uint32_t mode_count = c->modes_ptr ? mode_capacity : 0;
      g_drm_get_modes_fn(c->connector_id,
                         c->modes_ptr ? (struct drm_mode_modeinfo *)c->modes_ptr : NULL,
                         &mode_count);
      c->count_modes = mode_count;
    } else if (c->modes_ptr) {
      struct drm_mode_modeinfo *m = (struct drm_mode_modeinfo *)c->modes_ptr;
      m->clock = 60000;
      m->hdisplay = fb_get_width();
      m->hsync_start = m->hdisplay + 8;
      m->hsync_end = m->hdisplay + 16;
      m->htotal = m->hdisplay + 32;
      m->vdisplay = fb_get_height();
      m->vsync_start = m->vdisplay + 4;
      m->vsync_end = m->vdisplay + 8;
      m->vtotal = m->vdisplay + 12;
      m->vrefresh = 60;
      m->flags = 0;
      m->type = 0x48; /* DRM_MODE_TYPE_DRIVER | PREFERRED */
      strcpy(m->name, "Native");
    }
    c->count_encoders = 0;
    if (conn->encoder) {
      c->count_encoders = 1;
      if (c->encoders_ptr)
        ((uint32_t *)c->encoders_ptr)[0] = conn->encoder->base.id;
    }
    klog_debug_puts("[DRM] GETCONNECTOR out id=");
    klog_debug_uint64(c->connector_id);
    klog_debug_puts(" conn=");
    klog_debug_uint64(c->connection);
    klog_debug_puts(" enc=");
    klog_debug_uint64(c->encoder_id);
    klog_debug_puts(" modes=");
    klog_debug_uint64(c->count_modes);
    klog_debug_puts(" props=");
    klog_debug_uint64(c->count_props);
    klog_debug_puts(" encoders=");
    klog_debug_uint64(c->count_encoders);
    klog_debug_puts(" modes_ptr=0x");
    klog_debug_hex64(c->modes_ptr);
    klog_debug_puts(" props_ptr=0x");
    klog_debug_hex64(c->props_ptr);
    klog_debug_puts("\n");
    spinlock_release(&dev->lock);
    return 0;
  }

  /* ── Framebuffer management ──────────────────────────────────────── */
  case DRM_IOCTL_MODE_ADDFB: {
    struct drm_mode_fb_cmd *cmd = (struct drm_mode_fb_cmd *)arg;
    klog_debug_puts("[DRM] ADDFB in handle=");
    klog_debug_uint64(cmd->handle);
    klog_debug_puts(" width=");
    klog_debug_uint64(cmd->width);
    klog_debug_puts(" height=");
    klog_debug_uint64(cmd->height);
    klog_debug_puts(" pitch=");
    klog_debug_uint64(cmd->pitch);
    klog_debug_puts(" bpp=");
    klog_debug_uint64(cmd->bpp);
    klog_debug_puts(" depth=");
    klog_debug_uint64(cmd->depth);
    klog_debug_puts("\n");
    /* Resolve local handle → global gem object */
    struct drm_gem_object *gem = drm_file_gem_lookup(file, cmd->handle);
    if (!gem) {
      klog_debug_puts("[DRM] ADDFB missing GEM handle=");
      klog_debug_uint64(cmd->handle);
      klog_debug_puts("\n");
      return -2; /* ENOENT */
    }
    /* Temporarily patch handle to global for drm_framebuffer_create */
    uint32_t saved = cmd->handle;
    cmd->handle = gem->handle;
    struct drm_framebuffer *fb = drm_framebuffer_create(dev, cmd);
    cmd->handle = saved;
    if (!fb) {
      klog_debug_puts("[DRM] ADDFB framebuffer create failed\n");
      return -1;
    }
    cmd->fb_id = fb->base.id;
    klog_debug_puts("[DRM] ADDFB out fb_id=");
    klog_debug_uint64(cmd->fb_id);
    klog_debug_puts(" gem_phys=0x");
    klog_debug_hex64(gem->phys_addr);
    klog_debug_puts("\n");
    return 0;
  }
  case DRM_IOCTL_MODE_RMFB: {
    uint32_t fb_id = *(uint32_t *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, fb_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_FB) {
      spinlock_release(&dev->lock);
      return -1;
    }
    spinlock_release(&dev->lock);
    drm_framebuffer_free(dev, (struct drm_framebuffer *)mobj);
    return 0;
  }
  case DRM_IOCTL_MODE_ADDFB2:
    /* Full multi-planar path — resolves handles via global gem list */
    return drm_ioctl_addfb2(file, dev, arg);

  /* ── Legacy modesetting ──────────────────────────────────────────── */
  case DRM_IOCTL_MODE_SETCRTC: {
    struct drm_mode_crtc *crtc_cmd = (struct drm_mode_crtc *)arg;
    klog_debug_puts("[DRM] SETCRTC crtc=");
    klog_debug_uint64(crtc_cmd->crtc_id);
    klog_debug_puts(" fb=");
    klog_debug_uint64(crtc_cmd->fb_id);
    klog_debug_puts(" connectors=");
    klog_debug_uint64(crtc_cmd->count_connectors);
    klog_debug_puts(" mode_valid=");
    klog_debug_uint64(crtc_cmd->mode_valid);
    klog_debug_puts(" mode=");
    klog_debug_uint64(crtc_cmd->mode.hdisplay);
    klog_debug_puts("x");
    klog_debug_uint64(crtc_cmd->mode.vdisplay);
    klog_debug_puts("\n");
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj =
        drm_mode_object_find(dev, crtc_cmd->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, crtc_cmd->fb_id);
    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC) {
      klog_debug_puts("[DRM] SETCRTC bad crtc id\n");
      spinlock_release(&dev->lock);
      return -1;
    }
    if (crtc_cmd->fb_id && (!fb_obj || fb_obj->type != DRM_MODE_OBJECT_FB))
      klog_debug_puts("[DRM] SETCRTC warning: fb id not found\n");
    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    if (fb_obj && fb_obj->type == DRM_MODE_OBJECT_FB) {
      crtc->fb = (struct drm_framebuffer *)fb_obj;
      if (g_drm_set_fb_fn)
        g_drm_set_fb_fn(crtc->base.id, crtc->fb);
    }
    spinlock_release(&dev->lock);
    drm_commit_flipped_crtc(dev, crtc_cmd->crtc_id);
    return 0;
  }
  case DRM_IOCTL_MODE_PAGE_FLIP: {
    struct drm_mode_crtc_page_flip *flip =
        (struct drm_mode_crtc_page_flip *)arg;
#if DRM_DEBUG_LOGGING
    klog_debug_puts("[DRM] PAGE_FLIP crtc=");
    klog_debug_uint64(flip->crtc_id);
    klog_debug_puts(" fb=");
    klog_debug_uint64(flip->fb_id);
    klog_debug_puts(" flags=0x");
    klog_debug_hex32(flip->flags);
    klog_debug_puts(" user_data=0x");
    klog_debug_hex64(flip->user_data);
    klog_debug_puts("\n");
#endif
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, flip->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, flip->fb_id);
    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC || !fb_obj ||
        fb_obj->type != DRM_MODE_OBJECT_FB) {
      klog_debug_puts("[DRM] PAGE_FLIP rejected: bad crtc or fb\n");
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    crtc->fb = (struct drm_framebuffer *)fb_obj;
    if (g_drm_set_fb_fn)
      g_drm_set_fb_fn(crtc->base.id, crtc->fb);
    if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
      /* Register flip with virtio hook first so it can record file+user_data */
      if (g_drm_pageflip_fn)
        g_drm_pageflip_fn(file, node, flip->crtc_id, flip->fb_id, flip->user_data);
      spinlock_release(&dev->lock);
      drm_commit_flipped_crtc(dev, flip->crtc_id);
      /* If no virtio hook took ownership, deliver event via legacy path */
      if (!g_drm_pageflip_fn) {
        struct drm_event_vblank ev = {0};
        ev.base.type = DRM_EVENT_FLIP_COMPLETE;
        ev.base.length = sizeof(ev);
        ev.user_data = flip->user_data;
        drm_fill_vblank_event(&ev, flip->crtc_id);
        drm_file_send_event(file, &ev, node);
      }
      return 0;
    }
    spinlock_release(&dev->lock);
    drm_commit_flipped_crtc(dev, flip->crtc_id);
    return 0;
  }

  /* ── Atomic modesetting ──────────────────────────────────────────── */
  case DRM_IOCTL_MODE_ATOMIC: {
    int ret = drm_ioctl_atomic(node, file, dev, arg);
    if (ret == 0 &&
        !(((struct drm_mode_atomic *)arg)->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
#if DRM_DEBUG_LOGGING
      klog_debug_puts("[DRM] ATOMIC commit triggering drm_commit\n");
#endif
      drm_sync_cursor_backend(dev, 0);
      drm_commit_atomic(dev);
    }
    return ret;
  }
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    return drm_ioctl_obj_getprops(dev, arg);
  case DRM_IOCTL_MODE_GETPROPERTY:
    return drm_ioctl_getproperty(dev, arg);
  case DRM_IOCTL_MODE_SETPROPERTY:
    return 0; /* stub */
  case DRM_IOCTL_MODE_DIRTYFB:
  case DRM_IOCTL_MODE_DIRTYFB_LEGACY: {
    struct drm_mode_fb_dirty_cmd *dirty = (void *)arg;
    return drm_dirtyfb(dev, dirty);
  }
  case DRM_IOCTL_MODE_CREATEPROPBLOB: {
    struct drm_mode_create_blob *b = (struct drm_mode_create_blob *)arg;
    if (!b->data || !b->length)
      return -14; /* EFAULT */
    struct drm_prop_blob *blob =
        drm_blob_create(dev, (void *)b->data, b->length);
    if (!blob)
      return -12; /* ENOMEM */
    b->blob_id = blob->id;
    return 0;
  }
  case DRM_IOCTL_MODE_DESTROYPROPBLOB: {
    struct drm_mode_destroy_blob *b = (struct drm_mode_destroy_blob *)arg;
    drm_blob_destroy(dev, b->blob_id);
    return 0;
  }

  /* ── GEM PRIME / DMA-buf ─────────────────────────────────────────── */
  case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
    struct drm_prime_handle *p = (struct drm_prime_handle *)arg;
    struct drm_gem_object *obj = drm_file_gem_lookup(file, p->handle);
    if (!obj)
      return -2; /* ENOENT */
    int prime_fd = drm_prime_export(obj);
    if (prime_fd < 0)
      return -1; /* generic error for now */
    p->fd = prime_fd;
    return 0;
  }
  case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
    struct drm_prime_handle *p = (struct drm_prime_handle *)arg;
    struct drm_gem_object *obj = drm_prime_import(p->fd);
    if (!obj)
      return -2; /* ENOENT */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h)
      return -12; /* ENOMEM */
    p->handle = local_h;
    return 0;
  }

  case DRM_IOCTL_WAIT_VBLANK: {
    union drm_wait_vblank *vbl = (union drm_wait_vblank *)arg;
    uint32_t type = vbl->request.type;
    uint64_t user_data = vbl->request.signal;
    uint64_t ms = lapic_timer_get_ms();
    uint32_t seq = drm_event_sequence++;
#if DRM_DEBUG_LOGGING
    klog_debug_puts("[DRM] WAIT_VBLANK type=0x");
    klog_debug_hex32(type);
    klog_debug_puts(" seq_in=");
    klog_debug_uint64(vbl->request.sequence);
    klog_debug_puts("\n");
#endif
    vbl->reply.type = type;
    vbl->reply.sequence = seq;
    vbl->reply.tval_sec = (long)(ms / 1000);
    vbl->reply.tval_usec = (long)((ms % 1000) * 1000);
    if (type & DRM_VBLANK_EVENT) {
      struct drm_event_vblank ev = {0};
      ev.base.type = DRM_EVENT_VBLANK;
      ev.base.length = sizeof(ev);
      ev.user_data = user_data;
      ev.tv_sec = (uint32_t)vbl->reply.tval_sec;
      ev.tv_usec = (uint32_t)vbl->reply.tval_usec;
      ev.sequence = seq;
      ev.crtc_id = 0;
      drm_file_send_event(file, &ev, node);
    }
    return 0;
  }

  case DRM_IOCTL_MODE_CURSOR: {
    struct drm_mode_cursor *cur = (struct drm_mode_cursor *)arg;

#if DRM_DEBUG_LOGGING
    klog_debug_puts("[DRM] MODE_CURSOR flags=0x");
    klog_debug_hex32(cur->flags);
    klog_debug_puts(" handle=");
    klog_debug_uint64(cur->handle);
    klog_debug_puts(" x=");
    klog_debug_uint64((uint32_t)cur->x);
    klog_debug_puts(" y=");
    klog_debug_uint64((uint32_t)cur->y);
    klog_debug_puts(" w=");
    klog_debug_uint64(cur->width);
    klog_debug_puts(" h=");
    klog_debug_uint64(cur->height);
    klog_debug_puts("\n");
#endif

    struct drm_clip_rect old_damage = {0}, new_damage = {0};
    bool have_old_damage = false, have_new_damage = false;
    spinlock_acquire(&dev->lock);

    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
      if (obj->type != DRM_MODE_OBJECT_CRTC)
        continue;

      struct drm_crtc *crtc = (struct drm_crtc *)obj;
      struct drm_plane *cursor = crtc->cursor;

      if (!cursor)
        continue;

      have_old_damage = drm_cursor_damage_rect(cursor, &old_damage);
      uint32_t flags =
          cur->flags ? cur->flags : (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE);

      if (flags & DRM_MODE_CURSOR_MOVE) {
        cursor->crtc_x = cur->x;
        cursor->crtc_y = cur->y;
      }

      if (flags & DRM_MODE_CURSOR_BO) {
        if (cur->width)
          cursor->crtc_w = cur->width;
        if (cur->height)
          cursor->crtc_h = cur->height;
        cursor->src_x = 0;
        cursor->src_y = 0;
        cursor->src_w = cursor->crtc_w << 16;
        cursor->src_h = cursor->crtc_h << 16;
        cursor->hotspot_x = 0;
        cursor->hotspot_y = 0;

        if (cur->handle == 0) {
          cursor->fb = NULL;
          break;
        }

        struct drm_gem_object *gem = drm_file_gem_lookup(file, cur->handle);
        if (!gem) {
          spinlock_release(&dev->lock);
          klog_debug_puts("[DRM] MODE_CURSOR invalid GEM handle\n");
          return -2; /* ENOENT */
        }

        static struct drm_framebuffer legacy_cursor_fb;
        memset(&legacy_cursor_fb, 0, sizeof(legacy_cursor_fb));

        legacy_cursor_fb.width = cur->width ? cur->width : 64;
        legacy_cursor_fb.height = cur->height ? cur->height : 64;
        legacy_cursor_fb.pitch = legacy_cursor_fb.width * 4;
        legacy_cursor_fb.bpp = 32;
        legacy_cursor_fb.gem_obj = gem;

        cursor->fb = &legacy_cursor_fb;
      }
      have_new_damage = drm_cursor_damage_rect(cursor, &new_damage);
      break;
    }

    spinlock_release(&dev->lock);
    drm_sync_cursor_backend(dev, cur->flags ? cur->flags :
                            (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE));
    drm_commit_cursor_damage(dev, &old_damage, have_old_damage,
                             &new_damage, have_new_damage);
    return 0;
  }

  case DRM_IOCTL_MODE_CURSOR2: {
    struct drm_mode_cursor2 *cur = (struct drm_mode_cursor2 *)arg;

#if DRM_DEBUG_LOGGING
    klog_debug_puts("[DRM] MODE_CURSOR2 flags=0x");
    klog_debug_hex32(cur->flags);
    klog_debug_puts(" handle=");
    klog_debug_uint64(cur->handle);
    klog_debug_puts(" x=");
    klog_debug_uint64((uint32_t)cur->x);
    klog_debug_puts(" y=");
    klog_debug_uint64((uint32_t)cur->y);
    klog_debug_puts(" w=");
    klog_debug_uint64(cur->width);
    klog_debug_puts(" h=");
    klog_debug_uint64(cur->height);
    klog_debug_puts(" hot=");
    klog_debug_uint64((uint32_t)cur->hot_x);
    klog_debug_puts(",");
    klog_debug_uint64((uint32_t)cur->hot_y);
    klog_debug_puts("\n");
#endif

    struct drm_clip_rect old_damage = {0}, new_damage = {0};
    bool have_old_damage = false, have_new_damage = false;
    spinlock_acquire(&dev->lock);

    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
      if (obj->type != DRM_MODE_OBJECT_CRTC)
        continue;

      struct drm_crtc *crtc = (struct drm_crtc *)obj;
      struct drm_plane *cursor = crtc->cursor;

      if (!cursor)
        continue;

      have_old_damage = drm_cursor_damage_rect(cursor, &old_damage);
      uint32_t flags =
          cur->flags ? cur->flags : (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE);

      if (flags & DRM_MODE_CURSOR_MOVE) {
        cursor->crtc_x = cur->x;
        cursor->crtc_y = cur->y;
      }

      if (flags & DRM_MODE_CURSOR_BO) {
        if (cur->width)
          cursor->crtc_w = cur->width;
        if (cur->height)
          cursor->crtc_h = cur->height;
        cursor->src_x = 0;
        cursor->src_y = 0;
        cursor->src_w = cursor->crtc_w << 16;
        cursor->src_h = cursor->crtc_h << 16;
        cursor->hotspot_x = cur->hot_x;
        cursor->hotspot_y = cur->hot_y;

        if (cur->handle == 0) {
          cursor->fb = NULL;
          break;
        }

        struct drm_gem_object *gem = drm_file_gem_lookup(file, cur->handle);
        if (!gem) {
          spinlock_release(&dev->lock);
          klog_debug_puts("[DRM] MODE_CURSOR2 invalid GEM handle\n");
          return -2; /* ENOENT */
        }

        static struct drm_framebuffer legacy_cursor2_fb;
        memset(&legacy_cursor2_fb, 0, sizeof(legacy_cursor2_fb));

        legacy_cursor2_fb.width = cur->width ? cur->width : 64;
        legacy_cursor2_fb.height = cur->height ? cur->height : 64;
        legacy_cursor2_fb.pitch = legacy_cursor2_fb.width * 4;
        legacy_cursor2_fb.bpp = 32;
        legacy_cursor2_fb.gem_obj = gem;

        cursor->fb = &legacy_cursor2_fb;
      }
      have_new_damage = drm_cursor_damage_rect(cursor, &new_damage);
      break;
    }

    spinlock_release(&dev->lock);
    drm_sync_cursor_backend(dev, cur->flags ? cur->flags :
                            (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE));
    drm_commit_cursor_damage(dev, &old_damage, have_old_damage,
                             &new_damage, have_new_damage);
    return 0;
  }

  case DRM_IOCTL_MODE_GETGAMMA:
  case DRM_IOCTL_MODE_SETGAMMA:
    return 0; /* stub */
  default: {
    klog_debug_puts("[DRM] unknown ioctl request=0x");
    klog_debug_hex32(request);
    klog_debug_puts("\n");
    return -25; /* ENOTTY */
  }
  }
}

/* ── mmap — resolve GEM offset to physical pages ─────────────────────────── */

static uint64_t drm_vfs_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                             uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)flags;
  (void)prot;
  struct drm_file *file = node_to_file(node);
  if (!file || length == 0)
    return (uint64_t)-1;
  if ((offset & 0x1000000000000000ULL) == 0)
    return (uint64_t)-1;
  uint64_t phys = offset & ~0x1000000000000000ULL;

  /* Resolve the opaque MAP_DUMB token to an object owned by this DRM file. */
  enum drm_gem_cache_mode cache_mode = DRM_GEM_CACHE_WB;
  struct drm_gem_object *mapped_obj = NULL;
  uint64_t mapped_offset = 0;
  bool found = false;
  spinlock_acquire(&file->lock);
  for (uint32_t i = 1; i < DRM_MAX_HANDLES_PER_FILE; i++) {
    struct drm_gem_object *candidate = file->handles[i];
    if (!candidate || phys < candidate->phys_addr)
      continue;
    uint64_t object_offset = phys - candidate->phys_addr;
    if (object_offset <= candidate->size &&
        length <= candidate->size - object_offset) {
      cache_mode = candidate->cache_mode;
      found = true;
      mapped_obj = candidate;
      mapped_offset = object_offset;
      break;
    }
  }
  if (!found && file->hw_fb_gem && phys >= file->hw_fb_gem->phys_addr) {
    uint64_t object_offset = phys - file->hw_fb_gem->phys_addr;
    if (object_offset <= file->hw_fb_gem->size &&
        length <= file->hw_fb_gem->size - object_offset) {
      cache_mode = file->hw_fb_gem->cache_mode;
      found = true;
      mapped_obj = file->hw_fb_gem;
      mapped_offset = object_offset;
    }
  }
  spinlock_release(&file->lock);
  if (!found)
    return (uint64_t)-1;
  uint64_t vaddr = addr;
  if (vaddr == 0) {
    extern uint64_t mm_alloc_mmap_region(uint64_t length);
    vaddr = mm_alloc_mmap_region(length);
    if (vaddr == 0)
      vaddr = 0x500000000000ULL + (offset & 0xFFFFFFF);
  }
  if (vaddr == 0)
    return (uint64_t)-1;

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW;
  if (cache_mode == DRM_GEM_CACHE_WC)
    page_flags |= PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_PAT;
  uint32_t num_pages = (length + 4095) / 4096;
  uint64_t *pml4 = vmm_get_active_pml4();
  for (uint32_t i = 0; i < num_pages; i++) {
    uint64_t page_offset = mapped_offset + (uint64_t)i * 4096;
    uint64_t page_phys = mapped_obj->get_page_phys
        ? mapped_obj->get_page_phys(mapped_obj, (uint32_t)(page_offset / 4096))
        : mapped_obj->phys_addr + page_offset;
    if (!page_phys || !vmm_map_page(pml4, vaddr + (uint64_t)i * 4096,
                                    page_phys, page_flags))
      return (uint64_t)-1;
  }
  return vaddr;
}

/* ── poll / read — per-client event queue ────────────────────────────────── */

static int drm_poll(struct vfs_node *node, int events) {
  struct drm_file *file = node_to_file(node);
  int revents = (0x0004 | 0x0100); /* POLLOUT | POLLWRNORM always */
  if (file) {
    spinlock_acquire(&file->lock);
    if (!list_empty(&file->event_queue))
      revents |= (0x0001 | 0x0040); /* POLLIN | POLLRDNORM */
    spinlock_release(&file->lock);
  }
  return revents & events;
}

static uint32_t drm_read(struct vfs_node *node, uint32_t offset,
                         uint32_t length, uint8_t *buffer) {
  struct drm_file *file = node_to_file(node);
  (void)offset;
  if (!file)
    return 0;

  spinlock_acquire(&file->lock);
  while (list_empty(&file->event_queue)) {
    spinlock_release(&file->lock);
    struct thread *current = sched_get_current();
    wait_queue_entry_t entry = {.thread = current, .next = NULL};
    wait_queue_add(&file->event_wq, &entry);
    current->state = THREAD_BLOCKED;
    sched_yield();
    wait_queue_remove(&file->event_wq, &entry);
    current->state = THREAD_RUNNING;
    spinlock_acquire(&file->lock);
  }

  struct drm_pending_event *e =
      list_first_entry(&file->event_queue, struct drm_pending_event, list);
  uint32_t event_size = e->event.base.length;
  if (length < event_size) {
    spinlock_release(&file->lock);
    return 0;
  }
  memcpy(buffer, &e->event, event_size);
  list_del(&e->list);
  spinlock_release(&file->lock);
  kfree(e);
  return event_size;
}

/* ── Per-client node factory and lookup metadata ───────────────────── */
/*
 * finddir() is also called by getdents64() merely to determine d_type and by
 * stat-family path resolution. It must therefore not allocate per-open state.
 * Mesa scans /dev/dri several times before choosing a device; allocating a
 * drm_file here leaked every scan because no fd existed to release it.
 */
static vfs_node_t drm_card_metadata_node;
static vfs_node_t *drm_open_instance(vfs_node_t *metadata);

static struct dirent *drm_dri_readdir(vfs_node_t *dir, uint32_t index) {
  static struct dirent entry;

  memset(&entry, 0, sizeof(entry));
  if (index == 0) {
    strcpy(entry.name, ".");
    entry.ino = dir->inode;
    entry.d_type = DT_DIR;
  } else if (index == 1) {
    strcpy(entry.name, "..");
    entry.ino = dir->inode;
    entry.d_type = DT_DIR;
  } else if (index == 2) {
    strcpy(entry.name, "card0");
    entry.ino = (226U << 8) | 0U;
    entry.d_type = DT_CHR;
  } else {
    return NULL;
  }

  return &entry;
}

static vfs_node_t *drm_dri_finddir(vfs_node_t *dir, char *name) {
  (void)dir;
  if (strcmp(name, "card0") != 0)
    return NULL;

  return &drm_card_metadata_node;
}

static vfs_node_t *drm_alloc_client_node(void) {

  /* Allocate per-client drm_file */
  struct drm_file *file = drm_file_alloc(&global_drm_dev);
  if (!file)
    return NULL;

  /* Allocate a fresh non-persistent clone node */
  vfs_node_t *clone = kmalloc(sizeof(vfs_node_t));
  if (!clone) {
    drm_file_free(file);
    return NULL;
  }
  vfs_node_init(clone);
  strcpy(clone->name, "card0");
  clone->flags = FS_CHARDEV; /* non-persistent: freed when refcount→0 */
  clone->mask = 0666;
  clone->inode = (226U << 8) | 0U; /* makedev(226,0) — DRM major:minor */
  clone->device = file;            /* ← per-client state */
  clone->ioctl = drm_ioctl;
  clone->mmap = drm_vfs_mmap;
  clone->open = drm_open;
  clone->close = drm_close;
  clone->read = drm_read;
  clone->poll = drm_poll;
  clone->wait_queue = &file->event_wq;
  clone->refcount = 0; /* first vfs_open() creates the fd reference */

  return clone;
}
vfs_node_t *drm_create_client_node(void) {
  return drm_alloc_client_node();
}

bool drm_is_card_node(vfs_node_t *node) {
  return node == &drm_card_metadata_node;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void drm_init(void) {
  memset(&global_drm_dev, 0, sizeof(struct drm_device));
  global_drm_dev.name = "card0";
  global_drm_dev.next_gem_handle = 1;
  global_drm_dev.next_kms_id = 1000;
  global_drm_dev.next_blob_id = 1;
  global_drm_dev.next_prime_id = 1;
  spinlock_init(&global_drm_dev.lock);
  INIT_LIST_HEAD(&global_drm_dev.gem_objects);
  INIT_LIST_HEAD(&global_drm_dev.kms_objects);
  INIT_LIST_HEAD(&global_drm_dev.event_queue);
  INIT_LIST_HEAD(&global_drm_dev.blob_objects);
  INIT_LIST_HEAD(&global_drm_dev.file_list);
  wait_queue_init(&global_drm_dev.event_wq);
  drm_kms_init(&global_drm_dev);

  /* Bridge hardware framebuffer as a global GEM object */
  void *fb_base = fb_get_base();
  if (fb_base) {
    uint64_t fb_phys =
        vmm_virt_to_phys(vmm_get_active_pml4(), (uint64_t)fb_base);
    size_t fb_size = fb_get_height() * fb_get_pitch();
    struct drm_gem_object *fb_obj = kmalloc(sizeof(struct drm_gem_object));
    if (fb_obj) {
      memset(fb_obj, 0, sizeof(struct drm_gem_object));
      fb_obj->dev = &global_drm_dev;
      fb_obj->size = fb_size;
      fb_obj->phys_addr = fb_phys;
      fb_obj->virt_addr = fb_base;
      fb_obj->cache_mode = DRM_GEM_CACHE_WC;
      fb_obj->refcount = 1;
      fb_obj->handle = 0xF0B0;
      spinlock_acquire(&global_drm_dev.lock);
      list_add_tail(&fb_obj->list, &global_drm_dev.gem_objects);
      spinlock_release(&global_drm_dev.lock);
      klog_puts("[DRM] Bridged HW framebuffer to GEM handle 0xF0B0\n");
    }
  }
}

void drm_register_vfs(void) {
  vfs_node_t *dev_dir = vfs_resolve_path("/dev");
  if (!dev_dir)
    return;
  vfs_mkdir(dev_dir, "dri", 0755);
  vfs_node_t *dri_dir = vfs_resolve_path("/dev/dri");
  if (!dri_dir)
    return;

  vfs_node_init(&drm_card_metadata_node);
  strcpy(drm_card_metadata_node.name, "card0");
  drm_card_metadata_node.flags = FS_CHARDEV | FS_PERSISTENT;
  drm_card_metadata_node.mask = 0666;
  drm_card_metadata_node.inode = (226U << 8) | 0U;
  drm_card_metadata_node.device = &global_drm_dev;
  drm_card_metadata_node.open_instance = drm_open_instance;
  drm_card_metadata_node.refcount = 1;

  /* Keep enumeration consistent with the dynamic per-open node factory.
   * Mesa scans /dev/dri before opening a preferred KMS/render device. */
  dri_dir->flags |= FS_DENTRY_NOCACHE;
  dri_dir->readdir = drm_dri_readdir;
  dri_dir->finddir = drm_dri_finddir;

  klog_puts("[DRM] Registered /dev/dri/card0 (per-client mode)\n");
}
static vfs_node_t *drm_open_instance(vfs_node_t *metadata) {
  (void)metadata;
  return drm_alloc_client_node();
}
