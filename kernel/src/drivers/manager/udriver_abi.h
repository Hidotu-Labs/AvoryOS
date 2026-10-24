#ifndef UDRIVER_ABI_H
#define UDRIVER_ABI_H

#include <stdint.h>

/*
 * ABI definitions used by the kernel's /dev/driverctl implementation.
 * Hardware is acquired separately through CLAIM or the matching sysfs bind
 * file.
 */
#define UDRV_ABI_VERSION 1U
#define UDRV_NAME_LEN 64U
#define UDRV_BUS_LEN 16U
#define UDRV_DEVICE_LEN 64U
#define UDRV_MAX_IDS 16U
#define UDRV_NET_NAME_LEN 16U
#define UDRV_NET_FRAME_MAX 1518U

#define UDRV_MATCH_PCI_ID (1U << 0)
#define UDRV_MATCH_PCI_CLASS (1U << 1)
#define UDRV_MATCH_NAME (1U << 2)

struct udrv_match {
  uint32_t flags;
  uint16_t vendor;
  uint16_t device;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t reserved;
  char name[UDRV_DEVICE_LEN];
};

struct udrv_register {
  uint32_t abi_version;
  uint32_t id_count;
  char driver_name[UDRV_NAME_LEN];
  char bus_name[UDRV_BUS_LEN];
  struct udrv_match ids[UDRV_MAX_IDS];
};

struct udrv_claim {
  char device_name[UDRV_DEVICE_LEN];
};

struct udrv_status {
  uint32_t abi_version;
  uint32_t owner_tgid;
  uint32_t registered;
  uint32_t claim_count;
  char driver_name[UDRV_NAME_LEN];
  char bus_name[UDRV_BUS_LEN];
};

#define UDRV_RESOURCE_NONE 0U
#define UDRV_RESOURCE_MEM 1U
#define UDRV_RESOURCE_IO 2U
#define UDRV_RESOURCE_IRQ 3U
#define UDRV_RESOURCE_F_MMAP (1U << 0)
#define UDRV_RESOURCE_F_PORT_IO (1U << 1)
#define UDRV_RESOURCE_F_IRQ (1U << 2)

struct udrv_device_info {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t resource_count;
  uint16_t vendor;
  uint16_t device;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t reserved;
};

struct udrv_resource_info {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t index;
  uint32_t type;
  uint32_t flags;
  uint32_t reserved;
  uint64_t start;
  uint64_t length;
  uint64_t mmap_offset;
};

struct udrv_pci_config {
  char device_name[UDRV_DEVICE_LEN];
  uint16_t offset;
  uint8_t width;
  uint8_t write;
  uint32_t value;
};

struct udrv_port_io {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t resource_index;
  uint32_t offset;
  uint8_t width;
  uint8_t write;
  uint16_t reserved;
  uint32_t value;
};

#define UDRV_IRQ_F_MASK_ON_EVENT (1U << 0)

#define UDRV_IRQ_MODE_NONE 0U
#define UDRV_IRQ_MODE_MSI 1U
#define UDRV_IRQ_MODE_MSIX 2U
#define UDRV_IRQ_MODE_INTX 3U

#define UDRV_IRQ_ACTION_STATUS 0U
#define UDRV_IRQ_ACTION_ACK 1U
#define UDRV_IRQ_ACTION_MASK 2U
#define UDRV_IRQ_ACTION_UNMASK 3U
#define UDRV_IRQ_ACTION_RELEASE 4U

#define UDRV_IRQ_EVENT_F_NEEDS_ACK (1U << 0)
#define UDRV_IRQ_EVENT_F_OVERFLOW (1U << 1)

struct udrv_irq_setup {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t resource_index;
  uint32_t vector_count;
  uint32_t flags;
  uint32_t mode;
  uint32_t vector;
  uint32_t reserved;
};

struct udrv_irq_control {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t action;
  uint32_t mode;
  uint64_t pending;
  uint64_t total;
  uint32_t masked;
  uint32_t reserved;
};

struct udrv_irq_event {
  char device_name[UDRV_DEVICE_LEN];
  uint64_t count;
  uint64_t total;
  uint32_t mode;
  uint32_t vector;
  uint32_t flags;
  uint32_t reserved;
};

#define UDRV_DMA_F_32BIT (1U << 0)
#define UDRV_DMA_F_64BIT (1U << 1)
#define UDRV_DMA_F_REQUIRE_IOMMU (1U << 2)
#define UDRV_DMA_F_TRUSTED (1U << 3)

#define UDRV_DMA_CAP_COHERENT (1U << 0)
#define UDRV_DMA_CAP_IOMMU (1U << 1)
#define UDRV_DMA_CAP_TRUSTED_ONLY (1U << 2)

struct udrv_dma_info {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t capabilities;
  uint32_t max_buffers;
  uint64_t max_allocation;
  uint64_t max_total;
  uint64_t allocated;
  uint32_t buffer_count;
  uint32_t bus_master_enabled;
};

struct udrv_dma_alloc {
  char device_name[UDRV_DEVICE_LEN];
  uint64_t size;
  uint32_t flags;
  uint32_t handle;
  uint64_t dma_address;
  uint64_t length;
  uint64_t mmap_offset;
};

