/* Phase 5 C2 — Linux IRQ core + MSI/MSI-X bridge self-test.
 *
 * Uses QEMU's EDU (1234:11e8) as the deterministic interrupt source:
 * BAR0 +0x24 is the IRQ status register, +0x60 raises an interrupt and +0x64
 * acknowledges it.  The suite covers:
 *
 *   1. MSI allocation + request_irq + exact interrupt counts,
 *   2. disable_irq()/enable_irq() gating,
 *   3. the legacy INTx fallback (PCI_IRQ_LEGACY),
 *   4. request_threaded_irq() running the thread function outside IRQ context,
 *   5. shared handlers on one vector,
 *   6. devm_request_irq() release on devres teardown,
 *   7. a 10k-interrupt stress loop with a PMM baseline.
 *
 * Without EDU the suite logs [SKIP] and never fails. */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/string.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_mm.h>

#define P5I_EDU_VENDOR 0x1234
#define P5I_EDU_DEVICE 0x11e8
#define P5I_EDU_IRQ_STATUS 0x24
#define P5I_EDU_IRQ_RAISE 0x60
#define P5I_EDU_IRQ_ACK 0x64
#define P5I_IRQ_VALUE 0x1

static int p5i_failures;

static void p5i_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: irq %s\n", what);
}

static void p5i_fail(const char *what, long v) {
  p5i_failures++;
  klogf("[FAIL] LinuxKPI: irq %s (%ld)\n", what, v);
}

struct p5i_ctx {
  void __iomem *mmio;
  volatile int count;
  volatile int thread_count;
  volatile int thread_in_irq;
  volatile int hard_in_irq;
};

static struct p5i_ctx p5i_ctx_a;
static struct p5i_ctx p5i_ctx_b;

static irqreturn_t p5i_handler(int irq, void *dev_id) {
  struct p5i_ctx *c = dev_id;

  (void)irq;
  if (!in_interrupt())
    c->hard_in_irq = 1;
  c->count++;
  writel(P5I_IRQ_VALUE, c->mmio + P5I_EDU_IRQ_ACK);
  return IRQ_HANDLED;
}

static irqreturn_t p5i_thread_hard(int irq, void *dev_id) {
  struct p5i_ctx *c = dev_id;

  (void)irq;
  writel(P5I_IRQ_VALUE, c->mmio + P5I_EDU_IRQ_ACK);
  return IRQ_WAKE_THREAD;
}

static irqreturn_t p5i_thread_fn(int irq, void *dev_id) {
  struct p5i_ctx *c = dev_id;

  (void)irq;
  if (in_interrupt())
    c->thread_in_irq = 1;
  c->thread_count++;
  return IRQ_HANDLED;
}

static void p5i_raise(void) { writel(P5I_IRQ_VALUE, p5i_ctx_a.mmio + P5I_EDU_IRQ_RAISE); }

