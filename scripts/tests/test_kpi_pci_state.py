#!/usr/bin/env python3
"""Exercise real PCI snapshot/restore against W1C and ordering simulation."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/pci.c').read_text()
a=s.index('struct pci_saved_state {');b=s.index('int pci_load_and_free_saved_state',a)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "pci_regs.h"
typedef uint32_t u32;typedef uint16_t u16;
#define ENOTSUPP 524
#define GFP_KERNEL 0
#define kzalloc(n,g) calloc(1,n)
struct hlist_node {struct hlist_node *next;};
struct hlist_head {struct hlist_node *first;};
#define hlist_empty(h) (!(h)->first)
#define hlist_entry(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static void hlist_add_head(struct hlist_node *n,struct hlist_head *h){n->next=h->first;h->first=n;}
struct pci_dev {u32 saved_config_space[16];bool state_saved;int hdr_type,pcie_cap,msi_cap,msix_cap;struct hlist_head saved_cap_space;};
static uint8_t config[4096],before[4096];static int writes,fail_read=-1;
static bool decoding;
static int read_config(int offset,void *p,int size){if(offset==fail_read)return 1;memcpy(p,config+offset,size);return 0;}
static int pci_read_config_word(struct pci_dev*d,int o,u16*p){(void)d;return read_config(o,p,2);}
static int pci_read_config_dword(struct pci_dev*d,int o,u32*p){(void)d;return read_config(o,p,4);}
static void write_config(int o,uint32_t v,int size){
 writes++;
 if(o>=0x10&&o<=0x27)assert(!decoding);
 /* W1C neighbors must never be touched. */
 assert(o!=0 && o!=8 && o!=0x2c && o!=0x34 && o!=0x48+2 && o!=0x68+2);
 assert(!(o<=6&&o+size>6));
 memcpy(config+o,&v,size);
 if(o==4)decoding=v&(PCI_COMMAND_MEMORY|PCI_COMMAND_IO|PCI_COMMAND_MASTER);
}
static int pci_write_config_word(struct pci_dev*d,int o,u16 v){(void)d;write_config(o,v,2);return 0;}
static int pci_write_config_dword(struct pci_dev*d,int o,u32 v){(void)d;write_config(o,v,4);return 0;}
static int pci_write_config_byte(struct pci_dev*d,int o,uint8_t v){(void)d;write_config(o,v,1);return 0;}
static void klog_puts(const char*s){(void)s;}
'''+s[a:b]+r'''
int main(void){
 struct pci_dev dev={.hdr_type=PCI_HEADER_TYPE_NORMAL,.pcie_cap=0x40,.msi_cap=0x80,.msix_cap=0xa0};
 for(int wide=0;wide<2;wide++){
  for(int i=0;i<4096;i++)config[i]=i*17;
  u16 flags=2;memcpy(config+0x42,&flags,2);
  flags=PCI_MSI_FLAGS_ENABLE|PCI_MSI_FLAGS_MASKBIT|(wide?PCI_MSI_FLAGS_64BIT:0);memcpy(config+0x82,&flags,2);
  flags=0;memcpy(config+0xa2,&flags,2);
  flags=7;memcpy(config+4,&flags,2);decoding=true;
  flags=PCI_EXP_DEVCTL_BCR_FLR|0x123;memcpy(config+0x48,&flags,2);
  assert(!pci_save_state(&dev));struct pci_saved_state *stored=pci_store_saved_state(&dev);assert(stored);
  memcpy(before,config,sizeof(config));
  memset(config+0x10,0,24);memset(config+0x84,0,wide?12:8);
  assert(!pci_load_saved_state(&dev,stored));writes=0;pci_restore_state(&dev);
  assert(!dev.state_saved&&decoding&&writes>0);
  assert(!memcmp(config,before,4)&&!memcmp(config+6,before+6,6));
  assert(!memcmp(config+0x10,before+0x10,24));
  assert(!memcmp(config+0x4a,before+0x4a,2));
  assert(!memcmp(config+0x84,before+0x84,wide?10:6));
  assert(config[wide?0x8e:0x8a]==0 && config[wide?0x8f:0x8b]==0);
  memcpy(&flags,config+0x48,2);assert(!(flags&PCI_EXP_DEVCTL_BCR_FLR));
  free(stored);
 }
 fail_read=8;assert(pci_save_state(&dev)==-EIO&&!dev.state_saved);fail_read=-1;
 u16 enabled=PCI_MSIX_FLAGS_ENABLE;memcpy(config+0xa2,&enabled,2);
 assert(pci_save_state(&dev)==-ENOTSUPP&&!dev.state_saved);
 dev.hdr_type=PCI_HEADER_TYPE_BRIDGE;assert(pci_save_state(&dev)==-ENOTSUPP);
 free(hlist_entry(dev.saved_cap_space.first,struct kpi_pci_saved_caps,node));
 puts("PASS: W1C/readonly protection, BAR-before-command, 32/64-bit MSI snapshot round-trip, FLR exclusion, read failure, MSI-X/bridge rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-pci-state-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 (p/'pci_regs.h').write_text((root/'kernel/linux/include/uapi/linux/pci_regs.h').read_text())
 subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
