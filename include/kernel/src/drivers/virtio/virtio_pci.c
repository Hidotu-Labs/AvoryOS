#include "drivers/virtio/virtio_pci.h"
#include "hal/hal.h"
#include "drivers/virtio/virtio.h"
#include "drivers/pci/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "console/klog.h"
#include "lib/string.h"
#include <stdint.h>
#include <stdbool.h>

// Helpers



// BAR Mapping

// Decode a BAR with PCI memory and I/O decoding disabled. A 64-bit BAR
// must be probed as one value; probing its halves separately can manufacture a
// near-2^64 size and make the mapper loop forever.
static uint64_t pci_bar_size(struct pci_device *pci, int bar_idx) {
  if (!pci || bar_idx < 0 || bar_idx >= 6)
    return 0;

  uint16_t reg = (uint16_t)(0x10 + bar_idx * 4);
  uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  uint32_t orig_lo = pci_config_read32(pci->bus, pci->slot, pci->func, reg);
  bool is_io = (orig_lo & 1U) != 0;
  bool is_64 = !is_io && (((orig_lo >> 1) & 3U) == 2U) && bar_idx < 5;
  uint32_t orig_hi = is_64 ? pci_config_read32(
      pci->bus, pci->slot, pci->func, (uint16_t)(reg + 4)) : 0;

  // Stop the function from decoding the temporary all-ones BAR address.
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     (uint16_t)(command & ~3U));
  pci_config_write32(pci->bus, pci->slot, pci->func, reg, 0xFFFFFFFFU);
  if (is_64)
    pci_config_write32(pci->bus, pci->slot, pci->func,
                       (uint16_t)(reg + 4), 0xFFFFFFFFU);

  uint32_t sized_lo = pci_config_read32(pci->bus, pci->slot, pci->func, reg);
  uint32_t sized_hi = is_64 ? pci_config_read32(
      pci->bus, pci->slot, pci->func, (uint16_t)(reg + 4)) : 0;

  // Restore the complete BAR before re-enabling decoding.
  pci_config_write32(pci->bus, pci->slot, pci->func, reg, orig_lo);
  if (is_64)
    pci_config_write32(pci->bus, pci->slot, pci->func,
                       (uint16_t)(reg + 4), orig_hi);
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command);

  if (is_io) {
    uint32_t mask = sized_lo & ~3U;
    return mask ? (uint64_t)((~mask) + 1U) : 0;
  }

  uint64_t mask = is_64 ? ((uint64_t)sized_hi << 32) |
                              (uint64_t)(sized_lo & ~0xFU)
                        : (uint64_t)(sized_lo & ~0xFU);
  if (!mask)
    return 0;
  if (is_64)
    return (~mask) + 1ULL;
  return (uint64_t)((~(uint32_t)mask) + 1U);
}

// Map a PCI BAR into kernel virtual address space.
// MMIO BARs are NOT part of Limine's HHDM — we must explicitly create
// page table entries.  We place them at phys + hhdm_offset so the rest
// of the driver code can use a uniform translation.
// Returns kernel virtual address, or 0 on failure.
static uint64_t map_bar(struct pci_device *pci, int bar_idx, uint64_t *out_size) {
  uint32_t bar_lo = pci->bar[bar_idx];

  if (bar_lo & 1) {
    // IO BAR — not memory mapped, use port I/O
    klog_puts("[VIRTIO-PCI] BAR");
    klog_putchar('0' + bar_idx);
    klog_puts(" is I/O — skipping\n");
    return 0;
  }

  uint64_t phys = bar_lo & 0xFFFFFFF0;
  int type = (bar_lo >> 1) & 3;

  if (type == 2 && bar_idx < 5) {
    // 64-bit BAR
    phys |= ((uint64_t)pci->bar[bar_idx + 1] << 32);
  }

  uint64_t size = pci_bar_size(pci, bar_idx);
  if (out_size) *out_size = size;

  if (size == 0 || phys == 0) return 0;

  // VirtIO capability BARs are small. Refuse absurd results so a broken
  // device or probe can never turn this into an unbounded mapping loop.
  if (size > (256ULL * 1024 * 1024)) {
    klog_puts("[VIRTIO-PCI] Refusing unreasonable BAR size: ");
    klog_hex64(size);
    klog_puts("\n");
    return 0;
  }

  // Map each page of the BAR into kernel address space.
  // We place it at the HHDM address (phys + hhdm_offset) for consistency,
  // but since MMIO isn't covered by Limine's HHDM we create the entries
  // ourselves with PWT+PCD (write-through, cache-disable) for MMIO safety.
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t virt_base = phys + hhdm;
  uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t *pml4 = vmm_get_active_pml4();

  // PWT (bit 3) + PCD (bit 4) = uncacheable/write-through for MMIO
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | (1ULL << 3) | (1ULL << 4);

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t p = phys + i * PAGE_SIZE;
    uint64_t v = virt_base + i * PAGE_SIZE;
    if (!vmm_map_page(pml4, v, p, flags)) {
      klog_puts("[VIRTIO-PCI] FATAL: vmm_map_page failed for BAR\n");
      return 0;
    }
    vmm_flush_tlb(v);
  }

  klog_puts("[VIRTIO-PCI] BAR");
  klog_putchar('0' + bar_idx);
  klog_puts(" phys=");
  klog_hex64(phys);
  klog_puts(" size=");
  klog_hex32((uint32_t)size);
  klog_puts(" mapped ");
  klog_uint64(num_pages);
  klog_puts(" pages at virt=");
  klog_hex64(virt_base);
  klog_puts("\n");

  return virt_base;
}

