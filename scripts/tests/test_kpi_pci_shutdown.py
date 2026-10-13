#!/usr/bin/env python3
"""Exercise the native PCI shutdown dispatch and reboot integration."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linuxkpi/src/pci.c').read_text()
start = source.index('void linuxkpi_pci_shutdown(void) {')
end = source.index('\nvoid pci_unregister_driver', start)
code = r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
struct pci_dev;
struct pci_driver { const char *name; void (*shutdown)(struct pci_dev *); };
struct pci_dev { struct pci_driver *driver; int id; };
static struct pci_dev **kpi_devs;
static int kpi_dev_count, calls[3], count;
static const char *pci_name(struct pci_dev *d) { (void)d; return "test"; }
static void klogf(const char *f, ...) { (void)f; }
static void shutdown_cb(struct pci_dev *d) {
  calls[count++] = d->id;
  /* A second caller must not repeat shutdown during a callback. */
  extern void linuxkpi_pci_shutdown(void);
  linuxkpi_pci_shutdown();
}
'''+source[start:end]+r'''
int main(void) {
  struct pci_driver bound = { "bound", shutdown_cb }, no_cb = { "no_cb", NULL };
  struct pci_dev a = { &bound, 1 }, b = { NULL, 2 }, c = { &no_cb, 3 }, d = { &bound, 4 };
  struct pci_dev *devices[] = { &a, &b, NULL, &c, &d };
  kpi_devs = devices; kpi_dev_count = 5;
  linuxkpi_pci_shutdown(); linuxkpi_pci_shutdown();
  assert(count == 2 && calls[0] == 4 && calls[1] == 1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/'shutdown.c'; exe = Path(tmp)/'shutdown'
    src.write_text(code)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
reboot = (root/'kernel/src/syscalls/sys_process.c').read_text()
for marker in ['System reboot requested', 'System power-off requested', 'System halt requested']:
    branch = reboot.split(marker,1)[1].split('break;',1)[0]
    assert branch.index('linuxkpi_pci_shutdown();') < branch.index('nvme_shutdown();')
    assert branch.index('linuxkpi_pci_shutdown();') < branch.index('acpi_' if 'halt' not in marker else 'hal_irq_disable();')
print('PASS: bound devices shut down once, in reverse order, before native power/reset')
