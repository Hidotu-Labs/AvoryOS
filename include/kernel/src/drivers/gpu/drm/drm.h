#ifndef DRM_H
#define DRM_H

#include "../../../fs/vfs.h"
#include "../../../lib/list.h"
#include "../../../lib/string.h"
#include "../../../lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

#ifndef DRM_DEBUG_LOGGING
#define DRM_DEBUG_LOGGING 0
#endif

#define DRM_MAJOR 226

// KMS Object types
#define DRM_MODE_OBJECT_CRTC 0xcccccccc
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee
#define DRM_MODE_OBJECT_CONNECTOR 0x000000c0
#define DRM_MODE_OBJECT_ENCODER 0xeec0ffee
#define DRM_MODE_OBJECT_FB 0xfbfbfbfb

// Standard DRM IOCTLs (simplified)
#define DRM_IOCTL_VERSION 0xC0406400
#define DRM_IOCTL_GET_UNIQUE 0xC0106401
#define DRM_IOCTL_SET_VERSION 0xC0106407
#define DRM_IOCTL_GET_CAP 0xC010640C
/* Ascent private GEM stress-test ioctl; Linux 0x6401 is GET_UNIQUE. */
#define DRM_IOCTL_GEM_CREATE 0xC01064E0
#define DRM_IOCTL_GEM_FREE 0x40086402
#define DRM_IOCTL_GEM_MMAP 0xC0106403
#define DRM_IOCTL_GEM_CLOSE 0x40086409
#define DRM_IOCTL_WAIT_VBLANK 0xC018643A

#define DRM_IOCTL_SET_MASTER 0x0000641E
#define DRM_IOCTL_DROP_MASTER 0x0000641F

#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC 0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC 0xC06864A2
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7
#define DRM_IOCTL_MODE_GETENCODER 0xC01464A6
#define DRM_IOCTL_MODE_CURSOR 0xC01C64A3
#define DRM_IOCTL_MODE_GETGAMMA 0xC02064A4
#define DRM_IOCTL_MODE_SETGAMMA 0xC02064A5
#define DRM_IOCTL_MODE_CURSOR2 0xC02464BB
#define DRM_IOCTL_MODE_ADDFB 0xC01C64AE
#define DRM_IOCTL_MODE_RMFB 0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP 0xC01864B0
#define DRM_IOCTL_MODE_CREATE_DUMB 0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB 0xC01064B3
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640D
#define DRM_IOCTL_MODE_GETPROPERTY 0xC04064AA
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xC01064B5
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xC02064B9
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY 0xC01864BA
#define DRM_IOCTL_MODE_ATOMIC 0xC03864BC
#define DRM_IOCTL_MODE_CREATEPROPBLOB 0xC01064BD
#define DRM_IOCTL_MODE_DESTROYPROPBLOB 0xC00464BE
#define DRM_IOCTL_MODE_ADDFB2 0xC06864B8
#define DRM_IOCTL_MODE_CREATE_LEASE 0xC01864C6
#define DRM_IOCTL_MODE_SETPROPERTY 0xC01064AB
#define DRM_IOCTL_MODE_DIRTYFB 0xC01864B1
#define DRM_IOCTL_MODE_DIRTYFB_LEGACY 0x401064B1

#define DRM_CLIENT_CAP_STEREO_3D 1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC 3
#define DRM_CLIENT_CAP_ASPECT_RATIO 4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6

#define DRM_FILE_CAP_STEREO_3D (1U << DRM_CLIENT_CAP_STEREO_3D)
#define DRM_FILE_CAP_UNIVERSAL_PLANES (1U << DRM_CLIENT_CAP_UNIVERSAL_PLANES)
#define DRM_FILE_CAP_ATOMIC (1U << DRM_CLIENT_CAP_ATOMIC)
#define DRM_FILE_CAP_ASPECT_RATIO (1U << DRM_CLIENT_CAP_ASPECT_RATIO)
#define DRM_FILE_CAP_WRITEBACK_CONNECTORS                                      \
  (1U << DRM_CLIENT_CAP_WRITEBACK_CONNECTORS)
#define DRM_FILE_CAP_CURSOR_PLANE_HOTSPOT                                      \
  (1U << DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT)

