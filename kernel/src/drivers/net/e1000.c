#include "drivers/net/e1000.h"
#include "drivers/net/e1000_regs.h"
#include "console/klog.h"
#include "cpu/tsc.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/pci_irq.h"
#include "fs/sysfs_pci.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/dma_alloc.h"
#include "net/core.h"
#include "apic/lapic_timer.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"
#include "socket/af_netlink.h"

#define PCI_COMMAND_MEMORY (1u << 1)
#define PCI_COMMAND_MASTER (1u << 2)
#define PCI_COMMAND_INTX_DISABLE (1u << 10)
#define RESET_TIMEOUT_MS 100u
#define EEPROM_TIMEOUT_MS 20u
#define RING_COUNT 256u
#define BUFFER_SIZE 2048u
#define TX_TIMEOUT_MS 2000u
#define SERVICE_INTERVAL_MS 100u
#define RX_POLL_BUDGET 64u
#define RX_INT_MASK (E1000_INT_RXDMT0 | E1000_INT_RXO | E1000_INT_RXT0)

extern const char *kernel_boot_cmdline;

struct e1000_profile {
  const char *driver_name;
  const char *log_tag;
  uint16_t vendor_id;
  uint16_t device_id;
  uint32_t mmio_size;
  uint32_t eerd_start;
  uint32_t eerd_done;
  uint8_t eerd_address_shift;
  uint8_t eerd_data_shift;
};

static const struct e1000_profile profile_82540em = {
    .driver_name = "e1000",
    .log_tag = "E1000",
    .vendor_id = E1000_VENDOR,
    .device_id = E1000_82540EM,
    .mmio_size = E1000_MMIO_SIZE,
    .eerd_start = E1000_EERD_START,
    .eerd_done = E1000_EERD_DONE,
    .eerd_address_shift = E1000_EERD_ADDR_SHIFT,
    .eerd_data_shift = E1000_EERD_DATA_SHIFT,
};

/* QEMU e1000e models the Intel 82574L. Its EERD address and DONE fields use
 * the newer layout; keep it separate from the 82540EM profile above. */
static const struct e1000_profile profile_82574l = {
    .driver_name = "e1000e",
    .log_tag = "E1000E",
    .vendor_id = E1000_VENDOR,
    .device_id = E1000E_82574L,
    .mmio_size = E1000_MMIO_SIZE,
    .eerd_start = E1000E_EERD_START,
    .eerd_done = E1000E_EERD_DONE,
    .eerd_address_shift = E1000E_EERD_ADDR_SHIFT,
    .eerd_data_shift = E1000E_EERD_DATA_SHIFT,
};

struct e1000_rx_desc {
  uint64_t address;
  uint16_t length, checksum;
  uint8_t status, errors;
  uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
  uint64_t address;
  uint16_t length;
  uint8_t cso, command, status, css;
  uint16_t special;
} __attribute__((packed));

_Static_assert(sizeof(struct e1000_rx_desc) == 16, "RX descriptor layout");
_Static_assert(sizeof(struct e1000_tx_desc) == 16, "TX descriptor layout");

struct e1000 {
  const struct e1000_profile *profile;
  struct pci_device *pci;
  volatile uint8_t *regs;
  uint16_t saved_command;
  uint8_t mac[6];
  uint64_t cycles_per_ms;
  struct net_device netdev;
  spinlock_t lock;
  struct pci_irq irq;
  dma_buffer_t *rx_ring, *tx_ring, *rx_data, *tx_data;
  uint16_t rx_next, tx_next, tx_clean, tx_pending;
  bool bound, active, rx_discard, rx_poll_pending, quarantined;
  /* Only task context takes changing; IRQ/data paths take lock and check active. */
  bool changing, carrier, notify_link, test_configured, test_stall, restore_default;
  uint64_t tx_progress_ms;
  uint32_t last_cause;
};

/* Stable control blocks keep queued net_packet/sysfs references and installed
 * IRQ callbacks valid across unbind. The legacy IRQ registry has no concurrent
 * uninstall grace period, so each slot installs its callback once per boot.
 * DMA allocations are still released after a successful stop/reset. */
static struct e1000 controllers[NET_DEVICE_MAX];
static bool shutting_down, monitor_started, monitor_changing;
static wait_queue_t monitor_wait;
static wait_queue_entry_t monitor_entry;
static void service_worker(void);
static bool start(struct net_device *netdev);
static void stop(struct net_device *netdev);
static bool link_up(struct net_device *netdev);
static int transmit(struct net_device *netdev, const void *frame, size_t length);
static bool reset_device(struct e1000 *dev);

enum e1000e_irq_policy {
  E1000E_IRQ_AUTO,
  E1000E_IRQ_INTX,
  E1000E_IRQ_MSI,
};

static bool is_e1000e(const struct e1000 *dev) {
  return dev->profile && dev->profile->device_id == E1000E_82574L;
}

