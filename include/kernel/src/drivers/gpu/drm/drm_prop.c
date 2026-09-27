#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../apic/lapic_timer.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "drm.h"

extern drm_pageflip_fn_t g_drm_pageflip_fn;

/* ── Global property catalogue ──────────────────────────────────────────── */

static const struct drm_property_def drm_prop_catalogue[] = {
    /* Plane properties */
    {DRM_PROP_ID_CRTC_ID, DRM_PROP_TYPE_OBJECT | DRM_PROP_FLAG_ATOMIC, "CRTC_ID", 0, UINT32_MAX},
    {DRM_PROP_ID_FB_ID, DRM_PROP_TYPE_OBJECT | DRM_PROP_FLAG_ATOMIC, "FB_ID", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_X, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "SRC_X", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_Y, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "SRC_Y", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_W, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "SRC_W", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_H, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "SRC_H", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_X, DRM_PROP_TYPE_SIGNED_RANGE | DRM_PROP_FLAG_ATOMIC, "CRTC_X", (uint64_t)INT32_MIN, INT32_MAX},
    {DRM_PROP_ID_CRTC_Y, DRM_PROP_TYPE_SIGNED_RANGE | DRM_PROP_FLAG_ATOMIC, "CRTC_Y", (uint64_t)INT32_MIN, INT32_MAX},
    {DRM_PROP_ID_CRTC_W, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "CRTC_W", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_H, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "CRTC_H", 0, UINT32_MAX},
    {DRM_PROP_ID_HOTSPOT_X, DRM_PROP_TYPE_SIGNED_RANGE | DRM_PROP_FLAG_ATOMIC, "HOTSPOT_X", 0, INT32_MAX},
    {DRM_PROP_ID_HOTSPOT_Y, DRM_PROP_TYPE_SIGNED_RANGE | DRM_PROP_FLAG_ATOMIC, "HOTSPOT_Y", 0, INT32_MAX},
    {DRM_PROP_ID_FB_DAMAGE_CLIPS, DRM_PROP_TYPE_BLOB | DRM_PROP_FLAG_ATOMIC,
     "FB_DAMAGE_CLIPS", 0, UINT32_MAX},
    /* CRTC properties */
    {DRM_PROP_ID_ACTIVE, DRM_PROP_TYPE_RANGE | DRM_PROP_FLAG_ATOMIC, "ACTIVE", 0, 1},
    {DRM_PROP_ID_MODE_ID, DRM_PROP_FLAG_ATOMIC | DRM_PROP_TYPE_BLOB, "MODE_ID",
     0, UINT32_MAX},
    /* Connector properties */
    {DRM_PROP_ID_DPMS, DRM_PROP_TYPE_ENUM, "DPMS", 0, 3},
    {DRM_PROP_ID_CONNECTOR_ID, DRM_PROP_TYPE_OBJECT | DRM_PROP_FLAG_ATOMIC | DRM_PROP_FLAG_IMMUTABLE,
     "CONNECTOR_ID", 0, UINT32_MAX},
    /* Plane type property */
    {DRM_PROP_ID_TYPE, DRM_PROP_TYPE_ENUM | DRM_PROP_FLAG_IMMUTABLE, "type", 0,
     2},
    /* Note: DRM_PROP_ID_CRTC_ID is shared between planes and connectors —
     * the same catalogue entry covers both (name "CRTC_ID", atomic flag). */
};

#define PROP_CATALOGUE_SIZE                                                    \
  (sizeof(drm_prop_catalogue) / sizeof(drm_prop_catalogue[0]))

const struct drm_property_def *drm_prop_find_def(uint32_t prop_id) {
  for (size_t i = 0; i < PROP_CATALOGUE_SIZE; i++) {
    if (drm_prop_catalogue[i].id == prop_id)
      return &drm_prop_catalogue[i];
  }
  return NULL;
}

/* ── Object property helpers ─────────────────────────────────────────────── */

