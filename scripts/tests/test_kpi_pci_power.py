#!/usr/bin/env python3
"""Validate software state cannot pretend an unsupported transition happened."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/src/pci.c').read_text()
a=s.index('int pci_set_power_state(');b=s.index('void pci_disable_device',a)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#define ENOTSUPP 524
#define PCI_D0 0
#define PCI_D3hot 3
#define PCI_D3cold 4
typedef unsigned short u16;
typedef int pci_power_t;typedef int pm_message_t;
#define PCI_COMMAND_IO 1
#define PCI_COMMAND_MEMORY 2
#define atomic_read(p) (*(p))
#define atomic_inc(p) (++*(p))
struct pci_dev {int current_state,enable_cnt;void *sysdata;};
static int commands;
static void linuxkpi_pci_command_update(void*p,unsigned short set,unsigned short clear){(void)p;(void)set;(void)clear;commands++;}
'''+s[a:b]+s[s.index('static int kpi_pci_do_enable('):s.index('int pci_enable_device(',s.index('static int kpi_pci_do_enable('))]+r'''
int main(void){
 struct pci_dev d={PCI_D0};assert(!pci_set_power_state(&d,PCI_D0));
 for(int s=1;s<=4;s++){assert(pci_set_power_state(&d,s)==-ENOTSUPP&&d.current_state==PCI_D0);}
 assert(pci_set_power_state(&d,-1)==-EINVAL&&pci_set_power_state(NULL,0)==-EINVAL);
 d.current_state=PCI_D3hot;assert(pci_set_power_state(&d,PCI_D0)==-ENOTSUPP&&d.current_state==PCI_D3hot);
 assert(kpi_pci_do_enable(&d,true,true)==-ENOTSUPP&&!commands&&!d.enable_cnt);
 d.current_state=PCI_D0;assert(!kpi_pci_do_enable(&d,true,true)&&commands==1&&d.enable_cnt==1);
 assert(pci_wake_from_d3(&d,true)==-ENOTSUPP&&!pci_wake_from_d3(&d,false));
 assert(pci_choose_state(&d,0)==PCI_D0);
 puts("PASS: unsupported D0/D3 transitions/wake fail without changing current_state");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-pci-power-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