static enum e1000e_irq_policy e1000e_irq_policy(void) {
  const char *cmdline = kernel_boot_cmdline ? kernel_boot_cmdline : "";
  static const char option[] = "e1000e_irq=";
  for (const char *p = cmdline; *p;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *end = p;
    while (*end && *end != ' ') end++;
    if ((size_t)(end - p) >= sizeof(option) - 1 &&
        !memcmp(p, option, sizeof(option) - 1)) {
      const char *value = p + sizeof(option) - 1;
      size_t length = (size_t)(end - value);
      if (length == 3 && !memcmp(value, "msi", 3)) return E1000E_IRQ_MSI;
      if (length == 4 && !memcmp(value, "intx", 4)) return E1000E_IRQ_INTX;
      if (length == 4 && !memcmp(value, "auto", 4)) return E1000E_IRQ_AUTO;
      klogf("[E1000E] invalid e1000e_irq value; using auto\n");
      return E1000E_IRQ_AUTO;
    }
    p = end;
  }
  return E1000E_IRQ_AUTO;
}

static const struct net_device_ops ops = {
    .start = start, .stop = stop, .link_up = link_up, .transmit = transmit,
};

static void lifecycle_lock(struct e1000 *dev) {
  while (__atomic_test_and_set(&dev->changing, __ATOMIC_ACQUIRE))
    sched_yield();
}

static void lifecycle_unlock(struct e1000 *dev) {
  __atomic_clear(&dev->changing, __ATOMIC_RELEASE);
}

static bool ensure_worker(void) {
  while (__atomic_test_and_set(&monitor_changing, __ATOMIC_ACQUIRE))
    sched_yield();
  if (!monitor_started) {
    wait_queue_init(&monitor_wait);
    struct thread *worker = sched_create_kernel_thread(service_worker, cpu_get_current(), false);
    if (worker) {
      strcpy(worker->comm, "e1000-service");
      sched_set_priority(worker, 0, -10);
      monitor_started = true;
      sched_enqueue_thread(worker, cpu_get_current());
    }
  }
  bool ready = monitor_started;
  __atomic_clear(&monitor_changing, __ATOMIC_RELEASE);
  return ready;
}

static uint32_t read32(struct e1000 *dev, uint32_t reg) {
  return *(volatile uint32_t *)(dev->regs + reg);
}

static void write32(struct e1000 *dev, uint32_t reg, uint32_t value) {
  *(volatile uint32_t *)(dev->regs + reg) = value;
}

static void flush(struct e1000 *dev) {
  (void)read32(dev, E1000_STATUS);
}

/* TSC deadlines work during early boot, without scheduler/timer IRQ progress. */
static bool wait32(struct e1000 *dev, uint32_t reg, uint32_t mask,
                   uint32_t expected, unsigned timeout_ms) {
  uint64_t start = hal_cpu_cycle_count();
  do {
    uint32_t value = read32(dev, reg);
    if (value == UINT32_MAX)
      return false;
    if ((value & mask) == expected)
      return true;
    hal_cpu_relax();
  } while (hal_cpu_cycle_count() - start < dev->cycles_per_ms * timeout_ms);
  return false;
}

static void delay_ms(struct e1000 *dev, unsigned ms) {
  uint64_t start = hal_cpu_cycle_count();
  while (hal_cpu_cycle_count() - start < dev->cycles_per_ms * ms)
    hal_cpu_relax();
}

static void set_command(struct e1000 *dev, uint16_t command) {
  struct pci_device *pci = dev->pci;
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command);
}

/* Leave failed probes and stopped devices unable to initiate DMA or deliver
 * controller interrupts. Keep register decoding enabled while the persistent
 * slot owns MMIO. */
static void quiesce(struct e1000 *dev) {
  if (dev->regs) {
    write32(dev, E1000_IMC, UINT32_MAX);
    write32(dev, E1000_RCTL, 0);
    write32(dev, E1000_TCTL, 0);
    flush(dev);
    (void)read32(dev, E1000_ICR);
  }
  set_command(dev, ((dev->saved_command | (dev->regs ? PCI_COMMAND_MEMORY : 0)) &
                   ~PCI_COMMAND_MASTER) |
                       PCI_COMMAND_INTX_DISABLE);
}

/* The PCI MSI Enable bit gates all messages from this single-vector source.
 * Keep its callback/vector installed for the slot's lifetime, and only toggle
 * delivery after active has changed under dev->lock. */
static bool mask_msi(struct e1000 *dev, bool masked) {
  if (dev->irq.mode != PCI_IRQ_MSI)
    return true;
  return pci_irq_mask(&dev->irq, 0, masked);
}

static bool valid_mac(const uint8_t mac[6]) {
  bool zero = true;
  for (unsigned i = 0; i < 6; i++)
    zero &= mac[i] == 0;
  return !zero && !(mac[0] & 1);
}

static bool read_mac(struct e1000 *dev) {
  /* RAR0 normally holds the auto-loaded permanent address after reset. */
  uint32_t low = read32(dev, E1000_RAL0);
  uint32_t high = read32(dev, E1000_RAH0);
  for (unsigned i = 0; i < 4; i++)
    dev->mac[i] = low >> (i * 8);
  dev->mac[4] = high;
  dev->mac[5] = high >> 8;
  if ((high & E1000_RAH_AV) && valid_mac(dev->mac))
    return true;

  /* EEPROM command layouts are selected by controller profile. */
  for (unsigned i = 0; i < 3; i++) {
    write32(dev, E1000_EERD,
            (i << dev->profile->eerd_address_shift) | dev->profile->eerd_start);
    if (!wait32(dev, E1000_EERD, dev->profile->eerd_done,
                dev->profile->eerd_done,
                EEPROM_TIMEOUT_MS))
      return false;
    uint32_t word = read32(dev, E1000_EERD) >> dev->profile->eerd_data_shift;
    dev->mac[i * 2] = word;
    dev->mac[i * 2 + 1] = word >> 8;
  }
  return valid_mac(dev->mac);
}