// PCI Capability Walking

// Read a VirtIO PCI capability at 'cap_off' in config space.
static void read_pci_cap(struct pci_device *pci, uint8_t cap_off,
                         struct virtio_pci_cap *out) {
  uint32_t dw0 = pci_config_read32(pci->bus, pci->slot, pci->func, cap_off);
  uint32_t dw1 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 4);
  uint32_t dw2 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 8);
  uint32_t dw3 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 12);

  out->cap_vndr  = dw0 & 0xFF;
  out->cap_next  = (dw0 >> 8) & 0xFF;
  out->cap_len   = (dw0 >> 16) & 0xFF;
  out->cfg_type  = (dw0 >> 24) & 0xFF;
  out->bar       = dw1 & 0xFF;
  out->id        = (dw1 >> 8) & 0xFF;
  out->padding[0] = (dw1 >> 16) & 0xFF;
  out->padding[1] = (dw1 >> 24) & 0xFF;
  out->offset    = dw2;
  out->length    = dw3;
}

// Public API

bool virtio_pci_init(struct virtio_pci_device *vdev, struct pci_device *pci) {
  memset(vdev, 0, sizeof(*vdev));
  vdev->pci = pci;

  // Enable bus mastering and memory space
  pci_enable_bus_mastering(pci);
  uint32_t cmd = pci_config_read32(pci->bus, pci->slot, pci->func, 0x04);
  cmd |= (1 << 1); // Memory Space Enable
  pci_config_write32(pci->bus, pci->slot, pci->func, 0x04, cmd);

  // Walk PCI capabilities
  uint32_t status = pci_config_read32(pci->bus, pci->slot, pci->func, 0x04);
  if (!((status >> 16) & (1 << 4))) {
    klog_puts("[VIRTIO-PCI] No capabilities list\n");
    return false;
  }

  uint8_t cap_off = pci_config_read32(pci->bus, pci->slot, pci->func, 0x34) &
                    0xFF;

  bool found_common = false, found_notify = false;
  bool found_isr = false;

  while (cap_off != 0 && cap_off != 0xFF) {
    uint32_t cap_header = pci_config_read32(pci->bus, pci->slot, pci->func,
                                            cap_off);
    uint8_t cap_id = cap_header & 0xFF;

    if (cap_id == 0x09) {
      // Vendor-specific = VirtIO
      struct virtio_pci_cap cap;
      read_pci_cap(pci, cap_off, &cap);

      // Map the BAR if not yet mapped
      if (cap.bar < 6 && vdev->bar_virt[cap.bar] == 0) {
        vdev->bar_virt[cap.bar] = map_bar(pci, cap.bar, &vdev->bar_size[cap.bar]);
      }

      uint64_t base = vdev->bar_virt[cap.bar];
      if (base == 0) {
        cap_off = (cap_header >> 8) & 0xFF;
        continue;
      }

      switch (cap.cfg_type) {
      case VIRTIO_PCI_CAP_COMMON_CFG:
        vdev->common = (volatile struct virtio_pci_common_cfg *)(base +
                                                                  cap.offset);
        found_common = true;
        klog_puts("[VIRTIO-PCI] Found COMMON_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts("\n");
        break;

      case VIRTIO_PCI_CAP_NOTIFY_CFG:
        vdev->notify_base = (volatile uint16_t *)(base + cap.offset);
        // The multiplier is at cap_off + 16 (after the standard 16-byte cap)
        vdev->notify_off_multiplier = pci_config_read32(
            pci->bus, pci->slot, pci->func, cap_off + 16);
        found_notify = true;
        klog_puts("[VIRTIO-PCI] Found NOTIFY_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts(" mult=");
        klog_hex32(vdev->notify_off_multiplier);
        klog_puts("\n");
        break;

      case VIRTIO_PCI_CAP_ISR_CFG:
        vdev->isr = (volatile uint8_t *)(base + cap.offset);
        found_isr = true;
        klog_puts("[VIRTIO-PCI] Found ISR_CFG\n");
        break;

      case VIRTIO_PCI_CAP_DEVICE_CFG:
        vdev->device_cfg = (volatile uint8_t *)(base + cap.offset);
        klog_puts("[VIRTIO-PCI] Found DEVICE_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts("\n");
        break;

      default:
        break;
      }
    }

    cap_off = (cap_header >> 8) & 0xFF;
  }

  if (!found_common || !found_notify || !found_isr) {
    klog_puts("[VIRTIO-PCI] Missing required capabilities (common=");
    klog_putchar(found_common ? '1' : '0');
    klog_puts(" notify=");
    klog_putchar(found_notify ? '1' : '0');
    klog_puts(" isr=");
    klog_putchar(found_isr ? '1' : '0');
    klog_puts(")\n");
    return false;
  }

  return true;
}

uint64_t virtio_pci_read_features(struct virtio_pci_device *vdev) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  cfg->device_feature_select = 0;
  __asm__ volatile("" ::: "memory");
  uint32_t lo = cfg->device_feature;

  cfg->device_feature_select = 1;
  __asm__ volatile("" ::: "memory");
  uint32_t hi = cfg->device_feature;

  return ((uint64_t)hi << 32) | lo;
}

void virtio_pci_write_features(struct virtio_pci_device *vdev,
                               uint64_t features) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  cfg->driver_feature_select = 0;
  __asm__ volatile("" ::: "memory");
  cfg->driver_feature = (uint32_t)(features & 0xFFFFFFFF);

  cfg->driver_feature_select = 1;
  __asm__ volatile("" ::: "memory");
  cfg->driver_feature = (uint32_t)(features >> 32);
}

void virtio_pci_set_status(struct virtio_pci_device *vdev, uint8_t status) {
  vdev->common->device_status = status;
  __asm__ volatile("" ::: "memory");
}

uint8_t virtio_pci_get_status(struct virtio_pci_device *vdev) {
  return vdev->common->device_status;
}

bool virtio_pci_reset(struct virtio_pci_device *vdev) {
  if (!vdev || !vdev->common)
    return false;
  vdev->common->device_status = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (uint32_t spins = 0; spins < 1000000; spins++) {
    if (vdev->common->device_status == 0)
      return true;
    hal_cpu_relax();
  }
  return false;
}

bool virtio_pci_negotiate(struct virtio_pci_device *vdev,
                          uint64_t wanted, uint64_t required,
                          uint64_t *negotiated) {
  if (!vdev || !vdev->common || (required & ~wanted))
    return false;

  if (!virtio_pci_reset(vdev))
    return false;
  virtio_pci_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
  virtio_pci_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER);

  uint64_t offered = virtio_pci_read_features(vdev);
  if ((offered & required) != required) {
    virtio_pci_set_failed(vdev);
    return false;
  }

  uint64_t accepted = offered & wanted;
  virtio_pci_write_features(vdev, accepted);
  uint8_t status = virtio_pci_get_status(vdev);
  virtio_pci_set_status(vdev, status | VIRTIO_STATUS_FEATURES_OK);
  status = virtio_pci_get_status(vdev);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    virtio_pci_set_failed(vdev);
    return false;
  }
  if (negotiated)
    *negotiated = accepted;
  return true;
}

