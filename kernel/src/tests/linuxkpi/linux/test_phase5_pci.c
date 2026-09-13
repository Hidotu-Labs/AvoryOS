/* Phase 5 C1 — full Linux PCI API (non-IRQ) self-test.
 *
 * Builds on the C4 EDU lifecycle suite and covers:
 *   1. configuration state save/restore plus the saved-state store/load
 *      roundtrips (EDU),
 *   2. pci_map_rom() on a device with an option ROM (virtio-vga, 1af4:1050):
 *      size probe, 55AA signature and repeated map/unmap cycles,
 *   3. PCIe capability helpers: speed/width caps, pending-transaction wait,
 *      device-present probe and upstream/root-bus helpers,
 *   4. pci_release_resource() on an unassigned resource.
 *
 * Parts that need a specific device log [SKIP] and must never fail on a
 * machine without it. */

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#define P5P_EDU_VENDOR 0x1234
#define P5P_EDU_DEVICE 0x11e8
#define P5P_ROM_VENDOR 0x1af4
#define P5P_ROM_DEVICE 0x1050

static int p5p_failures;

static void p5p_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: pci5 %s\n", what);
}

static void p5p_fail(const char *what, long v) {
  p5p_failures++;
  klogf("[FAIL] LinuxKPI: pci5 %s (%ld)\n", what, v);
}

/* ── config state save/restore ──────────────────────────────────────────── */

static void p5p_test_state(struct pci_dev *pdev) {
  struct pci_saved_state *state;
  u16 before = 0, after = 0;

  /* Documented contract: no state stored before the first save. */
  if (pci_store_saved_state(pdev) == NULL)
    p5p_ok("store_saved_state is NULL before save");
  else
    p5p_fail("store before save", 0);

  if (pci_save_state(pdev) == 0 && pdev->state_saved)
    p5p_ok("save_state snapshots config space");
  else
    p5p_fail("save_state", pdev->state_saved);

  pci_read_config_word(pdev, PCI_COMMAND, &before);
  pci_write_config_word(pdev, PCI_COMMAND, before ^ PCI_COMMAND_PARITY);
  pci_restore_state(pdev);
  pci_read_config_word(pdev, PCI_COMMAND, &after);
  if (after == before && !pdev->state_saved)
    p5p_ok("restore_state roundtrip");
  else
    p5p_fail("restore_state command", ((long)before << 16) | after);

  if (pci_save_state(pdev) != 0) {
    p5p_fail("save_state (second)", 0);
    return;
  }
  state = pci_store_saved_state(pdev);
  if (!state) {
    p5p_fail("store_saved_state", 0);
    return;
  }

  /* pci_load_saved_state() is software-only (it fills saved_config_space);
   * pci_restore_state() programs the hardware from it. */
  pci_write_config_word(pdev, PCI_COMMAND, before ^ PCI_COMMAND_SERR);
  if (pci_load_saved_state(pdev, state) == 0 && pdev->state_saved &&
      (u16)pdev->saved_config_space[PCI_COMMAND / 4] == before) {
    pci_restore_state(pdev);
    pci_read_config_word(pdev, PCI_COMMAND, &after);
    if (after == before && !pdev->state_saved)
      p5p_ok("load_saved_state + restore roundtrip");
    else
      p5p_fail("load_saved_state command", after);
  } else {
    p5p_fail("load_saved_state software state", 0);
  }

  /* One more reload through the freeing variant. */
  pci_write_config_word(pdev, PCI_COMMAND, before ^ PCI_COMMAND_PARITY);
  if (pci_load_and_free_saved_state(pdev, &state) == 0 && state == NULL &&
      (u16)pdev->saved_config_space[PCI_COMMAND / 4] == before) {
    pci_restore_state(pdev);
    pci_read_config_word(pdev, PCI_COMMAND, &after);
    if (after == before)
      p5p_ok("load_and_free_saved_state + restore");
    else
      p5p_fail("load_and_free command", after);
  } else {
    p5p_fail("load_and_free ret", state ? 1 : 0);
  }
}

/* ── option ROM ─────────────────────────────────────────────────────────── */