static void free_dma(struct e1000 *dev) {
  dma_free(dev->rx_ring);
  dma_free(dev->tx_ring);
  dma_free(dev->rx_data);
  dma_free(dev->tx_data);
  dev->rx_ring = dev->tx_ring = dev->rx_data = dev->tx_data = NULL;
  __atomic_store_n(&dev->netdev.stats.dma_bytes, 0, __ATOMIC_RELEASE);
}

/* Called with the IRQ/data lock held. Slow logging and userland notification
 * run in the service thread, never under this lock or in hard IRQ context. */
static void update_link(struct e1000 *dev) {
  uint32_t status = read32(dev, E1000_STATUS);
  bool up = dev->active && status != UINT32_MAX && (status & E1000_STATUS_LU);
  if (up != dev->carrier) {
    dev->carrier = up;
    dev->notify_link = true;
    dev->netdev.stats.link_changes++;
  }
}

static void reclaim_tx(struct e1000 *dev) {
  struct e1000_tx_desc *ring = dev->tx_ring->virt;
  while (dev->tx_pending) {
    struct e1000_tx_desc *desc = &ring[dev->tx_clean];
    uint8_t status = __atomic_load_n(&desc->status, __ATOMIC_ACQUIRE);
    if (!(status & E1000_TX_DD))
      break;
    if (status & E1000_TX_ERRORS) {
      dev->netdev.stats.tx_errors++;
      if (status & (1u << 3))
        dev->netdev.stats.tx_underruns++;
    } else {
      dev->netdev.stats.tx_packets++;
      dev->netdev.stats.tx_bytes += desc->length;
    }
    dev->tx_clean = (dev->tx_clean + 1) % RING_COUNT;
    dev->tx_pending--;
    dev->tx_progress_ms = lapic_timer_get_ms();
  }
}

static int transmit(struct net_device *netdev, const void *frame, size_t length) {
  struct e1000 *dev = netdev->driver_private;
  if (!frame || length < 14 || length > NET_FRAME_MAX - 4) {
    __atomic_fetch_add(&netdev->stats.tx_errors, 1, __ATOMIC_RELAXED);
    return -1;
  }
  spinlock_acquire(&dev->lock);
  if (!dev->active) {
    spinlock_release(&dev->lock);
    return -1;
  }
  update_link(dev);
  if (!dev->carrier) {
    netdev->stats.tx_dropped++;
    spinlock_release(&dev->lock);
    return -1;
  }
  reclaim_tx(dev);
  /* Leave one descriptor unused: TDH == TDT must always mean empty. */
  if (dev->tx_pending == RING_COUNT - 1) {
    netdev->stats.queue_full++;
    netdev->stats.tx_dropped++;
    spinlock_release(&dev->lock);
    return -1;
  }
  unsigned slot = dev->tx_next;
  uint8_t *buffer = (uint8_t *)dev->tx_data->virt + slot * BUFFER_SIZE;
  size_t wire_length = length < 60 ? 60 : length;
  memcpy(buffer, frame, length);
  if (wire_length > length)
    memset(buffer + length, 0, wire_length - length);
  struct e1000_tx_desc *desc =
      &((struct e1000_tx_desc *)dev->tx_ring->virt)[slot];
  desc->length = wire_length;
  desc->cso = desc->css = 0;
  desc->special = 0;
  desc->command = E1000_TX_EOP | E1000_TX_IFCS | E1000_TX_RS;
  desc->status = 0;
  dev->tx_next = (slot + 1) % RING_COUNT;
  if (!dev->tx_pending)
    dev->tx_progress_ms = lapic_timer_get_ms();
  dev->tx_pending++;
  /* Coherent WB DMA: publish data and descriptor before the UC doorbell. */
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (!dev->test_stall)
    write32(dev, E1000_TDT, dev->tx_next);
  spinlock_release(&dev->lock);
  return 0;
}

/* Run from the service thread with dev->lock held. RX interrupts are masked
 * while polling; return true when another bounded pass is needed. */
