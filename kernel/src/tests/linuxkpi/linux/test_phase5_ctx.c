/* Phase 5 C6 — context tracking + might_sleep self-test.
 *
 * The IRQs-on-syscall change is behavioral and is proven by the userland
 * regression and the 1 h dma-buf soak; this suite verifies the mechanisms it
 * depends on:
 *
 *   1. the boot-test kthread runs in process context: preempt_count() == 0,
 *      !in_interrupt(), !in_atomic(), IRQs enabled,
 *   2. preempt_disable()/preempt_enable() move the per-CPU counter and
 *      in_atomic() follows it,
 *   3. might_sleep() is quiet in process context and a real sleep works,
 *   4. might_sleep() detects an atomic section (WARN once, never BUG),
 *   5. a real EDU MSI handler observes in_interrupt()/in_atomic() and
 *      might_sleep() warns from inside it.
 *
 * Without EDU (plain `make run`) part 5 logs [SKIP] and never fails. */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/preempt.h>
#include <linux/string.h>

#include <linuxkpi/log.h>

#define P5X_EDU_VENDOR 0x1234
#define P5X_EDU_DEVICE 0x11e8
#define P5X_EDU_IRQ_RAISE 0x60
#define P5X_EDU_IRQ_ACK 0x64
#define P5X_IRQ_VALUE 0x1

static int p5x_failures;

static void p5x_ok(const char *what) {
  klogf("[  OK  ] LinuxKPI: ctx %s\n", what);
}

static void p5x_fail(const char *what, long v) {
  p5x_failures++;
  klogf("[FAIL] LinuxKPI: ctx %s (%ld)\n", what, v);
}

struct p5x_irq_ctx {
  void __iomem *mmio;
  volatile int fired;
  volatile int in_irq;
  volatile int atomic;
  volatile int warns_delta;
};

static irqreturn_t p5x_handler(int irq, void *dev_id) {
  struct p5x_irq_ctx *c = dev_id;
  int before = kpi_might_sleep_warnings();

  (void)irq;
  c->in_irq = in_interrupt() ? 1 : 0;
  c->atomic = in_atomic() ? 1 : 0;
  /* Expected to warn: hard IRQ context is not sleepable. */
  might_sleep();
  c->warns_delta = kpi_might_sleep_warnings() - before;
  c->fired = 1;
  writel(P5X_IRQ_VALUE, c->mmio + P5X_EDU_IRQ_ACK);
  return IRQ_HANDLED;
}

/* ── suite ──────────────────────────────────────────────────────────────── */

void linuxkpi_test_phase5_ctx(void) {
  struct pci_dev *edu;
  struct p5x_irq_ctx ctx;
  int warns, irq = -1, ret;

  p5x_failures = 0;
  memset(&ctx, 0, sizeof(ctx));
  klog_puts("[LINUXKPI] Phase 5 context self-test\n");

  /* 1: process-context invariants. */
  if (preempt_count() == 0 && !in_interrupt() && !in_atomic() &&
      !irqs_disabled())
    p5x_ok("process context: preempt 0, IRQs on, not atomic");
  else
    p5x_fail("process context",
             (long)(preempt_count() + in_interrupt() * 10 +
                    in_atomic() * 100 + irqs_disabled() * 1000));

  /* 2: preempt counter transitions. */
  preempt_disable();
  if (preempt_count() == 1 && in_atomic() && !in_interrupt())
    p5x_ok("preempt_disable raises the count and in_atomic()");
  else
    p5x_fail("preempt_disable", preempt_count());
  preempt_enable();
  if (preempt_count() == 0 && !in_atomic())
    p5x_ok("preempt_enable restores the context");
  else
    p5x_fail("preempt_enable", preempt_count());

  /* 3: might_sleep is quiet in process context; a real sleep is fine. */
  warns = kpi_might_sleep_warnings();
  might_sleep();
  msleep(1);
  if (kpi_might_sleep_warnings() == warns)
    p5x_ok("might_sleep quiet in process context");
  else
    p5x_fail("might_sleep process context",
             kpi_might_sleep_warnings() - warns);

  /* 4: might_sleep detects an atomic section (WARN once, no BUG). */
  preempt_disable();
  might_sleep();
  might_sleep();
  preempt_enable();
  if (kpi_might_sleep_warnings() == warns + 2)
    p5x_ok("might_sleep detects atomic calls");
  else
    p5x_fail("might_sleep atomic", kpi_might_sleep_warnings() - warns);

  if (preempt_count() == 0 && !in_interrupt())
    p5x_ok("context recovered after the warnings");
  else
    p5x_fail("post-warning context", preempt_count());

  /* 5: a real IRQ handler observes IRQ context. */
  edu = pci_get_device(P5X_EDU_VENDOR, P5X_EDU_DEVICE, NULL);
  if (!edu) {
    klog_puts("[SKIP] LinuxKPI: ctx IRQ-context check (no edu device)\n");
  } else if (pci_enable_device(edu) != 0 ||
             pci_request_region(edu, 0, "p5ctx") != 0) {
    p5x_fail("edu enable/region", 0);
  } else {
    ctx.mmio = pci_iomap(edu, 0, 0);
    if (!ctx.mmio) {
      p5x_fail("edu iomap", 0);
    } else {
      pci_set_master(edu);
      ret = pci_alloc_irq_vectors(edu, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
      if (ret == 1)
        irq = pci_irq_vector(edu, 0);
      if (ret != 1 || irq < 0 ||
          request_irq((unsigned int)irq, p5x_handler, IRQF_SHARED, "p5ctx",
                      &ctx) != 0) {
        p5x_fail("edu irq setup", ret);
        pci_free_irq_vectors(edu);
      } else {
        int i;

        writel(P5X_IRQ_VALUE, ctx.mmio + P5X_EDU_IRQ_RAISE);
        for (i = 0; i < 100 && !ctx.fired; i++)
          msleep(1);

        if (ctx.fired)
          p5x_ok("EDU handler ran");
        else
          p5x_fail("EDU handler", 0);
        if (ctx.in_irq && ctx.atomic)
          p5x_ok("handler sees in_interrupt() + in_atomic()");
        else
          p5x_fail("handler context", ctx.in_irq * 10 + ctx.atomic);
        if (ctx.warns_delta == 1)
          p5x_ok("might_sleep warned from the IRQ handler");
        else
          p5x_fail("handler might_sleep", ctx.warns_delta);

        free_irq((unsigned int)irq, &ctx);
        pci_free_irq_vectors(edu);
      }
      pci_iounmap(edu, ctx.mmio);
    }
    pci_release_region(edu, 0);
    pci_disable_device(edu);
  }
  if (edu)
    pci_dev_put(edu);

  if (p5x_failures == 0)
    klog_puts("[  OK  ] LinuxKPI: ctx suite complete\n");
  else
    klogf("[FAIL] LinuxKPI: ctx suite had %d failure(s)\n", p5x_failures);
}
