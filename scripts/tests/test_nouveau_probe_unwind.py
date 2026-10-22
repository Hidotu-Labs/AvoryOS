#!/usr/bin/env python3
"""Exercise NVKM construction ownership at the LinuxKPI boundary."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/linuxkpi/src/nouveau_probe_unwind.c').read_text()
source=re.sub(r'^#include.*$', '', source, flags=re.M)
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define AVORYOS_NOUVEAU_COMPAT 1
#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,18,42)
typedef uint64_t u64;
#define PCI_SLOT(x) ((x)>>3)
#define PCI_FUNC(x) ((x)&7)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct pci_bus { int number; };
struct pci_dev { struct pci_bus *bus; int devfn,enables; };
struct nvkm_device { int mutex; struct { struct { void *next; } intr; } intr; u64 handle; };
struct nvkm_device_pci { struct nvkm_device device; struct pci_dev *pdev; };
static struct nvkm_device *registered;
static int mode,live,deleted,irq_ctors;
static int pci_domain_nr(struct pci_bus*b){(void)b;return 0;}
static void mutex_init(int*m){*m=1;}
static struct nvkm_device *nvkm_device_find(u64 handle){return registered&&registered->handle==handle?registered:NULL;}
static void pci_disable_device(struct pci_dev*p){assert(p->enables);p->enables--;}
static void kfree(void*p){assert(p&&live);live--;free(p);}
static void nvkm_intr_ctor(struct nvkm_device*d){irq_ctors++;d->intr.intr.next=d;}
static void nvkm_device_del(struct nvkm_device **out){
 struct nvkm_device*d=*out;assert(d&&registered==d&&d->intr.intr.next);
 registered=NULL;deleted++;struct nvkm_device_pci*p=container_of(d,struct nvkm_device_pci,device);
 pci_disable_device(p->pdev);kfree(p);*out=NULL;
}
int linuxkpi_original_nvkm_device_pci_new(struct pci_dev*p,const char*c,const char*g,struct nvkm_device**out){
 (void)c;(void)g;
 if(mode==0)return -EIO; /* before ownership transfer */
 p->enables++;
 if(mode==1){pci_disable_device(p);return -ENOMEM;} /* allocator failure */
 struct nvkm_device_pci*object=calloc(1,sizeof(*object));assert(object);live++;
 object->pdev=p;*out=&object->device;
 if(mode==2)return -EEXIST; /* duplicate: new object never registered */
 object->device.handle=(u64)p->bus->number<<16|(u64)PCI_SLOT(p->devfn)<<8|PCI_FUNC(p->devfn);
 registered=&object->device;
 if(mode!=3){object->device.mutex=1;nvkm_intr_ctor(&object->device);}
 return mode==5?0:-ENOENT; /* early mapping error, missing firmware, success */
}
'''
main=r'''
int main(int argc,char**argv){
 struct pci_bus bus={.number=0};struct pci_dev pci={.bus=&bus,.devfn=24};
 struct nvkm_device old={.handle=0x300},*out=(void*)1;
 bool bypass=argc>1;(void)argv;
 for(mode=0;mode<=5;mode++){
  int before=deleted,ctors=irq_ctors;registered=mode==2?&old:NULL;
  if(bypass)out=NULL;
  int ret=bypass?linuxkpi_original_nvkm_device_pci_new(&pci,"cfg","dbg",&out):nvkm_device_pci_new(&pci,"cfg","dbg",&out);
  if(mode==5){assert(!ret&&out&&registered==out&&pci.enables==1);nvkm_device_del(&out);}
  else {
   assert(ret==(mode==0?-EIO:mode==1?-ENOMEM:mode==2?-EEXIST:-ENOENT));
   assert(!out&&!pci.enables&&!live&&"partial NVKM ownership leaked");
   assert(registered==(mode==2?&old:NULL));
   assert(deleted-before==(mode>=3));
   assert(irq_ctors-ctors==(mode>=3));
  }
  assert(!out&&!live&&!pci.enables);
 }
 puts("PASS: original errors preserved; partial/early devices reclaimed, duplicate owner retained, successful ownership unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-probe-unwind-') as tmp:
    path=Path(tmp)/'test.c';exe=Path(tmp)/'test'
    path.write_text(shim+source+main)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
    broken=subprocess.run([str(exe),'without-adapter'],capture_output=True,text=True,timeout=10)
    assert broken.returncode!=0 and 'partial NVKM ownership leaked' in broken.stderr,broken.stderr
    print('PASS: constructor failure without the ownership adapter fails the same regression')
    subprocess.run([str(exe)],check=True,timeout=10)
