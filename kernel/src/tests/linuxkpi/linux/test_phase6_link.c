/* Phase 6 C3 — amdgpu link + probe-gate self-test.
 *
 * The amdgpu object set is linked behind KPI_AMDGPU; this suite asserts the
 * boot-safety contract of the Phase 6 bring-up:
 *
 *   - the "amdgpu" driver is registered in the LinuxKPI PCI registry;
 *   - with kpi_amdgpu=0 the passed GPU, if present, stays unbound and is
 *     never probed (the C2 revert path must keep working);
 *   - with kpi_amdgpu=1 (the default since C3) the passed GPU is routed
 *     through amdgpu_pci_probe(); C4 requires the probe to complete and bind
 *     the driver, so a gate-on probe that does not bind is a suite failure
 *     (run C4 boots from a cold GPU; see docs/amdgpu-testing.md).
 *
 * When amdgpu is not linked (KPI_AMDGPU=0) the driver is not registered, so
 * the suite logs [SKIP] and never fails.  The helpers it calls live in
 * linuxkpi/src/pci.c and linuxkpi/src/vmalloc.c next to the state they read. */

#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

extern bool linuxkpi_pci_driver_registered(const char *name);
extern bool linuxkpi_pci_amdgpu_gate(void);
extern bool linuxkpi_pci_amdgpu_probe_attempted(void);
extern int linuxkpi_pci_amdgpu_probe_result(void);
extern bool linuxkpi_ioremap_was_mapped(resource_size_t offset,
                                        unsigned long size);

#define P6L_VENDOR 0x1002
#define P6L_DEVICE 0x164e

static int p6l_failures;

static void p6l_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: link %s\n", what);
}

static void p6l_fail(const char *what, long v) {
  p6l_failures++;
  klogf("[FAIL] LinuxKPI: link %s (%ld)\n", what, v);
}

void linuxkpi_test_phase6_link(void) {
  struct pci_dev *pdev;
  bool gate;

  p6l_failures = 0;

  if (!linuxkpi_pci_driver_registered("amdgpu")) {
    klog_puts("[SKIP] LinuxKPI: link suite (amdgpu not linked)\n");
    return;
  }
  p6l_ok("amdgpu pci driver registered");

  pdev = pci_get_device(P6L_VENDOR, P6L_DEVICE, NULL);
  gate = linuxkpi_pci_amdgpu_gate();

  if (!pdev) {
    p6l_ok("no AMD GPU present (gate not exercised)");
  } else if (!gate) {
    if (linuxkpi_pci_amdgpu_probe_attempted())
      p6l_fail("gate off but amdgpu probe ran", 1);
    else if (pdev->dev.driver == NULL)
      p6l_ok("gate off leaves the passed GPU unbound and unprobed");
    else
      p6l_fail("gate off but the GPU is bound", 1);
  } else if (pdev->dev.driver && pdev->dev.driver->name &&
             !strcmp(pdev->dev.driver->name, "amdgpu")) {
    if (linuxkpi_pci_amdgpu_probe_result() == 0)
      p6l_ok("gate on: amdgpu initialized and bound");
    else
      p6l_fail("gate on: bound to amdgpu but probe result nonzero",
               linuxkpi_pci_amdgpu_probe_result());
  } else if (linuxkpi_pci_amdgpu_probe_attempted()) {
    klogf("[INFO] LinuxKPI: link amdgpu probe result %d "
          "(cold-start the GPU before a C4 boot)\n",
          linuxkpi_pci_amdgpu_probe_result());
    if (linuxkpi_ioremap_was_mapped(pci_resource_start(pdev, 5),
                                    pci_resource_len(pdev, 5)))
      p6l_fail("gate on: amdgpu init did not complete (BAR5 mapped)",
               linuxkpi_pci_amdgpu_probe_result());
    else
      p6l_fail("gate on: amdgpu probe failed before BAR5 was mapped",
               linuxkpi_pci_amdgpu_probe_result());
  } else {
    p6l_fail("gate on but no amdgpu probe was attempted", 0);
  }

  if (!p6l_failures)
    p6l_ok("suite complete");
}