struct udrv_dma_free {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t handle;
  uint32_t reserved;
};

struct udrv_bus_master {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t enable;
  uint32_t enabled;
};

#define UDRV_NET_F_LINK_UP (1U << 0)

struct udrv_net_register {
  char device_name[UDRV_DEVICE_LEN];
  char interface_name[UDRV_NET_NAME_LEN];
  uint8_t mac[6];
  uint16_t mtu;
  uint32_t flags;
};

struct udrv_net_frame {
  char device_name[UDRV_DEVICE_LEN];
  uint16_t length;
  uint16_t reserved;
  uint8_t data[UDRV_NET_FRAME_MAX];
};

struct udrv_net_state {
  char device_name[UDRV_DEVICE_LEN];
  uint32_t flags;
  uint32_t tx_queued;
  uint64_t rx_packets;
  uint64_t tx_packets;
  uint64_t rx_dropped;
  uint64_t tx_dropped;
};

#define UDRV_MMAP_TOKEN_SHIFT 48U
#define UDRV_MMAP_RESOURCE_SHIFT 40U
#define UDRV_MMAP_INNER_MASK ((1ULL << UDRV_MMAP_RESOURCE_SHIFT) - 1ULL)
#define UDRV_MMAP_TOKEN_MAX 0x3fffU
#define UDRV_MMAP_DMA_TAG (0x4000ULL << UDRV_MMAP_TOKEN_SHIFT)
#define UDRV_MMAP_DMA_HANDLE_MASK 0x3fffU
#define UDRV_MMAP_DMA_INNER_MASK ((1ULL << UDRV_MMAP_TOKEN_SHIFT) - 1ULL)

#define UDRV_IOC_NRBITS 8U
#define UDRV_IOC_TYPEBITS 8U
#define UDRV_IOC_SIZEBITS 14U
#define UDRV_IOC_NRSHIFT 0U
#define UDRV_IOC_TYPESHIFT (UDRV_IOC_NRSHIFT + UDRV_IOC_NRBITS)
#define UDRV_IOC_SIZESHIFT (UDRV_IOC_TYPESHIFT + UDRV_IOC_TYPEBITS)
#define UDRV_IOC_DIRSHIFT (UDRV_IOC_SIZESHIFT + UDRV_IOC_SIZEBITS)
#define UDRV_IOC_WRITE 1U
#define UDRV_IOC_READ 2U
#define UDRV_IOC(dir, type, nr, size)                                      \
  (((uint32_t)(dir) << UDRV_IOC_DIRSHIFT) |                                \
   ((uint32_t)(type) << UDRV_IOC_TYPESHIFT) |                              \
   ((uint32_t)(nr) << UDRV_IOC_NRSHIFT) |                                  \
   ((uint32_t)(size) << UDRV_IOC_SIZESHIFT))

#define UDRV_IOCTL_REGISTER                                                \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x00, sizeof(struct udrv_register))
#define UDRV_IOCTL_CLAIM                                                   \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x01, sizeof(struct udrv_claim))
#define UDRV_IOCTL_RELEASE                                                 \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x02, sizeof(struct udrv_claim))
#define UDRV_IOCTL_GET_STATUS                                              \
  UDRV_IOC(UDRV_IOC_READ, 'U', 0x03, sizeof(struct udrv_status))
#define UDRV_IOCTL_GET_DEVICE                                              \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x04, sizeof(struct udrv_device_info))
#define UDRV_IOCTL_GET_RESOURCE                                            \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x05, sizeof(struct udrv_resource_info))
#define UDRV_IOCTL_PCI_CONFIG                                              \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x06, sizeof(struct udrv_pci_config))
#define UDRV_IOCTL_PORT_IO                                                 \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x07, sizeof(struct udrv_port_io))
#define UDRV_IOCTL_IRQ_SETUP                                               \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x08, sizeof(struct udrv_irq_setup))
#define UDRV_IOCTL_IRQ_CONTROL                                             \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x09, sizeof(struct udrv_irq_control))
#define UDRV_IOCTL_DMA_INFO                                                \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x0a, sizeof(struct udrv_dma_info))
#define UDRV_IOCTL_DMA_ALLOC                                               \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x0b, sizeof(struct udrv_dma_alloc))
#define UDRV_IOCTL_DMA_FREE                                                \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x0c, sizeof(struct udrv_dma_free))
#define UDRV_IOCTL_BUS_MASTER                                              \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x0d, sizeof(struct udrv_bus_master))
#define UDRV_IOCTL_NET_REGISTER                                            \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x0e, sizeof(struct udrv_net_register))
#define UDRV_IOCTL_NET_RX                                                  \
  UDRV_IOC(UDRV_IOC_WRITE, 'U', 0x0f, sizeof(struct udrv_net_frame))
#define UDRV_IOCTL_NET_TX                                                  \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x10, sizeof(struct udrv_net_frame))
#define UDRV_IOCTL_NET_STATE                                               \
  UDRV_IOC(UDRV_IOC_READ | UDRV_IOC_WRITE, 'U', 0x11, sizeof(struct udrv_net_state))

#endif