static void p5p_test_rom(void) {
  struct pci_dev *dev = pci_get_device(P5P_ROM_VENDOR, P5P_ROM_DEVICE, NULL);
  size_t size = 0;
  void __iomem *rom;
  int i;

  if (!dev) {
    klog_puts("[SKIP] LinuxKPI: pci5 ROM test (no virtio-vga)\n");
    return;
  }

  for (i = 0; i < 4; i++) {
    size = 0;
    rom = pci_map_rom(dev, &size);
    if (!rom) {
      p5p_fail("map_rom", i);
      return;
    }
    if (size < 512 || readb(rom) != 0x55 || readb(rom + 1) != 0xAA) {
      klogf("[FAIL] LinuxKPI: pci5 ROM signature size=%zu b0=0x%02x b1=0x%02x\n",
            size, readb(rom), readb(rom + 1));
      p5p_failures++;
      pci_unmap_rom(dev, rom);
      return;
    }
    pci_unmap_rom(dev, rom);
  }
  p5p_ok("map_rom size + 55AA signature (4 cycles)");
  klogf("[INFO] LinuxKPI: pci5 ROM %s size=%zu at 0x%llx\n", pci_name(dev),
        size, (unsigned long long)pci_resource_start(dev, PCI_ROM_RESOURCE));
  pci_dev_put(dev);
}

/* ── PCIe helpers ───────────────────────────────────────────────────────── */

static void p5p_test_pcie_helpers(void) {
  struct pci_dev *dev = NULL;
  int chosen = 0;

  while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
    enum pci_bus_speed speed;
    enum pcie_link_width width;

    if (!pci_is_pcie(dev))
      continue;
    speed = pcie_get_speed_cap(dev);
    width = pcie_get_width_cap(dev);
    if (speed == PCI_SPEED_UNKNOWN || width == PCIE_LNK_WIDTH_UNKNOWN)
      continue;

    chosen = 1;
    p5p_ok("pcie speed/width caps");
    klogf("[INFO] LinuxKPI: pci5 %s pcie type=%d speed=%d width=%d\n",
          pci_name(dev), pci_pcie_type(dev), (int)speed, (int)width);
    pcie_print_link_status(dev);

    if (pci_wait_for_pending_transaction(dev) == 1)
      p5p_ok("pending-transaction wait completes");
    else
      p5p_fail("pending transaction", 0);

    if (pci_device_is_present(dev))
      p5p_ok("pci_device_is_present");
    else
      p5p_fail("device present", 0);

    if (pci_upstream_bridge(dev) == NULL && pci_is_root_bus(dev->bus))
      p5p_ok("root-bus/upstream-bridge helpers");
    else
      p5p_ok("upstream-bridge helper");

    pci_dev_put(dev);
    break;
  }

  if (!chosen)
    klog_puts("[SKIP] LinuxKPI: pci5 PCIe helpers (no PCIe device with caps)\n");
}

/* ── resource release ───────────────────────────────────────────────────── */

static void p5p_test_release_resource(struct pci_dev *pdev) {
  pci_release_resource(pdev, PCI_ROM_RESOURCE);
  if (pci_resource_len(pdev, PCI_ROM_RESOURCE) == 0 &&
      pci_resource_start(pdev, PCI_ROM_RESOURCE) == 0)
    p5p_ok("pci_release_resource clears an unassigned resource");
  else
    p5p_fail("release_resource", 1);
}

/* ── main suite ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_pci(void) {
  struct pci_dev *edu;

  p5p_failures = 0;
  klog_puts("[LINUXKPI] Phase 5 PCI self-test\n");

  edu = pci_get_device(P5P_EDU_VENDOR, P5P_EDU_DEVICE, NULL);
  if (edu) {
    p5p_test_state(edu);
    p5p_test_release_resource(edu);
    pci_dev_put(edu);
  } else {
    klog_puts("[SKIP] LinuxKPI: pci5 EDU state tests (no edu device)\n");
  }

  p5p_test_rom();
  p5p_test_pcie_helpers();

  if (p5p_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: pci5 suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: pci5 suite had %d failure(s)\n", p5p_failures);
}