static bool drain_rx(struct e1000 *dev) {
  struct e1000_rx_desc *ring = dev->rx_ring->virt;
  struct net_rx_frame frames[RX_POLL_BUDGET];
  size_t frame_count = 0;
  unsigned consumed = 0;
  for (; consumed < RX_POLL_BUDGET; consumed++) {
    unsigned slot = dev->rx_next;
    struct e1000_rx_desc *desc = &ring[slot];
    uint8_t status = __atomic_load_n(&desc->status, __ATOMIC_ACQUIRE);
    if (!(status & E1000_RX_DD))
      break;
    bool end = (status & E1000_RX_EOP) != 0;
    /* 2 KiB buffers cover every supported frame. Drop the entire unexpected
     * multi-descriptor packet, including its final fragment. SECRC removes FCS. */
    if (dev->rx_discard) {
      dev->rx_discard = !end;
    } else if (!end || desc->errors || desc->length < 14 ||
               desc->length > NET_FRAME_MAX - 4) {
      dev->netdev.stats.rx_errors++;
      dev->netdev.stats.rx_dropped++;
      dev->rx_discard = !end;
    } else {
      frames[frame_count++] = (struct net_rx_frame){
          .data = (uint8_t *)dev->rx_data->virt + slot * BUFFER_SIZE,
          .length = desc->length,
      };
    }
    desc->length = desc->checksum = desc->special = 0;
    desc->errors = 0;
    __atomic_store_n(&desc->status, 0, __ATOMIC_RELEASE);
    dev->rx_next = (slot + 1) % RING_COUNT;
  }
  if (frame_count)
    (void)net_rx_submit_batch_irq(&dev->netdev, frames, frame_count);
  if (consumed) {
    /* Return a contiguous descriptor run with one tail write per batch. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    write32(dev, E1000_RDT,
            (dev->rx_next + RING_COUNT - 1) % RING_COUNT);
  }
  return (__atomic_load_n(&ring[dev->rx_next].status, __ATOMIC_ACQUIRE) &
          E1000_RX_DD) != 0;
}

static void interrupt_slot(unsigned index) {
  struct e1000 *dev = &controllers[index];
  bool wake_poller = false;
  spinlock_acquire(&dev->lock);
  if (dev->active) {
    uint32_t cause = read32(dev, E1000_ICR); /* read-to-clear, including shared IRQs */
    if (cause != UINT32_MAX && (cause & E1000_INT_MASK)) {
      dev->last_cause = cause;
      dev->netdev.stats.interrupts++;
      if (cause & E1000_INT_RXO)
        dev->netdev.stats.rx_overflows++;
      if (cause & E1000_INT_LSC)
        update_link(dev);
      if (cause & RX_INT_MASK) {
        /* Defer descriptor processing and frame copies out of hard IRQ. */
        write32(dev, E1000_IMC, RX_INT_MASK);
        __atomic_store_n(&dev->rx_poll_pending, true, __ATOMIC_RELEASE);
        wake_poller = true;
      }
      reclaim_tx(dev);
    }
  }
  spinlock_release(&dev->lock);
  if (wake_poller)
    wait_queue_wake_one(&monitor_wait);
}

#define SLOT_IRQ(n) static void irq##n(struct registers *regs) { \
  (void)regs; interrupt_slot(n); \
}
SLOT_IRQ(0) SLOT_IRQ(1) SLOT_IRQ(2) SLOT_IRQ(3)
SLOT_IRQ(4) SLOT_IRQ(5) SLOT_IRQ(6) SLOT_IRQ(7)
static const isr_t handlers[] = {irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7};
_Static_assert(NET_DEVICE_MAX == 8, "add IRQ wrappers for new slots");

static void program_rings(struct e1000 *dev) {
  memset(dev->rx_ring->virt, 0, dev->rx_ring->size);
  memset(dev->tx_ring->virt, 0, dev->tx_ring->size);
  struct e1000_rx_desc *rx = dev->rx_ring->virt;
  struct e1000_tx_desc *tx = dev->tx_ring->virt;
  for (unsigned i = 0; i < RING_COUNT; i++) {
    rx[i].address = dev->rx_data->phys + i * BUFFER_SIZE;
    tx[i].address = dev->tx_data->phys + i * BUFFER_SIZE;
    tx[i].status = E1000_TX_DD;
  }
  spinlock_acquire(&dev->lock);
  dev->rx_next = dev->tx_next = dev->tx_clean = dev->tx_pending = 0;
  dev->rx_poll_pending = false;
  dev->rx_discard = false;
  write32(dev, E1000_CTRL, read32(dev, E1000_CTRL) | E1000_CTRL_SLU);
  write32(dev, E1000_RAL0, (uint32_t)dev->mac[0] | (uint32_t)dev->mac[1] << 8 |
          (uint32_t)dev->mac[2] << 16 | (uint32_t)dev->mac[3] << 24);
  write32(dev, E1000_RAH0, dev->mac[4] | (uint32_t)dev->mac[5] << 8 | E1000_RAH_AV);
  for (unsigned i = 0; i < 128; i++)
    write32(dev, E1000_MTA + i * 4, 0);
  write32(dev, E1000_RDBAL, (uint32_t)dev->rx_ring->phys);
  write32(dev, E1000_RDBAH, dev->rx_ring->phys >> 32);
  write32(dev, E1000_RDLEN, RING_COUNT * sizeof(*rx));
  write32(dev, E1000_RDH, 0);
  write32(dev, E1000_RDT, RING_COUNT - 1);
  write32(dev, E1000_TDBAL, (uint32_t)dev->tx_ring->phys);
  write32(dev, E1000_TDBAH, dev->tx_ring->phys >> 32);
  write32(dev, E1000_TDLEN, RING_COUNT * sizeof(*tx));
  write32(dev, E1000_TDH, 0);
  write32(dev, E1000_TDT, 0);
  write32(dev, E1000_RXCSUM, 0);
  if (dev->profile->device_id == E1000E_82574L) {
    /* QEMU's 82574 reset defaults enable IP/TCP/UDP checksum processing.
     * Select the 16-byte legacy RX descriptor layout, disable RSS, checksum
     * and VLAN transforms plus TX queue 1, and apply the TARC0 erratum bit. */
    write32(dev, E1000_CTRL_EXT,
            read32(dev, E1000_CTRL_EXT) & ~E1000E_CTRL_EXT_VME);
    write32(dev, E1000_RFCTL,
            read32(dev, E1000_RFCTL) & ~E1000E_RFCTL_EXTEN);
    write32(dev, E1000_MRQC, 0);
    write32(dev, E1000_TARC0,
            read32(dev, E1000_TARC0) | E1000E_TARC_ENABLE |
                E1000E_TARC_82574_ERRATA);
    write32(dev, E1000_TARC1,
            read32(dev, E1000_TARC1) & ~E1000E_TARC_ENABLE);
  }
  write32(dev, E1000_RDTR, 0);
  write32(dev, E1000_RADV, 0);
  write32(dev, E1000_TIDV, 0);
  write32(dev, E1000_TADV, 0);
  write32(dev, E1000_ITR, 0);
  if (is_e1000e(dev)) {
    /* Use the legacy ICR read-to-clear path for both MSI and INTx. IAM is
     * reserved for explicit auto-mask/rearm policies; keep it disabled. */
    write32(dev, E1000_IAM, 0);
  }
  write32(dev, E1000_TIPG, 10u | (8u << 10) | (6u << 20));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  dev->active = true;
  dev->netdev.started = true;
  dev->tx_progress_ms = lapic_timer_get_ms();
  uint16_t command = pci_config_read16(dev->pci->bus, dev->pci->slot, dev->pci->func, 4);
  set_command(dev, command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE);
  write32(dev, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << 4) | (0x40u << 12));
  /* All-multicast supports IPv6 NDP without a multicast-filter API yet. */
  write32(dev, E1000_RCTL,
          E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_MPE |
              E1000_RCTL_SECRC);
  (void)read32(dev, E1000_ICR);
  write32(dev, E1000_IMS, E1000_INT_MASK);
  flush(dev);
  uint16_t active_command = command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
  if (dev->irq.mode == PCI_IRQ_INTX)
    active_command &= ~PCI_COMMAND_INTX_DISABLE;
  else
    active_command |= PCI_COMMAND_INTX_DISABLE;
  set_command(dev, active_command);
  update_link(dev);
  dev->notify_link = true;
  spinlock_release(&dev->lock);
}

