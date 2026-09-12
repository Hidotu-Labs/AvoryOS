/* Phase 4 C4 — LinuxKPI PCI API + EDU lifecycle test.
 *
 * Runs on a QEMU with `-device edu` (1234:11e8).  Exercises, against the real
 * native PCI device tree:
 *
 *   1. wrapper lookup and identity (vendor/device/class/subsystem),
 *   2. BAR resource decoding (BAR0 is 1 MB MMIO) and config access,
 *   3. pci_enable_device + pci_set_master command bits,
 *   4. pci_request_region + pci_iomap, then the QEMU EDU identification
 *      register through the mapping,
 *   5. a tiny struct pci_driver: register -> probe + drvdata roundtrip ->
 *      unregister -> remove.
 *
 * On a machine without EDU (plain `make run`) the suite logs [SKIP] and must
 * not fail. */

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#define P4P_EDU_VENDOR 0x1234
#define P4P_EDU_DEVICE 0x11e8
#define P4P_EDU_BAR0_SIZE (1024 * 1024)
#define P4P_EDU_ID_REG 0x00

static int p4p_failures;

static void p4p_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: pci %s\n", what);
}

static void p4p_fail(const char *what, long v) {
  p4p_failures++;
  klogf("[FAIL] LinuxKPI: pci %s (%ld)\n", what, v);
}

/* ── test driver lifecycle ──────────────────────────────────────────────── */

static int p4p_probe_calls;
static int p4p_remove_calls;
static int p4p_marker;

static int p4p_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
  (void)id;
  p4p_probe_calls++;
  pci_set_drvdata(pdev, &p4p_marker);
  return 0;
}

static void p4p_remove(struct pci_dev *pdev) {
  (void)pdev;
  p4p_remove_calls++;
}

static const struct pci_device_id p4p_ids[] = {
    { PCI_DEVICE(P4P_EDU_VENDOR, P4P_EDU_DEVICE) },
    { } /* terminator */
};

static struct pci_driver p4p_driver = {
    .name = "kpi-pci-test",
    .id_table = p4p_ids,
    .probe = p4p_probe,
    .remove = p4p_remove,
};

static void p4p_test_driver_lifecycle(struct pci_dev *edu) {
  if (pci_register_driver(&p4p_driver) != 0) {
    p4p_fail("register test driver", 0);
    return;
  }
  if (p4p_probe_calls == 1)
    p4p_ok("test driver probe called");
  else
    p4p_fail("probe call count", p4p_probe_calls);

  if (pci_get_drvdata(edu) == &p4p_marker &&
      pci_dev_driver(edu) == &p4p_driver &&
      p4p_driver.driver.name != NULL)
    p4p_ok("driver data + binding roundtrip");
  else
    p4p_fail("driver data roundtrip", pci_get_drvdata(edu) ? 1 : 0);

  pci_unregister_driver(&p4p_driver);
  if (p4p_remove_calls == 1 && pci_dev_driver(edu) == NULL)
    p4p_ok("test driver remove called on unregister");
  else
    p4p_fail("remove call count", p4p_remove_calls);
}

/* ── main suite ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase4_pci(void) {
  struct pci_dev *edu;
  u16 vendor = 0, device = 0, command = 0;
  void __iomem *mmio;
  u32 id;
  int ret;

  p4p_failures = 0;
  p4p_probe_calls = 0;
  p4p_remove_calls = 0;
  klog_puts("[LINUXKPI] Phase 4 PCI self-test\n");

  edu = pci_get_device(P4P_EDU_VENDOR, P4P_EDU_DEVICE, NULL);
  if (!edu) {
    klog_puts("[SKIP] LinuxKPI: pci EDU test (device not present)\n");
    return;
  }

  /* Identity. */
  pci_read_config_word(edu, PCI_VENDOR_ID, &vendor);
  pci_read_config_word(edu, PCI_DEVICE_ID, &device);
  if (vendor == P4P_EDU_VENDOR && device == P4P_EDU_DEVICE &&
      edu->vendor == P4P_EDU_VENDOR && edu->device == P4P_EDU_DEVICE)
    p4p_ok("EDU wrapper identity");
  else
    p4p_fail("EDU vendor/device", ((long)vendor << 16) | device);

  klogf("[INFO] LinuxKPI: pci EDU class=0x%06x revision=0x%02x\n",
        edu->class, edu->revision);

  /* BAR0: 1 MB MMIO, decoded into struct resource. */
  if ((pci_resource_flags(edu, 0) & IORESOURCE_MEM) &&
      !(pci_resource_flags(edu, 0) & IORESOURCE_IO) &&
      pci_resource_len(edu, 0) == P4P_EDU_BAR0_SIZE &&
      pci_resource_start(edu, 0) != 0)
    p4p_ok("BAR0 is 1 MB MMIO");
  else
    klogf("[FAIL] LinuxKPI: pci BAR0 flags=0x%lx start=0x%llx len=%llu\n",
          pci_resource_flags(edu, 0),
          (unsigned long long)pci_resource_start(edu, 0),
          (unsigned long long)pci_resource_len(edu, 0));

  /* Config access + enable + bus master. */
  if (pci_read_config_word(edu, PCI_COMMAND, &command) ==
      PCIBIOS_SUCCESSFUL)
    p4p_ok("config read (command register)");
  else
    p4p_fail("config read", command);

  ret = pci_enable_device(edu);
  if (ret == 0) {
    pci_set_master(edu);
    pci_read_config_word(edu, PCI_COMMAND, &command);
    if ((command & PCI_COMMAND_MEMORY) && (command & PCI_COMMAND_MASTER))
      p4p_ok("pci_enable_device + pci_set_master");
    else
      p4p_fail("command bits after enable", command);
  } else {
    p4p_fail("pci_enable_device", ret);
  }

  /* Region + mapping + EDU identification register. */
  ret = pci_request_region(edu, 0, "kpi-pci-test");
  if (ret == 0)
    p4p_ok("pci_request_region(BAR0)");
  else
    p4p_fail("pci_request_region(BAR0)", ret);

  mmio = pci_iomap(edu, 0, 0);
  if (!mmio) {
    p4p_fail("pci_iomap(BAR0)", 0);
  } else {
    id = readl(mmio + P4P_EDU_ID_REG);
    klogf("[INFO] LinuxKPI: pci EDU BAR0 id register=0x%08x\n", id);
    if (id == 0x010000ed)
      p4p_ok("BAR0 mapping reads the EDU identification register");
    else
      p4p_fail("EDU id register", (long)id);
    pci_iounmap(edu, mmio);
  }

  pci_release_region(edu, 0);
  pci_disable_device(edu);

  p4p_test_driver_lifecycle(edu);

  if (p4p_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: pci suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: pci suite had %d failure(s)\n", p4p_failures);
}
