#include "../../../console/klog.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"
#include "drm.h"
#include "../virtio_gpu/virtio_gpu.h"

extern struct drm_gem_object *ascentdrm_gem_find_by_handle(struct drm_device *dev, uint32_t handle);
extern void ascentdrm_obj_add_prop(struct drm_mode_object *obj, uint32_t prop_id, uint64_t default_val);
extern void ascentdrm_framebuffer_free_full(struct drm_device *dev,
                                             struct drm_framebuffer *fb);

void ascentdrm_mode_object_init(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type) {
    spinlock_acquire(&dev->lock);
    obj->id = dev->next_kms_id++;
    obj->type = type;
    obj->prop_count = 0;
    list_add_tail(&obj->list, &dev->kms_objects);
    spinlock_release(&dev->lock);
}

struct drm_plane *ascentdrm_plane_create(struct drm_device *dev, uint32_t possible_crtcs) {
    struct drm_plane *plane = kmalloc(sizeof(struct drm_plane));
    if (!plane) return NULL;
    memset(plane, 0, sizeof(struct drm_plane));
    
    plane->possible_crtcs = possible_crtcs;
    ascentdrm_mode_object_init(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    
    /* Attach standard plane properties */
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_TYPE,    0); /* Default to Overlay */
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_CRTC_ID, 0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_FB_ID,   0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_SRC_X,   0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_SRC_Y,   0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_SRC_W,   0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_SRC_H,   0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_CRTC_X,  0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_CRTC_Y,  0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_CRTC_W,  0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_CRTC_H,  0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_HOTSPOT_X, 0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_HOTSPOT_Y, 0);
    ascentdrm_obj_add_prop(&plane->base, DRM_PROP_ID_FB_DAMAGE_CLIPS, 0);
    return plane;
}

struct drm_crtc *ascentdrm_crtc_create(struct drm_device *dev, struct drm_plane *primary) {
    struct drm_crtc *crtc = kmalloc(sizeof(struct drm_crtc));
    if (!crtc) return NULL;
    memset(crtc, 0, sizeof(struct drm_crtc));

    crtc->primary = primary;
    ascentdrm_mode_object_init(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);

    /* Attach standard CRTC properties */
    ascentdrm_obj_add_prop(&crtc->base, DRM_PROP_ID_ACTIVE,  0);
    ascentdrm_obj_add_prop(&crtc->base, DRM_PROP_ID_MODE_ID, 0);
    return crtc;
}

struct drm_encoder *ascentdrm_encoder_create(struct drm_device *dev, uint32_t type, uint32_t possible_crtcs) {
    struct drm_encoder *enc = kmalloc(sizeof(struct drm_encoder));
    if (!enc) return NULL;
    memset(enc, 0, sizeof(struct drm_encoder));

    enc->encoder_type = type;
    enc->possible_crtcs = possible_crtcs;
    ascentdrm_mode_object_init(dev, &enc->base, DRM_MODE_OBJECT_ENCODER);
    return enc;
}

struct drm_connector *ascentdrm_connector_create(struct drm_device *dev, uint32_t type) {
    struct drm_connector *conn = kmalloc(sizeof(struct drm_connector));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(struct drm_connector));

    conn->connector_type = type;
    conn->connection_status = 1; // Connected
    ascentdrm_mode_object_init(dev, &conn->base, DRM_MODE_OBJECT_CONNECTOR);

    /* Attach standard connector properties.  NOTE: do not advertise a
     * CONNECTOR_ID object property here.  Real drivers have no such kernel
     * property (the id is the object id itself; Xorg publishes its own
     * CONNECTOR_ID RandR property from it), and Xorg's modesetting
     * set_property walk dereferences props[i].atoms[0] for every
     * registered non-RANGE/non-ENUM property - a NULL-atoms entry
     * segfaults Xorg the first time any client (e.g. KWin) sets an output
     * property past index 0. */
    ascentdrm_obj_add_prop(&conn->base, DRM_PROP_ID_DPMS,         0);
    ascentdrm_obj_add_prop(&conn->base, DRM_PROP_ID_CRTC_ID,      0); /* set by atomic */
    return conn;
}

