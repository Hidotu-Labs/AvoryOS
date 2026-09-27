#include "drivers/net/rtl8139.h"
#include "hal/hal.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/manager/device.h"
#include "fs/sysfs_pci.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "net/core.h"
#include <stddef.h>
#include <stdint.h>

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139
#define RTL_IDR0 0x00
#define RTL_TSAD0 0x20
#define RTL_RBSTART 0x30
#define RTL_CR 0x37
#define RTL_IMR 0x3C
#define RTL_ISR 0x3E
#define RTL_CONFIG1 0x52
#define RTL_MSR 0x58
#define CR_RESET 0x10
#define CR_RX 0x08
#define CR_TX 0x04
#define MSR_LINK_DOWN 0x04
#define RX_PAGES 10
#define TX_PAGES 2
#define TX_COUNT 4
#define TX_BUFFER_SIZE 2048

struct rtl8139 {
  struct net_device netdev;
  struct pci_device *pci;
  uint16_t io;
  uint8_t irq;
  void *rx_phys, *tx_phys;
  bool initialized;
};
static struct rtl8139 rtl;

static int rtl8139_dm_probe(struct device *dev) {
  return rtl.initialized && rtl.pci && rtl.pci->kernel_device == dev ? 0 : -1;
}

static void rtl8139_dm_remove(struct device *dev) {
  (void)dev;
}

static struct device_id rtl8139_dm_ids[] = {
    {.type = ID_PCI,
     .pci = {.vendor = RTL_VENDOR,
             .device = RTL_DEVICE,
             .match_class = false}},
};

static struct driver rtl8139_dm_driver = {
    .name = "rtl8139",
    .ids = rtl8139_dm_ids,
    .id_count = 1,
    .probe = rtl8139_dm_probe,
    .remove = rtl8139_dm_remove,
    .kind = DRIVER_KERNEL,
};

static bool link_up(struct net_device *netdev) {
  struct rtl8139 *dev = netdev->driver_private;
  return dev && dev->initialized && !(inb(dev->io + RTL_MSR) & MSR_LINK_DOWN);
}

static void stop(struct net_device *netdev) {
  struct rtl8139 *dev = netdev ? netdev->driver_private : NULL;
  if (!dev || !dev->pci)
    return;
  outw(dev->io + RTL_IMR, 0);
  outb(dev->io + RTL_CR, 0);
  uint16_t command = pci_config_read16(dev->pci->bus, dev->pci->slot,
                                       dev->pci->func, 0x04);
  pci_config_write16(dev->pci->bus, dev->pci->slot, dev->pci->func, 0x04,
                     command | (1u << 10));
  dev->initialized = false;
}
static const struct net_device_ops ops = {
    .transmit = rtl8139_transmit, .link_up = link_up, .stop = stop};
static void disable_hardware(void) {
  if (!rtl.pci)
    return;
  outw(rtl.io + RTL_IMR, 0);
  outb(rtl.io + RTL_CR, 0);
  uint16_t command = pci_config_read16(rtl.pci->bus, rtl.pci->slot,
                                       rtl.pci->func, 0x04);
  pci_config_write16(rtl.pci->bus, rtl.pci->slot, rtl.pci->func, 0x04,
                     command | (1u << 10));
  rtl.initialized = false;
}
static void release_dma(void) {
  if (rtl.rx_phys)
    pmm_free_pages(rtl.rx_phys, RX_PAGES);
  if (rtl.tx_phys)
    pmm_free_pages(rtl.tx_phys, TX_PAGES);
  rtl.rx_phys = rtl.tx_phys = NULL;
}
static bool valid_mac(const uint8_t mac[6]) {
  bool zero = true, ff = true;
  for (int i = 0; i < 6; i++) {
    zero &= mac[i] == 0;
    ff &= mac[i] == 0xff;
  }
  return !zero && !ff && !(mac[0] & 1);
}

