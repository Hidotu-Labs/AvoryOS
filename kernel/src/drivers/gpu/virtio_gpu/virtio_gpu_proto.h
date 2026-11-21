#ifndef DRIVERS_GPU_VIRTIO_GPU_PROTO_H
#define DRIVERS_GPU_VIRTIO_GPU_PROTO_H
#include <stdint.h>
#define VIRTIO_GPU_PCI_VENDOR_ID         0x1AF4
#define VIRTIO_GPU_DEVICE_ID             16
#define VIRTIO_GPU_PCI_DEVICE_ID_MODERN  (0x1040 + VIRTIO_GPU_DEVICE_ID)
#define VIRTIO_GPU_F_VIRGL               (1ULL << 0)
#define VIRTIO_GPU_F_EDID                (1ULL << 1)
#define VIRTIO_GPU_F_RESOURCE_UUID       (1ULL << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB       (1ULL << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT        (1ULL << 4)
#define VIRTIO_GPU_EVENT_DISPLAY         (1U << 0)
#define VIRTIO_GPU_MAX_SCANOUTS          16
#define VIRTIO_GPU_FLAG_FENCE            (1U << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX    (1U << 1)
#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK 0x000000ffU
/* blob_mem / blob_flags for RESOURCE_CREATE_BLOB */
#define VIRTIO_GPU_BLOB_MEM_GUEST        0x0001U
#define VIRTIO_GPU_BLOB_MEM_HOST3D       0x0002U
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST 0x0003U
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE    0x0001U
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE   0x0002U
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004U
/* map_info low bits from RESP_OK_MAP_INFO */
#define VIRTIO_GPU_MAP_CACHE_MASK        0x0FU
#define VIRTIO_GPU_MAP_CACHE_NONE        0x00U
#define VIRTIO_GPU_MAP_CACHE_CACHED      0x01U
#define VIRTIO_GPU_MAP_CACHE_UNCACHED    0x02U
#define VIRTIO_GPU_MAP_CACHE_WC          0x03U
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1U
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2U
#define VIRTIO_GPU_SHM_ID_HOST_VISIBLE   1U
#define VIRTIO_GPU_CAPSET_VIRGL          1U
#define VIRTIO_GPU_CAPSET_VIRGL2         2U
#define VIRTIO_GPU_CAPSET_GFXSTREAM      3U
#define VIRTIO_GPU_CAPSET_VENUS          4U
#define VIRTIO_GPU_CAPSET_CROSS_DOMAIN   5U
#define VIRTIO_GPU_CAPSET_DRM            6U

enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID = 0x010A,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,
    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D,
    VIRTIO_GPU_CMD_SUBMIT_3D,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID = 0x1104,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID,
    VIRTIO_GPU_RESP_OK_MAP_INFO,
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

struct virtio_gpu_config {
    uint32_t events_read, events_clear, num_scanouts, num_capsets;
} __attribute__((packed));
struct virtio_gpu_ctrl_hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx, padding[3];
} __attribute__((packed));
struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled, flags;
} __attribute__((packed));
struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));
struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout, padding;
} __attribute__((packed));
struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size, padding;
    uint8_t edid[1024];
} __attribute__((packed));
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, format, width, height;
} __attribute__((packed));
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id, resource_id;
} __attribute__((packed));
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length, padding;
} __attribute__((packed));
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, nr_entries;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));
struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct virtio_gpu_cursor_pos {
    uint32_t scanout_id, x, y, padding;
} __attribute__((packed));
struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id, hot_x, hot_y, padding;
} __attribute__((packed));
struct virtio_gpu_box { uint32_t x, y, z, w, h, d; };
struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t index, padding;
};
struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t id, version, size, padding;
};
struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t id, version;
};
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen, context_init;
    char name[64];
};
struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
};
struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, target, format, bind, width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags, padding;
};
struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id, level, stride, layer_stride;
};
struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size, padding;
};
struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, blob_mem, blob_flags, nr_entries;
    uint64_t blob_id, size;
    /* followed by nr_entries struct virtio_gpu_mem_entry (GUEST blobs only) */
} __attribute__((packed));
struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
    uint64_t offset; /* byte offset inside the host-visible shm window */
} __attribute__((packed));
struct virtio_gpu_resource_unmap_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id, padding;
} __attribute__((packed));
struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t map_info, padding;
} __attribute__((packed));
_Static_assert(sizeof(struct virtio_gpu_ctx_create) == 96, "virgl context ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_create_blob) == 56, "virtio-gpu blob create ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_map_blob) == 40, "virtio-gpu blob map ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_unmap_blob) == 32, "virtio-gpu blob unmap ABI");
_Static_assert(sizeof(struct virtio_gpu_resp_map_info) == 32, "virtio-gpu map info ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_create_3d) == 72, "virgl resource ABI");
_Static_assert(sizeof(struct virtio_gpu_transfer_host_3d) == 72, "virgl transfer ABI");
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24, "virtio-gpu control header ABI");
_Static_assert(sizeof(struct virtio_gpu_display_one) == 24, "virtio-gpu display entry ABI");
_Static_assert(sizeof(struct virtio_gpu_resp_display_info) == 408,
               "virtio-gpu display response ABI");
_Static_assert(sizeof(struct virtio_gpu_get_edid) == 32, "virtio-gpu get-edid ABI");
_Static_assert(sizeof(struct virtio_gpu_resp_edid) == 1056, "virtio-gpu edid response ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_create_2d) == 40, "virtio-gpu create2d ABI");
_Static_assert(sizeof(struct virtio_gpu_set_scanout) == 48, "virtio-gpu scanout ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_flush) == 48, "virtio-gpu flush ABI");
_Static_assert(sizeof(struct virtio_gpu_transfer_to_host_2d) == 56, "virtio-gpu transfer ABI");
_Static_assert(sizeof(struct virtio_gpu_resource_attach_backing) == 48, "virtio-gpu attach ABI");
_Static_assert(sizeof(struct virtio_gpu_update_cursor) == 56, "virtio-gpu cursor ABI");
#endif