bool virtio_pci_set_driver_ok(struct virtio_pci_device *vdev) {
  if (!vdev || !vdev->common)
    return false;
  uint8_t status = virtio_pci_get_status(vdev);
  if (!(status & VIRTIO_STATUS_FEATURES_OK) ||
      (status & (VIRTIO_STATUS_FAILED | VIRTIO_STATUS_DEVICE_NEEDS_RESET)))
    return false;
  virtio_pci_set_status(vdev, status | VIRTIO_STATUS_DRIVER_OK);
  return (virtio_pci_get_status(vdev) & VIRTIO_STATUS_DRIVER_OK) != 0;
}

void virtio_pci_set_failed(struct virtio_pci_device *vdev) {
  if (vdev && vdev->common)
    virtio_pci_set_status(vdev, virtio_pci_get_status(vdev) |
                                VIRTIO_STATUS_FAILED);
}

bool virtio_pci_setup_queue(struct virtio_pci_device *vdev,
                            uint16_t queue_index,
                            struct virtqueue *vq) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  // Select the queue
  cfg->queue_select = queue_index;
  __asm__ volatile("" ::: "memory");

  // Read the maximum queue size
  uint16_t max_size = cfg->queue_size;
  if (max_size == 0) {
    klog_puts("[VIRTIO-PCI] Queue ");
    klog_uint64(queue_index);
    klog_puts(" not available (size=0)\n");
    return false;
  }

  // Cap to our maximum
  uint16_t qsz = max_size;
  if (qsz > VIRTQ_MAX_SIZE)
    qsz = VIRTQ_MAX_SIZE;

  // Initialize the virtqueue memory
  if (!virtq_init(vq, qsz)) {
    klog_puts("[VIRTIO-PCI] Failed to allocate virtqueue memory\n");
    return false;
  }

  vq->queue_index = queue_index;

  // Write the queue size back (we may have reduced it)
  cfg->queue_size = qsz;
  __asm__ volatile("" ::: "memory");

  // Tell the device where the descriptor table, available ring,
  // and used ring are located
  cfg->queue_desc  = vq->desc_phys;
  cfg->queue_avail = vq->avail_phys;
  cfg->queue_used  = vq->used_phys;
  __asm__ volatile("" ::: "memory");

  // Compute the notify address for this queue
  uint16_t notify_off = cfg->queue_notify_off;
  vq->notify_addr = (volatile uint16_t *)((uint8_t *)vdev->notify_base +
                                           notify_off *
                                               vdev->notify_off_multiplier);

  // Disable MSI-X for this queue (use ISR polling)
  cfg->queue_msix_vector = 0xFFFF;
  __asm__ volatile("" ::: "memory");

  // Enable the queue
  cfg->queue_enable = 1;
  __asm__ volatile("" ::: "memory");

  klog_puts("[VIRTIO-PCI] Queue ");
  klog_uint64(queue_index);
  klog_puts(" initialized: size=");
  klog_uint64(qsz);
  klog_puts(" desc=");
  klog_hex64(vq->desc_phys);
  klog_puts("\n");

  return true;
}