bool rtl8139_phase1_init(void) {
  memset(&rtl, 0, sizeof(rtl));
  rtl.pci = pci_find_device_by_id(RTL_VENDOR, RTL_DEVICE);
  if (!rtl.pci) {
    klog_puts("[RTL8139] no device found\n");
    return false;
  }
  uint32_t bar0 = rtl.pci->bar[0];
  if (!(bar0 & 1) || (bar0 & ~3u) > 0xffffu) {
    klog_puts("[RTL8139] initialization failed: invalid BAR0\n");
    return false;
  }
  rtl.io = (uint16_t)(bar0 & ~3u);
  rtl.irq = rtl.pci->irq_line;
  uint16_t old_command = pci_config_read16(
      rtl.pci->bus, rtl.pci->slot, rtl.pci->func, 0x04);
  pci_config_write16(rtl.pci->bus, rtl.pci->slot, rtl.pci->func, 0x04,
                     old_command | 0x5u | (1u << 10));
  outw(rtl.io + RTL_IMR, 0);
  outw(rtl.io + RTL_ISR, 0xffff);
  outb(rtl.io + RTL_CONFIG1, 0);
  outb(rtl.io + RTL_CR, CR_RESET);
  uint32_t timeout = 1000000;
  while ((inb(rtl.io + RTL_CR) & CR_RESET) && --timeout)
    hal_cpu_relax();
  if (!timeout) {
    klog_puts("[RTL8139] initialization failed: reset timeout\n");
    pci_config_write16(rtl.pci->bus, rtl.pci->slot, rtl.pci->func, 0x04,
                       old_command | (1u << 10));
    return false;
  }
  rtl.netdev.stats.resets++;
  for (int i = 0; i < 6; i++)
    rtl.netdev.mac[i] = inb(rtl.io + RTL_IDR0 + i);
  if (!valid_mac(rtl.netdev.mac)) {
    klog_puts("[RTL8139] initialization failed: invalid MAC\n");
    disable_hardware();
    return false;
  }
  rtl.rx_phys = pmm_alloc_pages_constrained(RX_PAGES, UINT32_MAX);
  rtl.tx_phys = pmm_alloc_pages_constrained(TX_PAGES, UINT32_MAX);
  if (!rtl.rx_phys || !rtl.tx_phys) {
    klog_puts("[RTL8139] initialization failed: low DMA allocation\n");
    release_dma();
    disable_hardware();
    return false;
  }
  memset((void *)((uint64_t)rtl.rx_phys + pmm_get_hhdm_offset()), 0,
         RX_PAGES * PAGE_SIZE);
  memset((void *)((uint64_t)rtl.tx_phys + pmm_get_hhdm_offset()), 0,
         TX_PAGES * PAGE_SIZE);
  outl(rtl.io + RTL_RBSTART, (uint32_t)(uint64_t)rtl.rx_phys);
  for (uint32_t i = 0; i < TX_COUNT; i++)
    outl(rtl.io + RTL_TSAD0 + i * 4,
         (uint32_t)(uint64_t)rtl.tx_phys + i * TX_BUFFER_SIZE);
  strcpy(rtl.netdev.name, "eth0");
  rtl.netdev.mtu = NET_MTU_ETHERNET;
  rtl.netdev.ops = &ops;
  rtl.netdev.driver_private = &rtl;
  rtl.initialized = true;
  if (net_device_register(&rtl.netdev)) {
    klog_puts("[RTL8139] initialization failed: registration\n");
    stop(&rtl.netdev);
    release_dma();
    return false;
  }
  rtl8139_dm_driver.bus = pci_bus_type();
  dm_register_driver(&rtl8139_dm_driver);
  sysfs_pci_driver_registered(&rtl8139_dm_driver);
  if (!rtl.pci->kernel_device ||
      rtl.pci->kernel_device->driver != &rtl8139_dm_driver) {
    klog_puts("[RTL8139] initialization failed: device-model ownership\n");
    stop(&rtl.netdev);
    release_dma();
    return false;
  }
  sysfs_pci_device_bound(rtl.pci->kernel_device);
  klog_puts("[RTL8139] eth0 registered; RX/TX/INTx remain disabled\n");
  return true;
}

struct net_device *rtl8139_netdev(void) { return rtl.initialized ? &rtl.netdev : NULL; }
struct pci_device *rtl8139_pci(void) { return rtl.pci; }
uint16_t rtl8139_io_base(void) { return rtl.io; }
uint8_t rtl8139_irq_line(void) { return rtl.irq; }
void *rtl8139_rx_phys(void) { return rtl.rx_phys; }
void *rtl8139_tx_phys(void) { return rtl.tx_phys; }
bool rtl8139_present(void) { return rtl.initialized; }
