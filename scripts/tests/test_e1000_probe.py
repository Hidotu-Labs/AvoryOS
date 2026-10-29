#!/usr/bin/env python3
"""Run the actual e1000 probe against simulated PCI/MMIO failure conditions."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/drivers/net/e1000.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/pci_irq.h"
#include "drivers/net/e1000_regs.h"
#include "mm/dma_alloc.h"
#include "net/core.h"
#define PAGE_SIZE 4096
#define PAGE_FLAG_PRESENT 1
#define PAGE_FLAG_RW 2
#define PAGE_FLAG_PCD 16
#define PAGE_FLAG_PWT 8
#define PAGE_FLAG_NX (1ull<<63)
static void test_klog(const char *fmt,...) {(void)fmt;}
#define klogf test_klog
#define spinlock_acquire(l) ((void)(l))
#define spinlock_release(l) ((void)(l))
static uint32_t registers[E1000_MMIO_SIZE/4];
static struct device fixture={.name="0000:00:08.0"};
static struct pci_device pci;
static struct bus_type bus;
static uint16_t pci_command;
static unsigned allocated, maps, bound, allocation_calls, fail_alloc_at, irq_requests;
static unsigned msi_attempts, msi_vectors_allocated, msi_vectors_freed;
static unsigned active_msi_vectors, irq_mask_calls;
static uint32_t requested_irq_modes;
static uint64_t cycles;
static bool mapping_ok, reset_ok, eeprom_ok, irq_ok, rx_accept=true;
static bool msi_alloc_ok, msi_program_ok, intx_alloc_ok, msi_masked;
static bool msi_source_enabled;
static bool eeprom_zero;
static struct net_device *registered;
static unsigned received;
static void (*rx_hook)(void);
static void (*reset_hook)(void);
static uint64_t fake_ms;
static bool worker_ok=true;
static unsigned notifications;
const char *kernel_boot_cmdline="";
struct thread {char comm[32];uint64_t wakeup_ticks;int state;};
struct cpu_info;
enum {THREAD_SLEEPING=1};
static struct thread mock_thread;
static struct cpu_info *cpu_get_current(void){return NULL;}
static struct thread *sched_get_current(void){return &mock_thread;}
static struct thread *sched_create_kernel_thread(void (*entry)(void),struct cpu_info *cpu,bool enqueue) {
 (void)entry;(void)cpu;(void)enqueue;return worker_ok?&mock_thread:NULL;
}
static void sched_enqueue_thread(struct thread *t,struct cpu_info *cpu){(void)t;(void)cpu;}
static void sched_set_priority(struct thread *t,int policy,int priority){(void)t;(void)policy;(void)priority;}
static void sched_yield(void){}
static uint64_t lapic_timer_get_ms(void){return fake_ms;}
static uint64_t lapic_timer_get_ticks(void){return fake_ms;}
struct net_device *net_device_default(void){return registered;}
bool net_device_set_default(const char *name){return registered && !strcmp(name,registered->name);}
static void netlink_notify_link(struct net_device *d){assert(d==registered);notifications++;}
dma_buffer_t *dma_alloc(size_t n,uint32_t flags) {
 assert(flags==DMA_FLAG_32BIT);
 if(++allocation_calls==fail_alloc_at)return NULL;
 dma_buffer_t *b=calloc(1,sizeof(*b));assert(b);
 b->virt=aligned_alloc(4096,(n+4095)&~4095u);assert(b->virt);
 b->size=n;b->phys=(uintptr_t)b->virt;allocated++;return b;
}
void dma_free(dma_buffer_t *b) {if(b){assert(allocated);allocated--;free(b->virt);free(b);}}
bool pci_irq_request_modes(struct pci_device *p,struct pci_irq *irq,const isr_t *h,
                          uint16_t n,uint8_t dest,uint32_t modes) {
 (void)dest;assert(n==1 && h[0]);irq_requests++;requested_irq_modes=modes;
 if(!irq_ok)return false;
 if(modes&PCI_IRQ_MODE_MSI) {
  msi_attempts++;
  if(msi_alloc_ok) {
   msi_vectors_allocated++;
   if(msi_program_ok) {
    irq->dev=p;irq->mode=PCI_IRQ_MSI;irq->vector_count=1;
    irq->vectors[0]=64;irq->msi.dev=p;irq->msi.capability=0x50;
    irq->msi.enabled=true;active_msi_vectors++;msi_masked=false;
    msi_source_enabled=true;return true;
   }
   msi_vectors_freed++;msi_source_enabled=false;
  }
 }
 if((modes&PCI_IRQ_MODE_INTX) && intx_alloc_ok) {
  irq->dev=p;irq->mode=PCI_IRQ_INTX;irq->irq_line=p->irq_line;return true;
 }
 return false;
}
bool pci_irq_mask(struct pci_irq *irq,uint16_t index,bool masked) {
 assert(irq && irq->mode==PCI_IRQ_MSI && index==0 && irq->vector_count==1);
 irq_mask_calls++;msi_masked=masked;irq->msi.enabled=!masked;
 msi_source_enabled=!masked;return true;
}
int net_device_register(struct net_device *d) {assert(!registered);registered=d;d->registered=true;strcpy(d->name,"eth0");return 0;}
int net_device_unregister(struct net_device *d) {assert(registered==d);registered=NULL;d->registered=false;return 0;}
struct net_device *net_device_find(const char *name) {return registered && !strcmp(name,registered->name)?registered:NULL;}
net_rx_handler_t net_get_rx_handler(void) {return NULL;}
bool net_rx_submit_irq(struct net_device *d,const void *data,size_t n) {
 assert(data && n>=14 && n<=1514);
 if(rx_accept){received++;d->stats.rx_packets++;d->stats.rx_bytes+=n;}
 else {d->stats.rx_dropped++;d->stats.queue_full++;}
 if(rx_hook)rx_hook();
 return rx_accept;
}
static uint64_t tsc_get_freq_khz(void) {return 1000;}
uint64_t hal_cpu_cycle_count(void) {
 cycles+=1000;
 if((registers[E1000_CTRL/4]&E1000_CTRL_RST) && reset_hook)reset_hook();
 if(reset_ok && (registers[E1000_CTRL/4]&E1000_CTRL_RST))
  registers[E1000_CTRL/4]&=~E1000_CTRL_RST;
 uint32_t eerd_done=E1000_EERD_DONE;
 unsigned eerd_address_shift=E1000_EERD_ADDR_SHIFT;
 if(pci.device_id==E1000E_82574L) {
  eerd_done=E1000E_EERD_DONE;eerd_address_shift=E1000E_EERD_ADDR_SHIFT;
 }
 if(eeprom_ok && (registers[E1000_EERD/4]&E1000_EERD_START)) {
  unsigned word=(registers[E1000_EERD/4]>>eerd_address_shift)&0xff;
  const uint16_t mac[]={eeprom_zero?0:0x5452,eeprom_zero?0:0x1200,
                        eeprom_zero?0:0x5634};
  if(word<3)registers[E1000_EERD/4]|=eerd_done|((uint32_t)mac[word]<<16);
 }
 return cycles;
}
void hal_cpu_relax(void) {}
void *pci_map_mmio(uint64_t phys,uint64_t length) {
 assert(phys==0x100000 && length==E1000_MMIO_SIZE);maps++;
 return mapping_ok?registers:NULL;
}
uint32_t pci_get_device_count(void) {return 1;}
struct pci_device *asc_pci_get_device(uint32_t i) {return i==0?&pci:NULL;}
struct bus_type *asc_pci_bus_type(void) {return &bus;}
uint16_t pci_config_read16(uint8_t b,uint8_t s,uint8_t f,uint16_t o) {
 (void)b;(void)s;(void)f;assert(o==4);return pci_command;
}
void pci_config_write16(uint8_t b,uint8_t s,uint8_t f,uint16_t o,uint16_t v) {
 (void)b;(void)s;(void)f;assert(o==4);pci_command=v;
}
void dm_register_driver(struct driver *d) {(void)d;}
static void sysfs_pci_driver_registered(struct driver *d) {(void)d;}
static void sysfs_pci_device_bound(struct device *d) {(void)d;bound++;}
'''
test += source
test += r'''
static void setup(void) {
 assert(!allocated);
 memset(controllers,0,sizeof(controllers));registered=NULL;
 shutting_down=monitor_started=monitor_changing=false;
 fake_ms=100;notifications=0;worker_ok=true;kernel_boot_cmdline="";reset_hook=NULL;
 allocation_calls=fail_alloc_at=irq_requests=received=0;irq_ok=rx_accept=true;rx_hook=NULL;
 msi_attempts=msi_vectors_allocated=msi_vectors_freed=active_msi_vectors=0;
 irq_mask_calls=0;requested_irq_modes=0;msi_alloc_ok=msi_program_ok=intx_alloc_ok=true;
 msi_masked=false;msi_source_enabled=false;
 eeprom_zero=false;
 memset(registers,0,sizeof(registers));memset(&pci,0,sizeof(pci));
 fixture.driver_data=NULL;
 fixture.driver=&e1000_driver;
 pci.vendor_id=E1000_VENDOR;pci.device_id=E1000_82540EM;
 pci.class_code=2;pci.kernel_device=&fixture;
 pci.irq_line=11;
 pci.bar[0]=0x100000;pci.bar_size[0]=E1000_MMIO_SIZE;
 pci_command=7;maps=0;cycles=0;
 mapping_ok=reset_ok=true;eeprom_ok=false;
 registers[E1000_RAL0/4]=0x12005452;
 registers[E1000_RAH0/4]=E1000_RAH_AV|0x5634;
 registers[E1000_STATUS/4]=E1000_STATUS_LU;
}
static void safe_failure(void) {
 assert(probe(&fixture)==-1 && !allocated && !fixture.driver_data);
 assert(!(pci_command&4) && (pci_command&0x400));
}
static void refill_first(void) {
 if(received==RING_COUNT) {
  struct e1000_rx_desc *ring=controllers[0].rx_ring->virt;
  ring[0].length=64;ring[0].status=E1000_RX_DD|E1000_RX_EOP;
 }
}
static void test_rings(void) {
 setup();assert(probe(&fixture)==0);
 struct e1000 *dev=fixture.driver_data;
 assert(start(&dev->netdev) && allocated==4 && irq_requests==1 && dev->active);
 assert(dev->netdev.stats.dma_bytes==1056768);
 assert(start(&dev->netdev) && allocated==4 && irq_requests==1);
 assert((pci_command&6)==6 && !(pci_command&0x400));
 assert(registers[E1000_RCTL/4] & E1000_RCTL_SECRC);
 assert(registers[E1000_RCTL/4] & E1000_RCTL_MPE);
 unsigned char frame[1515];memset(frame,0xa5,sizeof(frame));
 assert(transmit(&dev->netdev,frame,13)==-1);
 assert(transmit(&dev->netdev,frame,1515)==-1);
 struct e1000_tx_desc *tx=dev->tx_ring->virt;
 for(unsigned i=0;i<RING_COUNT-1;i++)assert(!transmit(&dev->netdev,frame,14));
 assert(tx[0].length==60 && !memcmp(dev->tx_data->virt,frame,14));
 for(unsigned i=14;i<60;i++)assert(((unsigned char*)dev->tx_data->virt)[i]==0);
 assert(transmit(&dev->netdev,frame,64)==-1 && dev->netdev.stats.queue_full==1);
 for(unsigned i=0;i<100;i++)tx[i].status=E1000_TX_DD;
 reclaim_tx(dev);assert(dev->tx_pending==155 && dev->netdev.stats.tx_packets==100);
 for(unsigned i=0;i<100;i++)assert(!transmit(&dev->netdev,frame,1514));
 assert(dev->tx_next==99 && dev->tx_pending==255);
 for(unsigned i=0;i<RING_COUNT;i++)tx[i].status=E1000_TX_DD;
 reclaim_tx(dev);assert(!dev->tx_pending && dev->netdev.stats.tx_packets==355);
 assert(!transmit(&dev->netdev,frame,64));
 tx[dev->tx_clean].status=E1000_TX_DD|8;reclaim_tx(dev);
 assert(dev->netdev.stats.tx_underruns==1);

 struct e1000_rx_desc *rx=dev->rx_ring->virt;
 rx[0].length=64;rx[0].status=E1000_RX_DD|E1000_RX_EOP;
 rx[1].length=2049;rx[1].status=E1000_RX_DD|E1000_RX_EOP;
 rx[2].length=64;rx[2].status=E1000_RX_DD; /* discard through EOP */
 rx[3].length=64;rx[3].status=E1000_RX_DD|E1000_RX_EOP;
 drain_rx(dev);assert(received==1 && dev->rx_next==4 && !dev->rx_discard);
 assert(dev->netdev.stats.rx_errors==2 && registers[E1000_RDT/4]==3);
 rx_accept=false;rx[4].length=64;rx[4].status=E1000_RX_DD|E1000_RX_EOP;
 drain_rx(dev);assert(!rx[4].status && dev->rx_next==5 && received==1);
 rx_accept=true;
 /* A zero cause on a shared IRQ must not consume packets or increment hits. */
 registers[E1000_ICR/4]=0;interrupt_slot(0);assert(!dev->netdev.stats.interrupts);
 received=0;dev->rx_next=0;rx_hook=refill_first;
 for(unsigned i=0;i<RING_COUNT;i++){rx[i].length=64;rx[i].status=E1000_RX_DD|E1000_RX_EOP;}
 registers[E1000_ICR/4]=E1000_INT_RXT0;interrupt_slot(0);
 assert(received==RING_COUNT && dev->netdev.stats.interrupts==1);
 assert(registers[E1000_ICS/4]==E1000_INT_RXT0 && (rx[0].status&E1000_RX_DD));
 rx_hook=NULL;interrupt_slot(0);assert(received==RING_COUNT+1 && !rx[0].status);
 stop(&dev->netdev);assert(!allocated && !dev->active && !(pci_command&4));
 assert(!dev->netdev.stats.dma_bytes);
 assert(transmit(&dev->netdev,frame,64)==-1);
 assert(start(&dev->netdev) && allocated==4 && irq_requests==1);
 reset_ok=false;stop(&dev->netdev);
 assert(dev->quarantined && allocated==4 && !start(&dev->netdev));
 assert(dev->netdev.stats.dma_bytes==1056768);
 /* Only the simulation releases quarantined DMA during test teardown. */
 free_dma(dev);remove_device(&fixture);
 for(unsigned failure=1;failure<=4;failure++) {
  setup();assert(!probe(&fixture));dev=fixture.driver_data;
  fail_alloc_at=failure;assert(!start(&dev->netdev) && !allocated);
  assert(!(pci_command&4));remove_device(&fixture);
 }
 setup();assert(!probe(&fixture));dev=fixture.driver_data;
 irq_ok=false;assert(!start(&dev->netdev) && !allocated);
 assert(!(pci_command&4));remove_device(&fixture);
 puts("PASS: TX padding/validation/full/wrap/reclaim, RX validation/chains/pool exhaustion, shared INTx/budget continuation, DMA failures, reset quarantine");
}
static void request_shutdown(void) {shutting_down=true;}
static void test_watchdog(void) {
 setup();kernel_boot_cmdline="e1000_test_tx_stall=1";
 assert(!probe(&fixture));struct e1000 *dev=fixture.driver_data;
 assert(dev->test_stall && start(&dev->netdev));
 unsigned char frame[64]={0};assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 assert(!registers[E1000_TDT/4]);
 fake_ms+=TX_TIMEOUT_MS-1;service_device(dev);assert(!dev->netdev.stats.tx_timeouts);
 fake_ms++;uint64_t generation=dev->netdev.generation;
 dev->changing=true;service_device(dev);assert(!dev->netdev.stats.tx_timeouts);
 dev->changing=false;service_device(dev);
 assert(dev->active && !dev->test_stall && dev->netdev.stats.tx_timeouts==1);
 assert(dev->netdev.stats.recoveries==1 && !dev->netdev.stats.recovery_failures);
 assert(dev->netdev.generation>generation && dev->tx_pending==0);
 assert(allocated==4 && allocation_calls==4 && irq_requests==1);
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 ((struct e1000_tx_desc*)dev->tx_ring->virt)[dev->tx_clean].status=E1000_TX_DD;
 fake_ms+=TX_TIMEOUT_MS;service_device(dev); /* lost TX IRQ: reclaim, no reset */
 assert(!dev->tx_pending && dev->netdev.stats.tx_timeouts==1);
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 registers[E1000_STATUS/4]=0;service_device(dev);
 uint64_t changes=dev->netdev.stats.link_changes;
 assert(!dev->carrier && notifications && transmit(&dev->netdev,frame,sizeof(frame))==-1);
 fake_ms+=TX_TIMEOUT_MS*3;service_device(dev);
 assert(dev->netdev.stats.tx_timeouts==1);
 registers[E1000_STATUS/4]=E1000_STATUS_LU;service_device(dev);
 assert(dev->carrier && dev->netdev.stats.link_changes==changes+1);
 fake_ms+=TX_TIMEOUT_MS;reset_ok=false;service_device(dev);
 assert(dev->quarantined && !dev->active && allocated==4);
 assert(dev->netdev.stats.recovery_failures==1 && !(pci_command&4));
 service_device(dev);assert(dev->netdev.stats.recovery_failures==1);
 free_dma(dev);remove_device(&fixture);
 setup();assert(!probe(&fixture));dev=fixture.driver_data;worker_ok=false;
 assert(!start(&dev->netdev) && !allocated && !dev->active);remove_device(&fixture);
 setup();assert(!probe(&fixture));dev=fixture.driver_data;assert(start(&dev->netdev));
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 fake_ms+=TX_TIMEOUT_MS;reset_hook=request_shutdown;service_device(dev);
 assert(shutting_down && !dev->active && !allocated && !dev->netdev.stats.recoveries);
 assert(!(pci_command&4));remove_device(&fixture);
 setup();assert(!probe(&fixture));dev=fixture.driver_data;assert(start(&dev->netdev));
 e1000_shutdown();assert(!allocated && !dev->active && !(pci_command&4));
 assert(!start(&dev->netdev));interrupt_slot(0);e1000_shutdown();assert(!allocated);
 remove_device(&fixture);
 puts("PASS: watchdog deadline and reuse, lost IRQ reclaim, link down/up, lifecycle exclusion, failed recovery quarantine, worker failure, shutdown idempotence");
}
static void test_e1000e_probe(void) {
 const uint8_t expected[]={0x52,0x54,0,0x12,0x34,0x56};
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 assert(e1000e_driver.id_count==1 &&
        e1000e_driver.ids[0].pci.device==E1000E_82574L);
 assert(probe(&fixture)==0);
 struct e1000 *dev=fixture.driver_data;
 assert(dev && dev->profile==&profile_82574l);
 assert(!strcmp(e1000e_driver.name,"e1000e"));
 assert(!memcmp(dev->mac,expected,sizeof(expected)));
 assert(maps==1 && !(pci_command&PCI_COMMAND_MASTER));
 assert((pci_command&PCI_COMMAND_INTX_DISABLE)!=0);
 kernel_boot_cmdline="e1000e_irq=intx";
 assert(start(&dev->netdev) && dev->active && allocated==4 && irq_requests==1);
 assert(dev->irq.mode==PCI_IRQ_INTX && requested_irq_modes==PCI_IRQ_MODE_INTX);
 assert((pci_command&6)==6 && !(pci_command&PCI_COMMAND_INTX_DISABLE));
 assert(!(registers[E1000_CTRL_EXT/4]&E1000E_CTRL_EXT_VME));
 assert(!(registers[E1000_RFCTL/4]&E1000E_RFCTL_EXTEN));
 assert(!registers[E1000_MRQC/4]);
 assert((registers[E1000_TARC0/4]&(E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA))==
        (E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA));
 assert(!(registers[E1000_TARC1/4]&E1000E_TARC_ENABLE));
 assert((registers[E1000_RCTL/4]&E1000_RCTL_DTYP_MASK)==0);
 assert(!(registers[E1000_RCTL/4]&E1000_RCTL_VFE));
 assert(registers[E1000_RCTL/4]&E1000_RCTL_SECRC);
 assert(registers[E1000_RXCSUM/4]==0);
 unsigned char frame[64]={0};
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 struct e1000_tx_desc *tx=dev->tx_ring->virt;
 assert(tx[0].length==64 && tx[0].command==(E1000_TX_EOP|E1000_TX_IFCS|E1000_TX_RS));
 tx[0].status=E1000_TX_DD;reclaim_tx(dev);
 assert(!dev->tx_pending && dev->netdev.stats.tx_packets==1);
 for(unsigned i=0;i<RING_COUNT-1;i++)assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 assert(transmit(&dev->netdev,frame,sizeof(frame))==-1 && dev->netdev.stats.queue_full==1);
 for(unsigned i=0;i<RING_COUNT;i++)tx[i].status=E1000_TX_DD;
 reclaim_tx(dev);assert(!dev->tx_pending);
 struct e1000_rx_desc *rx=dev->rx_ring->virt;
 received=0;dev->rx_next=0;rx_hook=refill_first;
 for(unsigned i=0;i<RING_COUNT;i++) {
  rx[i].length=64;rx[i].status=E1000_RX_DD|E1000_RX_EOP;
 }
 registers[E1000_ICR/4]=E1000_INT_RXT0;interrupt_slot(0);
 assert(received==RING_COUNT && dev->netdev.stats.interrupts==1);
 assert(registers[E1000_ICS/4]==E1000_INT_RXT0 && (rx[0].status&E1000_RX_DD));
 rx_hook=NULL;interrupt_slot(0);
 assert(received==RING_COUNT+1 && !rx[0].status);
 stop(&dev->netdev);
 assert(!dev->active && !allocated && !dev->netdev.stats.dma_bytes);
 assert(!(pci_command&PCI_COMMAND_MASTER) && (pci_command&PCI_COMMAND_INTX_DISABLE));
 assert(!registers[E1000_RCTL/4] && !registers[E1000_TCTL/4]);
 /* A hardware reset restores defaults; every restart must reapply the
  * profile-specific queue and descriptor settings. */
 registers[E1000_RFCTL/4]=E1000E_RFCTL_EXTEN;
 registers[E1000_CTRL_EXT/4]=E1000E_CTRL_EXT_VME;
 registers[E1000_MRQC/4]=UINT32_MAX;
 registers[E1000_TARC0/4]&=~E1000E_TARC_82574_ERRATA;
 registers[E1000_TARC1/4]|=E1000E_TARC_ENABLE;
 assert(start(&dev->netdev) && allocated==4);
 assert(!(registers[E1000_RFCTL/4]&E1000E_RFCTL_EXTEN));
 assert(!(registers[E1000_CTRL_EXT/4]&E1000E_CTRL_EXT_VME));
 assert(!registers[E1000_MRQC/4]);
 assert((registers[E1000_TARC0/4]&(E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA))==
        (E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA));
 assert(!(registers[E1000_TARC1/4]&E1000E_TARC_ENABLE));
 stop(&dev->netdev);assert(!allocated);
 remove_device(&fixture);assert(!fixture.driver_data);

 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 registers[E1000_RAL0/4]=0;registers[E1000_RAH0/4]=0;eeprom_ok=true;
 assert(probe(&fixture)==0);
 dev=fixture.driver_data;
 assert(dev->profile==&profile_82574l &&
        !memcmp(dev->mac,expected,sizeof(expected)));
 remove_device(&fixture);

 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000_driver;
 assert(probe(&fixture)==-1 && !maps && !fixture.driver_data);
 setup();pci.device_id=E1000_82540EM;fixture.driver=&e1000e_driver;
 assert(probe(&fixture)==-1 && !maps && !fixture.driver_data);

 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 pci.bar_size[0]=0x1ffff;assert(probe(&fixture)==-1 && !maps);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 mapping_ok=false;safe_failure();assert(maps==1);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 reset_ok=false;safe_failure();assert(cycles>=100000);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 registers[E1000_RAL0/4]=0;registers[E1000_RAH0/4]=0;
 safe_failure(); /* EEPROM timeout */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 registers[E1000_RAL0/4]=0;registers[E1000_RAH0/4]=0;
 eeprom_ok=true;eeprom_zero=true;safe_failure(); /* invalid EEPROM MAC */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 registers[E1000_CTRL/4]=UINT32_MAX;safe_failure(); /* all-ones MMIO */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 pci.bar[0]=0xffff0000;pci.bar_size[0]=E1000_MMIO_SIZE;
 assert(probe(&fixture)==-1 && !maps);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 { struct pci_device occupied[NET_DEVICE_MAX]={0};
   for(unsigned i=0;i<NET_DEVICE_MAX;i++)controllers[i].pci=&occupied[i];
   assert(probe(&fixture)==-1 && !maps && pci_command==7);
 }
 puts("PASS: e1000e identity, profile, RAR MAC, EERD fallback, BAR size and driver separation");
}
static void test_e1000e_recovery(void) {
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000_test_tx_stall=1 e1000e_irq=intx";
 assert(!probe(&fixture));struct e1000 *dev=fixture.driver_data;
 assert(dev->test_stall && start(&dev->netdev));
 unsigned char frame[64]={0};assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 registers[E1000_STATUS/4]=0;service_device(dev);
 assert(!dev->carrier && !dev->netdev.stats.tx_timeouts && !dev->netdev.stats.recoveries);
 fake_ms+=TX_TIMEOUT_MS*3;service_device(dev);
 assert(!dev->netdev.stats.tx_timeouts && !dev->netdev.stats.recoveries);
 registers[E1000_STATUS/4]=E1000_STATUS_LU;service_device(dev);
 fake_ms+=TX_TIMEOUT_MS;service_device(dev);
 assert(dev->active && dev->netdev.stats.recoveries==1 && !dev->quarantined);
 assert(allocated==4 && dev->netdev.stats.dma_bytes==1056768);
 assert(!(registers[E1000_RFCTL/4]&E1000E_RFCTL_EXTEN));
 assert(!(registers[E1000_CTRL_EXT/4]&E1000E_CTRL_EXT_VME));
 assert(!registers[E1000_MRQC/4]);
 assert((registers[E1000_TARC0/4]&(E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA))==
        (E1000E_TARC_ENABLE|E1000E_TARC_82574_ERRATA));
 assert(!(registers[E1000_TARC1/4]&E1000E_TARC_ENABLE));
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 ((struct e1000_tx_desc*)dev->tx_ring->virt)[dev->tx_clean].status=E1000_TX_DD;
 fake_ms+=TX_TIMEOUT_MS;service_device(dev); /* reclaim despite a lost TX interrupt */
 assert(!dev->tx_pending && dev->netdev.stats.tx_timeouts==1);
 stop(&dev->netdev);assert(!allocated && !dev->netdev.stats.dma_bytes);
 remove_device(&fixture);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000_test_tx_stall=1 e1000e_irq=intx";
 assert(!probe(&fixture));dev=fixture.driver_data;assert(start(&dev->netdev));
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 reset_ok=false;fake_ms+=TX_TIMEOUT_MS;service_device(dev);
 assert(dev->quarantined && !dev->active && allocated==4);
 assert(dev->netdev.stats.recovery_failures==1 && !(pci_command&PCI_COMMAND_MASTER));
 free_dma(dev);remove_device(&fixture);
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 assert(!probe(&fixture));dev=fixture.driver_data;assert(start(&dev->netdev));
 e1000_shutdown();
 assert(!dev->active && !allocated && !(pci_command&PCI_COMMAND_MASTER));
 assert(pci_command&PCI_COMMAND_INTX_DISABLE);
 assert(!registers[E1000_RCTL/4] && !registers[E1000_TCTL/4]);
 remove_device(&fixture);
 puts("PASS: e1000e watchdog link-down suppression/recovery/lost-IRQ reclaim, reset quarantine, stop and shutdown");
}
static void test_e1000e_msi_policy(void) {
 unsigned char frame[64]={0};
 struct e1000 *dev;

 /* A failed MSI programming attempt must release its temporary vector and
  * fall back to INTx in auto mode without leaving MSI enabled. */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000e_irq=auto";msi_program_ok=false;
 assert(!probe(&fixture));dev=fixture.driver_data;
 assert(start(&dev->netdev));
 assert(requested_irq_modes==(PCI_IRQ_MODE_MSI|PCI_IRQ_MODE_INTX));
 assert(msi_attempts==1 && msi_vectors_allocated==1 &&
        msi_vectors_freed==1 && !active_msi_vectors && !msi_source_enabled);
 assert(dev->irq.mode==PCI_IRQ_INTX && !(pci_command&PCI_COMMAND_INTX_DISABLE));
 stop(&dev->netdev);remove_device(&fixture);

 /* Auto selects MSI when available. Recovery, stop and a PCI rebind retain
  * the callback/vector while keeping PCI delivery masked during inactivity. */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000_test_tx_stall=1 e1000e_irq=auto";
 assert(!probe(&fixture));dev=fixture.driver_data;
 assert(dev->test_stall && start(&dev->netdev));
 assert(requested_irq_modes==(PCI_IRQ_MODE_MSI|PCI_IRQ_MODE_INTX));
 assert(dev->irq.mode==PCI_IRQ_MSI && dev->irq.vector_count==1 &&
        pci_irq_vector(&dev->irq,0)==64 && active_msi_vectors==1);
 assert(msi_source_enabled && !msi_masked && dev->irq.msi.enabled);
 assert((pci_command&6)==6 && (pci_command&PCI_COMMAND_INTX_DISABLE));
 assert(registers[E1000_IAM/4]==0);
 assert(!transmit(&dev->netdev,frame,sizeof(frame)));
 fake_ms+=TX_TIMEOUT_MS;service_device(dev);
 assert(dev->active && dev->netdev.stats.recoveries==1 &&
        irq_requests==1 && msi_source_enabled && !msi_masked);
 stop(&dev->netdev);
 assert(!dev->active && !allocated && msi_masked && !msi_source_enabled &&
        active_msi_vectors==1 && (pci_command&PCI_COMMAND_INTX_DISABLE));
 remove_device(&fixture);
 assert(!probe(&fixture));dev=fixture.driver_data;
 assert(dev->irq.mode==PCI_IRQ_MSI && start(&dev->netdev));
 assert(irq_requests==1 && active_msi_vectors==1 && msi_source_enabled);
 stop(&dev->netdev);remove_device(&fixture);

 /* The forced INTx selector excludes MSI even when MSI is usable. */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000e_irq=intx";
 assert(!probe(&fixture));dev=fixture.driver_data;
 assert(start(&dev->netdev) && dev->irq.mode==PCI_IRQ_INTX);
 assert(requested_irq_modes==PCI_IRQ_MODE_INTX && !msi_attempts);
 stop(&dev->netdev);remove_device(&fixture);

 /* Forced MSI fails activation if no message route can be programmed. */
 setup();pci.device_id=E1000E_82574L;fixture.driver=&e1000e_driver;
 kernel_boot_cmdline="e1000e_irq=msi";msi_program_ok=false;
 assert(!probe(&fixture));dev=fixture.driver_data;
 assert(!start(&dev->netdev) && !allocated && dev->irq.mode==PCI_IRQ_NONE);
 assert(requested_irq_modes==PCI_IRQ_MODE_MSI && msi_vectors_allocated==1 &&
        msi_vectors_freed==1 && !active_msi_vectors && !msi_source_enabled);
 assert(!(pci_command&PCI_COMMAND_MASTER) &&
        (pci_command&PCI_COMMAND_INTX_DISABLE));
 remove_device(&fixture);
 puts("PASS: e1000e MSI preference, programming-failure INTx fallback, forced modes, MSI masking and retained-vector recovery/rebind");
}
int main(void) {
 setup();pci.bar[0]|=1;assert(probe(&fixture)==-1 && !maps && !allocated);
 setup();pci.bar_size[0]=4096;assert(probe(&fixture)==-1 && !maps);
 setup();pci.bar_size[0]=UINT64_MAX;assert(probe(&fixture)==-1 && !maps);
 setup();pci.linux_driver_name="other";assert(probe(&fixture)==-1 && !maps);
 setup();mapping_ok=false;safe_failure();assert(maps==1);
 setup();reset_ok=false;safe_failure();assert(cycles>=100000);
 setup();registers[E1000_RAH0/4]=0;safe_failure(); /* EEPROM timeout */
 setup();registers[E1000_RAL0/4]=0;registers[E1000_RAH0/4]=E1000_RAH_AV;
 safe_failure(); /* invalid all-zero address must not be published */
 setup();assert(probe(&fixture)==0 && !allocated);
 struct e1000 *dev=fixture.driver_data;
 const uint8_t expected[]={0x52,0x54,0,0x12,0x34,0x56};
 assert(!memcmp(dev->mac,expected,6));
 assert(!(pci_command&4) && (pci_command&0x402)==0x402);
 assert(registers[E1000_IMC/4]==UINT32_MAX);
 assert(!registers[E1000_RCTL/4] && !registers[E1000_TCTL/4]);
 remove_device(&fixture);assert(!allocated && !fixture.driver_data);
 assert(!(pci_command&4) && (pci_command&0x400));
 remove_device(&fixture); /* repeated remove is harmless */
 setup();registers[E1000_RAH0/4]=0;eeprom_ok=true;
 assert(probe(&fixture)==0 && !memcmp(((struct e1000*)fixture.driver_data)->mac,expected,6));
 remove_device(&fixture);
 puts("PASS: unsupported BAR/ownership, UC mapping, mapping failure, reset deadline, EEPROM timeout/fallback, invalid MAC, quiesce/remove");
 test_rings();
 test_watchdog();
 test_e1000e_probe();
 test_e1000e_msi_policy();
 test_e1000e_recovery();
}
'''
with tempfile.TemporaryDirectory(prefix='avory-e1000-probe-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'kernel/src'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