static void drm_kms_add_output(struct drm_device *dev,uint32_t scanout){
    uint32_t mask=scanout<32?(1U<<scanout):0;struct drm_plane *primary=ascentdrm_plane_create(dev,mask);if(!primary)return;ascentdrm_obj_set_prop(&primary->base,DRM_PROP_ID_TYPE,DRM_PLANE_TYPE_PRIMARY);struct drm_crtc *crtc=ascentdrm_crtc_create(dev,primary);if(!crtc)return;crtc->scanout_id=scanout;struct drm_plane *cursor=ascentdrm_plane_create(dev,mask);if(cursor){ascentdrm_obj_set_prop(&cursor->base,DRM_PROP_ID_TYPE,DRM_PLANE_TYPE_CURSOR);crtc->cursor=cursor;}struct drm_encoder *encoder=ascentdrm_encoder_create(dev,1,mask);if(encoder)encoder->scanout_id=scanout;struct drm_connector *connector=ascentdrm_connector_create(dev,11);if(connector){connector->encoder=encoder;connector->scanout_id=scanout;}
}
uint32_t ascentdrm_connector_scanout_id(uint32_t connector_id){struct drm_mode_object *o;list_for_each_entry(o,&global_ascentdrm_dev.kms_objects,list)if(o->type==DRM_MODE_OBJECT_CONNECTOR&&o->id==connector_id)return ((struct drm_connector*)o)->scanout_id;return 0;}
uint32_t ascentdrm_crtc_scanout_id(uint32_t crtc_id){struct drm_mode_object *o;list_for_each_entry(o,&global_ascentdrm_dev.kms_objects,list)if(o->type==DRM_MODE_OBJECT_CRTC&&o->id==crtc_id)return ((struct drm_crtc*)o)->scanout_id;return 0;}
void ascentdrm_ensure_outputs(struct drm_device *dev,uint32_t count){if(!dev)return;if(count>16)count=16;uint32_t have=0;spinlock_acquire(&dev->lock);struct drm_mode_object *o;list_for_each_entry(o,&dev->kms_objects,list)if(o->type==DRM_MODE_OBJECT_CONNECTOR)have++;spinlock_release(&dev->lock);while(have<count)drm_kms_add_output(dev,have++);}
void ascentdrm_update_output_state(struct drm_device *dev,uint32_t scanout,bool connected){if(!dev)return;spinlock_acquire(&dev->lock);struct drm_mode_object *o;list_for_each_entry(o,&dev->kms_objects,list)if(o->type==DRM_MODE_OBJECT_CONNECTOR&&((struct drm_connector*)o)->scanout_id==scanout)((struct drm_connector*)o)->connection_status=connected?1:2;spinlock_release(&dev->lock);}

void ascentdrm_kms_init(struct drm_device *dev) {
    klog_puts("[DRM] Initializing KMS components...\n");
    // 1. Create a primary plane
    struct drm_plane *primary = ascentdrm_plane_create(dev, 0x1);
    ascentdrm_obj_set_prop(&primary->base, DRM_PROP_ID_TYPE, DRM_PLANE_TYPE_PRIMARY);

    // 2. Create a CRTC and link to primary plane
    struct drm_crtc *crtc = ascentdrm_crtc_create(dev, primary);
    crtc->scanout_id = 0;

    struct drm_plane *cursor = ascentdrm_plane_create(dev, 0x1);
    ascentdrm_obj_set_prop(&cursor->base, DRM_PROP_ID_TYPE, DRM_PLANE_TYPE_CURSOR);
    crtc->cursor = cursor;

    // 3. Create an encoder linked to CRTC 1
    struct drm_encoder *encoder = ascentdrm_encoder_create(dev, 1 /* bits */, 0x1);
    encoder->scanout_id = 0;

    // 4. Create a connector linked to encoder
    struct drm_connector *connector = ascentdrm_connector_create(dev, 11 /* HDMI */);
    connector->encoder = encoder;
    connector->scanout_id = 0;

    klog_puts("[DRM] KMS Pipeline: Plane(");
    klog_uint64(primary->base.id);
    klog_puts(") -> CRTC(");
    klog_uint64(crtc->base.id);
    klog_puts(") -> Encoder(");
    klog_uint64(encoder->base.id);
    klog_puts(") -> Connector(");
    klog_uint64(connector->base.id);
    klog_puts(")\n");
}

struct drm_framebuffer *ascentdrm_framebuffer_create_for_file(
    struct drm_file *file, struct drm_device *dev,
    struct drm_mode_fb_cmd *cmd) {
    struct drm_gem_object *gem_obj = ascentdrm_gem_find_by_handle_get(dev, cmd->handle);
    if (!gem_obj) return NULL;

    /* Xorg's modesetting PRIME output sink uses legacy ADDFB for its
     * imported pixmap. Prepare the host-visible scanout backing here too,
     * before exposing a framebuffer that the VirtIO device cannot upload. */
    if (gem_obj->prime_buffer && virtio_gpu_is_initialized()) {
        int ret = -22;
        if (cmd->bpp == 32 && cmd->depth == 24) {
            ret = ascentdrm_gem_begin_cpu(gem_obj);
            if (!ret) {
                ret = virtio_gpu_prepare_prime_fb(gem_obj, cmd->width,
                                                  cmd->height, cmd->pitch, 0);
                ascentdrm_gem_end_cpu(gem_obj);
            }
        }
        if (ret) {
            if (ascentdrm_gem_put_last(gem_obj))
                ascentdrm_gem_object_free(dev, gem_obj);
            return NULL;
        }
    }

    struct drm_framebuffer *fb = kmalloc(sizeof(struct drm_framebuffer));
    if (!fb) {
        if (ascentdrm_gem_put_last(gem_obj))
            ascentdrm_gem_object_free(dev, gem_obj);
        return NULL;
    }
    memset(fb, 0, sizeof(struct drm_framebuffer));

    fb->owner_file = file;
    fb->width = cmd->width;
    fb->height = cmd->height;
    fb->pitch = cmd->pitch;
    fb->bpp = cmd->bpp;
    /* Legacy ADDFB has no fourcc. depth=32 means ARGB8888,
     * depth=24/bpp=32 means XRGB8888, bpp=16 means RGB565. */
    if (cmd->bpp == 32 && cmd->depth == 32)
        fb->pixel_format = 0x34325241; /* ARGB8888 */
    else if (cmd->bpp == 32)
        fb->pixel_format = 0x34325258; /* XRGB8888 */
    else if (cmd->bpp == 16)
        fb->pixel_format = 0x36315652; /* RGB565 */
    else
        fb->pixel_format = 0;
    fb->gem_obj = gem_obj;

    ascentdrm_mode_object_init(dev, &fb->base, DRM_MODE_OBJECT_FB);
    return fb;
}