/* Attach a property with its default value to a mode object */
void drm_obj_add_prop(struct drm_mode_object *obj, uint32_t prop_id,
                      uint64_t default_val) {
  if (obj->prop_count >= DRM_MAX_OBJ_PROPS)
    return;
  obj->props[obj->prop_count].prop_id = prop_id;
  obj->props[obj->prop_count].value = default_val;
  obj->prop_count++;
}

/* Get a property value from an object; returns -1 if not found */
int drm_obj_get_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t *out) {
  for (uint32_t i = 0; i < obj->prop_count; i++) {
    if (obj->props[i].prop_id == prop_id) {
      *out = obj->props[i].value;
      return 0;
    }
  }
  return -1;
}

/* Set a property value on an object; returns -1 if not found */
int drm_obj_set_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t value) {
  for (uint32_t i = 0; i < obj->prop_count; i++) {
    if (obj->props[i].prop_id == prop_id) {
      obj->props[i].value = value;
      return 0;
    }
  }
  return -1;
}

/* ── Blob management ─────────────────────────────────────────────────────── */

struct drm_prop_blob *drm_blob_create(struct drm_device *dev, const void *data,
                                      uint32_t length) {
  struct drm_prop_blob *blob = kmalloc(sizeof(struct drm_prop_blob));
  if (!blob)
    return NULL;

  blob->data = kmalloc(length);
  if (!blob->data) {
    kfree(blob);
    return NULL;
  }

  memcpy(blob->data, data, length);
  blob->length = length;

  spinlock_acquire(&dev->lock);
  blob->id = dev->next_blob_id++;
  list_add_tail(&blob->list, &dev->blob_objects);
  spinlock_release(&dev->lock);

  return blob;
}

struct drm_prop_blob *drm_blob_find(struct drm_device *dev, uint32_t id) {
  struct drm_prop_blob *b;
  list_for_each_entry(b, &dev->blob_objects, list) {
    if (b->id == id)
      return b;
  }
  return NULL;
}

void drm_blob_destroy(struct drm_device *dev, uint32_t id) {
  spinlock_acquire(&dev->lock);
  struct drm_prop_blob *b;
  list_for_each_entry(b, &dev->blob_objects, list) {
    if (b->id == id) {
      list_del(&b->list);
      spinlock_release(&dev->lock);
      kfree(b->data);
      kfree(b);
      return;
    }
  }
  spinlock_release(&dev->lock);
}

/* ── DRM_IOCTL_MODE_OBJ_GETPROPERTIES ───────────────────────────────────── */

int drm_ioctl_obj_getprops(struct drm_device *dev, uint64_t arg) {
  struct drm_mode_obj_get_properties *req =
      (struct drm_mode_obj_get_properties *)arg;

  klog_debug_puts("[DRM] OBJ_GETPROPS obj=");
  klog_debug_uint64(req->obj_id);
  klog_debug_puts(" type_in=0x");
  klog_debug_hex32(req->obj_type);
  klog_debug_puts(" count_in=");
  klog_debug_uint64(req->count_props);
  klog_debug_puts(" props_ptr=0x");
  klog_debug_hex64(req->props_ptr);
  klog_debug_puts(" vals_ptr=0x");
  klog_debug_hex64(req->prop_values_ptr);
  klog_debug_puts("\n");

  spinlock_acquire(&dev->lock);
  struct drm_mode_object *mobj = NULL;
  struct drm_mode_object *iter;
  list_for_each_entry(iter, &dev->kms_objects, list) {
    if (iter->id == req->obj_id) {
      mobj = iter;
      break;
    }
  }
  if (!mobj) {
    klog_debug_puts("[DRM] OBJ_GETPROPS missing obj=");
    klog_debug_uint64(req->obj_id);
    klog_debug_puts("\n");
    spinlock_release(&dev->lock);
    return -2;
  } /* ENOENT */

  uint32_t count = mobj->prop_count;
  if (req->props_ptr && req->count_props >= count) {
    uint32_t *prop_ids = (uint32_t *)req->props_ptr;
    uint64_t *prop_vals = (uint64_t *)req->prop_values_ptr;
    for (uint32_t i = 0; i < count; i++) {
      prop_ids[i] = mobj->props[i].prop_id;
      prop_vals[i] = mobj->props[i].value;
    }
  }
  req->count_props = count;
  klog_debug_puts("[DRM] OBJ_GETPROPS out obj=");
  klog_debug_uint64(req->obj_id);
  klog_debug_puts(" type=0x");
  klog_debug_hex32(mobj->type);
  klog_debug_puts(" count=");
  klog_debug_uint64(count);
  klog_debug_puts("\n");
  spinlock_release(&dev->lock);
  return 0;
}

