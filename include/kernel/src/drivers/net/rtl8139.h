#ifndef DRIVERS_NET_RTL8139_H
#define DRIVERS_NET_RTL8139_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct net_device;
struct pci_device;
bool rtl8139_phase1_init(void);
bool rtl8139_phase2_init(void);
bool rtl8139_phase3_init(void);
int rtl8139_transmit(struct net_device *, const void *, size_t);
struct net_device *rtl8139_netdev(void);
struct pci_device *rtl8139_pci(void);
uint16_t rtl8139_io_base(void);
uint8_t rtl8139_irq_line(void);
void *rtl8139_rx_phys(void);
void *rtl8139_tx_phys(void);
bool rtl8139_present(void);
#endif