#define DRM_CAP_DUMB_BUFFER 1
#define DRM_CAP_VBLANK_HIGH_CRTC 2
#define DRM_CAP_DUMB_PREFER_SHADOW 4
#define DRM_CAP_PRIME 5
#define DRM_CAP_TIMESTAMP_MONOTONIC 6
#define DRM_CAP_ASYNC_PAGE_FLIP 7
#define DRM_CAP_CURSOR_WIDTH 8
#define DRM_CAP_CURSOR_HEIGHT 9
#define DRM_CAP_ATOMIC 0x0A
#define DRM_CAP_ADDFB2_MODIFIERS 0x10

/* Atomic commit flags */
#define DRM_MODE_ATOMIC_TEST_ONLY 0x0100
#define DRM_MODE_ATOMIC_NONBLOCK 0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02

struct drm_mode_card_res {
  uint64_t fb_id_ptr;
  uint64_t crtc_id_ptr;
  uint64_t connector_id_ptr;
  uint64_t encoder_id_ptr;
  uint32_t count_fbs;
  uint32_t count_crtcs;
  uint32_t count_connectors;
  uint32_t count_encoders;
  uint32_t min_width, max_width;
  uint32_t min_height, max_height;
};

struct drm_mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];
};

struct drm_mode_get_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t gamma_size;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t gamma_size;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;
};

_Static_assert(sizeof(struct drm_mode_crtc) == 104, "drm_mode_crtc ABI size");
_Static_assert(offsetof(struct drm_mode_crtc, mode) == 36, "drm_mode_crtc ABI mode offset");

struct drm_mode_fb_cmd {
  uint32_t fb_id;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
  uint32_t depth;
  uint32_t handle;
};

struct drm_mode_crtc_page_flip {
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t flags;
  uint32_t reserved;
  uint64_t user_data;
};

struct drm_clip_rect { uint16_t x1, y1, x2, y2; };
struct drm_mode_fb_dirty_cmd {
  uint32_t fb_id, flags, color, num_clips;
  uint64_t clips_ptr;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_VBLANK_EVENT 0x04000000U

struct drm_wait_vblank_request {
  uint32_t type;
  uint32_t sequence;
  unsigned long signal;
};

struct drm_wait_vblank_reply {
  uint32_t type;
  uint32_t sequence;
  long tval_sec;
  long tval_usec;
};

union drm_wait_vblank {
  struct drm_wait_vblank_request request;
  struct drm_wait_vblank_reply reply;
};

#define DRM_MODE_CURSOR_BO 0x01
#define DRM_MODE_CURSOR_MOVE 0x02

struct drm_mode_cursor {
  uint32_t flags;
  uint32_t crtc_id;
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t handle;
};

struct drm_mode_cursor2 {
  uint32_t flags;
  uint32_t crtc_id;
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t handle;
  int32_t hot_x;
  int32_t hot_y;
};

struct drm_event {
  uint32_t type;
  uint32_t length;
};

#define DRM_EVENT_VBLANK 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

struct drm_event_vblank {
  struct drm_event base;
  uint64_t user_data;
  uint32_t tv_sec;
  uint32_t tv_usec;
  uint32_t sequence;
  uint32_t crtc_id;
};

struct drm_mode_get_connector {
  uint64_t encoders_ptr;
  uint64_t modes_ptr;
  uint64_t props_ptr;
  uint64_t prop_values_ptr;
  uint32_t count_modes;
  uint32_t count_props;
  uint32_t count_encoders;
  uint32_t encoder_id;
  uint32_t connector_id;
  uint32_t connector_type;
  uint32_t connector_type_id;
  uint32_t connection;
  uint32_t mm_width, mm_height;
  uint32_t subpixel;
  uint32_t pad;
};

struct drm_mode_get_encoder {
  uint32_t encoder_id;
  uint32_t encoder_type;
  uint32_t crtc_id;
  uint32_t possible_crtcs;
  uint32_t possible_clones;
};

struct drm_mode_create_dumb {
  uint32_t height;
  uint32_t width;
  uint32_t bpp;
  uint32_t flags;
  uint32_t handle;
  uint32_t pitch;
  uint64_t size;
};

struct drm_mode_map_dumb {
  uint32_t handle;
  uint32_t pad;
  uint64_t offset;
};

struct drm_version {
  int version_major;
  int version_minor;
  int version_patchlevel;
  size_t name_len;
  char *name;
  size_t date_len;
  char *date;
  size_t desc_len;
  char *desc;
};

struct drm_unique {
  size_t unique_len;
  char *unique;
};

struct drm_set_version {
  int drm_di_major;
  int drm_di_minor;
  int drm_dd_major;
  int drm_dd_minor;
};

struct drm_get_cap {
  uint64_t capability;
  uint64_t value;
};

struct drm_set_client_cap {
  uint64_t capability;
  uint64_t value;
};

struct drm_gem_create {
  uint64_t size;
  uint32_t handle;
  uint32_t pad;
};

struct drm_gem_free {
  uint32_t handle;
  uint32_t pad;
};

struct drm_gem_mmap {
  uint32_t handle;
  uint32_t pad;
  uint64_t offset;
  uint64_t addr;
};

#include "../../../sched/sched.h"
#include "../../../sched/wait.h"

/* Per-object property value slot — declared early, used by drm_mode_object */
struct drm_prop_value {
  uint32_t prop_id;
  uint64_t value;
};

#define DRM_MAX_OBJ_PROPS 16

struct drm_stats {
  uint64_t commits;
  uint64_t full_commits;
  uint64_t damage_commits;
  uint64_t direct_scanout_commits;
  uint64_t empty_commits;
  uint64_t copy_batches;
  uint64_t bytes_copied;
  uint64_t copy_cycles;
  uint64_t max_copy_cycles;
};

struct drm_device {
  const char *name;
  uint32_t minor;
  spinlock_t lock;
  struct list_head gem_objects;
  struct list_head kms_objects;
  struct list_head event_queue; /* legacy global queue (kept for compat) */
  struct list_head blob_objects;
  struct list_head file_list; /* all open drm_file instances */
  wait_queue_t event_wq;
  uint32_t next_gem_handle;
  uint32_t next_kms_id;
  uint32_t next_blob_id;
  uint32_t next_prime_id;
  uint32_t client_caps; /* global caps (legacy path) */

