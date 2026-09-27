/*
 * drm_fb.c — Full ADDFB2 implementation with multi-planar formats + modifiers
 *
 * Supports:
 *   - Single-plane packed formats (XRGB8888, ARGB8888, RGB565, etc.)
 *   - Multi-planar YUV formats (NV12, YUV420, etc.) — up to 3 planes
 *   - Format modifiers (LINEAR, INVALID/opaque)
 *   - Per-plane GEM handles, pitches, offsets
 *
 * The resulting drm_framebuffer_full is registered as a KMS object and
 * can be used with both legacy SETCRTC and atomic commits.
 */

#include "drm.h"
#include "../../../console/klog.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"

extern struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file,
                                                  uint32_t handle);
extern void drm_mode_object_init(struct drm_device *dev,
                                 struct drm_mode_object *obj, uint32_t type);

/* ── Format descriptor table ─────────────────────────────────────────────── */

struct drm_format_info {
    uint32_t format;        /* fourcc */
    uint8_t  num_planes;
    uint8_t  cpp[4];        /* bytes per pixel per plane (0 = sub-sampled) */
    uint8_t  hsub;          /* horizontal chroma subsampling */
    uint8_t  vsub;          /* vertical chroma subsampling */
    const char *name;
};

static const struct drm_format_info drm_formats[] = {
    /* Packed RGB */
    { 0x34325258, 1, {4,0,0,0}, 1, 1, "XRGB8888" },
    { 0x34325241, 1, {4,0,0,0}, 1, 1, "ARGB8888" },
    { 0x34324258, 1, {4,0,0,0}, 1, 1, "XBGR8888" },
    { 0x34324241, 1, {4,0,0,0}, 1, 1, "ABGR8888" },
    { 0x36314752, 1, {3,0,0,0}, 1, 1, "RGB888"   },
    { 0x36314742, 1, {3,0,0,0}, 1, 1, "BGR888"   },
    { 0x36314752, 1, {3,0,0,0}, 1, 1, "RGB888"   },
    { 0x36315652, 1, {2,0,0,0}, 1, 1, "RGB565"   },
    { 0x36314742, 1, {2,0,0,0}, 1, 1, "BGR565"   },
    /* Packed YUV */
    { 0x56595559, 1, {2,0,0,0}, 2, 1, "YUYV"     },
    { 0x59565955, 1, {2,0,0,0}, 2, 1, "UYVY"     },
    /* Semi-planar YUV (NV12 / NV21) */
    { 0x3231564e, 2, {1,2,0,0}, 2, 2, "NV12"     },
    { 0x3132564e, 2, {1,2,0,0}, 2, 2, "NV21"     },
    /* Planar YUV (YUV420 / YVU420) */
    { 0x32315559, 3, {1,1,1,0}, 2, 2, "YUV420"   },
    { 0x32315659, 3, {1,1,1,0}, 2, 2, "YVU420"   },
};

#define NUM_FORMATS (sizeof(drm_formats) / sizeof(drm_formats[0]))

static const struct drm_format_info *drm_format_lookup(uint32_t fourcc) {
    for (size_t i = 0; i < NUM_FORMATS; i++) {
        if (drm_formats[i].format == fourcc)
            return &drm_formats[i];
    }
    return NULL;
}

/* ── Validation ──────────────────────────────────────────────────────────── */