/* ── DRM_IOCTL_MODE_GETPROPERTY ─────────────────────────────────────────── */

int drm_ioctl_getproperty(struct drm_device *dev, uint64_t arg) {
  (void)dev;
  struct {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
  } *p = (void *)arg;

  klog_debug_puts("[DRM] GETPROPERTY id=");
  klog_debug_uint64(p->prop_id);
  klog_debug_puts(" values_ptr=0x");
  klog_debug_hex64(p->values_ptr);
  klog_debug_puts(" enum_ptr=0x");
  klog_debug_hex64(p->enum_blob_ptr);
  klog_debug_puts(" count_values_in=");
  klog_debug_uint64(p->count_values);
  klog_debug_puts(" count_enum_in=");
  klog_debug_uint64(p->count_enum_blobs);
  klog_debug_puts("\n");

  const struct drm_property_def *def = drm_prop_find_def(p->prop_id);
  if (!def) {
    /* Unknown property — return a harmless stub so userland doesn't crash */
    p->flags = 0;
    p->count_values = 0;
    p->count_enum_blobs = 0;
    strncpy(p->name, "Unknown", 32);
    klog_debug_puts("[DRM] GETPROPERTY unknown id=");
    klog_debug_uint64(p->prop_id);
    klog_debug_puts("\n");
    return 0;
  }

  p->flags = def->flags;
  p->count_enum_blobs = 0;
  strncpy(p->name, def->name, 32);

  /* For RANGE properties expose [min, max] */
  if ((def->flags & DRM_PROP_TYPE_RANGE) ||
      ((def->flags & DRM_PROP_EXTENDED_TYPE_MASK) ==
       DRM_PROP_TYPE_SIGNED_RANGE)) {
    p->count_values = 2;
    if (p->values_ptr) {
      uint64_t *vals = (uint64_t *)p->values_ptr;
      vals[0] = def->min_val;
      vals[1] = def->max_val;
    }
  } else if (def->flags & DRM_PROP_TYPE_ENUM) {
    /* Special-case enum labels that compositors expect. */
    if (def->id == DRM_PROP_ID_TYPE || def->id == DRM_PROP_ID_DPMS) {
      p->count_enum_blobs = (def->id == DRM_PROP_ID_TYPE) ? 3 : 4;
      if (p->enum_blob_ptr) {
        struct {
          uint64_t value;
          char name[32];
        } *enums = (void *)p->enum_blob_ptr;
        if (def->id == DRM_PROP_ID_TYPE) {
          enums[0].value = 0;
          strcpy(enums[0].name, "Overlay");
          enums[1].value = 1;
          strcpy(enums[1].name, "Primary");
          enums[2].value = 2;
          strcpy(enums[2].name, "Cursor");
        } else {
          enums[0].value = 0;
          strcpy(enums[0].name, "On");
          enums[1].value = 1;
          strcpy(enums[1].name, "Standby");
          enums[2].value = 2;
          strcpy(enums[2].name, "Suspend");
          enums[3].value = 3;
          strcpy(enums[3].name, "Off");
        }
      }
    } else {
      p->count_enum_blobs = 0;
    }
    p->count_values = 0;
  } else {
    p->count_values = 0;
  }
  klog_debug_puts("[DRM] GETPROPERTY out id=");
  klog_debug_uint64(p->prop_id);
  klog_debug_puts(" name=");
  klog_debug_puts(p->name);
  klog_debug_puts(" flags=0x");
  klog_debug_hex32(p->flags);
  klog_debug_puts(" values=");
  klog_debug_uint64(p->count_values);
  klog_debug_puts(" enums=");
  klog_debug_uint64(p->count_enum_blobs);
  klog_debug_puts("\n");
  return 0;
}