  /* Damage from the latest atomic request, consumed by drm.c immediately. */
  uint8_t pending_damage_valid;
  struct drm_clip_rect pending_damage;
};

struct drm_mode_object {
  uint32_t id;
  uint32_t type;
  struct list_head list;
  /* Property values attached to this object */
  struct drm_prop_value props[DRM_MAX_OBJ_PROPS];
  uint32_t prop_count;
};

struct drm_plane {
  struct drm_mode_object base;
  uint32_t possible_crtcs;
  uint32_t formats[8];
  int format_count;
  struct drm_framebuffer *fb;
  uint32_t src_x;
  uint32_t src_y;
  uint32_t src_w;
  uint32_t src_h;
  int32_t crtc_x;
  int32_t crtc_y;
  uint32_t crtc_w;
  uint32_t crtc_h;
  int32_t hotspot_x;
  int32_t hotspot_y;
};

struct drm_crtc {
  struct drm_mode_object base;
  uint32_t scanout_id;
  struct drm_plane *primary;
  struct drm_plane *cursor;
  struct drm_framebuffer *fb;
};

struct drm_framebuffer {
  struct drm_mode_object base;
  uint32_t width, height;
  uint32_t pitch, bpp;
  uint32_t pixel_format; /* DRM_FORMAT_* fourcc; important for cursor alpha */
  struct drm_gem_object *gem_obj;