static bool start_device(struct e1000 *dev) {
  struct net_device *netdev = &dev->netdev;
  if (__atomic_load_n(&shutting_down, __ATOMIC_ACQUIRE) || dev->quarantined)
    return false;
  if (dev->active)
    return true;
  if (!ensure_worker()) {
    klogf("[%s] service worker creation failed\n", dev->profile->log_tag);
    return false;
  }
  dev->rx_ring = dma_alloc(RING_COUNT * sizeof(struct e1000_rx_desc), DMA_FLAG_32BIT);
  dev->tx_ring = dma_alloc(RING_COUNT * sizeof(struct e1000_tx_desc), DMA_FLAG_32BIT);
  dev->rx_data = dma_alloc(RING_COUNT * BUFFER_SIZE, DMA_FLAG_32BIT);
  dev->tx_data = dma_alloc(RING_COUNT * BUFFER_SIZE, DMA_FLAG_32BIT);
  uint64_t bytes = (dev->rx_ring ? dev->rx_ring->size : 0) +
                   (dev->tx_ring ? dev->tx_ring->size : 0) +
                   (dev->rx_data ? dev->rx_data->size : 0) +
                   (dev->tx_data ? dev->tx_data->size : 0);
  __atomic_store_n(&dev->netdev.stats.dma_bytes, bytes, __ATOMIC_RELEASE);
  if (!dev->rx_ring || !dev->tx_ring || !dev->rx_data || !dev->tx_data) {
    klogf("[%s] %s DMA allocation failed\n", dev->profile->log_tag,
          netdev->name);
    free_dma(dev);
    return false;
  }
  unsigned index = dev - controllers;
  if (dev->irq.mode == PCI_IRQ_NONE) {
    uint32_t modes = PCI_IRQ_MODE_INTX;
    enum e1000e_irq_policy policy = E1000E_IRQ_INTX;
    if (is_e1000e(dev)) {
      policy = e1000e_irq_policy();
      modes = policy == E1000E_IRQ_INTX ? PCI_IRQ_MODE_INTX :
              policy == E1000E_IRQ_MSI ? PCI_IRQ_MODE_MSI :
                  PCI_IRQ_MODE_MSI | PCI_IRQ_MODE_INTX;
    }
    if (!pci_irq_request_modes(dev->pci, &dev->irq, &handlers[index], 1, 0,
                               modes)) {
      klogf("[%s] %s %s interrupt registration failed\n",
            dev->profile->log_tag, netdev->name,
            policy == E1000E_IRQ_MSI ? "forced MSI" :
            policy == E1000E_IRQ_INTX ? "INTx" : "MSI/INTx");
      free_dma(dev);
      return false;
    }
  }
  if (dev->irq.mode == PCI_IRQ_MSI && !mask_msi(dev, true)) {
    klogf("[%s] %s failed to mask MSI before activation\n",
          dev->profile->log_tag, netdev->name);
    free_dma(dev);
    return false;
  }
  program_rings(dev);
  if (dev->irq.mode == PCI_IRQ_MSI && !mask_msi(dev, false)) {
    klogf("[%s] %s failed to enable MSI after activation\n",
          dev->profile->log_tag, netdev->name);
    (void)reset_device(dev);
    free_dma(dev);
    return false;
  }
  if (dev->irq.mode == PCI_IRQ_MSI)
    klogf("[%s] %s RX/TX enabled; MSI vector %u, legacy INTx disabled, "
          "%u descriptors per ring, link=%s\n",
          dev->profile->log_tag, netdev->name,
          pci_irq_vector(&dev->irq, 0), RING_COUNT,
          link_up(netdev) ? "up" : "down");
  else
    klogf("[%s] %s RX/TX enabled; INTx IRQ %u, %u descriptors per ring, link=%s\n",
        dev->profile->log_tag, netdev->name, dev->irq.irq_line, RING_COUNT,
        link_up(netdev) ? "up" : "down");
  return true;
}

