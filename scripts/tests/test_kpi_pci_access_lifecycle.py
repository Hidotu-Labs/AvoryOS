#!/usr/bin/env python3
"""Run actual PCI wrappers with simulated config access and managed cleanup."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/pci.c').read_text()
baseline = os.environ.get('KPI_PCI_BASELINE')
old_source = Path(baseline).read_text() if baseline else None

def function(signature):
    selected = source
    if old_source and (os.environ.get('KPI_PCI_NEGATIVE') == 'access' or signature in ('void pci_disable_device(', 'static void kpi_pcim_disable_action(', 'int pcim_enable_device(')):
        selected = old_source
    start = selected.index(signature)
    end = selected.index('{', start) + 1
    depth = 1
    while depth:
        depth += (selected[end] == '{') - (selected[end] == '}')
        end += 1
    return selected[start:end]

shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
#define PCIBIOS_SUCCESSFUL 0
#define PCI_COMMAND_IO 1
#define PCI_COMMAND_MEMORY 2
#define PCI_COMMAND_MASTER 4
#define atomic_read(p) (*(p))
#define atomic_dec_and_test(p) (--*(p)==0)
struct device {int unused;};
struct pci_dev {struct device dev;void *sysdata;int cfg_size,enable_cnt,is_managed,is_busmaster;};
static int accesses,command_writes,enable_error,action_error;
static unsigned short command=PCI_COMMAND_IO|PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER|0x100;
static void (*cleanup)(void*);static void *cleanup_data;
static unsigned char linuxkpi_pci_config_read8(void *d,unsigned short w){(void)d;(void)w;accesses++;return 0xab;}
static unsigned short linuxkpi_pci_config_read16(void *d,unsigned short w){(void)d;(void)w;accesses++;return 0xabcd;}
static unsigned int linuxkpi_pci_config_read32(void *d,unsigned short w){(void)d;(void)w;accesses++;return 0xabcdef01;}
#define write_native(width,type) static void linuxkpi_pci_config_write##width(void*d,unsigned short w,type v){(void)d;(void)w;(void)v;accesses++;}
write_native(8,unsigned char)
write_native(16,unsigned short)
write_native(32,unsigned int)
static void linuxkpi_pci_set_bus_mastering(void*d,int enabled){(void)d;command=enabled?command|PCI_COMMAND_MASTER:command&~PCI_COMMAND_MASTER;command_writes++;}
static void linuxkpi_pci_command_update(void*d,unsigned short set,unsigned short clear){(void)d;command=(command&~clear)|set;command_writes++;}
static int pci_enable_device(struct pci_dev*d){if(enable_error)return enable_error;d->enable_cnt++;return 0;}
static int devm_add_action(struct device*d,void(*fn)(void*),void *p){(void)d;if(action_error)return action_error;cleanup=fn;cleanup_data=p;return 0;}
static int devm_add_action_or_reset(struct device*d,void(*fn)(void*),void*p){int r=devm_add_action(d,fn,p);if(r)fn(p);return r;}
'''
signatures = ('static bool kpi_pci_cfg_ok(', 'int pci_read_config_byte(',
              'int pci_read_config_word(', 'int pci_read_config_dword(',
              'int pci_write_config_byte(', 'int pci_write_config_word(',
              'int pci_write_config_dword(', 'void pci_disable_device(',
              'static void kpi_pcim_disable_action(', 'int pcim_enable_device(')
main = r'''
int main(void){
 struct pci_dev d={.sysdata=(void*)1,.cfg_size=256};u8 b;u16 w;u32 l;
 assert(!pci_read_config_byte(&d,255,&b)&&b==0xab);
 assert(!pci_read_config_word(&d,254,&w)&&w==0xabcd);
 assert(!pci_read_config_dword(&d,252,&l)&&l==0xabcdef01);
 int n=accesses;
 int invalid[]={-1,256,4096,INT_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
  assert(pci_read_config_byte(&d,invalid[i],&b)==PCIBIOS_BAD_REGISTER_NUMBER);
  assert(pci_read_config_word(&d,invalid[i],&w)==PCIBIOS_BAD_REGISTER_NUMBER);
  assert(pci_write_config_dword(&d,invalid[i],1)==PCIBIOS_BAD_REGISTER_NUMBER);
 }
 assert(pci_read_config_word(&d,3,&w)==PCIBIOS_BAD_REGISTER_NUMBER);
 assert(pci_write_config_dword(&d,2,1)==PCIBIOS_BAD_REGISTER_NUMBER);
 assert(pci_read_config_byte(NULL,0,&b)==PCIBIOS_BAD_REGISTER_NUMBER);
 assert(pci_read_config_byte(&d,0,NULL)==PCIBIOS_BAD_REGISTER_NUMBER);
 assert(accesses==n);
 d.cfg_size=4096;assert(!pci_write_config_dword(&d,4092,1));
 assert(!pci_write_config_word(&d,4094,1));assert(!pci_write_config_byte(&d,4095,1));
 assert(pci_write_config_word(&d,4095,1)==PCIBIOS_BAD_REGISTER_NUMBER);
 assert(pci_write_config_dword(&d,4093,1)==PCIBIOS_BAD_REGISTER_NUMBER);
 /* A failed enable must never publish a cleanup for resources not acquired. */
 enable_error=-EIO;assert(pcim_enable_device(&d)==-EIO&&!cleanup&&!d.enable_cnt&&!d.is_managed);
 enable_error=0;action_error=-ENOMEM;
 assert(pcim_enable_device(&d)==-ENOMEM&&!cleanup&&!d.enable_cnt&&!d.is_managed);
 /* Nouveau disables before deleting its DRM/NVKM device. MMIO must remain
  * decoded for that destructor; only bus mastering is disabled on x86. */
 assert(command==(PCI_COMMAND_IO|PCI_COMMAND_MEMORY|0x100));
 assert(command_writes==1);
 action_error=0;assert(!pcim_enable_device(&d)&&cleanup&&d.enable_cnt==1&&d.is_managed);
 assert(!pcim_enable_device(&d)&&d.enable_cnt==1);
 cleanup(cleanup_data);assert(!d.enable_cnt&&!d.is_managed&&command_writes==2);
 pci_disable_device(&d);assert(!d.enable_cnt&&command_writes==2);
 command|=PCI_COMMAND_MASTER;d.is_busmaster=1;
 d.enable_cnt=2;pci_disable_device(&d);assert(d.enable_cnt==1&&command_writes==2&&d.is_busmaster);
 assert(command&PCI_COMMAND_MASTER);
 pci_disable_device(&d);assert(!d.enable_cnt&&command_writes==3&&!d.is_busmaster);
 assert(command==(PCI_COMMAND_IO|PCI_COMMAND_MEMORY|0x100));
 /* Managed cleanup must allow a later probe to enable the function again. */
 assert(!pcim_enable_device(&d)&&d.enable_cnt==1&&d.is_managed);
 cleanup(cleanup_data);assert(!d.enable_cnt&&!d.is_managed&&command_writes==4);
 puts("PASS: config bounds/alignment/overflow, managed cleanup/nesting, bus master off with MMIO/IO preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-pci-access-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(shim+'\n'.join(function(s) for s in signatures)+main)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