static int drm_fb_validate(struct drm_file *file,
                           struct drm_mode_fb_cmd2 *cmd,
                           const struct drm_format_info *info) {
    if (cmd->width == 0 || cmd->height == 0) {
        klog_puts("[DRM] ADDFB2: zero dimensions\n");
        return -1;
    }
    if (cmd->width > 4096 || cmd->height > 4096) {
        klog_puts("[DRM] ADDFB2: dimensions exceed 4096\n");
        return -1;
    }

    for (int p = 0; p < info->num_planes; p++) {
        if (cmd->handles[p] == 0) {
            klog_puts("[DRM] ADDFB2: missing handle for plane ");
            klog_uint64(p);
            klog_puts("\n");
            return -1;
        }
        if (cmd->pitches[p] == 0) {
            klog_puts("[DRM] ADDFB2: zero pitch for plane ");
            klog_uint64(p);
            klog_puts("\n");
            return -1;
        }
        /* Verify the gem handle exists */
        if (!drm_file_gem_lookup(file, cmd->handles[p])) {
            klog_puts("[DRM] ADDFB2: invalid gem handle for plane ");
            klog_uint64(p);
            klog_puts("\n");
            return -1;
        }
    }

    /* Modifier sanity: we support LINEAR and INVALID (opaque/driver-specific) */
    for (int p = 0; p < info->num_planes; p++) {
        uint64_t mod = cmd->modifier[p];
        if (mod != DRM_FORMAT_MOD_LINEAR && mod != DRM_FORMAT_MOD_INVALID) {
            klog_puts("[DRM] ADDFB2: unsupported modifier 0x");
            klog_hex32((uint32_t)(mod >> 32));
            klog_hex32((uint32_t)mod);
            klog_puts(" (treating as LINEAR)\n");
            /* Non-fatal: fall back to linear */
        }
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * drm_framebuffer_create_full — full ADDFB2 path.
 *
 * Allocates a drm_framebuffer_full, validates all planes, registers it as
 * a KMS object, and returns a pointer to the embedded drm_framebuffer for
 * use by the rest of the KMS stack.
 */
struct drm_framebuffer *drm_framebuffer_create_full(struct drm_file *file,
                                                    struct drm_device *dev,
                                                    struct drm_mode_fb_cmd2 *cmd) {
    const struct drm_format_info *info = drm_format_lookup(cmd->pixel_format);
    if (!info) {
        klog_puts("[DRM] ADDFB2: unknown pixel format 0x");
        klog_hex32(cmd->pixel_format);
        klog_puts("\n");
        return NULL;
    }

    if (drm_fb_validate(file, cmd, info) != 0)
        return NULL;

    struct drm_framebuffer_full *fb = kmalloc(sizeof(struct drm_framebuffer_full));
    if (!fb) return NULL;
    memset(fb, 0, sizeof(struct drm_framebuffer_full));

    fb->base_fb.width = cmd->width;
    fb->base_fb.height = cmd->height;
    fb->base_fb.pixel_format = cmd->pixel_format;
    fb->width        = cmd->width;
    fb->height       = cmd->height;
    fb->pixel_format = cmd->pixel_format;
    fb->modifier     = cmd->modifier[0]; /* primary plane modifier */
    fb->flags        = cmd->flags;

    /* Legacy compat: derive bpp from format info */
    fb->base_fb.bpp = info->cpp[0] * 8;
    fb->base_fb.pitch = cmd->pitches[0];
    fb->bpp   = fb->base_fb.bpp;
    fb->pitch = fb->base_fb.pitch;

    for (int p = 0; p < info->num_planes; p++) {
        fb->gem_obj[p] = drm_file_gem_lookup(file, cmd->handles[p]);
        fb->pitches[p] = cmd->pitches[p];
        fb->offsets[p] = cmd->offsets[p];
        if (fb->gem_obj[p])
            fb->gem_obj[p]->refcount++;
    }

    fb->base_fb.gem_obj = fb->gem_obj[0];

    drm_mode_object_init(dev, &fb->base_fb.base, DRM_MODE_OBJECT_FB);

    klog_puts("[DRM] ADDFB2: created fb id=");
    klog_uint64(fb->base_fb.base.id);
    klog_puts(" fmt=");
    klog_puts(info->name);
    klog_puts(" ");
    klog_uint64(cmd->width);
    klog_puts("x");
    klog_uint64(cmd->height);
    klog_puts(" planes=");
    klog_uint64(info->num_planes);
    klog_puts(" mod=");
    klog_hex32((uint32_t)cmd->modifier[0]);
    klog_puts("\n");

    /* Return as drm_framebuffer* — the base is the first member */
    return &fb->base_fb;
}

/*
 * drm_framebuffer_free_full — release a full framebuffer.
 */
void drm_framebuffer_free_full(struct drm_device *dev,
                               struct drm_framebuffer *fb_base) {
    struct drm_framebuffer_full *fb = (struct drm_framebuffer_full *)fb_base;

    spinlock_acquire(&dev->lock);
    list_del(&fb->base_fb.base.list);
    spinlock_release(&dev->lock);

    struct drm_gem_object *release[DRM_MAX_FB_PLANES] = {0};
    int release_count = 0;
    for (int p = 0; p < DRM_MAX_FB_PLANES; p++) {
        struct drm_gem_object *gem = fb->gem_obj[p];
        if (!gem) continue;
        gem->refcount--;
        if (gem->refcount <= 0) {
            bool seen = false;
            for (int i = 0; i < release_count; i++)
                if (release[i] == gem) seen = true;
            if (!seen) release[release_count++] = gem;
        }
    }
    for (int i = 0; i < release_count; i++)
        drm_gem_object_free(dev, release[i]);
    kfree(fb);
}

/*
 * drm_ioctl_addfb2 — handle DRM_IOCTL_MODE_ADDFB2.
 *
 * Supports both single-plane (legacy compat) and multi-planar paths.
 * Also handles the DRM_MODE_FB_MODIFIERS flag.
 */
int drm_ioctl_addfb2(struct drm_file *file, struct drm_device *dev, uint64_t arg) {
    struct drm_mode_fb_cmd2 *cmd = (struct drm_mode_fb_cmd2 *)arg;

    klog_puts("[DRM] ADDFB2 in width=");
    klog_uint64(cmd->width);
    klog_puts(" height=");
    klog_uint64(cmd->height);
    klog_puts(" fmt=0x");
    klog_hex32(cmd->pixel_format);
    klog_puts(" flags=0x");
    klog_hex32(cmd->flags);
    klog_puts(" h0=");
    klog_uint64(cmd->handles[0]);
    klog_puts(" pitch0=");
    klog_uint64(cmd->pitches[0]);
    klog_puts(" off0=");
    klog_uint64(cmd->offsets[0]);
    klog_puts(" mod0=0x");
    klog_hex64(cmd->modifier[0]);
    klog_puts("\n");

    struct drm_framebuffer *fb = drm_framebuffer_create_full(file, dev, cmd);
    if (!fb) {
        klog_puts("[DRM] ADDFB2 failed\n");
        return -22; /* EINVAL */
    }

    cmd->fb_id = fb->base.id;
    klog_puts("[DRM] ADDFB2 out fb_id=");
    klog_uint64(cmd->fb_id);
    klog_puts("\n");
    return 0;
}
