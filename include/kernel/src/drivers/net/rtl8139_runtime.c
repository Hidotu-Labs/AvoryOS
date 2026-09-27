#include "drivers/net/rtl8139.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "drivers/pci/pci.h"
#include "io/io.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "net/core.h"
#include "sched/sched.h"

#include <stddef.h>
#include <stdint.h>

#define RTL_TSD0 0x10
#define RTL_TSAD0 0x20
#define RTL_CR 0x37
#define RTL_CAPR 0x38
#define RTL_CBR 0x3A
#define RTL_IMR 0x3C
#define RTL_ISR 0x3E
#define RTL_TCR 0x40
#define RTL_RCR 0x44

#define CR_BUFFER_EMPTY 0x01
#define CR_TX_ENABLE 0x04
#define CR_RX_ENABLE 0x08

#define INT_RX_OK 0x0001
#define INT_RX_ERROR 0x0002
#define INT_TX_OK 0x0004
#define INT_TX_ERROR 0x0008
#define INT_RX_OVERFLOW 0x0010
#define INT_LINK_CHANGE 0x0020
#define INT_TIMEOUT 0x4000
#define INT_KNOWN (INT_RX_OK | INT_RX_ERROR | INT_TX_OK | INT_TX_ERROR | \
                   INT_RX_OVERFLOW | INT_LINK_CHANGE | INT_TIMEOUT)

#define TSD_OWN (1u << 13)
#define TSD_UNDERRUN (1u << 14)
#define TSD_OK (1u << 15)
#define TSD_ABORT (1u << 30)

#define RCR_ACCEPT_PHYSICAL (1u << 1)
#define RCR_ACCEPT_MULTICAST (1u << 2)
#define RCR_ACCEPT_BROADCAST (1u << 3)
#define RCR_WRAP (1u << 7)
#define RCR_MAX_DMA (7u << 8)
#define RCR_RBLEN_32K (2u << 11)
#define RCR_NO_THRESHOLD (7u << 13)
#define TCR_MAX_DMA (7u << 8)
#define TCR_IFG96 (3u << 24)

#define RX_RING_SIZE 32768u
#define TX_COUNT 4u
#define TX_BUFFER_SIZE 2048u
#define ETH_HEADER_SIZE 14u
#define ETH_MIN_NO_FCS 60u

static spinlock_t tx_lock = SPINLOCK_INIT;
static spinlock_t rx_lock = SPINLOCK_INIT;
static bool tx_busy[TX_COUNT];
static uint8_t tx_next;
static uint32_t rx_offset;
static bool phase2_ready;
static bool phase3_ready;

static uint8_t *tx_virt(void) {
  return (uint8_t *)((uint64_t)rtl8139_tx_phys() + pmm_get_hhdm_offset());
}

static uint8_t *rx_virt(void) {
  return (uint8_t *)((uint64_t)rtl8139_rx_phys() + pmm_get_hhdm_offset());
}

static void reclaim_tx_locked(void) {
  struct net_device *dev = rtl8139_netdev();
  uint16_t io = rtl8139_io_base();
  if (!dev)
    return;
  for (uint32_t i = 0; i < TX_COUNT; i++) {
    if (!tx_busy[i])
      continue;
    uint32_t status = inl(io + RTL_TSD0 + i * 4);
    if (!(status & (TSD_OWN | TSD_OK | TSD_ABORT | TSD_UNDERRUN)))
      continue;
    tx_busy[i] = false;
    if (status & TSD_OK)
      dev->stats.tx_packets++;
    else
      dev->stats.tx_errors++;
  }
}

int rtl8139_transmit(struct net_device *dev, const void *frame, size_t length) {
  if (!phase2_ready || dev != rtl8139_netdev() || !frame ||
      length < ETH_HEADER_SIZE || length > NET_FRAME_MAX - 4) {
    if (dev)
      dev->stats.tx_errors++;
    return -1;
  }

  spinlock_acquire(&tx_lock);
  uint8_t slot = tx_next;
  bool found = false;
  for (int attempt = 0; attempt < 32 && !found; attempt++) {
    reclaim_tx_locked();
    for (uint32_t i = 0; i < TX_COUNT; i++) {
      slot = (uint8_t)((tx_next + i) % TX_COUNT);
      if (!tx_busy[slot]) {
        found = true;
        break;
      }
    }
    if (found)
      break;
    /* All four descriptors are in flight.  Drop the lock and let the
     * scheduler (and the TX-complete IRQ) make progress instead of spinning
     * with interrupts disabled. */
    spinlock_release(&tx_lock);
    sched_yield();
    spinlock_acquire(&tx_lock);
  }
  if (!found) {
    dev->stats.tx_dropped++;
    dev->stats.queue_full++;
    spinlock_release(&tx_lock);
    return -1;
  }

  size_t wire_length = length < ETH_MIN_NO_FCS ? ETH_MIN_NO_FCS : length;
  uint8_t *buffer = tx_virt() + slot * TX_BUFFER_SIZE;
  memcpy(buffer, frame, length);
  if (wire_length > length)
    memset(buffer + length, 0, wire_length - length);

  tx_busy[slot] = true;
  tx_next = (uint8_t)((slot + 1) % TX_COUNT);
  dev->stats.tx_bytes += wire_length;
  outl(rtl8139_io_base() + RTL_TSD0 + slot * 4, (uint32_t)wire_length);
  reclaim_tx_locked();
  spinlock_release(&tx_lock);
  return 0;
}