static bool start(struct net_device *netdev) {
  struct e1000 *dev = netdev->driver_private;
  lifecycle_lock(dev);
  bool ok = start_device(dev);
  lifecycle_unlock(dev);
  return ok;
}

static bool link_up(struct net_device *netdev) {
  struct e1000 *dev = netdev->driver_private;
  spinlock_acquire(&dev->lock);
  bool up = dev->active && (read32(dev, E1000_STATUS) & E1000_STATUS_LU);
  spinlock_release(&dev->lock);
  return up;
}

/* Caller owns changing. Taking lock waits for any executing IRQ/transmit path
 * to leave the rings. active=false then prevents all new access, including
 * callbacks retained by the IRQ registry. MSI delivery is masked before reset. */
static bool reset_device(struct e1000 *dev) {
  spinlock_acquire(&dev->lock);
  if (dev->tx_ring)
    reclaim_tx(dev);
  dev->netdev.stats.tx_dropped += dev->tx_pending;
  dev->tx_pending = 0;
  dev->active = false;
  dev->netdev.started = false;
  __atomic_store_n(&dev->rx_poll_pending, false, __ATOMIC_RELEASE);
  __atomic_add_fetch(&dev->netdev.generation, 1, __ATOMIC_RELEASE);
  if (dev->carrier) {
    dev->carrier = false;
    dev->netdev.stats.link_changes++;
  }
  dev->notify_link = true;
  quiesce(dev);
  bool dma = dev->rx_ring || dev->tx_ring || dev->rx_data || dev->tx_data;
  spinlock_release(&dev->lock);
  if (!mask_msi(dev, true))
    klogf("[%s] %s MSI mask failed after quiesce\n", dev->profile->log_tag,
          dev->netdev.name);
  if (dev->quarantined) {
    return false;
  }
  if (dma) {
    write32(dev, E1000_CTRL, read32(dev, E1000_CTRL) | E1000_CTRL_RST);
    flush(dev);
  }
  if (dma) {
    if (!wait32(dev, E1000_CTRL, E1000_CTRL_RST, 0, RESET_TIMEOUT_MS)) {
      dev->quarantined = true;
      dev->netdev.stats.recovery_failures++;
      klogf("[%s] %s reset timeout; DMA buffers quarantined\n",
            dev->profile->log_tag, dev->netdev.name);
      return false;
    }
    delay_ms(dev, 10);
    quiesce(dev);
    dev->netdev.stats.resets++;
  }
  return !dev->quarantined;
}

static void stop(struct net_device *netdev) {
  struct e1000 *dev = netdev->driver_private;
  lifecycle_lock(dev);
  if (reset_device(dev))
    free_dma(dev);
  lifecycle_unlock(dev);
}

static void service_device(struct e1000 *dev) {
  if (__atomic_test_and_set(&dev->changing, __ATOMIC_ACQUIRE))
    return;
  bool stalled = false;
  uint32_t pending = 0, head = 0, tail = 0, cause = 0;
  spinlock_acquire(&dev->lock);
  if (dev->active && !__atomic_load_n(&shutting_down, __ATOMIC_ACQUIRE)) {
    reclaim_tx(dev); /* also reclaim completions whose TX interrupt was lost */
    update_link(dev); /* poll as well as observing LSC interrupts */
    uint64_t now = lapic_timer_get_ms();
    if (!dev->carrier)
      dev->tx_progress_ms = now;
    stalled = dev->carrier && dev->tx_pending &&
              now - dev->tx_progress_ms >= TX_TIMEOUT_MS;
    if (stalled) {
      dev->netdev.stats.tx_timeouts++;
      pending = dev->tx_pending;
      head = read32(dev, E1000_TDH);
      tail = read32(dev, E1000_TDT);
      cause = dev->last_cause;
    }
  }
  spinlock_release(&dev->lock);
  if (stalled)
    klogf("[%s] %s TX stalled: pending=%u TDH=%u TDT=%u ICR=%x; resetting\n",
          dev->profile->log_tag, dev->netdev.name, pending, head, tail, cause);
  if (stalled && reset_device(dev)) {
    dev->test_stall = false;
    if (__atomic_load_n(&shutting_down, __ATOMIC_ACQUIRE)) {
      free_dma(dev);
    } else {
      program_rings(dev); /* reuse allocations only after reset acknowledged */
      if (!mask_msi(dev, false)) {
        klogf("[%s] %s failed to restore MSI after recovery\n",
              dev->profile->log_tag, dev->netdev.name);
        dev->netdev.stats.recovery_failures++;
        if (reset_device(dev))
          free_dma(dev);
      } else {
        dev->netdev.stats.recoveries++;
        klogf("[%s] %s TX recovery complete\n", dev->profile->log_tag,
              dev->netdev.name);
      }
    }
  }
  spinlock_acquire(&dev->lock);
  bool notify = dev->notify_link;
  bool carrier = dev->carrier;
  dev->notify_link = false;
  spinlock_release(&dev->lock);
  if (notify) {
    klogf("[%s] %s link %s\n", dev->profile->log_tag, dev->netdev.name,
          carrier ? "up" : "down");
    if (net_device_default() == &dev->netdev)
      netlink_notify_link(&dev->netdev);
  }
  lifecycle_unlock(dev);
}