  /* Legacy DIRTYFB damage retained until this buffer is next scanned out.
   * A new framebuffer is always uploaded in full once; afterwards a producer
   * which supplies DIRTYFB can avoid a redundant full upload on PAGE_FLIP. */
  uint8_t scanout_valid;
  uint8_t pending_damage_valid;
  struct drm_clip_rect pending_damage;
};

struct drm_encoder {
  struct drm_mode_object base;
  uint32_t scanout_id;
  uint32_t possible_crtcs;
  uint32_t encoder_type;
};

struct drm_connector {
  struct drm_mode_object base;
  uint32_t connector_type;
  uint32_t connection_status;
  uint32_t scanout_id;
  struct drm_encoder *encoder;
};

struct drm_pending_event {
  struct drm_event_vblank event;
  struct list_head list;
};

/* ── Property system ─────────────────────────────────────────────────────── */

#define DRM_PROP_TYPE_RANGE (1U << 1)
#define DRM_PROP_FLAG_IMMUTABLE (1U << 2)
#define DRM_PROP_TYPE_ENUM (1U << 3)
#define DRM_PROP_TYPE_BLOB (1U << 4)
#define DRM_PROP_TYPE_BITMASK (1U << 5)
#define DRM_PROP_TYPE_OBJECT (1U << 6)
#define DRM_PROP_TYPE_SIGNED_RANGE (2U << 6)
#define DRM_PROP_EXTENDED_TYPE_MASK 0x0000ffc0U
#define DRM_PROP_FLAG_ATOMIC 0x80000000U

/* Well-known property IDs (fixed, so userland can hardcode them) */
#define DRM_PROP_ID_CRTC_ID 1
#define DRM_PROP_ID_FB_ID 2
#define DRM_PROP_ID_SRC_X 3
#define DRM_PROP_ID_SRC_Y 4
#define DRM_PROP_ID_SRC_W 5
#define DRM_PROP_ID_SRC_H 6
#define DRM_PROP_ID_CRTC_X 7
#define DRM_PROP_ID_CRTC_Y 8
#define DRM_PROP_ID_CRTC_W 9
#define DRM_PROP_ID_CRTC_H 10
#define DRM_PROP_ID_ACTIVE 11
#define DRM_PROP_ID_MODE_ID 12
#define DRM_PROP_ID_DPMS 13
#define DRM_PROP_ID_CONNECTOR_ID 14
#define DRM_PROP_ID_TYPE 15
#define DRM_PROP_ID_HOTSPOT_X 16
#define DRM_PROP_ID_HOTSPOT_Y 17
#define DRM_PROP_ID_FB_DAMAGE_CLIPS 18
#define DRM_PROP_ID_MAX 19

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2

struct drm_property_def {
  uint32_t id;
  uint32_t flags;
  char name[32];
  uint64_t min_val;
  uint64_t max_val;
};

/* ── Blob objects ────────────────────────────────────────────────────────── */
struct drm_prop_blob {
  uint32_t id;
  uint32_t length;
  void *data;
  struct list_head list;
};

/* Payload used by the standard FB_DAMAGE_CLIPS blob property. */
struct drm_mode_rect {
  int32_t x1;
  int32_t y1;
  int32_t x2;
  int32_t y2;
};

/* ── Atomic ioctl structs ────────────────────────────────────────────────── */
struct drm_mode_atomic {
  uint32_t flags;
  uint32_t count_objs;
  uint64_t objs_ptr;        /* uint32_t[] of object IDs */
  uint64_t count_props_ptr; /* uint32_t[] of prop counts per object */
  uint64_t props_ptr;       /* uint32_t[] of prop IDs (flattened) */
  uint64_t prop_values_ptr; /* uint64_t[] of prop values (flattened) */
  uint64_t reserved;
  uint64_t user_data;
};

struct drm_mode_obj_get_properties {
  uint64_t props_ptr;
  uint64_t prop_values_ptr;
  uint32_t count_props;
  uint32_t obj_id;
  uint32_t obj_type;
  uint32_t pad;
};

struct drm_mode_create_blob {
  uint64_t data;
  uint32_t length;
  uint32_t blob_id;
};

struct drm_mode_destroy_blob {
  uint32_t blob_id;
};

struct drm_mode_fb_cmd2 {
  uint32_t fb_id;
  uint32_t width, height;
  uint32_t pixel_format;
  uint32_t flags;
  uint32_t handles[4];
  uint32_t pitches[4];
  uint32_t offsets[4];
  uint64_t modifier[4];
};

/* ── GEM PRIME / DMA-buf ─────────────────────────────────────────────────── */
#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0xC008642D
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xC008642E

struct drm_prime_handle {
  uint32_t handle;
  uint32_t flags;
  int32_t fd;
};

/* ── Per-file (per-client) DRM state ─────────────────────────────────────── */
#define DRM_MAX_HANDLES_PER_FILE 256

/*
 * drm_file — allocated once per open() of /dev/dri/card0.
 * Tracks per-client GEM handle namespace, event queue, and caps.
 */
struct drm_file {
  struct drm_device *dev;

  /* Per-client GEM handle table: maps local handle → global gem object */
  struct drm_gem_object *handles[DRM_MAX_HANDLES_PER_FILE];
  uint32_t next_handle; /* next local handle to assign (1-based) */

  /*
   * Legacy hardware FB gem object (handle 0xF0B0).
   * Stored separately because 0xF0B0 > DRM_MAX_HANDLES_PER_FILE.
   * drm_file_gem_lookup falls back to this when handle == 0xF0B0.
   */
  struct drm_gem_object *hw_fb_gem;

  /* Per-client event queue (so two clients don't steal each other's events) */
  struct list_head event_queue;
  wait_queue_t event_wq;