struct drm_framebuffer *ascentdrm_framebuffer_create(
    struct drm_device *dev, struct drm_mode_fb_cmd *cmd) {
    return ascentdrm_framebuffer_create_for_file(NULL, dev, cmd);
}

void ascentdrm_framebuffer_unlink(struct drm_device *dev,
                                  struct drm_framebuffer *fb) {
    spinlock_acquire(&dev->lock);
    list_del(&fb->base.list);
    /* KMS objects must never retain a framebuffer that is being deleted. */
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
        if (obj->type == DRM_MODE_OBJECT_CRTC) {
            struct drm_crtc *crtc = (struct drm_crtc *)obj;
            if (crtc->fb == fb)
                crtc->fb = NULL;
        } else if (obj->type == DRM_MODE_OBJECT_PLANE) {
            struct drm_plane *plane = (struct drm_plane *)obj;
            if (plane->fb == fb)
                plane->fb = NULL;
        }
    }
    spinlock_release(&dev->lock);
}

void ascentdrm_framebuffer_free(struct drm_device *dev,
                                struct drm_framebuffer *fb) {
    if (fb->is_full) {
        ascentdrm_framebuffer_free_full(dev, fb);
        return;
    }

    ascentdrm_framebuffer_unlink(dev, fb);

    if (fb->gem_obj) {
        struct drm_gem_object *gem = fb->gem_obj;
        fb->gem_obj = NULL;
        if (ascentdrm_gem_put_last(gem))
            ascentdrm_gem_object_free(dev, gem);
    }
    kfree(fb);
}

static bool ascentdrm_framebuffer_in_use_locked(struct drm_device *dev,
                                                 struct drm_framebuffer *fb) {
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
        if (obj->type == DRM_MODE_OBJECT_CRTC &&
            ((struct drm_crtc *)obj)->fb == fb)
            return true;
        if (obj->type == DRM_MODE_OBJECT_PLANE &&
            ((struct drm_plane *)obj)->fb == fb)
            return true;
    }
    return false;
}

void ascentdrm_framebuffer_reap_closed(struct drm_device *dev) {
    if (!dev)
        return;

    for (;;) {
        struct drm_framebuffer *dead = NULL;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *obj;
        list_for_each_entry(obj, &dev->kms_objects, list) {
            if (obj->type != DRM_MODE_OBJECT_FB)
                continue;
            struct drm_framebuffer *fb = (struct drm_framebuffer *)obj;
            if (fb->closed && !fb->destroying &&
                !ascentdrm_framebuffer_in_use_locked(dev, fb)) {
                fb->destroying = 1;
                dead = fb;
                break;
            }
        }
        spinlock_release(&dev->lock);

        if (!dead)
            return;
        ascentdrm_framebuffer_free(dev, dead);
    }
}

void ascentdrm_framebuffer_close_file(struct drm_device *dev,
                                      struct drm_file *file) {
    if (!dev || !file)
        return;

    spinlock_acquire(&dev->lock);
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
        if (obj->type != DRM_MODE_OBJECT_FB)
            continue;
        struct drm_framebuffer *fb = (struct drm_framebuffer *)obj;
        if (fb->owner_file == file) {
            fb->owner_file = NULL;
            fb->closed = 1;
        }
    }
    spinlock_release(&dev->lock);

    ascentdrm_framebuffer_reap_closed(dev);
}

void ascentdrm_framebuffer_drop_closed_scanout(struct drm_device *dev) {
    if (!dev)
        return;

    spinlock_acquire(&dev->lock);
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
        if (obj->type == DRM_MODE_OBJECT_CRTC) {
            struct drm_crtc *crtc = (struct drm_crtc *)obj;
            if (crtc->fb && crtc->fb->closed)
                crtc->fb = NULL;
        } else if (obj->type == DRM_MODE_OBJECT_PLANE) {
            struct drm_plane *plane = (struct drm_plane *)obj;
            if (plane->fb && plane->fb->closed)
                plane->fb = NULL;
        }
    }
    spinlock_release(&dev->lock);

    ascentdrm_framebuffer_reap_closed(dev);
}
