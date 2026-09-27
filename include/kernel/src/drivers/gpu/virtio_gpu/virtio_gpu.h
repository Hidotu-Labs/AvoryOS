#ifndef DRIVERS_GPU_VIRTIO_GPU_H
#define DRIVERS_GPU_VIRTIO_GPU_H
#include "drivers/gpu/virtio_gpu/virtio_gpu_proto.h"
#include "drivers/virtio/virtio.h"
#include "drivers/virtio/virtio_pci.h"
#include "mm/dma_alloc.h"
#include <stdbool.h>
#include <stdint.h>
struct drm_file;
struct vfs_node;
#define VIRTIO_GPU_MAX_MODES 32
struct virtio_gpu_mode {
    uint16_t width, height;
    uint16_t refresh;
    uint8_t preferred;
};
struct virtio_gpu_scanout_state {
    bool enabled, edid_valid;
    uint16_t mode_count;
    uint32_t edid_size;
    uint8_t edid[1024];
    struct virtio_gpu_mode modes[VIRTIO_GPU_MAX_MODES];
};

struct virtio_gpu_stats {
    uint64_t commands_submitted, commands_completed, commands_timed_out;
    uint64_t host_errors, invalid_responses;
    uint64_t present_batches, frames_presented, damage_pixels;
    uint64_t present_wait_ms, max_present_wait_ms, present_failures;
    uint64_t interrupt_mode, interrupts, msix_interrupts, intx_interrupts;
    uint64_t control_interrupts, cursor_interrupts, config_interrupts, watchdog_polls;
    uint64_t async_submitted, async_completed, async_dropped;
    uint64_t cursor_commands, cursor_completions, cursor_moves, cursor_updates;
    uint64_t cursor_hides, cursor_coalesced, cursor_failures, cursor_max_in_flight;
    uint64_t cursor_runtime_ms;
    uint64_t frames_in_flight, max_frames_in_flight;
    uint64_t config_events, display_refreshes, hotplug_events;
    uint64_t edid_reads, edid_failures, edid_modes;
};

struct virtio_gpu_device {
    struct virtio_pci_device transport;
    struct virtqueue controlq, cursorq;
    volatile struct virtio_gpu_config *config;
    dma_buffer_t *command_dma;
    dma_buffer_t *present_dma;
    dma_buffer_t *scanout_dma;
    uint64_t negotiated_features, next_fence_id;
    uint32_t num_scanouts, num_capsets;
    uint32_t next_resource_id, scanout_resource_id, queued_scanout_resource_id, resources_live;
    uint32_t head_resource[VIRTIO_GPU_MAX_SCANOUTS], queued_head_resource[VIRTIO_GPU_MAX_SCANOUTS];
    uint32_t scanout_width, scanout_height;
    struct drm_file *pending_flip_file;
    struct vfs_node *pending_flip_node;
    uint64_t pending_flip_user_data;
    uint32_t pending_flip_crtc_id, flip_sequence;
    bool pending_flip;
    bool initialized, command_busy;
    bool owns_scanout;
    struct virtio_gpu_resp_display_info display_info;
    struct virtio_gpu_scanout_state scanouts[VIRTIO_GPU_MAX_SCANOUTS];
    struct virtio_gpu_stats stats;
};

bool virtio_gpu_init(void);
bool virtio_gpu_is_initialized(void);
bool virtio_gpu_get_display_info(struct virtio_gpu_resp_display_info *out);
bool virtio_gpu_phase2_stress_test(uint32_t iterations);
bool virtio_gpu_phase3_init_scanout(void);
bool virtio_gpu_phase3_stress_test(uint32_t resource_cycles, uint32_t frames);
bool virtio_gpu_phase4_bind_drm(void);
bool virtio_gpu_phase4_stress_test(uint32_t resource_cycles, uint32_t damage_cycles);
bool virtio_gpu_phase6_start(void);
bool virtio_gpu_phase7_start(void);
bool virtio_gpu_phase8_start(void);
uint32_t virtio_gpu_scanout_count(void);
bool virtio_gpu_scanout_summary(uint32_t scanout, bool *enabled, char *modes, uint32_t capacity);
void virtio_gpu_get_stats(struct virtio_gpu_stats *out);
#endif