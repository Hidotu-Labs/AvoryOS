#!/usr/bin/env python3
"""Run actual bus construction/linking and BDF lookup with nested bridges."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/linuxkpi/src/pci.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]+'\n'

shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(head) ((head)->next=(head), (head)->prev=(head))
static void list_add_tail(struct list_head *entry, struct list_head *head) {
 entry->prev=head->prev; entry->next=head; head->prev->next=entry; head->prev=entry;
}
struct kobject { const char *name; struct kobject *parent; };
struct device { struct kobject kobj; struct device *parent; void *bus; void (*release)(struct device *); char init_name[40]; };
struct pci_dev;
struct pci_bus {
 unsigned number, primary;
 struct device dev;
 struct pci_bus *parent;
 struct pci_dev *self;
 struct device *bridge;
 struct list_head node, devices, resources, children, slots;
};
struct pci_dev {
 struct device dev;
 struct pci_bus *bus, *subordinate;
 unsigned hdr_type, devfn, primary, secondary;
 int error;
};
#define GFP_KERNEL 0
#define PCI_HEADER_TYPE_BRIDGE 1
#define PCI_PRIMARY_BUS 0x18
#define PCI_SECONDARY_BUS 0x19
static bool fail_alloc;
static void *kzalloc(size_t size, int flags) { (void)flags; return fail_alloc ? NULL : calloc(1, size); }
static void device_initialize(struct device *device) { (void)device; }
static void dev_set_name(struct device *device, const char *format, ...) {
 va_list args; va_start(args, format); vsnprintf(device->init_name,sizeof(device->init_name),format,args); va_end(args);
}
static void kpi_pci_dev_release(struct device *device) { (void)device; }
static int pci_read_config_byte(struct pci_dev *device, int offset, u8 *value) {
 *value=offset==PCI_PRIMARY_BUS ? device->primary : device->secondary; return device->error;
}
static int pci_bus_type;
static struct pci_bus kpi_root_bus;
static struct pci_bus *kpi_pci_buses[256];
static struct pci_dev **kpi_devs;
static int kpi_dev_count;
'''
main = r'''
int main(void) {
 kpi_pci_root_init();
 assert(kpi_pci_bus_get(0)==&kpi_root_bus);
 struct pci_bus *first=kpi_pci_bus_get(1), *nested=kpi_pci_bus_get(2);
 assert(first && nested && first!=nested && first->number==1 && nested->number==2);
 assert(kpi_pci_bus_get(1)==first);
 fail_alloc=true; assert(!kpi_pci_bus_get(3) && !kpi_pci_buses[3]); fail_alloc=false;
 struct pci_bus *third=kpi_pci_bus_get(3), *last=kpi_pci_bus_get(255);
 assert(last && last->number==255 && third);
 struct pci_dev root={.bus=&kpi_root_bus,.hdr_type=1,.devfn=16,.primary=0,.secondary=1};
 struct pci_dev branch={.bus=first,.hdr_type=1,.primary=1,.secondary=2};
 struct pci_dev gpu={.bus=first,.devfn=0}, audio={.bus=first,.devfn=1};
 struct pci_dev same_slot={.bus=nested,.devfn=0};
 struct pci_dev malformed={.bus=nested,.hdr_type=1,.devfn=8,.primary=2,.secondary=1};
 struct pci_dev missing={.bus=&kpi_root_bus,.hdr_type=1,.devfn=24,.primary=0,.secondary=3,.error=-1};
 struct pci_dev final_bridge={.bus=nested,.hdr_type=1,.devfn=16,.primary=2,.secondary=255};
 struct pci_dev *devices[]={&gpu,&same_slot,&audio,&branch,&root,&malformed,&missing,&final_bridge};
 kpi_devs=devices; kpi_dev_count=sizeof(devices)/sizeof(*devices);
 kpi_pci_link_buses();
 assert(first->parent==&kpi_root_bus && first->self==&root && root.subordinate==first);
 assert(nested->parent==first && nested->self==&branch && branch.subordinate==nested);
 assert(last->parent==nested && final_bridge.subordinate==last);
 assert(!third->parent && !missing.subordinate && !malformed.subordinate);
 assert(gpu.dev.parent==&root.dev && gpu.dev.kobj.parent==&root.dev.kobj);
 assert(same_slot.dev.parent==&branch.dev && branch.dev.parent==&root.dev);
 assert(first->dev.parent==&root.dev && first->bridge==&root.dev);
 assert(pci_get_domain_bus_and_slot(0,1,0)==&gpu);
 assert(pci_get_domain_bus_and_slot(0,1,1)==&audio);
 assert(pci_get_domain_bus_and_slot(0,2,0)==&same_slot);
 assert(!pci_get_domain_bus_and_slot(0,0,0) && !pci_get_domain_bus_and_slot(1,1,0));
 assert(((gpu.bus->number<<8)|gpu.devfn)==0x100);
 assert(first->children.next==&nested->node && kpi_root_bus.children.next==&first->node);
 kpi_pci_link_buses();
 assert(first->children.next==&nested->node && first->children.prev==&nested->node);
 for(unsigned index=1;index<256;index++)free(kpi_pci_buses[index]);
}
'''
constructor = function('static struct pci_dev *kpi_pci_dev_new(')
assert 'pdev->bus = kpi_pci_bus_get(info.bus);' in constructor
assert 'pdev->dev.parent = &pdev->bus->dev;' in constructor
assert 'list_add_tail(&pdev->bus_list, &pdev->bus->devices);' in constructor
scan = function('void linuxkpi_pci_scan(')
assert 'kpi_pci_link_buses();' in scan
functions = ''.join(function(signature) for signature in (
    'static void kpi_pci_root_init(', 'static struct pci_bus *kpi_pci_bus_get(',
    'static void kpi_pci_link_buses(', 'struct pci_dev *pci_get_domain_bus_and_slot('))
with tempfile.TemporaryDirectory(prefix='kpi-pci-buses-') as temporary:
    path = Path(temporary)/'test.c'
    path.write_text(shim+functions+main)
    executable = path.with_suffix('')
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                    str(path),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
print('PASS: actual PCI bus identity, nested/unordered bridge links, BDF lookup, bus 255, allocation failure and cycle rejection')