static void service_worker(void) {
  struct thread *self = sched_get_current();
  memset(&monitor_entry, 0, sizeof(monitor_entry));
  monitor_entry.thread = self;
  for (;;) {
    bool more_rx = false;
    for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
      struct e1000 *dev = &controllers[i];
      spinlock_acquire(&dev->lock);
      if (dev->active &&
          __atomic_load_n(&dev->rx_poll_pending, __ATOMIC_ACQUIRE)) {
        bool pending = drain_rx(dev);
        more_rx |= pending;
        if (!pending) {
          __atomic_store_n(&dev->rx_poll_pending, false, __ATOMIC_RELEASE);
          write32(dev, E1000_IMS, RX_INT_MASK);
          flush(dev);
        }
      }
      spinlock_release(&dev->lock);
    }
    if (!__atomic_load_n(&shutting_down, __ATOMIC_ACQUIRE))
      for (unsigned i = 0; i < NET_DEVICE_MAX; i++)
        service_device(&controllers[i]);

    if (more_rx) {
      sched_yield(); /* Keep each poll bounded and let other runnable work run. */
      continue;
    }

    /* Pair waiter publication with an RX-pending recheck, so an interrupt
     * arriving during the park handoff cannot leave the poller asleep. */
    hal_irq_state_t flags = hal_irq_save();
    self->wakeup_ticks = lapic_timer_get_ticks() + SERVICE_INTERVAL_MS;
    self->state = THREAD_SLEEPING;
    wait_queue_add(&monitor_wait, &monitor_entry);
    bool pending = false;
    for (unsigned i = 0; i < NET_DEVICE_MAX; i++)
      pending |= __atomic_load_n(&controllers[i].rx_poll_pending,
                                 __ATOMIC_ACQUIRE);
    if (pending)
      self->state = THREAD_RUNNING;
    hal_irq_restore(flags);
    if (!pending)
      sched_yield();
    flags = hal_irq_save();
    self->state = THREAD_RUNNING;
    self->wakeup_ticks = 0;
    wait_queue_remove(&monitor_wait, &monitor_entry);
    hal_irq_restore(flags);
  }
}

void e1000_shutdown(void) {
  __atomic_store_n(&shutting_down, true, __ATOMIC_RELEASE);
  for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
    struct e1000 *dev = &controllers[i];
    if (!dev->pci)
      continue;
    stop(&dev->netdev);
    klogf("[%s] %s shutdown: DMA/interrupts disabled, buffers=%s\n",
          dev->profile->log_tag, dev->netdev.name,
          dev->quarantined ? "quarantined" : "released");
  }
}

