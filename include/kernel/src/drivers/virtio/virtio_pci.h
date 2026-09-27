#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include <stdint.h>
#include "drivers/pci/pci.h"

// VirtIO PCI Vendor / Device IDs
#define VIRTIO_PCI_VENDOR_ID            0x1AF4

// Transitional device IDs (0x1000–0x103F)
#define VIRTIO_PCI_DEVICE_NET           0x1000
#define VIRTIO_PCI_DEVICE_BLK           0x1001
#define VIRTIO_PCI_DEVICE_CONSOLE       0x1003
#define VIRTIO_PCI_DEVICE_GPU_LEGACY    0x1010

// Modern (non-transitional) device IDs (0x1040+)
#define VIRTIO_PCI_DEVICE_GPU           0x1050

// PCI Capability Types (§4.1.4)
#define VIRTIO_PCI_CAP_COMMON_CFG       1
#define VIRTIO_PCI_CAP_NOTIFY_CFG       2
#define VIRTIO_PCI_CAP_ISR_CFG          3
#define VIRTIO_PCI_CAP_DEVICE_CFG       4
#define VIRTIO_PCI_CAP_PCI_CFG          5

// Common Configuration Structure (§4.1.4.3)
// This is mapped via BAR + offset from the VIRTIO_PCI_CAP_COMMON_CFG cap.
struct virtio_pci_common_cfg {
  /* About the whole device */
  uint32_t device_feature_select;   // RW
  uint32_t device_feature;          // RO
  uint32_t driver_feature_select;   // RW
  uint32_t driver_feature;          // RW
  uint16_t msix_config;             // RW
  uint16_t num_queues;              // RO
  uint8_t  device_status;           // RW
  uint8_t  config_generation;       // RO
  /* About a specific virtqueue */
  uint16_t queue_select;            // RW
  uint16_t queue_size;              // RW
  uint16_t queue_msix_vector;       // RW
  uint16_t queue_enable;            // RW
  uint16_t queue_notify_off;        // RO
  uint64_t queue_desc;              // RW — physical address
  uint64_t queue_avail;             // RW — physical address
  uint64_t queue_used;              // RW — physical address
} __attribute__((packed));

// Notify Configuration (§4.1.4.4)
// The notify_off_multiplier comes from the capability itself (extra field).
// The actual notify address for queue q = notify_base + queue_notify_off * multiplier.

// ISR Status (§4.1.4.5)
// A single byte; bit 0 = queue interrupt, bit 1 = device config change.

// Per-capability header in PCI config space
struct virtio_pci_cap {
  uint8_t  cap_vndr;     // Generic PCI field: PCI_CAP_ID_VNDR (0x09)
  uint8_t  cap_next;     // Next capability offset (0 = end)
  uint8_t  cap_len;      // Length of this capability structure
  uint8_t  cfg_type;     // VIRTIO_PCI_CAP_*
  uint8_t  bar;          // Which BAR
  uint8_t  id;           // Multiple capabilities of same type
  uint8_t  padding[2];
  uint32_t offset;       // Offset within the BAR
  uint32_t length;       // Length of the structure
};

// Resolved capability pointers (after BAR mapping)
struct virtio_pci_device {
  struct pci_device *pci;

  // Memory-mapped capability regions (kernel virtual addresses)
  volatile struct virtio_pci_common_cfg *common;
  volatile uint8_t  *isr;
  volatile uint8_t  *device_cfg;
  volatile uint16_t *notify_base;
  uint32_t notify_off_multiplier;

  // BAR virtual base addresses (cached for offset calculations)
  uint64_t bar_virt[6];
  uint64_t bar_size[6];
  struct pci_msix msix;
};

// VirtIO PCI Transport API

// Probe a PCI device for VirtIO capabilities and map the BARs.
// Returns true on success (all required capabilities found).
bool virtio_pci_init(struct virtio_pci_device *vdev, struct pci_device *pci);

// Read device features (both low 32 and high 32 bits).
uint64_t virtio_pci_read_features(struct virtio_pci_device *vdev);

// Write driver features (both low 32 and high 32 bits).
void virtio_pci_write_features(struct virtio_pci_device *vdev, uint64_t features);

// Standard device initialization sequence helpers.
void virtio_pci_set_status(struct virtio_pci_device *vdev, uint8_t status);
uint8_t virtio_pci_get_status(struct virtio_pci_device *vdev);
bool virtio_pci_reset(struct virtio_pci_device *vdev);

// Perform the modern VirtIO reset/status/feature handshake.
bool virtio_pci_negotiate(struct virtio_pci_device *vdev,
                          uint64_t wanted, uint64_t required,
                          uint64_t *negotiated);
bool virtio_pci_set_driver_ok(struct virtio_pci_device *vdev);
void virtio_pci_set_failed(struct virtio_pci_device *vdev);

// Setup a virtqueue: allocates memory, writes addresses to device.
// The queue_index selects which device queue to configure.
// Returns true on success.
struct virtqueue;
bool virtio_pci_setup_queue(struct virtio_pci_device *vdev,
                            uint16_t queue_index,
                            struct virtqueue *vq);

bool virtio_pci_msix_init(struct virtio_pci_device *vdev);
bool virtio_pci_msix_route(struct virtio_pci_device *vdev, uint16_t entry,
                           uint8_t vector, uint8_t destination_apic);
bool virtio_pci_msix_assign_queue(struct virtio_pci_device *vdev,
                                  uint16_t queue, uint16_t entry);
bool virtio_pci_msix_assign_config(struct virtio_pci_device *vdev,
                                   uint16_t entry);
bool virtio_pci_msix_enable(struct virtio_pci_device *vdev);
void virtio_pci_msix_disable(struct virtio_pci_device *vdev);

// Notify the device that new buffers are available on queue queue_index.
void virtio_pci_notify(struct virtio_pci_device *vdev, uint16_t queue_index,
                       struct virtqueue *vq);

#endif
