#!/usr/bin/env python3
"""Run actual PCI registration/probe code with synthetic NVIDIA devices."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/linuxkpi/src/pci.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
struct module;
struct device_driver { const char *name; void *bus, *dev_groups; };
struct device { struct device_driver *driver; };
struct pci_device_id { int dummy; };
struct pci_dev;
struct pci_driver {
 const char *name; struct device_driver driver; const struct pci_device_id *id_table;
 int (*probe)(struct pci_dev *,const struct pci_device_id *);
};
struct pci_dev { unsigned vendor, class; void *sysdata; struct pci_driver *driver; struct device dev; };
struct linuxkpi_pci_info { unsigned bus,slot,func; };
struct list_head { int dummy; };
struct kpi_pci_driver { struct pci_driver *drv; struct list_head node; };
static struct kpi_pci_driver *registered[8];
static int registrations,probes,published,snapshots;
static int pci_bus_type,kpi_pci_drivers;
static bool kpi_amdgpu=true,kpi_amdgpu_probed,kpi_nouveau,kpi_nouveau_probed;
static int kpi_amdgpu_probe_result;
static char kpi_nouveau_bdf[32];
static struct pci_dev *kpi_devs[4];
static int kpi_dev_count;
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define list_add_tail(n,h) (registered[registrations++]=(struct kpi_pci_driver *)((char *)(n)-offsetof(struct kpi_pci_driver,node)))
#define list_for_each_entry(v,h,m) for(int index=0;index<registrations && ((v)=registered[index]);index++)
#define klogf(...) ((void)0)
static void linuxkpi_pci_native_snapshot(void *handle,struct linuxkpi_pci_info *info) { snapshots++;*info=*(struct linuxkpi_pci_info *)handle; }
static void linuxkpi_pci_native_driver(void *handle,const char *name) { (void)handle;(void)name;published++; }
static const struct pci_device_id *pci_match_id(const struct pci_device_id *id,struct pci_dev *dev) { (void)dev;return id; }
static const char *pci_name(struct pci_dev *dev) { (void)dev;return "synthetic"; }
static void devres_release_all(struct device *dev) { (void)dev; }
static void pci_set_drvdata(struct pci_dev *dev,void *data) { (void)dev;(void)data; }
static void device_add_groups(struct device *dev,void *groups) { (void)dev;(void)groups; }
static int fake_probe(struct pci_dev *dev,const struct pci_device_id *id) { (void)dev;(void)id;probes++;return 0; }
'''
main = r'''
int main(void) {
 struct linuxkpi_pci_info identities[]={{3,0,0},{4,0,0},{3,0,1}};
 struct pci_dev devices[]={
  {.vendor=0x10de,.class=0x030000,.sysdata=&identities[0]},
  {.vendor=0x10de,.class=0x030200,.sysdata=&identities[1]},
  {.vendor=0x10de,.class=0x040300,.sysdata=&identities[2]},
 };
 static const struct pci_device_id ids={0};
 struct pci_driver nouveau={.name="nouveau",.id_table=&ids,.probe=fake_probe};
 for(int i=0;i<3;i++) kpi_devs[i]=&devices[i];kpi_dev_count=3;
 assert(!__pci_register_driver(&nouveau,NULL,"nouveau"));
 assert(linuxkpi_pci_driver_registered("nouveau") && !probes && !published && !snapshots && !kpi_nouveau_probed);
 kpi_nouveau=true;
 kpi_pci_probe_device(&nouveau,&devices[0]);assert(!probes); /* empty target */
 strcpy(kpi_nouveau_bdf,"0000:03:00.0junk");
 kpi_pci_probe_device(&nouveau,&devices[0]);assert(!probes);
 strcpy(kpi_nouveau_bdf,"0001:03:00.0");
 kpi_pci_probe_device(&nouveau,&devices[0]);assert(!probes);
 strcpy(kpi_nouveau_bdf,"0000:03:00.1");
 kpi_pci_probe_device(&nouveau,&devices[2]);assert(!probes); /* audio */
 strcpy(kpi_nouveau_bdf,"0000:03:00.0");
 kpi_pci_probe_device(&nouveau,&devices[1]);assert(!probes); /* other GPU */
 kpi_pci_probe_device(&nouveau,&devices[0]);assert(probes==1 && published==1 && kpi_nouveau_probed);
 assert(devices[0].driver==&nouveau && !devices[1].driver && !devices[2].driver);
 struct pci_driver amd={.name="amdgpu",.id_table=&ids,.probe=fake_probe};
 kpi_nouveau=false;
 kpi_pci_probe_device(&amd,&devices[1]);assert(probes==2); /* independent gate */
 for(int i=0;i<registrations;i++)free(registered[i]);
 puts("PASS: Nouveau registers without probing; explicit matching display BDF required; AMD remains independent");
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-gate-') as tmp:
    path = Path(tmp)/'test'
    bodies = ''.join(function(name) for name in [
        'static bool kpi_nouveau_probe_allowed(',
        'bool linuxkpi_pci_driver_registered(',
        'static void kpi_pci_probe_device(',
        'int __pci_register_driver(',
    ])
    path.with_suffix('.c').write_text(shim+bodies+main)
    subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined',
                    str(path.with_suffix('.c')),'-o',str(path)],check=True)
    subprocess.run([str(path)],check=True,timeout=10)
