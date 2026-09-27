#ifndef VIRTIO_VIRTIO_H
#define VIRTIO_VIRTIO_H

#include "lock/spinlock.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// VirtIO Device Status Bits (§2.1)
#define VIRTIO_STATUS_ACKNOWLEDGE       1
#define VIRTIO_STATUS_DRIVER            2
#define VIRTIO_STATUS_DRIVER_OK         4
#define VIRTIO_STATUS_FEATURES_OK       8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED            128

// VirtIO Feature Bits
#define VIRTIO_F_VERSION_1              (1ULL << 32)  // VirtIO 1.0+

// Virtqueue Descriptor Flags
#define VIRTQ_DESC_F_NEXT               1   // Buffer continues via 'next'
#define VIRTQ_DESC_F_WRITE              2   // Device writes (vs. reads)
#define VIRTQ_DESC_F_INDIRECT           4   // Buffer is a list of descriptors

// Virtqueue Available Ring Flags
#define VIRTQ_AVAIL_F_NO_INTERRUPT      1

// Virtqueue Used Ring Flags
#define VIRTQ_USED_F_NO_NOTIFY          1

// Virtqueue Descriptor
struct virtq_desc {
  uint64_t addr;       // Guest-physical address
  uint32_t len;        // Length of buffer
  uint16_t flags;      // VIRTQ_DESC_F_*
  uint16_t next;       // Next desc index (if VIRTQ_DESC_F_NEXT)
} __attribute__((packed));

// Available Ring
struct virtq_avail {
  uint16_t flags;
  uint16_t idx;        // Where the driver would put the next desc entry
  uint16_t ring[];     // Descriptor chain heads (variable length)
  // Followed by: uint16_t used_event; (if VIRTIO_F_EVENT_IDX)
} __attribute__((packed));

// Used Ring Element
struct virtq_used_elem {
  uint32_t id;         // Index of start of used descriptor chain
  uint32_t len;        // Total length of the descriptor chain which was used
} __attribute__((packed));

// Used Ring
struct virtq_used {
  uint16_t flags;
  uint16_t idx;        // Where the device would put the next used entry
  struct virtq_used_elem ring[];
  // Followed by: uint16_t avail_event; (if VIRTIO_F_EVENT_IDX)
} __attribute__((packed));

struct virtq_iov {
  uint64_t phys_addr;
  uint32_t len;
};

struct virtq_stats {
  uint64_t submitted;
  uint64_t completed;
  uint64_t rejected;
  uint64_t invalid_used;
};

// Virtqueue (driver-side bookkeeping)
#define VIRTQ_MAX_SIZE 256

struct virtqueue {
  // Memory layout (all contiguous, physically contiguous)
  volatile struct virtq_desc  *desc;
  volatile struct virtq_avail *avail;
  volatile struct virtq_used  *used;

  // Driver bookkeeping
  uint16_t num;                // Queue size (number of descriptors)
  uint16_t free_head;          // Head of free descriptor list
  uint16_t num_free;           // Number of free descriptors
  uint16_t last_used_idx;      // Last seen used ring index
  uint16_t queue_index;

  // Physical base of the entire allocation (for the device)
  uint64_t desc_phys;
  uint64_t avail_phys;
  uint64_t used_phys;
  uint64_t alloc_phys;
  uint32_t alloc_pages;

  // Notify offset multiplier and address
  volatile uint16_t *notify_addr;

  spinlock_t lock;
  void *cookies[VIRTQ_MAX_SIZE];
  bool chain_head[VIRTQ_MAX_SIZE];
  bool desc_in_use[VIRTQ_MAX_SIZE];
  struct virtq_stats stats;
};

// Virtqueue Management API

// Allocate and initialize a virtqueue with 'num' entries.
// Returns true on success. All physical/virtual pointers are filled in.
bool virtq_init(struct virtqueue *vq, uint16_t num);
void virtq_destroy(struct virtqueue *vq);

// Add a single read-only buffer to the virtqueue (device reads from it).
// Returns the descriptor index, or -1 on failure.
int virtq_add_buf_readonly(struct virtqueue *vq, uint64_t phys_addr,
                           uint32_t len);

// Add a pair of buffers: one device-readable (request), one device-writable
// (response). Returns the head descriptor index, or -1 on failure.
int virtq_add_buf_chain(struct virtqueue *vq,
                        uint64_t req_phys, uint32_t req_len,
                        uint64_t resp_phys, uint32_t resp_len);

// Make the descriptors visible to the device (update avail ring).
void virtq_kick(struct virtqueue *vq);

// Check if the device has completed any requests. Returns true if there
// is a new used element; fills *id and *len.
bool virtq_poll(struct virtqueue *vq, uint32_t *id, uint32_t *len);

// Reclaim a descriptor (chain) so it can be reused.
void virtq_free_desc(struct virtqueue *vq, uint16_t head);

// Submit an arbitrary device-readable/device-writable descriptor chain.
int virtq_submit(struct virtqueue *vq,
                 const struct virtq_iov *out, size_t out_count,
                 const struct virtq_iov *in, size_t in_count,
                 void *cookie);
int virtq_submit_deferred(struct virtqueue *vq,
                          const struct virtq_iov *out, size_t out_count,
                          const struct virtq_iov *in, size_t in_count,
                          void *cookie);

// Consume, validate, and reclaim one completed request.
bool virtq_poll_complete(struct virtqueue *vq, void **cookie, uint32_t *len);
bool virtq_is_idle(struct virtqueue *vq);
void virtq_get_stats(struct virtqueue *vq, struct virtq_stats *out);

// Self-test
bool virtio_self_test(void);

#endif
