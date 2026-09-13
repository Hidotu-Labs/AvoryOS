/* Phase 5 C7 — VFIO GPU validation self-test.
 *
 * Gated by the `kpi_vfio_test` module parameter (default 0, set on the kernel
 * cmdline only for the run-vfio validation boot) so a later P6a boot can bind
 * amdgpu without the test owning the device.  With the parameter set it:
 *
 *   1. locates 1002:164e (Raphael), logs name/class/revision and BAR sizes,
 *   2. enables the device, requests all regions, maps BAR5 and reads one
 *      register (no writes), sets/clears bus mastering,
 *   3. maps the VBIOS ROM, logs size + first bytes + CRC32 (the host compares
 *      it with zlib.crc32 of build/vfio/vbios.rom),
 *   4. allocates one MSI/MSI-X vector, installs a request_irq handler (the
 *      GPU does not raise without firmware), then frees it,
 *   5. releases regions and disables the device, leaving it as found.
 *
 * Without the parameter the suite logs [SKIP] and never fails; with the
 * parameter set but no device it fails (the run-vfio setup is incomplete). */

#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#define P5V_VENDOR 0x1002
#define P5V_DEVICE 0x164e
#define P5V_MMIO_BAR 5

static bool kpi_vfio_test;
module_param(kpi_vfio_test, bool, 0);

static int p5v_failures;

static void p5v_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: vfio %s\n", what);
}

static void p5v_fail(const char *what, long v) {
  p5v_failures++;
  klogf("[FAIL] LinuxKPI: vfio %s (%ld)\n", what, v);
}

static irqreturn_t p5v_irq(int irq, void *dev_id) {
  (void)irq;
  (void)dev_id;
  /* The GPU raises nothing without its firmware; the handler only proves the
   * vector can be routed to a Linux request_irq action. */
  return IRQ_HANDLED;
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_vfio(void) {
  struct pci_dev *gpu;
  void __iomem *mmio, *rom;
  size_t rom_size = 0;
  unsigned int irq = 0;
  int i, ret;

  p5v_failures = 0;

  if (!kpi_vfio_test) {
    klog_puts("[SKIP] LinuxKPI: vfio suite (kpi_vfio_test=0)\n");
    return;
  }

  klog_puts("[LINUXKPI] Phase 5 VFIO self-test\n");

  gpu = pci_get_device(P5V_VENDOR, P5V_DEVICE, NULL);
  if (!gpu) {
    p5v_fail("1002:164e not present", 0);
    return;
  }

  klogf("[INFO] LinuxKPI: vfio GPU %s class=0x%06x rev=0x%02x\n",
        pci_name(gpu), gpu->class >> 8, gpu->revision);
  for (i = 0; i < 6; i++) {
    if (gpu->resource[i].flags)
      klogf("[INFO] LinuxKPI: vfio BAR%d start=0x%llx size=0x%llx flags=0x%lx\n",
            i, (unsigned long long)gpu->resource[i].start,
            (unsigned long long)resource_size(&gpu->resource[i]),
            gpu->resource[i].flags);
  }

  ret = pci_enable_device(gpu);
  if (ret == 0)
    p5v_ok("pci_enable_device");
  else
    p5v_fail("pci_enable_device", ret);

  ret = pci_request_regions(gpu, "p5vfio");
  if (ret == 0)
    p5v_ok("pci_request_regions");
  else
    p5v_fail("pci_request_regions", ret);

  pci_set_master(gpu);
  mmio = pci_iomap(gpu, P5V_MMIO_BAR, 0);
  if (mmio) {
    u32 id = readl(mmio);

    klogf("[INFO] LinuxKPI: vfio BAR5[0x0]=0x%08x\n", id);
    p5v_ok("BAR5 mapped and read once");
  } else {
    p5v_fail("BAR5 iomap", 0);
  }
  pci_clear_master(gpu);

  rom = pci_map_rom(gpu, &rom_size);
  if (rom && rom_size) {
    u32 crc = crc32_le(~0u, (unsigned char const *)rom, rom_size) ^ ~0u;

    klogf("[INFO] LinuxKPI: vfio rom crc32=0x%08x size=%zu first=%02x %02x\n",
          crc, rom_size, readb(rom), readb((u8 __iomem *)rom + 1));
    p5v_ok("pci_map_rom VBIOS image (compare crc32 on the host)");
  } else {
    p5v_fail("pci_map_rom", (long)rom_size);
  }
  if (rom)
    pci_unmap_rom(gpu, rom);

  klogf("[INFO] LinuxKPI: vfio msi=%d msix=%d\n", pci_msi_vec_count(gpu),
        pci_msix_vec_count(gpu));
  ret = pci_alloc_irq_vectors(gpu, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
  if (ret == 1) {
    int vec = pci_irq_vector(gpu, 0);

    if (vec >= 0) {
      irq = (unsigned int)vec;
      if (request_irq(irq, p5v_irq, IRQF_SHARED, "p5vfio", gpu) == 0) {
        klogf("[INFO] LinuxKPI: vfio irq=%u installed\n", irq);
        p5v_ok("pci_alloc_irq_vectors + request_irq installed");
        free_irq(irq, gpu);
      } else {
        p5v_fail("request_irq", 0);
      }
    } else {
      p5v_fail("pci_irq_vector", vec);
    }
    pci_free_irq_vectors(gpu);
  } else {
    p5v_fail("pci_alloc_irq_vectors(1)", ret);
  }

  if (mmio)
    pci_iounmap(gpu, mmio);
  pci_release_regions(gpu);
  pci_disable_device(gpu);
  pci_dev_put(gpu);

  if (p5v_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: vfio suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: vfio suite had %d failure(s)\n", p5v_failures);
}