bool rtl8139_phase2_init(void) {
  if (!rtl8139_present())
    return false;
  spinlock_init(&tx_lock);
  spinlock_init(&rx_lock);
  memset(tx_busy, 0, sizeof(tx_busy));
  tx_next = 0;
  uint16_t io = rtl8139_io_base();
  outl(io + RTL_TCR, TCR_IFG96 | TCR_MAX_DMA);
  outb(io + RTL_CR, CR_TX_ENABLE);
  phase2_ready = true;
  klog_puts("[RTL8139] transmit path enabled\n");
  return true;
}

static bool rx_record_valid(uint16_t status, uint16_t dma_length,
                            uint16_t *frame_length) {
  if (!(status & 1) || dma_length < 8 || dma_length > NET_FRAME_MAX + 4)
    return false;
  *frame_length = (uint16_t)(dma_length - 4);
  return *frame_length >= ETH_HEADER_SIZE && *frame_length <= NET_FRAME_MAX;
}

static inline uint16_t rtl8139_capr(uint32_t offset) {
  return (uint16_t)(offset - 16u);
}

static void recover_rx_overflow(void) {
  uint16_t io = rtl8139_io_base();
  rx_offset = inw(io + RTL_CBR) % RX_RING_SIZE;
  outw(io + RTL_CAPR, rtl8139_capr(rx_offset));
}

static void drain_rx(void) {
  struct net_device *dev = rtl8139_netdev();
  if (!dev)
    return;
  spinlock_acquire(&rx_lock);
  uint16_t io = rtl8139_io_base();
  uint8_t *ring = rx_virt();
  uint32_t budget = NET_PACKET_POOL_SIZE;

  while (!(inb(io + RTL_CR) & CR_BUFFER_EMPTY) && budget--) {
    uint32_t offset = rx_offset % RX_RING_SIZE;
    uint16_t status = *(volatile uint16_t *)(ring + offset);
    uint16_t dma_length = *(volatile uint16_t *)(ring + offset + 2);
    uint16_t frame_length = 0;
    if (!rx_record_valid(status, dma_length, &frame_length)) {
      dev->stats.rx_errors++;
      recover_rx_overflow();
      spinlock_release(&rx_lock);
      return;
    }
    net_rx_submit_irq(dev, ring + offset + 4, frame_length);
    rx_offset = (offset + 4u + dma_length + 3u) & ~3u;
    rx_offset %= RX_RING_SIZE;
    outw(io + RTL_CAPR, rtl8139_capr(rx_offset));
  }
  spinlock_release(&rx_lock);
}

static void rtl8139_irq(struct registers *regs) {
  (void)regs;
  struct net_device *dev = rtl8139_netdev();
  if (!phase3_ready || !dev)
    return;
  uint16_t io = rtl8139_io_base();
  int loop_limit = 32;
  while (loop_limit-- > 0) {
    uint16_t status = inw(io + RTL_ISR);
    if (!status || status == 0xffff || !(status & INT_KNOWN))
      break;
    dev->stats.interrupts++;
    outw(io + RTL_ISR, status & INT_KNOWN);
    if (status & (INT_RX_OK | INT_RX_OVERFLOW))
      drain_rx();
    if (status & INT_TX_OK) {
      spinlock_acquire(&tx_lock);
      reclaim_tx_locked();
      spinlock_release(&tx_lock);
    }
    if (status & INT_RX_OVERFLOW)
      dev->stats.rx_overflows++;
    if (status & INT_RX_ERROR) {
      dev->stats.rx_errors++;
      spinlock_acquire(&rx_lock);
      recover_rx_overflow();
      spinlock_release(&rx_lock);
    }
    if (status & INT_TX_ERROR) {
      spinlock_acquire(&tx_lock);
      reclaim_tx_locked();
      spinlock_release(&tx_lock);
    }
    if (status & INT_LINK_CHANGE)
      dev->stats.link_changes++;
    if (status & INT_TIMEOUT)
      dev->stats.rx_errors++;
  }
}

bool rtl8139_phase3_init(void) {
  if (!phase2_ready || !rtl8139_present())
    return false;
  uint16_t io = rtl8139_io_base();
  outw(io + RTL_IMR, 0);
  if (!irq_install_handler(rtl8139_irq_line(), rtl8139_irq, 0x000f)) {
    klog_puts("[RTL8139] IRQ registration failed\n");
    return false;
  }
  rx_offset = 0;
  outl(io + RTL_RCR, RCR_ACCEPT_PHYSICAL | RCR_ACCEPT_MULTICAST |
                         RCR_ACCEPT_BROADCAST | RCR_WRAP |
                         RCR_RBLEN_32K |
                         RCR_MAX_DMA | RCR_NO_THRESHOLD);
  outw(io + RTL_CAPR, rtl8139_capr(rx_offset));
  outw(io + RTL_ISR, 0xffff);
  outb(io + RTL_CR, CR_RX_ENABLE | CR_TX_ENABLE);
  uint16_t mask = INT_RX_OK | INT_RX_ERROR | INT_TX_OK | INT_TX_ERROR |
                  INT_RX_OVERFLOW | INT_LINK_CHANGE;
  outw(io + RTL_IMR, mask);
  struct pci_device *pci = rtl8139_pci();
  uint16_t command =
      pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     command & ~(1u << 10));
  phase3_ready = true;
  klog_puts("[RTL8139] receive and interrupt paths enabled\n");
  return true;
}
