#!/usr/bin/env python3
"""Exercise actual PCI shutdown dispatch with the optional Nouveau removal."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/linuxkpi/src/pci.c').read_text()
start = source.index('void linuxkpi_pci_shutdown(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
implementation = source[start:end]
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct pci_dev;
struct pci_driver { const char *name; void (*shutdown)(struct pci_dev *); };
struct pci_dev { struct pci_driver *driver; bool allowed; int id; uint16_t vendor, device; void *sysdata; };
static bool kpi_nouveau_remove_on_shutdown;
static int kpi_dev_count = 4, removed, shutdowns, order[4];
static struct pci_dev devices[4], *kpi_devs[4];
static bool kpi_nouveau_probe_allowed(struct pci_dev *pdev) { return pdev->allowed; }
static const char *pci_name(struct pci_dev *pdev) { (void)pdev; return "fixture"; }
static void klogf(const char *format, ...) { (void)format; }
static uint32_t linuxkpi_pci_config_read32(void *data, unsigned short offset) {
  struct pci_dev *pdev = data;
  assert(offset == 0);
  return ((uint32_t)pdev->device << 16) | pdev->vendor;
}
static uint8_t linuxkpi_pci_config_read8(void *data, unsigned short offset) {
  (void)data; assert(offset == 0x3d); return 1;
}
static void pci_unregister_driver(struct pci_driver *driver) {
  assert(!strcmp(driver->name, "nouveau"));
  removed++;
  for (int i=0; i<kpi_dev_count; i++)
    if (kpi_devs[i] && kpi_devs[i]->driver==driver)
      kpi_devs[i]->driver=NULL;
}
static void shutdown_device(struct pci_dev *pdev) { order[shutdowns++]=pdev->id; }
'''
main = r'''
int main(int argc, char **argv) {
  assert(argc==2);
  int mode=atoi(argv[1]);
  struct pci_driver nouveau={.name="nouveau"};
  struct pci_driver amd={.name="amdgpu",.shutdown=shutdown_device};
  for (int i=0;i<4;i++) { devices[i].id=i; devices[i].sysdata=&devices[i]; kpi_devs[i]=&devices[i]; }
  devices[1].vendor=0x10de; devices[1].device=0x2803;
  devices[0].driver=&amd; devices[3].driver=&amd;
  devices[1].driver=mode==3 ? NULL : &nouveau; /* failed probe remains unbound */
  devices[1].allowed=mode!=2;
  kpi_nouveau_remove_on_shutdown=mode!=0;
  kpi_devs[2]=NULL;
  linuxkpi_pci_shutdown();
  assert(removed==(mode==1));
  assert(shutdowns==2 && order[0]==3 && order[1]==0);
  linuxkpi_pci_shutdown();
  assert(removed==(mode==1) && shutdowns==2); /* no second remove/shutdown */
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nouveau-shutdown-') as tmp:
    path=Path(tmp)/'test.c'
    path.write_text(shim+implementation+main)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
    for mode in range(4):
        subprocess.run([str(exe),str(mode)],check=True,timeout=10)
print('PASS: default unchanged, selected Nouveau removal, gate/failed-probe exclusion, reverse shutdown and idempotence')