/* ── Atomic commit engine ────────────────────────────────────────────────── */

extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev,
                                                     uint32_t handle);
extern void drm_file_send_event(struct drm_file *file,
                                struct drm_event_vblank *ev,
                                struct vfs_node *node);

static uint32_t drm_atomic_event_sequence = 1;

static uint32_t drm_plane_type(struct drm_plane *plane) {
  uint64_t type = DRM_PLANE_TYPE_OVERLAY;
  drm_obj_get_prop(&plane->base, DRM_PROP_ID_TYPE, &type);
  return (uint32_t)type;
}

static struct drm_crtc *drm_find_crtc(struct drm_device *dev, uint32_t id) {
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type == DRM_MODE_OBJECT_CRTC && obj->id == id)
      return (struct drm_crtc *)obj;
  }
  return NULL;
}

static struct drm_crtc *drm_find_crtc_for_plane(struct drm_device *dev,
                                                struct drm_plane *plane) {
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type != DRM_MODE_OBJECT_CRTC)
      continue;
    struct drm_crtc *crtc = (struct drm_crtc *)obj;
    if (crtc->primary == plane || crtc->cursor == plane)
      return crtc;
  }
  return NULL;
}

static int drm_cursor_fb_validate(struct drm_framebuffer *fb) {
  if (!fb)
    return 0;
  if (!fb->gem_obj || !fb->gem_obj->virt_addr || fb->bpp != 32)
    return -22;
  if (fb->width == 0 || fb->height == 0 || fb->width > 64 || fb->height > 64)
    return -22;
  if (fb->pitch < fb->width * 4)
    return -22;
  if (fb->pixel_format != 0x34325241 && fb->pixel_format != 0x34325258)
    return -22;
  return 0;
}

static void drm_fill_atomic_vblank_event(struct drm_event_vblank *ev,
                                         uint32_t crtc_id) {
  uint64_t ms = lapic_timer_get_ms();
  ev->tv_sec = (uint32_t)(ms / 1000);
  ev->tv_usec = (uint32_t)((ms % 1000) * 1000);
  ev->sequence = drm_atomic_event_sequence++;
  ev->crtc_id = crtc_id;
}

/*
 * Apply a single (object, property, value) triple.
 * Returns 0 on success, -1 on unknown object/property.
 */