static int probe(struct device *device) {
  const struct e1000_profile *profile =
      device && device->driver ? device->driver->private_data : NULL;
  if (!profile || !device->driver->name ||
      strcmp(device->driver->name, profile->driver_name))
    return -1;

  struct pci_device *pci = NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *candidate = asc_pci_get_device(i);
    if (candidate && candidate->kernel_device == device) {
      pci = candidate;
      break;
    }
  }
  if (!pci || pci->linux_driver_name || pci->vendor_id != profile->vendor_id ||
      pci->device_id != profile->device_id || pci->class_code != 2 ||
      pci->subclass != 0)
    return -1;

  /* Supported profiles use a non-prefetchable 32-bit memory BAR0. Fail
   * closed rather than truncating a 64-bit BAR or mapping I/O. */
  uint32_t bar = pci->bar[0];
  uint64_t phys = bar & ~0xfu;
  if ((bar & 0xfu) || !phys || (phys & (PAGE_SIZE - 1)) ||
      pci->bar_size[0] < profile->mmio_size ||
      pci->bar_size[0] > (1ull << 32) - phys) {
    klogf("[%s] %s invalid BAR0\n", profile->log_tag, device->name);
    return -1;
  }
  uint64_t khz = tsc_get_freq_khz();
  if (!khz || khz > UINT64_MAX / RESET_TIMEOUT_MS)
    return -1;
  struct e1000 *dev = NULL;
  for (unsigned i = 0; i < NET_DEVICE_MAX; i++) {
    if (controllers[i].pci == pci) {
      dev = &controllers[i];
      break;
    }
    if (!controllers[i].pci && !dev)
      dev = &controllers[i];
  }
  if (!dev || dev->bound || dev->quarantined)
    return -1;
  lifecycle_lock(dev);
  if (__atomic_load_n(&shutting_down, __ATOMIC_ACQUIRE) || dev->bound || dev->quarantined) {
    lifecycle_unlock(dev);
    return -1;
  }
  /* Slot addresses, locks and installed handlers survive unbind/reprobe. */
  dev->profile = profile;
  dev->pci = pci;
  dev->netdev.driver_private = dev;
  dev->cycles_per_ms = khz;
  dev->saved_command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  set_command(dev, (dev->saved_command | PCI_COMMAND_MEMORY |
                    PCI_COMMAND_INTX_DISABLE) & ~PCI_COMMAND_MASTER);

  void *mapped = pci_map_mmio(phys, profile->mmio_size);
  if (!mapped) {
    klogf("[%s] %s BAR0 mapping failed\n", profile->log_tag, device->name);
    goto fail;
  }
  dev->regs = (volatile uint8_t *)mapped;
  write32(dev, E1000_IMC, UINT32_MAX);
  write32(dev, E1000_RCTL, 0);
  write32(dev, E1000_TCTL, 0);
  flush(dev);
  (void)read32(dev, E1000_ICR);
  uint32_t ctrl = read32(dev, E1000_CTRL);
  if (ctrl == UINT32_MAX)
    goto fail;
  write32(dev, E1000_CTRL, ctrl | E1000_CTRL_RST);
  flush(dev);
  if (!wait32(dev, E1000_CTRL, E1000_CTRL_RST, 0, RESET_TIMEOUT_MS)) {
    klogf("[%s] %s reset timeout\n", profile->log_tag, device->name);
    goto fail;
  }
  /* Allow EEPROM auto-read to settle before consuming RAR0 or using EERD. */
  delay_ms(dev, 10);
  write32(dev, E1000_IMC, UINT32_MAX);
  write32(dev, E1000_RCTL, 0);
  write32(dev, E1000_TCTL, 0);
  (void)read32(dev, E1000_ICR);
  if (!read_mac(dev)) {
    klogf("[%s] %s MAC/EEPROM read failed\n", profile->log_tag,
          device->name);
    goto fail;
  }
  write32(dev, E1000_CTRL, read32(dev, E1000_CTRL) | E1000_CTRL_SLU);
  flush(dev);
  memcpy(dev->netdev.mac, dev->mac, sizeof(dev->mac));
  dev->netdev.driver_private = dev;
  dev->netdev.pci = pci;
  dev->netdev.mtu = NET_MTU_ETHERNET;
  dev->netdev.persistent = true;
  dev->netdev.ops = &ops;
  if (net_device_find(dev->netdev.name))
    dev->netdev.name[0] = 0;
  if (net_device_register(&dev->netdev)) {
    klogf("[%s] %s network registration failed\n", profile->log_tag,
          device->name);
    goto fail;
  }
  dev->bound = true;
  device->driver_data = dev;
  dev->netdev.stats.resets++;
  if (!dev->test_configured) {
    const char *option = kernel_boot_cmdline ? strstr(kernel_boot_cmdline, "e1000_test_tx_stall=1") : NULL;
    dev->test_stall = option && (option == kernel_boot_cmdline || option[-1] == ' ') &&
                       (!option[21] || option[21] == ' ');
    dev->test_configured = true;
    if (dev->test_stall)
      klogf("[%s] %s diagnostic TX stall armed\n", profile->log_tag,
            dev->netdev.name);
  }
  klogf("[%s] %s %04x:%04x MAC %02x:%02x:%02x:%02x:%02x:%02x link=%s; "
        "probe ready, RX/TX/DMA/interrupts disabled\n",
        profile->log_tag, device->name, profile->vendor_id, profile->device_id,
        dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
        dev->mac[4], dev->mac[5],
        (read32(dev, E1000_STATUS) & E1000_STATUS_LU) ? "up" : "down");
  /* A sysfs rebind after protocol startup must reactivate this NIC without
   * rerunning the global protocol initialization or creating new workers. */
  if (net_get_rx_handler()) {
    if (!start_device(dev)) {
      net_device_unregister(&dev->netdev);
      dev->bound = false;
      device->driver_data = NULL;
      goto fail;
    }
    dev->netdev.started = true;
    if (dev->restore_default) {
      net_device_set_default(dev->netdev.name);
      dev->restore_default = false;
    }
  }
  lifecycle_unlock(dev);
  return 0;

fail:
  quiesce(dev);
  lifecycle_unlock(dev);
  return -1;
}

static void remove_device(struct device *device) {
  struct e1000 *dev = device->driver_data;
  if (!dev)
    return;
  dev->restore_default = net_device_default() == &dev->netdev;
  stop(&dev->netdev);
  net_device_unregister(&dev->netdev);
  dev->bound = false;
  device->driver_data = NULL;
}

static struct device_id e1000_ids[] = {
    {.type = ID_PCI, .pci = {.vendor = E1000_VENDOR, .device = E1000_82540EM}},
};
static struct device_id e1000e_ids[] = {
    {.type = ID_PCI, .pci = {.vendor = E1000_VENDOR, .device = E1000E_82574L}},
};
static struct driver e1000_driver = {
    .name = "e1000", .ids = e1000_ids, .id_count = 1,
    .probe = probe, .remove = remove_device, .kind = DRIVER_KERNEL,
    .private_data = (void *)&profile_82540em,
};
static struct driver e1000e_driver = {
    .name = "e1000e", .ids = e1000e_ids, .id_count = 1,
    .probe = probe, .remove = remove_device, .kind = DRIVER_KERNEL,
    .private_data = (void *)&profile_82574l,
};

void e1000_init(void) {
  struct driver *drivers[] = {&e1000_driver, &e1000e_driver};
  struct bus_type *bus = asc_pci_bus_type();
  for (unsigned i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
    drivers[i]->bus = bus;
    dm_register_driver(drivers[i]);
    sysfs_pci_driver_registered(drivers[i]);
  }
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pci = asc_pci_get_device(i);
    if (pci && pci->kernel_device &&
        (pci->kernel_device->driver == &e1000_driver ||
         pci->kernel_device->driver == &e1000e_driver))
      sysfs_pci_device_bound(pci->kernel_device);
  }
}