static bool p5i_wait(volatile int *counter, int target, int timeout_ms) {
  for (int i = 0; i < timeout_ms; i++) {
    if (*counter >= target)
      return true;
    msleep(1);
  }
  return false;
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_irq(void) {
  struct pci_dev *edu;
  int irq = -1, ret;
  int msi_count, msix_count;
  unsigned long pmm_before;

  p5i_failures = 0;
  memset(&p5i_ctx_a, 0, sizeof(p5i_ctx_a));
  memset(&p5i_ctx_b, 0, sizeof(p5i_ctx_b));
  klog_puts("[LINUXKPI] Phase 5 IRQ self-test\n");

  edu = pci_get_device(P5I_EDU_VENDOR, P5I_EDU_DEVICE, NULL);
  if (!edu) {
    klog_puts("[SKIP] LinuxKPI: irq suite (no edu device)\n");
    return;
  }

  if (pci_enable_device(edu) != 0 || pci_request_region(edu, 0, "p5irq") != 0) {
    p5i_fail("edu enable/region", 0);
    goto out_put;
  }
  pci_set_master(edu);
  p5i_ctx_a.mmio = pci_iomap(edu, 0, 0);
  p5i_ctx_b.mmio = p5i_ctx_a.mmio;
  if (!p5i_ctx_a.mmio) {
    p5i_fail("edu iomap", 0);
    goto out_region;
  }

  msix_count = pci_msix_vec_count(edu);
  msi_count = pci_msi_vec_count(edu);
  klogf("[INFO] LinuxKPI: irq edu msi=%d msix=%d\n", msi_count, msix_count);
  if (msi_count < 1)
    p5i_fail("pci_msi_vec_count", msi_count);
  else
    p5i_ok("pci_msi_vec_count >= 1");

  /* 1 + 2: MSI (EDU has MSI but no MSI-X) and disable/enable gating. */
  ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
  if (ret != 1) {
    p5i_fail("pci_alloc_irq_vectors(MSI)", ret);
    goto out_unmap;
  }
  irq = pci_irq_vector(edu, 0);
  if (irq < 0) {
    p5i_fail("pci_irq_vector", irq);
    pci_free_irq_vectors(edu);
    goto out_unmap;
  }
  p5i_ok("pci_alloc_irq_vectors + pci_irq_vector");
  klogf("[INFO] LinuxKPI: irq MSI vector=%d cap=%d\n", irq, edu->msi_cap);

  if (request_irq((unsigned int)irq, p5i_handler, IRQF_SHARED, "p5irq-a",
                  &p5i_ctx_a) != 0) {
    p5i_fail("request_irq(MSI)", 0);
    pci_free_irq_vectors(edu);
    goto out_unmap;
  }
  {
    u16 mc = 0, data = 0;
    u32 addr = 0, hi = 0;
    int cap = edu->msi_cap;

    pci_read_config_word(edu, cap + 2, &mc);
    pci_read_config_dword(edu, cap + 4, &addr);
    pci_read_config_dword(edu, cap + 8, &hi);
    pci_read_config_word(edu, cap + 12, &data);
    klogf("[INFO] LinuxKPI: irq MSI mc=0x%04x addr=0x%08x hi=0x%08x data=0x%04x\n",
          mc, addr, hi, data);
  }

  p5i_raise();
  klogf("[INFO] LinuxKPI: irq status after raise=0x%x\n",
        readl(p5i_ctx_a.mmio + P5I_EDU_IRQ_STATUS));
  if (p5i_wait(&p5i_ctx_a.count, 1, 100) && p5i_ctx_a.hard_in_irq == 0)
    p5i_ok("MSI interrupt delivered to the handler");
  else
    p5i_fail("MSI delivery", p5i_ctx_a.count);

  disable_irq((unsigned int)irq);
  p5i_raise();
  msleep(20);
  if (p5i_ctx_a.count == 1)
    p5i_ok("disable_irq gates interrupts");
  else
    p5i_fail("disable_irq", p5i_ctx_a.count);
  enable_irq((unsigned int)irq);
  p5i_raise();
  if (p5i_wait(&p5i_ctx_a.count, 2, 100))
    p5i_ok("enable_irq resumes delivery");
  else
    p5i_fail("enable_irq", p5i_ctx_a.count);

  /* Exact-count loop: raise one at a time and wait for each. */
  {
    int i, target = p5i_ctx_a.count;

    for (i = 0; i < 64; i++) {
      p5i_raise();
      if (!p5i_wait(&p5i_ctx_a.count, target + i + 1, 50)) {
        p5i_fail("interrupt loop", p5i_ctx_a.count);
        break;
      }
    }
    if (i == 64)
      p5i_ok("64 raise/ack cycles exact");
  }

  free_irq((unsigned int)irq, &p5i_ctx_a);
  pci_free_irq_vectors(edu);

  /* 3: legacy INTx fallback. */
  if (edu->irq < 16) {
    ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_LEGACY);
    if (ret == 1 && (irq = pci_irq_vector(edu, 0)) >= 0 &&
        request_irq((unsigned int)irq, p5i_handler, IRQF_SHARED, "p5irq-intx",
                    &p5i_ctx_a) == 0) {
      int before = p5i_ctx_a.count;

      p5i_raise();
      if (p5i_wait(&p5i_ctx_a.count, before + 1, 100))
        p5i_ok("legacy INTx fallback delivers");
      else
        p5i_fail("INTx delivery", p5i_ctx_a.count);
      free_irq((unsigned int)irq, &p5i_ctx_a);
      pci_free_irq_vectors(edu);
    } else {
      p5i_fail("INTx allocation", ret);
      pci_free_irq_vectors(edu);
    }
  } else {
    klogf("[SKIP] LinuxKPI: irq INTx fallback (irq line %u)\n", edu->irq);
  }

  /* 4: threaded handler. */
  ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
  if (ret == 1 && (irq = pci_irq_vector(edu, 0)) >= 0 &&
      request_threaded_irq((unsigned int)irq, p5i_thread_hard, p5i_thread_fn,
                           IRQF_SHARED, "p5irq-thread", &p5i_ctx_a) == 0) {
    p5i_raise();
    if (p5i_wait(&p5i_ctx_a.thread_count, 1, 200) &&
        p5i_ctx_a.thread_in_irq == 0)
      p5i_ok("request_threaded_irq runs the thread function");
    else
      p5i_fail("threaded handler", p5i_ctx_a.thread_count);
    free_irq((unsigned int)irq, &p5i_ctx_a);
    pci_free_irq_vectors(edu);
  } else {
    p5i_fail("threaded allocation", ret);
    pci_free_irq_vectors(edu);
  }

  /* 5: shared handlers. */
  ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
  if (ret == 1 && (irq = pci_irq_vector(edu, 0)) >= 0 &&
      request_irq((unsigned int)irq, p5i_handler, IRQF_SHARED, "p5irq-sh1",
                  &p5i_ctx_a) == 0 &&
      request_irq((unsigned int)irq, p5i_handler, IRQF_SHARED, "p5irq-sh2",
                  &p5i_ctx_b) == 0) {
    p5i_raise();
    if (p5i_wait(&p5i_ctx_a.count, 1, 100) &&
        p5i_wait(&p5i_ctx_b.count, 1, 100))
      p5i_ok("shared handlers both run");
    else
      p5i_fail("shared handlers", p5i_ctx_b.count);
    free_irq((unsigned int)irq, &p5i_ctx_a);
    p5i_raise();
    if (p5i_wait(&p5i_ctx_b.count, 2, 100))
      p5i_ok("freeing one shared handler leaves the other");
    else
      p5i_fail("shared handler removal", p5i_ctx_b.count);
    free_irq((unsigned int)irq, &p5i_ctx_b);
    pci_free_irq_vectors(edu);
  } else {
    p5i_fail("shared allocation", ret);
    pci_free_irq_vectors(edu);
  }

  /* 6: devm_request_irq released by devres teardown. */
  ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
  if (ret == 1 && (irq = pci_irq_vector(edu, 0)) >= 0) {
    struct device *fake = kzalloc(sizeof(*fake), GFP_KERNEL);

    if (fake) {
      int before;

      device_initialize(fake);
      dev_set_name(fake, "p5irq-fake");
      if (devm_request_irq(fake, (unsigned int)irq, p5i_handler, 0, "p5devm",
                           &p5i_ctx_a) == 0) {
        before = p5i_ctx_a.count;
        p5i_raise();
        if (p5i_wait(&p5i_ctx_a.count, before + 1, 100))
          p5i_ok("devm_request_irq delivers");
        else
          p5i_fail("devm delivery", p5i_ctx_a.count);

        devres_release_all(fake);
        before = p5i_ctx_a.count;
        p5i_raise();
        msleep(20);
        if (p5i_ctx_a.count == before)
          p5i_ok("devres teardown frees the IRQ");
        else
          p5i_fail("devm release", p5i_ctx_a.count);
      } else {
        p5i_fail("devm_request_irq", 0);
      }
      kfree(fake);
    } else {
      p5i_fail("fake device alloc", 0);
    }
    pci_free_irq_vectors(edu);
  } else {
    p5i_fail("devm allocation", ret);
    pci_free_irq_vectors(edu);
  }

  /* 7: stress with exact counts and a PMM baseline. */
  ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
  if (ret == 1 && (irq = pci_irq_vector(edu, 0)) >= 0 &&
      request_irq((unsigned int)irq, p5i_handler, IRQF_SHARED, "p5irq-stress",
                  &p5i_ctx_a) == 0) {
    int i, target = p5i_ctx_a.count;
    int ok = 1;

    pmm_before = asc_pmm_get_free_pages();
    for (i = 0; i < 10000; i++) {
      p5i_raise();
      if (!p5i_wait(&p5i_ctx_a.count, target + i + 1, 20)) {
        ok = 0;
        break;
      }
    }
    if (ok && p5i_ctx_a.count == target + 10000)
      p5i_ok("10k interrupts exact");
    else
      p5i_fail("10k interrupt count", p5i_ctx_a.count - target);

    free_irq((unsigned int)irq, &p5i_ctx_a);
    pci_free_irq_vectors(edu);
    if (asc_pmm_get_free_pages() + 2 >= pmm_before)
      p5i_ok("IRQ stress PMM stable");
    else
      klogf("[FAIL] LinuxKPI: irq PMM baseline=%lu final=%lu\n", pmm_before,
            asc_pmm_get_free_pages());
  } else {
    p5i_fail("stress allocation", ret);
    pci_free_irq_vectors(edu);
  }

out_unmap:
  pci_iounmap(edu, p5i_ctx_a.mmio);
out_region:
  pci_release_region(edu, 0);
  pci_disable_device(edu);
out_put:
  pci_dev_put(edu);

  if (p5i_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: irq suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: irq suite had %d failure(s)\n", p5i_failures);
}