int atomic_apply_prop(struct drm_device *dev,
                             struct drm_mode_object *obj, uint32_t prop_id,
                             uint64_t value) {
  /* Validate the property exists in our catalogue */
  if (!drm_prop_find_def(prop_id))
    return -38; /* ENOSYS */

  switch (obj->type) {
  case DRM_MODE_OBJECT_PLANE: {
    struct drm_plane *plane = (struct drm_plane *)obj;
    switch (prop_id) {
    case DRM_PROP_ID_FB_ID: {
      uint32_t plane_type = drm_plane_type(plane);
      struct drm_framebuffer *new_fb = NULL;
      if (value != 0) {
        struct drm_mode_object *fbobj;
        list_for_each_entry(fbobj, &dev->kms_objects, list) {
          if (fbobj->type == DRM_MODE_OBJECT_FB &&
              fbobj->id == (uint32_t)value) {
            new_fb = (struct drm_framebuffer *)fbobj;
            break;
          }
        }
        if (!new_fb)
          return -2;
      }

      if (plane_type == DRM_PLANE_TYPE_CURSOR &&
          drm_cursor_fb_validate(new_fb) != 0) {
        klog_debug_puts("[DRM] atomic cursor: rejected invalid cursor FB\n");
        return -22;
      }

      plane->fb = new_fb;

      struct drm_crtc *owner = drm_find_crtc_for_plane(dev, plane);
      if (owner && owner->primary == plane)
        owner->fb = new_fb;

      if (new_fb) {
        if (plane->src_w == 0)
          plane->src_w = new_fb->width << 16;
        if (plane->src_h == 0)
          plane->src_h = new_fb->height << 16;
        if (plane->crtc_w == 0)
          plane->crtc_w = new_fb->width;
        if (plane->crtc_h == 0)
          plane->crtc_h = new_fb->height;
      }

      if (plane_type == DRM_PLANE_TYPE_CURSOR) {
#if DRM_DEBUG_LOGGING
        klog_debug_puts("[DRM] atomic cursor: FB_ID=");
        klog_debug_uint64((uint32_t)value);
        klog_debug_puts(" size=");
        klog_debug_uint64(new_fb ? new_fb->width : 0);
        klog_debug_puts("x");
        klog_debug_uint64(new_fb ? new_fb->height : 0);
        klog_debug_puts(" fmt=0x");
        klog_debug_hex32(new_fb ? new_fb->pixel_format : 0);
        klog_debug_puts("\n");
#endif
      }
      break;
    }
    case DRM_PROP_ID_CRTC_ID: {
      uint32_t plane_type = drm_plane_type(plane);
      if (value == 0) {
        plane->fb = NULL;
        struct drm_crtc *owner = drm_find_crtc_for_plane(dev, plane);
        if (owner && owner->primary == plane)
          owner->fb = NULL;
      } else {
        struct drm_crtc *crtc = drm_find_crtc(dev, (uint32_t)value);
        if (!crtc)
          return -2;
        if (plane_type == DRM_PLANE_TYPE_CURSOR)
          crtc->cursor = plane;
        else if (plane_type == DRM_PLANE_TYPE_PRIMARY) {
          crtc->primary = plane;
          if (plane->fb)
            crtc->fb = plane->fb;
        }
      }
      if (plane_type == DRM_PLANE_TYPE_CURSOR) {
#if DRM_DEBUG_LOGGING
        klog_debug_puts("[DRM] atomic cursor: CRTC_ID=");
        klog_debug_uint64((uint32_t)value);
        klog_debug_puts("\n");
#endif
      }
      break;
    }

    case DRM_PROP_ID_SRC_X:
      plane->src_x = (uint32_t)value;
      break;
    case DRM_PROP_ID_SRC_Y:
      plane->src_y = (uint32_t)value;
      break;
    case DRM_PROP_ID_SRC_W:
      plane->src_w = (uint32_t)value;
      break;
    case DRM_PROP_ID_SRC_H:
      plane->src_h = (uint32_t)value;
      break;

    case DRM_PROP_ID_CRTC_X:
      plane->crtc_x = (int32_t)value;
      break;
    case DRM_PROP_ID_CRTC_Y:
      plane->crtc_y = (int32_t)value;
      break;
    case DRM_PROP_ID_CRTC_W:
      plane->crtc_w = (uint32_t)value;
      break;
    case DRM_PROP_ID_CRTC_H:
      plane->crtc_h = (uint32_t)value;
      break;
    case DRM_PROP_ID_HOTSPOT_X:
      plane->hotspot_x = (int32_t)value;
      break;
    case DRM_PROP_ID_HOTSPOT_Y:
      plane->hotspot_y = (int32_t)value;
      break;
    case DRM_PROP_ID_FB_DAMAGE_CLIPS:
      /* The blob payload is consumed by drm_ioctl_atomic below. */
      break;
    default:
      return -1;
    }
    break;
  }
  case DRM_MODE_OBJECT_CRTC: {
    switch (prop_id) {
    case DRM_PROP_ID_ACTIVE:
      /* ACTIVE=0 means disable CRTC; we just track it */
      break;
    case DRM_PROP_ID_MODE_ID:
      /* MODE_ID points to a blob containing drm_mode_modeinfo */
      break;
    default:
      return -1;
    }
    break;
  }
  case DRM_MODE_OBJECT_CONNECTOR: {
    switch (prop_id) {
    case DRM_PROP_ID_DPMS:
    case DRM_PROP_ID_CONNECTOR_ID:
      break;
    case DRM_PROP_ID_CRTC_ID: {
      /*
       * wlroots sets CRTC_ID on the connector during atomic modeset.
       * Link this connector to the specified CRTC so the KMS pipeline
       * is complete: connector → encoder → CRTC.
       */
      struct drm_connector *conn = (struct drm_connector *)obj;
      if (value == 0) {
        /* Disconnect: detach encoder from any CRTC */
        (void)conn;
      } else {
        /* Find the CRTC and make sure the encoder points to it */
        struct drm_mode_object *cobj;
        list_for_each_entry(cobj, &dev->kms_objects, list) {
          if (cobj->type == DRM_MODE_OBJECT_CRTC &&
              cobj->id == (uint32_t)value) {
            /* The encoder already has possible_crtcs=0x1 covering
             * this CRTC — nothing structural to change, just
             * persist the value so GETCONNECTOR reflects it. */
            break;
          }
        }
      }
      break;
    }
    default:
      return -38; /* ENOSYS */
    }
    break;
  }
  default:
    return -38; /* ENOSYS */
  }

  /* Persist the value in the object's property table */
  drm_obj_set_prop(obj, prop_id, value);
  return 0;
}