bool virtio_pci_msix_init(struct virtio_pci_device *vdev) {
  return vdev && vdev->pci && pci_msix_init(vdev->pci, &vdev->msix);
}

bool virtio_pci_msix_route(struct virtio_pci_device *vdev, uint16_t entry,
                           uint8_t vector, uint8_t destination_apic) {
  return vdev && pci_msix_program(&vdev->msix, entry, vector, destination_apic);
}

bool virtio_pci_msix_assign_queue(struct virtio_pci_device *vdev,
                                  uint16_t queue, uint16_t entry) {
  if (!vdev || !vdev->common || entry >= vdev->msix.table_size) return false;
  vdev->common->queue_select = queue;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  vdev->common->queue_msix_vector = entry;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return vdev->common->queue_msix_vector != 0xFFFF;
}

bool virtio_pci_msix_assign_config(struct virtio_pci_device *vdev,
                                   uint16_t entry) {
  if (!vdev || !vdev->common || entry >= vdev->msix.table_size) return false;
  vdev->common->msix_config = entry;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return vdev->common->msix_config != 0xFFFF;
}

bool virtio_pci_msix_enable(struct virtio_pci_device *vdev) {
  return vdev && pci_msix_enable(&vdev->msix);
}

void virtio_pci_msix_disable(struct virtio_pci_device *vdev) {
  if (vdev) pci_msix_disable(&vdev->msix);
}

void virtio_pci_notify(struct virtio_pci_device *vdev, uint16_t queue_index,
                       struct virtqueue *vq) {
  (void)vdev;
  (void)queue_index;
  __asm__ volatile("" ::: "memory"); // Ensure all writes are visible
  *vq->notify_addr = queue_index;
}