  /* Per-client capabilities */
  uint32_t client_caps; /* DRM_FILE_CAP_* */
  uint32_t is_master;

  /* Optional per-open state owned by the hardware driver. */
  void *driver_private;

  spinlock_t lock;
  struct list_head list; /* linked into drm_device.file_list */
};

/* ── Full ADDFB2 framebuffer (multi-planar + modifiers) ──────────────────── */
#define DRM_FORMAT_MOD_INVALID (~0ULL)
#define DRM_FORMAT_MOD_LINEAR 0ULL

#define DRM_MAX_FB_PLANES 4

struct drm_framebuffer_full {
  struct drm_framebuffer base_fb;
  uint32_t width, height;
  uint32_t pixel_format; /* fourcc */
  uint64_t modifier;
  uint32_t flags;
  /* per-plane */
  struct drm_gem_object *gem_obj[DRM_MAX_FB_PLANES];
  uint32_t pitches[DRM_MAX_FB_PLANES];
  uint32_t offsets[DRM_MAX_FB_PLANES];
  /* legacy compat fields */
  uint32_t pitch, bpp;
};

void drm_init(void);
vfs_node_t *drm_create_client_node(void);
bool drm_is_card_node(vfs_node_t *node);
void drm_register_vfs(void);
void drm_stats_snapshot(struct drm_stats *out);

void drm_obj_add_prop(struct drm_mode_object *obj, uint32_t prop_id,
                      uint64_t default_val);
int drm_obj_set_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t value);
int drm_obj_get_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t *out);

enum drm_gem_cache_mode {
  DRM_GEM_CACHE_WB = 0,
  DRM_GEM_CACHE_WC,
};

// GEM internals
struct drm_gem_object {
  struct drm_device *dev;
  uint32_t handle;
  size_t size;
  uint64_t phys_addr;
  void *virt_addr;
  enum drm_gem_cache_mode cache_mode;
  struct list_head list;
  struct list_head file_list;
  int refcount;
  uint64_t (*get_page_phys)(struct drm_gem_object *obj, uint32_t page);
  void (*free)(struct drm_device *dev, struct drm_gem_object *obj);
  void *driver_private;
};

typedef void (*drm_commit_damage_fn_t)(struct drm_device *dev,
                                      const struct drm_clip_rect *clips,
                                      uint32_t num_clips,
                                      uint32_t target_fb_id);
typedef void (*drm_pageflip_fn_t)(struct drm_file *file, struct vfs_node *node,
                                  uint32_t crtc_id, uint32_t fb_id,
                                  uint64_t user_data);
typedef int (*drm_create_dumb_fn_t)(struct drm_device *dev, uint32_t width,
                                   uint32_t height, uint32_t bpp,
                                   struct drm_gem_object **obj_out);
typedef void (*drm_cursor_fn_t)(uint32_t crtc_id,
                                struct drm_gem_object *gem,
                                uint32_t width, uint32_t height, uint32_t pitch,
                                int32_t x, int32_t y,
                                int32_t hot_x, int32_t hot_y,
                                uint32_t flags);
typedef void (*drm_get_modes_fn_t)(uint32_t connector_id,
                                  struct drm_mode_modeinfo *modes,
                                  uint32_t *count);
uint32_t drm_connector_scanout_id(uint32_t connector_id);
uint32_t drm_crtc_scanout_id(uint32_t crtc_id);
void drm_ensure_outputs(struct drm_device *dev, uint32_t count);
void drm_update_output_state(struct drm_device *dev, uint32_t scanout, bool connected);
void drm_register_cursor_backend(drm_cursor_fn_t cursor);
void drm_register_scanout_backend(drm_create_dumb_fn_t create_dumb,
                                  drm_commit_damage_fn_t commit_damage,
                                  drm_get_modes_fn_t get_modes,
                                  drm_pageflip_fn_t pageflip);
struct drm_gem_object *drm_gem_object_create(struct drm_device *dev, size_t size);
void drm_gem_object_free(struct drm_device *dev, struct drm_gem_object *obj);
void drm_file_send_event(struct drm_file *file, struct drm_event_vblank *ev,
                         struct vfs_node *node);
uint32_t drm_file_gem_register(struct drm_file *file, struct drm_gem_object *obj);
struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file, uint32_t handle);
void drm_file_gem_release(struct drm_file *file, uint32_t handle);
extern struct drm_device global_drm_dev;

#endif