int drm_ioctl_obj_setproperty(struct drm_device *dev, uint64_t arg) {
  struct {
    uint64_t value;
    uint32_t prop_id;
    uint32_t obj_id;
    uint32_t obj_type;
  } *req = (void *)arg;

#if DRM_DEBUG_LOGGING
  klog_debug_puts("[DRM] OBJ_SETPROPERTY obj=");
  klog_debug_uint64(req->obj_id);
  klog_debug_puts(" type=0x");
  klog_debug_hex32(req->obj_type);
  klog_debug_puts(" prop=");
  klog_debug_uint64(req->prop_id);
  klog_debug_puts(" val=");
  klog_debug_uint64(req->value);
  klog_debug_puts("\n");
#endif

  spinlock_acquire(&dev->lock);
  struct drm_mode_object *mobj = NULL;
  struct drm_mode_object *iter;
  list_for_each_entry(iter, &dev->kms_objects, list) {
    if (iter->id == req->obj_id &&
        (req->obj_type == 0 || req->obj_type == iter->type)) {
      mobj = iter;
      break;
    }
  }
  if (!mobj) {
    spinlock_release(&dev->lock);
    return -2;
  }

  int ret = atomic_apply_prop(dev, mobj, req->prop_id, req->value);
  spinlock_release(&dev->lock);
  return ret;
}

