#!/usr/bin/env python3
"""Imported PCI binding must be discoverable by libudev and Xorg OutputClass."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def extract(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

common = '#include <assert.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n'
probe = common + r'''
struct pci_device_id {int unused;};
struct pci_dev;
struct pci_driver {const char *name; int (*probe)(struct pci_dev *, const struct pci_device_id *); struct {void *dev_groups;} driver;};
struct pci_dev {struct pci_driver *driver; struct {void *driver;} dev; void *sysdata;};
static bool matches=true, kpi_amdgpu_probed;
static int probe_result, kpi_amdgpu_probe_result, published, groups;
static const char *published_name;
static struct pci_device_id id;
static const struct pci_device_id *pci_match_id(void *table, struct pci_dev *p){(void)table;(void)p;return matches?&id:NULL;}
static const char *pci_name(struct pci_dev *p){(void)p;return "0000:00:03.0";}
static void device_add_groups(void *d,void *g){(void)d;(void)g;groups++;}
static void linuxkpi_pci_native_driver(void *handle,const char *name){assert(handle==(void *)123);published++;published_name=name;}
#define klogf(...) ((void)printf(__VA_ARGS__))
static int do_probe(struct pci_dev *p,const struct pci_device_id *i){(void)p;(void)i;return probe_result;}
'''
# Retain the real probe function; only its collaborators are mocked.
probe = probe.replace('struct pci_driver {const char *name;', 'struct pci_driver {void *id_table; const char *name;')
probe += extract('kernel/linuxkpi/src/pci.c', 'static void kpi_pci_probe_device(')
probe += r'''
int main(void){
 struct pci_driver d={.name="amdgpu",.probe=do_probe};
 struct pci_dev p={.sysdata=(void *)123};
 matches=false;kpi_pci_probe_device(&d,&p);assert(!p.driver&&!published&&!groups);
 matches=true;probe_result=-5;kpi_pci_probe_device(&d,&p);
 assert(!p.driver&&!p.dev.driver&&!published&&!groups&&kpi_amdgpu_probe_result==-5);
 probe_result=0;kpi_pci_probe_device(&d,&p);
 assert(p.driver==&d&&p.dev.driver==&d.driver&&published==1&&groups==1);
 assert(!strcmp(published_name,"amdgpu")&&kpi_amdgpu_probed);
 kpi_pci_probe_device(&d,&p);assert(published==1);
 p.driver=NULL;p.dev.driver=NULL;d.probe=NULL;kpi_pci_probe_device(&d,&p);
 assert(published==2&&p.driver==&d);
 puts("PASS: unmatched/failed probes publish no binding; successful binds publish the actual driver");
}
'''
identity = common + r'''
struct driver {int unused;};
struct device {const char *name;struct driver *driver;};
struct pci_device {struct device *kernel_device;const char *linux_driver_name;};
typedef struct node {char name[64],target[160];struct node *children[8];int count;} vfs_node_t;
static vfs_node_t drivers={.name="drivers"},devdir={.name="0000:00:03.0"},pool[8];
static vfs_node_t *pci_drivers_dir=&drivers;static int allocated;
static vfs_node_t *vfs_finddir(vfs_node_t *p,const char *name){if(!p)return NULL;for(int i=0;i<p->count;i++)if(!strcmp(p->children[i]->name,name))return p->children[i];return NULL;}
static vfs_node_t *pci_mkdir(vfs_node_t *p,const char *name){vfs_node_t *n=vfs_finddir(p,(char *)name);if(n)return n;n=&pool[allocated++];strcpy(n->name,name);p->children[p->count++]=n;return n;}
static void pci_symlink(vfs_node_t *p,const char *name,const char *target){assert(p);vfs_node_t *n=pci_mkdir(p,name);strcpy(n->target,target);}
static void asc_sysfs_remove(void *opaque,const char *name){vfs_node_t *p=opaque;if(!p)return;for(int i=0;i<p->count;i++)if(!strcmp(p->children[i]->name,name)){p->children[i]=p->children[--p->count];return;}}
static vfs_node_t *device_dir(struct pci_device *p){(void)p;return &devdir;}
static void add_driver_links(struct pci_device *p,struct driver *d){(void)p;(void)d;assert(!"unexpected native driver restore");}
'''
identity += extract('kernel/src/fs/sysfs_pci.c', 'void sysfs_pci_linux_driver(')
identity += r'''
int main(void){
 struct device d={.name="0000:00:03.0"};struct pci_device p={.kernel_device=&d};
 sysfs_pci_linux_driver(NULL,"amdgpu");assert(!drivers.count);
 sysfs_pci_linux_driver(&p,"amdgpu");assert(!strcmp(p.linux_driver_name,"amdgpu"));
 vfs_node_t *driver=vfs_finddir(&drivers,"amdgpu");assert(driver);
 assert(!strcmp(vfs_finddir(&devdir,"driver")->target,"../../../bus/pci/drivers/amdgpu"));
 assert(!strcmp(vfs_finddir(driver,d.name)->target,"../../../../devices/pci0000:00/0000:00:03.0"));
 sysfs_pci_linux_driver(&p,NULL);assert(!p.linux_driver_name&&!vfs_finddir(&devdir,"driver")&&!vfs_finddir(driver,d.name));
 sysfs_pci_linux_driver(&p,"amdgpu");assert(vfs_finddir(&devdir,"driver")&&vfs_finddir(driver,d.name));
 puts("PASS: libudev driver link, reciprocal device link, unbind removal and rebind");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-pci-driver-') as tmp:
    for name, code in [('probe', probe), ('identity', identity)]:
        source, binary = Path(tmp) / (name + '.c'), Path(tmp) / name
        source.write_text(code)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