int drm_ioctl_atomic(struct vfs_node *node, struct drm_file *file,
                     struct drm_device *dev, uint64_t arg) {
  struct drm_mode_atomic *req = (struct drm_mode_atomic *)arg;

  if (!req->count_objs)
    return 0;

  uint32_t *obj_ids = (uint32_t *)req->objs_ptr;
  uint32_t *prop_cnts = (uint32_t *)req->count_props_ptr;
  uint32_t *prop_ids = (uint32_t *)req->props_ptr;
  uint64_t *prop_vals = (uint64_t *)req->prop_values_ptr;

  if (!obj_ids || !prop_cnts || !prop_ids || !prop_vals)
    return -14; /* EFAULT */

  int test_only = (req->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0;
  int nonblock = (req->flags & DRM_MODE_ATOMIC_NONBLOCK) != 0;
  (void)nonblock;

  spinlock_acquire(&dev->lock);

  if (!test_only)
    dev->pending_damage_valid = 0;

  uint32_t event_crtc_id = 0;
  uint32_t prop_offset = 0;
  for (uint32_t i = 0; i < req->count_objs; i++) {
    uint32_t obj_id = obj_ids[i];
    uint32_t num_props = prop_cnts[i];

    /* Find the object */
    struct drm_mode_object *mobj = NULL;
    struct drm_mode_object *iter;
    list_for_each_entry(iter, &dev->kms_objects, list) {
      if (iter->id == obj_id) {
        mobj = iter;
        break;
      }
    }
    if (!mobj) {
      spinlock_release(&dev->lock);
      klog_debug_puts("[DRM] atomic: unknown object id=");
      klog_debug_uint64(obj_id);
      klog_debug_puts("\n");
      return -1;
    }

    for (uint32_t j = 0; j < num_props; j++) {
      uint32_t pid = prop_ids[prop_offset + j];
      uint64_t val = prop_vals[prop_offset + j];

#if DRM_DEBUG_LOGGING
      klog_debug_puts("[DRM] atomic: obj=");
      klog_debug_uint64(obj_id);
      klog_debug_puts(" prop=");
      klog_debug_uint64(pid);
      klog_debug_puts(" val=");
      klog_debug_uint64(val);
      klog_debug_puts("\n");
#endif

      if (mobj->type == DRM_MODE_OBJECT_CRTC)
        event_crtc_id = obj_id;
      if (pid == DRM_PROP_ID_CRTC_ID && val != 0)
        event_crtc_id = (uint32_t)val;

      if (!test_only) {
        if (mobj->type == DRM_MODE_OBJECT_PLANE &&
            pid == DRM_PROP_ID_FB_DAMAGE_CLIPS && val != 0) {
          struct drm_prop_blob *blob = drm_blob_find(dev, (uint32_t)val);
          if (blob && blob->length >= sizeof(struct drm_mode_rect) &&
              blob->length % sizeof(struct drm_mode_rect) == 0) {
            const struct drm_mode_rect *rects = blob->data;
            uint32_t count = blob->length / sizeof(*rects);
            int32_t x1 = INT32_MAX, y1 = INT32_MAX;
            int32_t x2 = 0, y2 = 0;

            for (uint32_t r = 0; r < count; r++) {
              if (rects[r].x2 <= rects[r].x1 ||
                  rects[r].y2 <= rects[r].y1)
                continue;
              int32_t rx1 = rects[r].x1 < 0 ? 0 : rects[r].x1;
              int32_t ry1 = rects[r].y1 < 0 ? 0 : rects[r].y1;
              int32_t rx2 = rects[r].x2 > UINT16_MAX
                                ? UINT16_MAX : rects[r].x2;
              int32_t ry2 = rects[r].y2 > UINT16_MAX
                                ? UINT16_MAX : rects[r].y2;
              if (rx2 <= rx1 || ry2 <= ry1)
                continue;
              if (rx1 < x1) x1 = rx1;
              if (ry1 < y1) y1 = ry1;
              if (rx2 > x2) x2 = rx2;
              if (ry2 > y2) y2 = ry2;
            }

            if (x1 < x2 && y1 < y2) {
              dev->pending_damage.x1 = (uint16_t)x1;
              dev->pending_damage.y1 = (uint16_t)y1;
              dev->pending_damage.x2 = (uint16_t)x2;
              dev->pending_damage.y2 = (uint16_t)y2;
              dev->pending_damage_valid = 1;
            }
          }
        }
        if (atomic_apply_prop(dev, mobj, pid, val) != 0) {
          klog_debug_puts("[DRM] atomic: unknown prop_id=");
          klog_debug_uint64(pid);
          klog_debug_puts(" on obj=");
          klog_debug_uint64(obj_id);
          klog_debug_puts(" (ignored)\n");
          /* Non-fatal: skip unknown props for forward compat */
        }
      }
    }
    prop_offset += num_props;
  }

  /* Fire a page-flip complete event to the calling client's queue */
  if (!test_only && (req->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
    if (g_drm_pageflip_fn) {
      g_drm_pageflip_fn(file, node, event_crtc_id, 0, req->user_data);
      spinlock_release(&dev->lock);
      goto done;
    }
    struct drm_event_vblank ev = {0};
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(struct drm_event_vblank);
    ev.user_data = req->user_data;
    drm_fill_atomic_vblank_event(&ev, event_crtc_id);
    spinlock_release(&dev->lock);
    drm_file_send_event(file, &ev, node);
    goto done;
  }

  spinlock_release(&dev->lock);
done:
#if DRM_DEBUG_LOGGING
  klog_debug_puts("[DRM] atomic commit: ");
  klog_debug_uint64(req->count_objs);
  klog_debug_puts(" objects, flags=0x");
  klog_debug_hex32(req->flags);
  klog_debug_puts(test_only ? " (TEST_ONLY)\n" : "\n");
#endif

  return 0;
}
