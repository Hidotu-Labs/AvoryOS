#include "drivers/storage/nvme.h"

#include "apic/lapic_timer.h"
#include "arch/uaccess.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "drivers/pci/pci_irq.h"
#include "drivers/storage/block.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/dma_alloc.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVME_MAX_CONTROLLERS 8
#define NVME_TIMEOUT_MS 5000u
#define NVME_BAR_MIN_SIZE 0x4000u

#define NVME_CID_MAX 64u
/* lba * 512 (the virtual byte offset in nvme_io) must not wrap, so a namespace
 * whose virtual sector count exceeds this is rejected at Identify time. */
#define NVME_MAX_SECTORS (UINT64_MAX / 512u)
/* PRP list pages handed out lazily per CID, up to the chain limit. */
#define NVME_PRP_PAGES NVME_PRP_LIST_PAGES

struct nvme_prp_pool {
  uint64_t *page[NVME_PRP_PAGES];
  uint64_t phys[NVME_PRP_PAGES];
};

struct nvme_queue {
  uint16_t qid;
  uint16_t depth; /* number of entries */
  uint16_t tail;  /* SQ producer index */
  uint16_t head;  /* CQ consumer index */
  uint8_t phase;  /* expected CQ phase bit */
  bool valid;
  nvme_command_t *sq;
  volatile nvme_completion_t *cq;
  uint64_t sq_phys;
  uint64_t cq_phys;
  uint32_t sq_db_off;
  uint32_t cq_db_off;
};

/*
 * One I/O queue pair plus everything that used to be controller-wide: the CID
 * bitmap/verdicts/SQ tail and the PRP list pages.  Submissions from one CPU
 * only touch its own queue's fields, so scaling never contends on a shared
 * lock.  A controller reset still tears all queues down together.
 */
struct nvme_iovq {
  struct nvme_queue q;
  spinlock_t lock;    /* CID bitmap, verdicts, SQ tail */
  spinlock_t cq_lock; /* single CQ consumer */
  uint64_t cid_in_use;
  volatile int cid_result[NVME_CID_MAX];
  wait_queue_t cid_wq[NVME_CID_MAX];
  struct nvme_prp_pool prp[NVME_CID_MAX];
  uint16_t vector;      /* MSI-X table index feeding this queue */
  uint16_t queue_index; /* 0-based position in ctrl->io[] */
  uint64_t submitted;   /* commands submitted (Phase 6 diagnostics) */
};

struct nvme_namespace {
  struct nvme_controller *ctrl;
  uint32_t nsid;
  bool present;
  uint64_t nsze; /* logical blocks */
  uint8_t lba_shift;
  uint32_t lba_size;
  uint64_t total_sectors; /* virtual 512-byte sectors */
  char name[16];
  struct block_device blkdev;

  /*
   * Sub-LBA (read-modify-write) support for namespaces whose logical block is
   * larger than a virtual sector.  A single DMA page holds any accepted LBA,
   * rmw_lock serializes the partial path and rmw_wq parks waiters on it.
   */
  void *rmw_buf;
  uint64_t rmw_phys;
  spinlock_t rmw_state;
  wait_queue_t rmw_wq;
  bool rmw_held;
};

struct nvme_controller {
  struct pci_device *pdev;
  volatile nvme_regs_t *regs;
  uint64_t bar_phys;
  uint64_t bar_size;
  uint64_t cap;
  uint32_t vs;
  uint8_t dstrd;
  uint8_t index;
  char name[16];

  bool started;
  volatile bool failed;
  bool io_ready;
  bool logged; /* Identify details already printed once */

  /*
   * True only while CC.EN=0 and CSTS.RDY=0 have been observed (or the
   * controller was never enabled).  It gates freeing DMA pages: a controller
   * that ignored CC.EN=0 could still be writing to a queue or bounce buffer,
   * so those pages stay allocated rather than becoming a use-after-free.
   * Set false when CC.EN=1 is acknowledged, true by nvme_controller_disable.
   */
  volatile bool dma_quiesced;

  uint8_t mdts;
  uint16_t nn;
  uint32_t max_transfer;
  char sn[21];
  char mn[41];
  char fr[9];

  /* Completion interrupt (MSI-X, MSI or INTx).  When unavailable the driver
   * polls and never asks the controller to raise anything. */
  struct pci_irq irq;
  bool irq_active;

  struct nvme_queue admin;
  spinlock_t admin_lock;
  uint32_t admin_cid;

  struct nvme_iovq io[NVME_MAX_QUEUES];
  unsigned io_queue_count;  /* successfully created queue pairs (>= 1) */
  unsigned wanted_queues;   /* min(ncpus, NVME_MAX_QUEUES) */

  /* Incremented on every enable.  A waiter that sees a different generation
   * knows its command was discarded by a reset and reports a transport error;
   * the recovery path also uses it to wait for an in-flight recovery. */
  volatile uint32_t generation;
  volatile int recovering;
  volatile uint64_t cpu_seen;  /* CPUs observed by the I/O path (debug) */
  volatile uint64_t apic_seen; /* LAPIC IDs observed by the I/O path */
  volatile uint64_t hint_hits; /* times the affinity hint picked a queue */
  volatile uint64_t hint_cpu[NVME_MAX_QUEUES]; /* histogram of hint CPUs */
  volatile uint64_t nohint_multi; /* calls without a single-CPU mask */
  volatile uint64_t nohint_none;  /* calls with no current thread */
  volatile uint64_t wait_blocks;  /* I/O waiters that slept */
  volatile uint64_t vector_irqs[PCI_IRQ_MAX_VECTORS]; /* IRQs per vector */

  void *id_virt;
  uint64_t id_phys;

  struct nvme_namespace namespaces[NVME_MAX_NAMESPACES];
  unsigned ns_count;
};

static struct nvme_controller controllers[NVME_MAX_CONTROLLERS];
static unsigned nvme_count;
/* Set once nvme_init() completes.  Before that the driver is used from the
 * boot thread while storage is still being brought up, where blocking on a
 * wait queue (and a scheduler tick) is unnecessary; poll instead. */
static bool nvme_runtime;
/* Test hook: while positive, submission doorbell writes are suppressed and
 * the count is decremented, so a command never reaches the controller and the
 * timeout/fail-stop (and, with recovery enabled, reset) path is exercised.
 * Only used by the NVME_SELFTEST fault-injection build. */
static volatile int nvme_fault_wedge;

/*
 * MSI-X vector -> queue dispatcher.  A vector feeds one queue when there are
 * enough vectors, or a range of queues when the MSI-X table (msix_qsize) is
 * smaller than the queue count.  INTx has no stable CPU vector here, so its
 * handler drains every queue of the controller directly.
 */
struct nvme_vector_entry {
  struct nvme_controller *ctrl;
  uint16_t queue_mask;
};
static struct nvme_vector_entry nvme_vector_map[256];

/* The calling CPU's I/O queue; CPUs beyond the queue count share modulo. */
static struct nvme_iovq *nvme_current_vq(struct nvme_controller *ctrl) {
  if (!ctrl->io_queue_count)
    return NULL;
  uint32_t cpu = 0;
  struct cpu_info *ci = cpu_get_current();
  if (ci)
    cpu = ci->cpu_id;

  /* User I/O syscalls run with IF masked (IA32_FMASK) and busy-poll the CQ,
   * so the scheduler cannot migrate the thread mid-request and every request
   * would otherwise land on the CPU that entered the syscall.  When the thread
   * is restricted to exactly one CPU, treat that as its queue affinity: a
   * caller that pins one worker per CPU then really drives one queue per CPU.
   * Unpinned threads keep using the current CPU. */
  struct thread *self = sched_get_current();
  if (self && self->cpu_affinity) {
    uint64_t mask = self->cpu_affinity;
    if ((mask & (mask - 1)) == 0) {
      uint32_t pinned = 0;
      while (!(mask & 1u)) {
        mask >>= 1;
        pinned++;
      }
      if (pinned < ctrl->io_queue_count) {
        cpu = pinned;
        ctrl->hint_hits++;
        ctrl->hint_cpu[pinned]++;
      }
    } else {
      ctrl->nohint_multi++;
    }
  } else {
    ctrl->nohint_none++;
  }

  ctrl->cpu_seen |= 1ull << (cpu & 63);
  ctrl->apic_seen |= 1ull << (lapic_get_id() & 63);
  return &ctrl->io[cpu % ctrl->io_queue_count];
}

/* MMIO helpers ------------------------------------------------------------ */

static inline uint32_t nvme_read32(const struct nvme_controller *ctrl,
                                   uint32_t offset) {
  return *(volatile uint32_t *)((volatile uint8_t *)ctrl->regs + offset);
}

static inline void nvme_write32(const struct nvme_controller *ctrl,
                                uint32_t offset, uint32_t value) {
  *(volatile uint32_t *)((volatile uint8_t *)ctrl->regs + offset) = value;
}

static inline uint64_t nvme_read64(const struct nvme_controller *ctrl,
                                   uint32_t offset) {
  return *(volatile uint64_t *)((volatile uint8_t *)ctrl->regs + offset);
}

static inline void nvme_write64(const struct nvme_controller *ctrl,
                                uint32_t offset, uint64_t value) {
  *(volatile uint64_t *)((volatile uint8_t *)ctrl->regs + offset) = value;
}

/* Little-endian field readers for the Identify payload. */
static inline uint16_t nvme_ld16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t nvme_ld32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t nvme_ld64(const uint8_t *p) {
  return (uint64_t)nvme_ld32(p) | ((uint64_t)nvme_ld32(p + 4) << 32);
}

/* Copy an Identify string into a NUL-terminated, space-trimmed buffer. */
static void nvme_copy_string(char *dst, size_t dst_size, const uint8_t *src,
                             size_t src_len) {
  size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0'))
    dst[--n] = '\0';
}

/* PCI/BAR helpers --------------------------------------------------------- */

static uint64_t nvme_bar0_phys(const struct pci_device *pci) {
  if (!pci || (pci->bar[0] & 1u))
    return 0; /* I/O BAR or missing */
  uint64_t result = pci->bar[0] & ~0xFULL;
  if (((pci->bar[0] >> 1) & 3u) == 2u)
    result |= (uint64_t)pci->bar[1] << 32;
  return result;
}

/* The DM bus device carries the probed BAR size as a resource.  Fall back to a
 * conservative 16 KiB (registers + doorbells) when the resource is missing. */
static uint64_t nvme_bar0_size(const struct device *dev) {
  for (size_t i = 0; i < dev->resource_count; i++) {
    const struct resource *r = &dev->resources[i];
    if (r->name && strcmp(r->name, "bar0") == 0 && r->type == RES_MEM &&
        r->end >= r->start)
      return r->end - r->start + 1;
  }
  return 0;
}

/* Map the PCI MMIO range at its HHDM slot as uncached, skipping pages that are
 * already mapped.  Mirrors the xHCI mapping approach: never remap a live
 * kernel mapping during early SMP boot. */
static bool nvme_map_mmio(uint64_t phys, uint64_t length) {
  if (!phys || !length)
    return false;

  uint64_t first = phys & ~0xFFFULL;
  uint64_t last = (phys + length - 1) & ~0xFFFULL;
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t flags =
      PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

  for (uint64_t page = first; page <= last; page += PAGE_SIZE) {
    uint64_t virt = page + hhdm;
    if (!vmm_virt_to_phys(pml4, virt) &&
        !vmm_map_page(pml4, virt, page, flags))
      return false;
    vmm_flush_tlb(virt);
  }
  return true;
}

static struct pci_device *nvme_pci_for_device(struct device *dev) {
  uint32_t count = pci_get_device_count();
  for (uint32_t i = 0; i < count; i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->kernel_device == dev)
      return pci;
  }
  return NULL;
}

/* Controller state -------------------------------------------------------- */

static bool nvme_wait_csts(struct nvme_controller *ctrl, uint32_t mask,
                           uint32_t expected, uint32_t timeout_ms) {
  uint64_t deadline = lapic_timer_get_ms() + timeout_ms;
  for (;;) {
    if ((nvme_read32(ctrl, NVME_REG_CSTS) & mask) == expected)
      return true;
    if (lapic_timer_get_ms() >= deadline)
      return false;
    hal_cpu_relax();
  }
}

/* Disable the controller and wait for CSTS.RDY to clear.  Idempotent.
 *
 * This is the DMA lifetime barrier: only after CSTS.RDY reads 0 has the
 * controller promised to stop issuing memory accesses, so callers may free
 * queue/bounce pages once ctrl->dma_quiesced is set.  A controller that never
 * clears RDY leaves the flag false and its DMA memory allocated. */
static bool nvme_controller_disable(struct nvme_controller *ctrl) {
  uint32_t cc = nvme_read32(ctrl, NVME_REG_CC);
  if (!(cc & NVME_CC_EN)) {
    ctrl->dma_quiesced = true;
    return true;
  }
  nvme_write32(ctrl, NVME_REG_CC, cc & ~NVME_CC_EN);
  bool stopped = nvme_wait_csts(ctrl, NVME_CSTS_RDY, 0, NVME_TIMEOUT_MS);
  if (stopped)
    ctrl->dma_quiesced = true;
  return stopped;
}

static void nvme_fail_stop(struct nvme_controller *ctrl, const char *reason) {
  if (ctrl->failed) {
    ctrl->io_ready = false;
    return;
  }
  ctrl->failed = true;
  ctrl->io_ready = false;
  ctrl->started = false;
  klogf("[NVMe] %s fail-stop: %s\n", ctrl->name, reason);
  /* Clearing CC.EN stops the controller from touching DMA buffers.  Waiting
   * for RDY=0 before returning is what lets a timed-out I/O free its bounce
   * buffer: the device has acknowledged that no transfer is in flight. */
  if (!nvme_controller_disable(ctrl))
    klogf("[NVMe] %s did not quiesce after fail-stop; DMA pages retained\n",
          ctrl->name);
}

static void nvme_log_controller(const struct nvme_controller *ctrl) {
  const struct pci_device *pci = ctrl->pdev;
  klogf("[  OK  ] NVMe: %02x:%02x.%u %s VS=%u.%u CAP{mqes=%u cqr=%u to=%u "
        "dstrd=%u css=%02x mpsmin=%u mpsmax=%u}\n",
        pci->bus, pci->slot, pci->func, ctrl->name,
        (unsigned)((ctrl->vs >> 16) & 0xFFFFu),
        (unsigned)((ctrl->vs >> 8) & 0xFFu), (unsigned)NVME_CAP_MQES(ctrl->cap),
        (unsigned)NVME_CAP_CQR(ctrl->cap), (unsigned)NVME_CAP_TO(ctrl->cap),
        (unsigned)NVME_CAP_DSTRD(ctrl->cap), (unsigned)NVME_CAP_CSS(ctrl->cap),
        (unsigned)NVME_CAP_MPSMIN(ctrl->cap),
        (unsigned)NVME_CAP_MPSMAX(ctrl->cap));
}

/* Queues ------------------------------------------------------------------ */

static void nvme_queue_reset(struct nvme_queue *q) {
  q->tail = 0;
  q->head = 0;
  q->phase = 1;
  if (q->sq)
    memset(q->sq, 0, NVME_PAGE_SIZE);
  if (q->cq)
    memset((void *)q->cq, 0, NVME_PAGE_SIZE);
}

static bool nvme_queue_alloc(struct nvme_queue *q, uint16_t qid, uint16_t depth,
                             uint8_t dstrd) {
  memset(q, 0, sizeof(*q));
  q->qid = qid;
  q->depth = depth;
  q->phase = 1;

  q->sq = (nvme_command_t *)dma_alloc_page_flags(DMA_FLAG_ANYWHERE, &q->sq_phys);
  q->cq = (volatile nvme_completion_t *)dma_alloc_page_flags(DMA_FLAG_ANYWHERE,
                                                             &q->cq_phys);
  if (!q->sq || !q->cq) {
    if (q->sq) {
      dma_free_page(q->sq);
      q->sq = NULL;
    }
    if (q->cq) {
      dma_free_page((void *)q->cq);
      q->cq = NULL;
    }
    return false;
  }

  q->sq_db_off = nvme_sq_doorbell_offset(qid, dstrd);
  q->cq_db_off = nvme_cq_doorbell_offset(qid, dstrd);
  q->valid = true;
  nvme_queue_reset(q);
  return true;
}

static void nvme_queue_free(struct nvme_queue *q) {
  if (q->sq) {
    dma_free_page(q->sq);
    q->sq = NULL;
  }
  if (q->cq) {
    dma_free_page((void *)q->cq);
    q->cq = NULL;
  }
  q->valid = false;
}

static void nvme_queue_depth_for(uint64_t cap, uint16_t *out) {
  uint32_t depth = (uint32_t)NVME_CAP_MQES(cap) + 1u;
  if (depth > NVME_ADMIN_QUEUE_DEPTH)
    depth = NVME_ADMIN_QUEUE_DEPTH;
  if (depth < 2)
    depth = 2; /* the spec minimum */
  *out = (uint16_t)depth;
}

/*
 * PRP list pages are allocated on first use and kept for the queue's
 * lifetime; a CID reuses the same pages for every command it carries, so a
 * steady workload allocates nothing.  Most commands never need one at all (a
 * single PRP1/PRP2 pair covers up to two pages) and only transfers above
 * 2 MiB reach the second chained page.
 */
static uint64_t *nvme_prp_page(struct nvme_iovq *vq, uint32_t cid,
                               unsigned index) {
  if (index >= NVME_PRP_PAGES || cid >= NVME_CID_MAX)
    return NULL;
  struct nvme_prp_pool *pool = &vq->prp[cid];
  if (!pool->page[index]) {
    uint64_t phys = 0;
    uint64_t *page =
        (uint64_t *)dma_alloc_page_flags(DMA_FLAG_ANYWHERE, &phys);
    if (!page)
      return NULL;
    memset(page, 0, NVME_PAGE_SIZE);
    pool->page[index] = page;
    pool->phys[index] = phys;
  }
  return pool->page[index];
}

/* Release every list page a queue accumulated.  Pages are allocated lazily by
 * nvme_prp_page() on the first transfer that needs a chain, so a boot with no
 * I/O pays nothing for them. */
static void nvme_free_prp_lists(struct nvme_iovq *vq) {
  for (uint32_t i = 0; i < NVME_CID_MAX; i++) {
    for (unsigned p = 0; p < NVME_PRP_PAGES; p++) {
      if (vq->prp[i].page[p]) {
        dma_free_page(vq->prp[i].page[p]);
        vq->prp[i].page[p] = NULL;
      }
    }
  }
}

/* Admin commands ---------------------------------------------------------- */

/*
 * Submit one admin command and poll its completion.  Admin commands are
 * serialized by admin_lock and only run during bring-up, so spinning with
 * interrupts masked is acceptable: a healthy command completes in
 * microseconds.  Returns 0 on success, 1 on a command status error and -1 on a
 * transport failure (timeout / controller failure).  `status` (when non-NULL)
 * always receives the 8-bit status code.
 */
static int nvme_admin_submit_locked(struct nvme_controller *ctrl,
                                    const nvme_command_t *cmd,
                                    uint32_t *result, uint16_t *status) {
  struct nvme_queue *q = &ctrl->admin;
  if (!q->valid || q->depth == 0)
    return -1;

  uint16_t cid = (uint16_t)(ctrl->admin_cid++ % q->depth);
  nvme_command_t *slot = &q->sq[q->tail];
  *slot = *cmd;
  slot->cid = cid;
  q->tail = (uint16_t)((q->tail + 1) % q->depth);
  /* Release fence between the SQE/tail stores and the doorbell MMIO write:
   * the controller must never observe the new tail before the command bytes.
   * x86 TSO already orders the stores; the fence makes the compiler honor it
   * and documents the invariant for other memory models. */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  nvme_write32(ctrl, q->sq_db_off, q->tail);

  uint64_t deadline = lapic_timer_get_ms() + NVME_TIMEOUT_MS;
  for (;;) {
    volatile nvme_completion_t *cqe = &q->cq[q->head];
    uint16_t cqe_status = cqe->status;
    if ((cqe_status & NVME_CQE_PHASE) == q->phase) {
      /* Same acquire as nvme_drain_cq(): phase set implies fields are valid. */
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      uint16_t sc = NVME_CQE_SC(cqe_status);
      uint16_t cqe_cid = cqe->cid;
      uint32_t res = cqe->result;
      q->head = (uint16_t)((q->head + 1) % q->depth);
      if (q->head == 0)
        q->phase ^= 1u;
      nvme_write32(ctrl, q->cq_db_off, q->head);

      if (status)
        *status = sc;
      if (result)
        *result = res;

      if (cqe_cid != cid) {
        klogf("[NVMe] %s admin completion CID mismatch: got %u want %u\n",
              ctrl->name, cqe_cid, cid);
        return -1;
      }
      return sc == NVME_SC_SUCCESS ? 0 : 1;
    }

    if (nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_CFS) {
      klogf("[NVMe] %s admin command: CSTS.CFS set\n", ctrl->name);
      nvme_fail_stop(ctrl, "admin CSTS.CFS");
      if (status)
        *status = 0x7F;
      return -1;
    }
    if (lapic_timer_get_ms() >= deadline) {
      klogf("[NVMe] %s admin command timeout cid=%u tail=%u head=%u\n",
            ctrl->name, cid, q->tail, q->head);
      nvme_fail_stop(ctrl, "admin command timeout");
      if (status)
        *status = 0x7F;
      return -1;
    }
    hal_cpu_relax();
  }
}

static int nvme_admin_cmd(struct nvme_controller *ctrl,
                          const nvme_command_t *cmd, uint32_t *result,
                          uint16_t *status) {
  spinlock_acquire(&ctrl->admin_lock);
  int rc = nvme_admin_submit_locked(ctrl, cmd, result, status);
  spinlock_release(&ctrl->admin_lock);
  return rc;
}

/* Run an Identify command and leave the 4 KiB payload in ctrl->id_virt. */
static int nvme_identify(struct nvme_controller *ctrl, uint32_t nsid,
                         uint32_t cns, uint16_t *status) {
  if (!ctrl->id_virt)
    return -1;

  nvme_command_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_IDENTIFY;
  cmd.nsid = nsid;
  cmd.prp1 = ctrl->id_phys;
  cmd.cdw10 = cns;

  uint16_t sc = 0;
  int rc = nvme_admin_cmd(ctrl, &cmd, NULL, &sc);
  if (status)
    *status = sc;
  return rc == 0 ? 0 : -1;
}

/*
 * Create one I/O queue pair (qid = index + 1): completion queue first, then
 * the submission queue bound to it.  IEN=1 points the CQ at its MSI-X vector
 * when a vector feeds this queue; INTx and polled modes post to vector IV=0
 * or stay silent.  On partial failure the CQ is deleted so a later retry (or
 * the next controller reset) starts from a clean device state.
 */
static int nvme_create_io_queue(struct nvme_controller *ctrl, unsigned index) {
  struct nvme_iovq *vq = &ctrl->io[index];
  uint16_t qid = (uint16_t)(index + 1);
  uint16_t depth = vq->q.depth;
  uint32_t qsize = (uint32_t)(depth - 1) << 16;
  uint32_t iv = (ctrl->irq_active && ctrl->irq.mode == PCI_IRQ_MSIX)
                    ? vq->vector
                    : 0;

  nvme_command_t cmd;
  uint16_t sc = 0;

  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_CQ;
  cmd.prp1 = vq->q.cq_phys;
  cmd.cdw10 = qid | qsize;
  cmd.cdw11 =
      NVME_CQ_PC | (ctrl->irq_active ? NVME_CQ_IEN : 0u) | NVME_CQ_IV(iv);
  int rc = nvme_admin_cmd(ctrl, &cmd, NULL, &sc);
  if (rc != 0 || sc != NVME_SC_SUCCESS) {
    klogf("[NVMe] %s create I/O CQ qid=%u failed (rc=%d sc=%02x)\n", ctrl->name,
          qid, rc, sc);
    return -1;
  }

  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_SQ;
  cmd.prp1 = vq->q.sq_phys;
  cmd.cdw10 = qid | qsize;
  cmd.cdw11 = NVME_SQ_PC | NVME_SQ_QPRIO(0) | NVME_SQ_CQID(qid);
  rc = nvme_admin_cmd(ctrl, &cmd, NULL, &sc);
  if (rc != 0 || sc != NVME_SC_SUCCESS) {
    klogf("[NVMe] %s create I/O SQ qid=%u failed (rc=%d sc=%02x)\n", ctrl->name,
          qid, rc, sc);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_DELETE_CQ;
    cmd.cdw10 = qid;
    nvme_admin_cmd(ctrl, &cmd, NULL, NULL);
    return -1;
  }
  return 0;
}

/*
 * Bring up as many queue pairs as the controller lets us.  wanted_queues is
 * min(ncpus, NVME_MAX_QUEUES); a device with a smaller max_ioqpairs (QEMU's
 * max_ioqpairs= knob) rejects the extra qids and we keep the ones that
 * worked, with CPUs sharing them modulo the count.
 */
static int nvme_create_io_queues(struct nvme_controller *ctrl) {
  unsigned created = 0;
  uint16_t depth = 0;
  nvme_queue_depth_for(ctrl->cap, &depth);

  for (unsigned i = 0; i < ctrl->wanted_queues; i++) {
    if (!ctrl->io[i].q.valid &&
        !nvme_queue_alloc(&ctrl->io[i].q, (uint16_t)(i + 1), depth,
                          ctrl->dstrd)) {
      klogf("[NVMe] %s I/O queue %u allocation failed\n", ctrl->name, i);
      break;
    }
    /* The CQ's interrupt vector must be known before the Create CQ command:
     * it goes into the IV field. */
    ctrl->io[i].queue_index = (uint16_t)i;
    ctrl->io[i].vector = (uint16_t)((ctrl->irq_active &&
                                     ctrl->irq.mode == PCI_IRQ_MSIX &&
                                     i < ctrl->irq.vector_count)
                                        ? i
                                        : 0);
    if (nvme_create_io_queue(ctrl, i) != 0)
      break;
    created++;
  }

  if (created == 0) {
    klogf("[NVMe] %s no I/O queue pair could be created\n", ctrl->name);
    return -1;
  }
  ctrl->io_queue_count = created;
  return 0;
}

/*
 * Rebuild the vector -> queue map after the actual queue count is known.
 * With enough MSI-X vectors each queue gets its own; when the table is too
 * small, the last allocated vector takes the remaining queues (one scan).
 */
static void nvme_irq_map_queues(struct nvme_controller *ctrl) {
  if (!ctrl->irq_active)
    return;
  if (ctrl->irq.mode != PCI_IRQ_MSIX)
    return; /* INTx/MSI drain the whole controller at dispatch time */

  uint16_t vectors = ctrl->irq.vector_count;
  for (uint16_t i = 0; i < vectors; i++) {
    uint8_t vector = pci_irq_vector(&ctrl->irq, i);
    /* 0xFF is the "no vector" marker; the map has UINT8_MAX+1 entries, so
     * every other byte is in range. */
    if (vector == 0xFF)
      continue;

    /* Each vector owns queue i; the final vector also takes every queue
     * beyond the vector count (the "one scanning all" fallback). */
    uint16_t first = i;
    uint16_t end = (i + 1 == vectors && vectors <= ctrl->io_queue_count)
                       ? (uint16_t)ctrl->io_queue_count
                       : (uint16_t)(i + 1);
    uint16_t mask = 0;
    if (first < ctrl->io_queue_count) {
      if (end > ctrl->io_queue_count)
        end = (uint16_t)ctrl->io_queue_count;
      for (uint16_t q = first; q < end; q++)
        mask |= (uint16_t)(1u << q);
    }
    nvme_vector_map[vector].ctrl = ctrl;
    nvme_vector_map[vector].queue_mask = mask;
  }
}

/*
 * Drop this controller's vector-map entries.  The interrupt vectors are about
 * to be returned to the CPU vector pool and can be handed to another device
 * (or the same controller after a re-probe), so a stale entry pointing at a
 * freed-or-reused controller slot must not survive release.  Must run before
 * pci_irq_release clears irq.vector_count.
 */
static void nvme_irq_map_clear(struct nvme_controller *ctrl) {
  if (!ctrl->irq_active)
    return;
  for (uint16_t i = 0; i < ctrl->irq.vector_count; i++) {
    uint8_t vector = pci_irq_vector(&ctrl->irq, i);
    if (vector == 0xFF)
      continue;
    nvme_vector_map[vector].ctrl = NULL;
    nvme_vector_map[vector].queue_mask = 0;
  }
}

/* Identify parsing --------------------------------------------------------- */

/*
 * Largest transfer the driver will submit given the controller's MDTS byte.
 * MDTS is 2^MDTS * MPS with 0 meaning "no limit" (NVMe 1.4 §3.1.3.1).  The
 * shift is clamped below 64 because a hostile/broken Identify byte must not
 * reach undefined behavior, and the comparison caps the result by
 * NVME_MAX_TRANSFER before any 32-bit truncation can happen.
 */
static uint32_t nvme_max_transfer_for(uint8_t mdts) {
  uint32_t max_transfer = NVME_MAX_TRANSFER;
  if (mdts > 0 && mdts < 64) {
    uint64_t mdts_bytes = (uint64_t)NVME_PAGE_SIZE << mdts;
    if (mdts_bytes < max_transfer)
      max_transfer = (uint32_t)mdts_bytes;
  }
  if (max_transfer < 512)
    max_transfer = 512;
  return max_transfer;
}

static void nvme_parse_controller(struct nvme_controller *ctrl) {
  const uint8_t *id = (const uint8_t *)ctrl->id_virt;
  nvme_copy_string(ctrl->sn, sizeof(ctrl->sn), id + NVME_ID_CTRL_SN, 20);
  nvme_copy_string(ctrl->mn, sizeof(ctrl->mn), id + NVME_ID_CTRL_MN, 40);
  nvme_copy_string(ctrl->fr, sizeof(ctrl->fr), id + NVME_ID_CTRL_FR, 8);
  ctrl->mdts = id[NVME_ID_CTRL_MDTS];
  ctrl->nn = nvme_ld16(id + NVME_ID_CTRL_NN);

  ctrl->max_transfer = nvme_max_transfer_for(ctrl->mdts);

  if (!ctrl->logged) {
    klogf("[  OK  ] NVMe: %s SN=\"%s\" MN=\"%s\" FR=\"%s\" NN=%u MDTS=%u "
          "max_xfer=%u\n",
          ctrl->name, ctrl->sn, ctrl->mn, ctrl->fr, ctrl->nn, ctrl->mdts,
          ctrl->max_transfer);
    ctrl->logged = true;
  }
}

static struct nvme_namespace *nvme_ns_slot(struct nvme_controller *ctrl,
                                           uint32_t nsid) {
  struct nvme_namespace *free_slot = NULL;
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    struct nvme_namespace *ns = &ctrl->namespaces[i];
    if (ns->nsid == nsid)
      return ns;
    if (!free_slot && ns->nsid == 0)
      free_slot = ns;
  }
  return free_slot;
}

/*
 * Validate namespace geometry and compute the virtual (512-byte) sector
 * count.  Pure arithmetic so the boot audit self-test can exercise every
 * bound with synthetic values.  nvme_io() relies on the result for its
 * lba*512 byte-offset math, which is why the UINT64_MAX/512 cap is part of
 * the acceptance rather than a plain 64-bit multiply overflow check.
 */
static bool nvme_ns_geometry(uint64_t nsze, uint8_t lbads, uint16_t ms,
                             uint64_t *total_sectors) {
  if (nsze == 0 || lbads < NVME_MIN_LBA_SHIFT || lbads > NVME_MAX_LBA_SHIFT ||
      ms != 0)
    return false;
  uint64_t units = (uint64_t)1 << (lbads - 9u); /* virtual sectors per LBA */
  if (nsze > UINT64_MAX / units)
    return false;
  uint64_t sectors = nsze * units;
  if (sectors > NVME_MAX_SECTORS)
    return false;
  *total_sectors = sectors;
  return true;
}

static bool nvme_parse_namespace(struct nvme_controller *ctrl, uint32_t nsid) {
  const uint8_t *id = (const uint8_t *)ctrl->id_virt;
  uint64_t nsze = nvme_ld64(id + NVME_ID_NS_NSZE);
  uint8_t nlbaf = id[NVME_ID_NS_NLBAF];
  uint8_t flbas = id[NVME_ID_NS_FLBAS];
  uint8_t lbaf_idx = flbas & 0x0Fu;

  if (nsze == 0)
    return false;
  if ((flbas & 0x10u) || lbaf_idx > nlbaf) {
    klogf("[NVMe] %s nsid=%u unsupported FLBAS=%02x NLBAF=%u\n", ctrl->name,
          nsid, flbas, nlbaf);
    return false;
  }

  uint32_t lbaf = nvme_ld32(id + NVME_ID_NS_LBAF + 4u * lbaf_idx);
  uint8_t lbads = NVME_LBAF_LBADS(lbaf);
  uint16_t ms = NVME_LBAF_MS(lbaf);
  if (lbads == 0) {
    klogf("[NVMe] %s nsid=%u LBAF[%u] has no valid LBA size\n", ctrl->name,
          nsid, lbaf_idx);
    return false;
  }
  if (lbads < NVME_MIN_LBA_SHIFT || lbads > NVME_MAX_LBA_SHIFT || ms != 0) {
    klogf("[NVMe] %s nsid=%u LBA format LBADS=%u MS=%u unsupported "
          "(accepted: 512..4096-byte LBAs, no metadata)\n",
          ctrl->name, nsid, lbads, ms);
    return false;
  }

  struct nvme_namespace *ns = nvme_ns_slot(ctrl, nsid);
  if (!ns) {
    klogf("[NVMe] %s namespace table full, skipping nsid=%u\n", ctrl->name,
          nsid);
    return false;
  }

  uint64_t total_sectors = 0;
  if (!nvme_ns_geometry(nsze, lbads, ms, &total_sectors)) {
    klogf("[NVMe] %s nsid=%u NSZE=%llu LBADS=%u overflows the virtual sector "
          "count\n",
          ctrl->name, nsid, (unsigned long long)nsze, lbads);
    return false;
  }

  ns->nsid = nsid;
  ns->present = true;
  ns->nsze = nsze;
  ns->lba_shift = lbads;
  ns->lba_size = 1u << lbads;
  ns->total_sectors = total_sectors;
  ns->ctrl = ctrl;

  /* Sub-LBA writes need a full LBA in a DMA-safe page. */
  if (ns->lba_size > 512 && !ns->rmw_buf) {
    ns->rmw_buf =
        dma_alloc_page_flags(DMA_FLAG_ANYWHERE, &ns->rmw_phys);
    if (!ns->rmw_buf) {
      klogf("[NVMe] %s nsid=%u RMW bounce allocation failed\n", ctrl->name,
            nsid);
      ns->present = false;
      return false;
    }
    /* A partial read of a block whose device content is stale must never leak
     * allocator bytes to the caller before the read fills the page. */
    memset(ns->rmw_buf, 0, NVME_PAGE_SIZE);
  }

  if (!ns->blkdev.read_sectors)
    klogf("[  OK  ] NVMe: %s nsid=%u NSZE=%llu LBADS=%u (%u-byte LBAs, "
          "%llu sectors)\n",
          ctrl->name, nsid, (unsigned long long)nsze, lbads, ns->lba_size,
          (unsigned long long)ns->total_sectors);
  return true;
}

/*
 * Identify the active namespace ID list (CNS 0x02) and probe only the NSIDs
 * the controller reports.  One admin command replaces probing every NSID up to
 * CAP.NN: a 256-namespace controller would otherwise cost 256 synchronous
 * round-trips at boot, and some devices stall on Identify for detached NSIDs.
 * The list buffer is overwritten by the first per-namespace Identify, so the
 * IDs are copied out first.  Returns false when the controller rejects
 * CNS 0x02 (pre-1.1 or namespace-management disabled), letting the caller fall
 * back to the sequential scan.
 */
static bool nvme_scan_active_namespaces(struct nvme_controller *ctrl) {
  const uint8_t *id = (const uint8_t *)ctrl->id_virt;
  uint16_t sc = 0;

  if (nvme_identify(ctrl, 0, NVME_ID_CNS_ACTIVE_NS, &sc) != 0 ||
      sc != NVME_SC_SUCCESS)
    return false;

  uint32_t list[NVME_MAX_NAMESPACES];
  unsigned count = 0;
  for (unsigned i = 0; i < NVME_IDENTIFY_SIZE / 4u && count < NVME_MAX_NAMESPACES;
       i++) {
    uint32_t nsid = nvme_ld32(id + 4u * i);
    if (nsid == 0)
      break; /* the list is zero-padded after the last active ID */
    list[count++] = nsid;
  }

  for (unsigned i = 0; i < count; i++) {
    if (nvme_identify(ctrl, list[i], NVME_ID_CNS_NAMESPACE, &sc) != 0 || sc)
      continue;
    if (nvme_parse_namespace(ctrl, list[i]))
      ctrl->ns_count++;
  }
  return true;
}

static void nvme_scan_namespaces(struct nvme_controller *ctrl) {
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++)
    ctrl->namespaces[i].present = false;
  ctrl->ns_count = 0;

  if (nvme_scan_active_namespaces(ctrl))
    return;

  /* Fallback for controllers without the active namespace list. */
  unsigned limit = ctrl->nn;
  if (limit > NVME_MAX_NAMESPACES)
    limit = NVME_MAX_NAMESPACES;

  for (uint32_t nsid = 1; nsid <= limit; nsid++) {
    uint16_t sc = 0;
    if (nvme_identify(ctrl, nsid, NVME_ID_CNS_NAMESPACE, &sc) != 0 || sc) {
      /* Inactive or detached namespace: normal, just skip it. */
      continue;
    }
    if (nvme_parse_namespace(ctrl, nsid))
      ctrl->ns_count++;
  }
}

/* Block device registration ------------------------------------------------- */

static struct nvme_namespace *nvme_ns_from_block(struct block_device *dev) {
  return (struct nvme_namespace *)dev->driver_data;
}

static int nvme_ns_read(struct block_device *dev, uint64_t lba, uint32_t count,
                        void *buf);
static int nvme_ns_write(struct block_device *dev, uint64_t lba,
                         uint32_t count, const void *buf);
static int nvme_ns_write_fua(struct block_device *dev, uint64_t lba,
                             uint32_t count, const void *buf);
static int nvme_ns_flush(struct block_device *dev);

static void nvme_register_namespaces(struct nvme_controller *ctrl) {
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    struct nvme_namespace *ns = &ctrl->namespaces[i];
    if (!ns->present || ns->nsid == 0)
      continue;
    if (ns->blkdev.read_sectors)
      continue; /* already registered during an earlier start */

    snprintf(ns->name, sizeof(ns->name), "%sn%u", ctrl->name, ns->nsid);
    memset(&ns->blkdev, 0, sizeof(ns->blkdev));
    strncpy(ns->blkdev.name, ns->name, sizeof(ns->blkdev.name) - 1);
    ns->blkdev.sector_size = 512;
    ns->blkdev.total_sectors = ns->total_sectors;
    ns->blkdev.read_sectors = nvme_ns_read;
    ns->blkdev.write_sectors = nvme_ns_write;
    ns->blkdev.write_sectors_fua = nvme_ns_write_fua;
    ns->blkdev.flush = nvme_ns_flush;
    ns->blkdev.driver_data = ns;

    if (block_register(&ns->blkdev) != 0)
      klogf("[NVMe] %s registration failed for %s\n", ctrl->name, ns->name);
  }
}

/* Controller bring-up / teardown ------------------------------------------- */

static bool nvme_controller_enable_admin(struct nvme_controller *ctrl) {
  nvme_queue_reset(&ctrl->admin);

  nvme_write64(ctrl, NVME_REG_ASQ, ctrl->admin.sq_phys);
  nvme_write64(ctrl, NVME_REG_ACQ, ctrl->admin.cq_phys);
  nvme_write32(ctrl, NVME_REG_AQA,
               (uint32_t)(ctrl->admin.depth - 1) |
                   ((uint32_t)(ctrl->admin.depth - 1) << 16));
  nvme_write32(ctrl, NVME_REG_CC,
               NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                   NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16);
  /* CC.EN=1 may start descriptor fetches at any moment; only an observed
   * RDY=0 (nvme_controller_disable) restores the quiesced guarantee. */
  ctrl->dma_quiesced = false;

  if (!nvme_wait_csts(ctrl, NVME_CSTS_RDY, NVME_CSTS_RDY, NVME_TIMEOUT_MS)) {
    klogf("[NVMe] %s CC.EN=1 never reached CSTS.RDY (CC=%08x CSTS=%08x)\n",
          ctrl->name, nvme_read32(ctrl, NVME_REG_CC),
          nvme_read32(ctrl, NVME_REG_CSTS));
    return false;
  }
  if (nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_CFS) {
    klogf("[NVMe] %s controller fatal status after enable\n", ctrl->name);
    return false;
  }

  /* Completion delivery.  With a message/pin interrupt, CLR the mask bits so
   * the device can signal vector IV; otherwise mask everything so a polled
   * controller cannot turn completions into an unhandled INTx storm. */
  if (ctrl->irq_active)
    nvme_write32(ctrl, NVME_REG_INTMC, 0xFFFFFFFFu);
  else
    nvme_write32(ctrl, NVME_REG_INTMS, 0xFFFFFFFFu);
  return true;
}

static int nvme_controller_start(struct nvme_controller *ctrl) {
  ctrl->started = false;
  ctrl->io_ready = false;
  ctrl->failed = false;
  ctrl->io_queue_count = 0;

  /* Bumped first: waiters holding a CID from the previous generation bail out
   * as soon as they observe it, before the bitmaps are cleared underneath them
   * (and their eventual nvme_free_cid() is suppressed). */
  ctrl->generation++;

  /* A previous fail-stop may have left CIDs reserved for commands that never
   * completed; CC.EN=0 discarded their context, so start clean. */
  for (unsigned v = 0; v < NVME_MAX_QUEUES; v++) {
    struct nvme_iovq *vq = &ctrl->io[v];
    spinlock_acquire(&vq->lock);
    vq->cid_in_use = 0;
    for (unsigned i = 0; i < NVME_CID_MAX; i++)
      vq->cid_result[i] = 0;
    spinlock_release(&vq->lock);
  }

  if (!ctrl->id_virt) {
    ctrl->id_virt =
        dma_alloc_page_flags(DMA_FLAG_ANYWHERE, &ctrl->id_phys);
    if (!ctrl->id_virt) {
      klogf("[NVMe] %s identify buffer allocation failed\n", ctrl->name);
      return -1;
    }
  }

  uint16_t depth = 0;
  nvme_queue_depth_for(ctrl->cap, &depth);

  if (!ctrl->admin.valid &&
      !nvme_queue_alloc(&ctrl->admin, 0, depth, ctrl->dstrd)) {
    klogf("[NVMe] %s admin queue allocation failed\n", ctrl->name);
    return -1;
  }

  if (!nvme_controller_disable(ctrl)) {
    klogf("[NVMe] %s did not quiesce before enable\n", ctrl->name);
    return -1;
  }
  if (!nvme_controller_enable_admin(ctrl)) {
    nvme_controller_disable(ctrl);
    return -1;
  }

  ctrl->admin_cid = 0;

  if (nvme_identify(ctrl, 0, NVME_ID_CNS_CONTROLLER, NULL) != 0) {
    klogf("[NVMe] %s Identify Controller failed\n", ctrl->name);
    nvme_controller_disable(ctrl);
    return -1;
  }
  nvme_parse_controller(ctrl);
  nvme_scan_namespaces(ctrl);

  if (nvme_create_io_queues(ctrl) != 0) {
    nvme_controller_disable(ctrl);
    return -1;
  }
  nvme_irq_map_queues(ctrl);

  ctrl->started = true;
  ctrl->io_ready = true;
  return 0;
}

static void __attribute__((unused))
nvme_controller_stop(struct nvme_controller *ctrl) {
  ctrl->io_ready = false;
  ctrl->started = false;

  /* Stop each CQ consumer before wiping its ring so an in-flight drain cannot
   * observe a half-cleared queue; lock order matches nvme_drain_cq(). */
  for (unsigned v = 0; v < NVME_MAX_QUEUES; v++) {
    struct nvme_iovq *vq = &ctrl->io[v];
    if (!vq->q.valid)
      continue;
    spinlock_acquire(&vq->cq_lock);
    spinlock_acquire(&vq->lock);
    vq->cid_in_use = 0;
    for (unsigned i = 0; i < NVME_CID_MAX; i++)
      vq->cid_result[i] = 0;
    nvme_queue_reset(&vq->q);
    spinlock_release(&vq->lock);
    spinlock_release(&vq->cq_lock);
  }
  ctrl->io_queue_count = 0;

  nvme_controller_disable(ctrl);
  nvme_queue_reset(&ctrl->admin);
}

/*
 * Graceful shutdown: request CC.SHN=normal, wait for CSTS.SHST to report
 * "shutdown complete", then clear CC.EN.  Returns true when the controller
 * acknowledged the shutdown (or was already disabled), false on timeout.
 */
static bool nvme_controller_shutdown(struct nvme_controller *ctrl) {
  ctrl->io_ready = false;
  ctrl->started = false;

  uint32_t cc = nvme_read32(ctrl, NVME_REG_CC);
  bool acknowledged = true;
  if (cc & NVME_CC_EN) {
    nvme_write32(ctrl, NVME_REG_CC,
                 (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL);
    acknowledged = nvme_wait_csts(ctrl, NVME_CSTS_SHST_MASK,
                                  NVME_CSTS_SHST_COMPLETE, 2000);
    if (!acknowledged)
      klogf("[NVMe] %s CC.SHN did not complete; forcing disable\n",
            ctrl->name);
  }
  nvme_controller_disable(ctrl);
  return acknowledged;
}

void nvme_shutdown(void) {
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    uint32_t cc = nvme_read32(ctrl, NVME_REG_CC);
    if (!(cc & NVME_CC_EN))
      continue;
    /* Phase 6 diagnostics: how evenly the workload spread over queues. */
    klogf("NVME-QSTAT-CPU: %s cpu_seen=%llx apic_seen=%llx hint_hits=%llu "
          "multi=%llu none=%llu hints=%llu/%llu/%llu/%llu\n",
          ctrl->name, (unsigned long long)ctrl->cpu_seen,
          (unsigned long long)ctrl->apic_seen,
          (unsigned long long)ctrl->hint_hits,
          (unsigned long long)ctrl->nohint_multi,
          (unsigned long long)ctrl->nohint_none,
          (unsigned long long)ctrl->hint_cpu[0],
          (unsigned long long)ctrl->hint_cpu[1],
          (unsigned long long)ctrl->hint_cpu[2],
          (unsigned long long)ctrl->hint_cpu[3]);
    for (unsigned q = 0; q < ctrl->io_queue_count; q++)
      klogf("NVME-QSTAT: %s qid=%u submitted=%llu\n", ctrl->name,
            ctrl->io[q].q.qid, (unsigned long long)ctrl->io[q].submitted);
    klogf("NVME-QSTAT-WAIT: %s sleeps=%llu irqs=%llu/%llu/%llu/%llu\n",
          ctrl->name, (unsigned long long)ctrl->wait_blocks,
          (unsigned long long)ctrl->vector_irqs[0],
          (unsigned long long)ctrl->vector_irqs[1],
          (unsigned long long)ctrl->vector_irqs[2],
          (unsigned long long)ctrl->vector_irqs[3]);
    bool ok = nvme_controller_shutdown(ctrl);
    klogf("NVME-SHUTDOWN: %s SHST=%s\n", ctrl->name,
          ok ? "complete" : "timeout");
  }
}

static void nvme_controller_release(struct nvme_controller *ctrl) {
  bool stopped = nvme_controller_disable(ctrl);

  /* Return the vectors first: the map entry must be gone before the vector
   * number can be recycled by another device. */
  nvme_irq_map_clear(ctrl);
  if (ctrl->irq_active) {
    pci_irq_release(&ctrl->irq);
    ctrl->irq_active = false;
  }

  /*
   * Only free DMA pages once the controller has acknowledged CC.EN=0.  A
   * controller that still has RDY set may be executing a transfer into these
   * pages; leaking them is recoverable, handing them to the allocator while
   * the device writes is not.  Probe failure paths reach here, so the leak is
   * bounded by the number of failed controllers.
   */
  if (!stopped) {
    klogf("[NVMe] %s refused to quiesce during release; DMA pages retained\n",
          ctrl->name);
    return;
  }

  for (unsigned v = 0; v < NVME_MAX_QUEUES; v++) {
    nvme_free_prp_lists(&ctrl->io[v]);
    nvme_queue_free(&ctrl->io[v].q);
  }
  nvme_queue_free(&ctrl->admin);
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    if (ctrl->namespaces[i].rmw_buf) {
      dma_free_page(ctrl->namespaces[i].rmw_buf);
      ctrl->namespaces[i].rmw_buf = NULL;
    }
  }
  if (ctrl->id_virt) {
    dma_free_page(ctrl->id_virt);
    ctrl->id_virt = NULL;
  }
}

/* I/O completion plumbing -------------------------------------------------- */

/*
 * Consume every ready entry from the I/O completion queue.  The CQ is
 * single-consumer: cq_lock serializes head/phase updates.  Each completion
 * records a once-only verdict in cid_result[] and wakes the CID's waiter.
 *
 * `wake` is false only for the lost-interrupt injection build: the entry is
 * still consumed and the verdict recorded, but the wait queue is left alone,
 * forcing the waiter onto its scheduler-tick fallback.
 */
static void nvme_drain_cq(struct nvme_controller *ctrl, struct nvme_iovq *vq,
                          bool wake) {
  struct nvme_queue *q = &vq->q;
  /* During a reset the queue is being reinitialized; a concurrent waiter must
   * not touch the CQ memory until the new generation is live. */
  if (!q->valid || !ctrl->io_ready)
    return;

  spinlock_acquire(&vq->cq_lock);
  for (;;) {
    volatile nvme_completion_t *cqe = &q->cq[q->head];
    uint16_t status = cqe->status;
    if ((status & NVME_CQE_PHASE) != q->phase)
      break;

    /* Acquire fence: the device writes the CQE fields before toggling the
     * phase bit, so a matching phase guarantees cid/result are valid.  The
     * fence keeps the compiler from speculating those reads above the check
     * (x86 load-load order already preserves it on the CPU side). */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    uint16_t cid = cqe->cid;
    int verdict = NVME_CQE_SC(status) == NVME_SC_SUCCESS ? 1 : -1;

    if (cid < NVME_CID_MAX) {
      spinlock_acquire(&vq->lock);
      if (vq->cid_result[cid] == 0)
        vq->cid_result[cid] = verdict;
      spinlock_release(&vq->lock);
      if (wake)
        wait_queue_wake_all(&vq->cid_wq[cid]);
    }

    q->head = (uint16_t)((q->head + 1) % q->depth);
    if (q->head == 0)
      q->phase ^= 1u;
  }
  nvme_write32(ctrl, q->cq_db_off, q->head);
  spinlock_release(&vq->cq_lock);
}

/*
 * Completion interrupt.  Its vector was allocated per queue by
 * nvme_irq_setup(); the dispatch map says which queues a vector feeds (all of
 * them when the MSI-X table is smaller than the queue count).  INTx has a
 * shared line and no stable vector, so it drains every queue.
 *
 * With NVME_NOIRQ the driver never requests a handler, so the function is
 * compiled out to keep the -Wall -Wextra build clean.
 */
#ifndef NVME_NOIRQ
static void nvme_irq_handler(struct registers *regs) {
  uint8_t vector = (uint8_t)regs->int_no;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->irq_active || !ctrl->io_ready)
      continue;

#ifdef NVME_FAULT_DROP_IRQ
    const bool wake = false;
#else
    const bool wake = true;
#endif

    if (ctrl->irq.mode == PCI_IRQ_INTX) {
      for (unsigned q = 0; q < ctrl->io_queue_count; q++)
        nvme_drain_cq(ctrl, &ctrl->io[q], wake);
      continue;
    }

    struct nvme_vector_entry *entry = &nvme_vector_map[vector];
    if (entry->ctrl != ctrl)
      continue;
    ctrl->vector_irqs[vector % PCI_IRQ_MAX_VECTORS]++;
    for (unsigned q = 0; q < ctrl->io_queue_count; q++) {
      if (entry->queue_mask & (uint16_t)(1u << q))
        nvme_drain_cq(ctrl, &ctrl->io[q], wake);
    }
  }
}
#endif /* !NVME_NOIRQ */

/* Request one completion vector per queue, with the phase 4 fallbacks. */
static bool nvme_irq_setup(struct nvme_controller *ctrl) {
#ifdef NVME_NOIRQ
  (void)ctrl;
  return false;
#else
  uint32_t modes = PCI_IRQ_MODE_INTX;
#ifndef NVME_DISABLE_MSIX
  modes |= PCI_IRQ_MODE_MSIX;
#endif
#ifndef NVME_DISABLE_MSI
  modes |= PCI_IRQ_MODE_MSI;
#endif

  unsigned want = ctrl->wanted_queues ? ctrl->wanted_queues : 1;
  if (want > NVME_MAX_QUEUES)
    want = NVME_MAX_QUEUES;

  /* Prefer one MSI-X vector per queue; falls through to the single-vector
   * chain (MSI-X entry 0, MSI, INTx) when the table is too small. */
#ifndef NVME_DISABLE_MSIX
  if (want > 1) {
    isr_t handlers[NVME_MAX_QUEUES];
    uint8_t destinations[NVME_MAX_QUEUES];
    uint8_t bsp = (uint8_t)lapic_get_id();
    for (unsigned i = 0; i < want; i++) {
      handlers[i] = nvme_irq_handler;
      destinations[i] = bsp;
      /* Vector i targets CPU i when that CPU is known, so queue i's
       * completions are drained by the CPU that submits to it. */
      struct cpu_info *ci = cpu_get_info(i);
      if (ci && ci->status != CPU_STATUS_OFFLINE)
        destinations[i] = (uint8_t)ci->apic_id;
    }
    if (pci_irq_request_modes_routed(ctrl->pdev, &ctrl->irq, handlers,
                                     destinations, (uint16_t)want,
                                     PCI_IRQ_MODE_MSIX)) {
      ctrl->irq_active = true;
      klogf("[  OK  ] NVMe: %s completions: MSI-X %u vectors (one per queue)\n",
            ctrl->name, ctrl->irq.vector_count);
      klogf("NVME-IRQ: %s mode=msix vectors=%u queues=%u\n", ctrl->name,
            ctrl->irq.vector_count, want);
      for (unsigned i = 0; i < want; i++)
        klogf("NVME-IRQ-DEST: %s vec=%u apic=%u\n", ctrl->name,
              pci_irq_vector(&ctrl->irq, i), destinations[i]);
      return true;
    }
  }
#endif

  isr_t handler = nvme_irq_handler;
  if (!pci_irq_request_modes(ctrl->pdev, &ctrl->irq, &handler, 1,
                             (uint8_t)lapic_get_id(), modes))
    return false;

  ctrl->irq_active = true;
  if (ctrl->irq.mode == PCI_IRQ_MSIX) {
    klogf("[  OK  ] NVMe: %s completions: MSI-X vector=%u\n", ctrl->name,
          pci_irq_vector(&ctrl->irq, 0));
    klogf("NVME-IRQ: %s mode=msix vector=%u\n", ctrl->name,
          pci_irq_vector(&ctrl->irq, 0));
  } else if (ctrl->irq.mode == PCI_IRQ_MSI) {
    klogf("[  OK  ] NVMe: %s completions: MSI vector=%u\n", ctrl->name,
          pci_irq_vector(&ctrl->irq, 0));
    klogf("NVME-IRQ: %s mode=msi vector=%u\n", ctrl->name,
          pci_irq_vector(&ctrl->irq, 0));
  } else {
    klogf("[  OK  ] NVMe: %s completions: INTx line=%u\n", ctrl->name,
          (unsigned)ctrl->pdev->irq_line);
    klogf("NVME-IRQ: %s mode=intx line=%u\n", ctrl->name,
          (unsigned)ctrl->pdev->irq_line);
  }
  return true;
#endif
}

/* Take a completed verdict for `cid`; 0 means it is still outstanding. */
static int nvme_take_verdict(struct nvme_iovq *vq, uint32_t cid) {
  if (cid >= NVME_CID_MAX)
    return 0;
  spinlock_acquire(&vq->lock);
  int verdict = vq->cid_result[cid];
  if (verdict) {
    vq->cid_result[cid] = 0;
    vq->cid_in_use &= ~(1ull << cid);
  }
  spinlock_release(&vq->lock);
  return verdict;
}

/*
 * Whether the caller may sleep.  User I/O syscalls currently run with
 * interrupts masked (IA32_FMASK), so the polled path is the normal one; when
 * a caller has interrupts enabled the waiter may block and be woken from a
 * completion IRQ.  Early boot (nvme_runtime false) simply spins.
 */
static bool nvme_may_block(void) {
  if (!nvme_runtime)
    return false;
  struct thread *self = sched_get_current();
  if (!self || self->is_idle)
    return false;
  hal_irq_state_t flags = hal_irq_save();
  hal_irq_restore(flags);
  return (flags & SPINLOCK_RFLAGS_IF) != 0;
}

#ifdef NVME_RESET_RECOVERY
/*
 * Bring a fail-stopped controller back by resetting it.  Only one recovery
 * runs at a time; a thread that arrives while one is in flight waits for it
 * (observable through the generation change) instead of racing CC.EN.
 */
static bool nvme_recover(struct nvme_controller *ctrl) {
  if (!ctrl)
    return false;
  if (__atomic_exchange_n(&ctrl->recovering, 1, __ATOMIC_ACQ_REL)) {
    uint32_t generation = ctrl->generation;
    uint64_t deadline = lapic_timer_get_ms() + NVME_TIMEOUT_MS;
    while (__atomic_load_n(&ctrl->recovering, __ATOMIC_ACQUIRE) &&
           ctrl->generation == generation &&
           lapic_timer_get_ms() < deadline) {
      if (nvme_may_block())
        sched_yield();
      else
        hal_cpu_relax();
    }
    return !ctrl->failed && ctrl->started && ctrl->io_ready;
  }

  klogf("[NVMe] %s reset recovery after I/O transport failure\n", ctrl->name);
  nvme_controller_stop(ctrl);
  bool ok = nvme_controller_start(ctrl) == 0;
  if (!ok)
    nvme_fail_stop(ctrl, "reset recovery failed");
  __atomic_store_n(&ctrl->recovering, 0, __ATOMIC_RELEASE);
  return ok;
}
#endif

/*
 * Wait for a submitted command.  Returns 0 on success, -1 on a command status
 * error and -2 on a transport failure (timeout, controller reset/fatal).  Only
 * -2 may be retried by the recovery path.
 */
static int nvme_io_wait(struct nvme_controller *ctrl, struct nvme_iovq *vq,
                        uint32_t cid, uint32_t generation) {
  struct thread *self = sched_get_current();
  bool can_block = nvme_may_block();
  wait_queue_entry_t wqe = {.thread = self, .next = NULL};
  uint64_t deadline = lapic_timer_get_ms() + NVME_TIMEOUT_MS;

  for (;;) {
    /* A reset discarded every outstanding command; this CID's verdict will
     * never arrive and the new generation owns the queue state. */
    if (ctrl->generation != generation)
      return -2;

    int verdict = nvme_take_verdict(vq, cid);
    if (verdict)
      return verdict > 0 ? 0 : -1;

    nvme_drain_cq(ctrl, vq, true);
    verdict = nvme_take_verdict(vq, cid);
    if (verdict)
      return verdict > 0 ? 0 : -1;

    if (ctrl->failed)
      return -2;

    if (nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_CFS) {
      nvme_fail_stop(ctrl, "I/O CSTS.CFS");
      return -2;
    }

    if (lapic_timer_get_ms() >= deadline) {
      klogf("[NVMe] %s I/O timeout qid=%u cid=%u tail=%u head=%u phase=%u\n",
            ctrl->name, vq->q.qid, cid, vq->q.tail, vq->q.head, vq->q.phase);
      nvme_fail_stop(ctrl, "I/O command timeout");
      return -2;
    }

    if (!can_block) {
      hal_cpu_relax();
      continue;
    }

    /* Block first, then link: a completion that lands between linking and the
     * blocked-store must not be overwritten by it. */
    self->state = THREAD_BLOCKED;
    ctrl->wait_blocks++;
    wait_queue_add(&vq->cid_wq[cid], &wqe);
    if (__atomic_load_n(&vq->cid_result[cid], __ATOMIC_ACQUIRE) != 0) {
      self->state = THREAD_RUNNING;
      wait_queue_remove(&vq->cid_wq[cid], &wqe);
      continue;
    }
    self->wakeup_ticks = lapic_timer_get_ticks() + 10;
    sched_yield();
    self->wakeup_ticks = 0;
    self->state = THREAD_RUNNING;
    wait_queue_remove(&vq->cid_wq[cid], &wqe);
  }
}

/*
 * Build PRP1/PRP2 for a kernel virtual buffer.  Only PRP1 may carry an offset:
 * every following page starts at a page boundary, so the PRP list entries are
 * naturally aligned.  A list that needs more than one page is chained: the
 * last slot of a page points at the next list page with bit 0 set.  `cid`
 * selects the per-command list pool.
 */
static bool nvme_build_prp(struct nvme_iovq *vq, uint32_t cid, uint64_t va,
                           uint32_t bytes, uint64_t *prp1_out,
                           uint64_t *prp2_out) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  uint64_t *pml4 = (uint64_t *)cr3;

  /* Callers cap transfers, but the builder rejects anything oversized by
   * itself so a future caller cannot overflow `entries` or the NLB field. */
  if (bytes == 0 || bytes > NVME_MAX_TRANSFER)
    return false;

  uint64_t first = vmm_virt_to_phys(pml4, va);
  if (!first)
    return false;

  uint32_t first_len = NVME_PAGE_SIZE - (uint32_t)(va & (NVME_PAGE_SIZE - 1));
  *prp1_out = first;
  *prp2_out = 0;
  if (bytes <= first_len)
    return true;

  uint32_t remaining = bytes - first_len;
  uint64_t next = va + first_len;
  if (next < va) /* virtual address wrapped */
    return false;

  if (remaining <= NVME_PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, next);
    if (!phys)
      return false;
    *prp2_out = phys;
    return true;
  }

  uint32_t entries = (remaining + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
  if (entries > NVME_MAX_PRP_ENTRIES)
    return false;
  /* The last listed page must not wrap either; entries is small (<= 1534) but
   * `next` can start anywhere in the address space, so check explicitly. */
  if (next > UINT64_MAX - (uint64_t)(entries - 1) * NVME_PAGE_SIZE)
    return false;

  uint64_t *list = nvme_prp_page(vq, cid, 0);
  if (!list)
    return false;

  unsigned page = 0;
  uint32_t placed = 0;
  for (uint32_t i = 0; i < entries; i++) {
    /* Slot 511 is the chain link unless this is the final entry. */
    if (placed == NVME_PRP_LIST_ENTRIES - 1 && i + 1 < entries) {
      if (page + 1 >= NVME_PRP_PAGES)
        return false;
      uint64_t *next_page = nvme_prp_page(vq, cid, page + 1);
      if (!next_page)
        return false;
      list[placed] = vq->prp[cid].phys[page + 1] | 1u;
      page++;
      list = next_page;
      placed = 0;
    }

    uint64_t phys =
        vmm_virt_to_phys(pml4, next + (uint64_t)i * NVME_PAGE_SIZE);
    if (!phys)
      return false;
    list[placed++] = phys;
  }

  __atomic_thread_fence(__ATOMIC_RELEASE);
  *prp2_out = vq->prp[cid].phys[0];
  return true;
}

/*
 * Claim the first free CID on this queue.  The caller holds vq->lock.  The
 * scan is bounded by both the queue depth and NVME_CID_MAX, so the 64-bit
 * bitmap shift can never overflow; -1 means every CID is outstanding.
 */
static int nvme_cid_claim_locked(struct nvme_iovq *vq) {
  for (uint32_t i = 0; i < vq->q.depth && i < NVME_CID_MAX; i++) {
    if (!(vq->cid_in_use & (1ull << i))) {
      vq->cid_in_use |= 1ull << i;
      vq->cid_result[i] = 0;
      return (int)i;
    }
  }
  return -1;
}

/*
 * Reserve a CID on this queue, draining completions while it is saturated.
 * A controller that never completes anything must not spin forever: after
 * NVME_TIMEOUT_MS the controller is fail-stopped with a diagnostic instead.
 */
static int nvme_alloc_cid(struct nvme_controller *ctrl, struct nvme_iovq *vq) {
  uint64_t deadline = lapic_timer_get_ms() + NVME_TIMEOUT_MS;
  for (;;) {
    spinlock_acquire(&vq->lock);
    if (ctrl->failed || !ctrl->io_ready) {
      spinlock_release(&vq->lock);
      return -1;
    }
    int cid = nvme_cid_claim_locked(vq);
    uint64_t in_use = vq->cid_in_use;
    spinlock_release(&vq->lock);
    if (cid >= 0)
      return cid;

    nvme_drain_cq(ctrl, vq, true);
    if (lapic_timer_get_ms() >= deadline) {
      klogf("[NVMe] %s CID exhaustion qid=%u depth=%u in_use=%llx\n",
            ctrl->name, vq->q.qid, vq->q.depth,
            (unsigned long long)in_use);
      nvme_fail_stop(ctrl, "CID exhaustion");
      return -1;
    }
    if (nvme_may_block())
      sched_yield();
    else
      hal_cpu_relax();
  }
}

static void nvme_free_cid(struct nvme_iovq *vq, uint32_t cid) {
  if (cid >= NVME_CID_MAX)
    return;
  spinlock_acquire(&vq->lock);
  vq->cid_result[cid] = 0;
  vq->cid_in_use &= ~(1ull << cid);
  spinlock_release(&vq->lock);
}

/*
 * Copy a prepared command into this queue's submission queue and ring its
 * doorbell.  The generation re-check closes the race with a concurrent reset:
 * a command reserved against the old queue must not be appended to the new
 * one (the CID bitmap has already been cleared, so the caller abandons it).
 * Returns 0 when the command was submitted, -1 when it was not.
 */
static int nvme_sq_submit(struct nvme_controller *ctrl, struct nvme_iovq *vq,
                          uint16_t cid, const nvme_command_t *cmd,
                          uint32_t generation) {
  spinlock_acquire(&vq->lock);
  if (ctrl->generation != generation || ctrl->failed || !ctrl->io_ready) {
    spinlock_release(&vq->lock);
    return -1;
  }
  nvme_command_t *slot = &vq->q.sq[vq->q.tail];
  *slot = *cmd;
  slot->cid = cid;
  vq->q.tail = (uint16_t)((vq->q.tail + 1) % vq->q.depth);
  vq->submitted++;

  bool wedge = false;
  if (nvme_fault_wedge > 0) {
    nvme_fault_wedge--;
    wedge = true; /* test hook: swallow this command */
  }
  /* Same release ordering as the admin path: SQE + tail before doorbell. */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  if (!wedge)
    nvme_write32(ctrl, vq->q.sq_db_off, vq->q.tail);
  spinlock_release(&vq->lock);
  return 0;
}

/*
 * Submit one NVM Read/Write for a kernel virtual buffer.  `nlb` is in
 * logical blocks of the namespace; the caller has already capped the transfer
 * at ctrl->max_transfer and made it a whole number of logical blocks.
 * Returns 0 on success, -1 on a command status error and -2 on a transport
 * failure; only -2 is retried by the reset-recovery wrapper.
 */
static int nvme_io_direct_once(struct nvme_controller *ctrl,
                               struct nvme_namespace *ns, uint64_t slba,
                               uint32_t nlb, uint64_t va, bool write,
                               bool fua) {
  struct nvme_iovq *vq = nvme_current_vq(ctrl);
  if (!vq)
    return -2;
  /* NLB is a zero-based 16-bit field: 1..65536 blocks per command.  The
   * caller's chunking keeps this true; the check is a belt-and-braces guard
   * against a future caller building an unencodable command. */
  if (nlb == 0 || nlb > 0x10000u)
    return -1;
  uint32_t generation = ctrl->generation;
  int cid = nvme_alloc_cid(ctrl, vq);
  if (cid < 0)
    return -2;

  uint32_t bytes = nlb * ns->lba_size;
  uint64_t prp1 = 0, prp2 = 0;
  if (!nvme_build_prp(vq, (uint32_t)cid, va, bytes, &prp1, &prp2)) {
    /* The bitmap is only ours while the generation that handed it out is
     * current; after a reset it was cleared wholesale. */
    if (ctrl->generation == generation)
      nvme_free_cid(vq, (uint32_t)cid);
    static uint32_t prp_fail;
    if (__atomic_add_fetch(&prp_fail, 1, __ATOMIC_RELAXED) <= 8)
      klogf("[NVMe] %s PRP build failed: va=%llx bytes=%u\n", ctrl->name,
            (unsigned long long)va, bytes);
    return -1;
  }

  nvme_command_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = write ? NVME_CMD_WRITE : NVME_CMD_READ;
  cmd.nsid = ns->nsid;
  cmd.prp1 = prp1;
  cmd.prp2 = prp2;
  cmd.cdw10 = (uint32_t)(slba & 0xFFFFFFFFu);
  cmd.cdw11 = (uint32_t)(slba >> 32);
  cmd.cdw12 = ((nlb - 1u) & 0xFFFFu) |
              ((write && fua) ? NVME_RW_FUA : 0u); /* NLB is zero-based */

  if (nvme_sq_submit(ctrl, vq, (uint16_t)cid, &cmd, generation) != 0) {
    if (ctrl->generation == generation)
      nvme_free_cid(vq, (uint32_t)cid);
    return -2;
  }

  return nvme_io_wait(ctrl, vq, (uint32_t)cid, generation);
}

/* Re-issue a command after a reset when the build enables recovery. */
static int nvme_io_direct(struct nvme_controller *ctrl,
                          struct nvme_namespace *ns, uint64_t slba,
                          uint32_t nlb, uint64_t va, bool write, bool fua) {
  int rc = nvme_io_direct_once(ctrl, ns, slba, nlb, va, write, fua);
#ifdef NVME_RESET_RECOVERY
  if (rc == -2 && nvme_recover(ctrl))
    rc = nvme_io_direct_once(ctrl, ns, slba, nlb, va, write, fua);
#endif
  return rc;
}

/*
 * Read/write through a physically contiguous bounce buffer.  Used for user
 * pointers and buffers that are not 512-byte aligned, both of which cannot be
 * handed directly to the device.
 */
static int nvme_io_bounce(struct nvme_controller *ctrl,
                          struct nvme_namespace *ns, uint64_t slba,
                          uint32_t nlb, uint8_t *buf, bool write,
                          bool user_ptr, uint32_t chunk_bytes, bool fua) {
  uint32_t pages = (chunk_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  void *bounce_phys = pmm_alloc_pages(pages);
  if (!bounce_phys) {
    klogf("[NVMe] %s bounce allocation failed (%u bytes)\n", ctrl->name,
          chunk_bytes);
    return -1;
  }
  uint8_t *bounce =
      (uint8_t *)((uint64_t)bounce_phys + pmm_get_hhdm_offset());
  int rc = 0;

  /* Zero first: neither a device short-read nor a partially filled write path
   * may expose stale allocator contents through the bounce page. */
  memset(bounce, 0, (size_t)pages * PAGE_SIZE);

  if (write) {
    if (user_ptr) {
      if (copy_from_user(bounce, buf, chunk_bytes) != 0)
        rc = -1;
    } else {
      memcpy(bounce, buf, chunk_bytes);
    }
  }

  if (rc == 0)
    rc = nvme_io_direct(ctrl, ns, slba, nlb, (uint64_t)bounce, write, fua);

  if (rc == 0 && !write) {
    if (user_ptr) {
      if (copy_to_user(buf, bounce, chunk_bytes) != 0)
        rc = -1;
    } else {
      memcpy(buf, bounce, chunk_bytes);
    }
  }

  /*
   * Free the bounce once no command can still be writing to it.  A fail-stop
   * clears CC.EN and waits for CSTS.RDY=0 before returning, which sets
   * dma_quiesced; only a controller that refused to stop keeps the page (a
   * bounded leak beats a device writing into recycled memory).  On success
   * the completion itself is the evidence that the transfer is over.
   */
  if (rc == 0 || ctrl->dma_quiesced)
    pmm_free_pages(bounce_phys, pages);
  return rc;
}

/* Sub-LBA (4Kn and up) support -------------------------------------------- */

/*
 * Serialize partial-logical-block read-modify-write sequences per namespace.
 * Two partial writes to the same LBA that interleave (read, read, write,
 * write) would otherwise each restore stale bytes for the other's range.
 * The lock blocks on a wait queue; during early boot it spins instead.
 */
static void nvme_rmw_lock(struct nvme_namespace *ns) {
  for (;;) {
    spinlock_acquire(&ns->rmw_state);
    bool got = !ns->rmw_held;
    if (got)
      ns->rmw_held = true;
    spinlock_release(&ns->rmw_state);
    if (got)
      return;

    if (!nvme_may_block()) {
      hal_cpu_relax();
      continue;
    }

    struct thread *self = sched_get_current();
    wait_queue_entry_t wqe = {.thread = self, .next = NULL};
    self->state = THREAD_BLOCKED;
    wait_queue_add(&ns->rmw_wq, &wqe);
    /* Re-check after linking: a release raced between the first test and the
     * blocked store must still hand the lock over. */
    spinlock_acquire(&ns->rmw_state);
    got = !ns->rmw_held;
    if (got)
      ns->rmw_held = true;
    spinlock_release(&ns->rmw_state);
    if (!got) {
      self->wakeup_ticks = lapic_timer_get_ticks() + 10;
      sched_yield();
      self->wakeup_ticks = 0;
    }
    self->state = THREAD_RUNNING;
    wait_queue_remove(&ns->rmw_wq, &wqe);
    if (got)
      return;
  }
}

static void nvme_rmw_unlock(struct nvme_namespace *ns) {
  spinlock_acquire(&ns->rmw_state);
  ns->rmw_held = false;
  spinlock_release(&ns->rmw_state);
  wait_queue_wake_all(&ns->rmw_wq);
}

/*
 * Read or write `len` bytes contained in one logical block: read the block
 * into the namespace RMW page, patch or extract the requested slice, and for
 * writes put the whole block back.  Serialized by nvme_rmw_lock().
 */
static int nvme_io_partial(struct nvme_namespace *ns, uint64_t plba,
                           uint8_t *buf, uint32_t off, uint32_t len, bool write,
                           bool user_ptr, bool fua) {
  if (!ns->rmw_buf || len == 0 || off + len > ns->lba_size)
    return -1;

  struct nvme_controller *ctrl = ns->ctrl;
  uint64_t rmw_va = ns->rmw_phys + pmm_get_hhdm_offset();
  uint8_t *rmw = (uint8_t *)ns->rmw_buf;

  nvme_rmw_lock(ns);
  int rc = nvme_io_direct(ctrl, ns, plba, 1, rmw_va, false, false);
  if (rc == 0 && write) {
    if (user_ptr) {
      if (copy_from_user(rmw + off, buf, len) != 0)
        rc = -1;
    } else {
      memcpy(rmw + off, buf, len);
    }
    if (rc == 0)
      rc = nvme_io_direct(ctrl, ns, plba, 1, rmw_va, true, fua);
  } else if (rc == 0) {
    if (user_ptr) {
      if (copy_to_user(buf, rmw + off, len) != 0)
        rc = -1;
    } else {
      memcpy(buf, rmw + off, len);
    }
  }
  nvme_rmw_unlock(ns);
  return rc;
}

/* Submit `lbas` whole logical blocks, chunked to respect max_transfer. */
static int nvme_io_range(struct nvme_controller *ctrl,
                         struct nvme_namespace *ns, uint64_t plba,
                         uint64_t lbas, uint8_t *buf, bool write, bool fua) {
  uint32_t max_lbas = ctrl->max_transfer / ns->lba_size;
  if (max_lbas == 0)
    max_lbas = 1;

  while (lbas) {
    uint32_t chunk = lbas > max_lbas ? max_lbas : (uint32_t)lbas;
    int rc = nvme_io_direct(ctrl, ns, plba, chunk, (uint64_t)buf, write, fua);
    if (rc != 0)
      return rc;
    plba += chunk;
    buf += (uint64_t)chunk * ns->lba_size;
    lbas -= chunk;
  }
  return 0;
}

#ifdef NVME_FUA
#define NVME_FUA_DEFAULT true
#else
#define NVME_FUA_DEFAULT false
#endif

/*
 * Translate a request in 512-byte virtual sectors into namespace logical
 * blocks.  Fully covered blocks are handed to the device; partially covered
 * edges go through the RMW page, which is what makes 4Kn media usable while
 * still exposing a 512-byte block device to the filesystem layer.
 */
static int nvme_io(struct nvme_namespace *ns, uint64_t lba, uint32_t count,
                   uint8_t *buf, bool write, bool fua) {
  struct nvme_controller *ctrl = ns->ctrl;
  if (!ctrl || !ns->present || !ctrl->started || !ctrl->io_ready ||
      ctrl->failed || !buf)
    return -1;
  if (count == 0)
    return 0;
  if (lba >= ns->total_sectors ||
      (uint64_t)count > ns->total_sectors - lba)
    return -1;

  uint64_t total_bytes = (uint64_t)count * 512u;
  uint64_t byte_off = lba * 512u;
  bool user_ptr = is_user_ptr((uint64_t)buf);

  if ((byte_off & (ns->lba_size - 1)) == 0 &&
      (total_bytes & (ns->lba_size - 1)) == 0) {
    uint64_t plba = byte_off >> ns->lba_shift;
    uint64_t lbas = total_bytes >> ns->lba_shift;

    if (!user_ptr && ((uint64_t)buf & 0x1FFu) == 0)
      return nvme_io_range(ctrl, ns, plba, lbas, buf, write, fua);

    /* User or odd-aligned buffer: stage full logical blocks through the
     * physically contiguous bounce buffer. */
    uint64_t done = 0;
    while (done < total_bytes) {
      uint64_t chunk = total_bytes - done;
      if (chunk > ctrl->max_transfer)
        chunk = ctrl->max_transfer;
      chunk &= ~((uint64_t)ns->lba_size - 1);
      if (chunk == 0)
        chunk = ns->lba_size;
      uint32_t nlb = (uint32_t)(chunk >> ns->lba_shift);
      int rc = nvme_io_bounce(ctrl, ns, plba + (done >> ns->lba_shift), nlb,
                              buf + done, write, user_ptr, (uint32_t)chunk,
                              fua);
      if (rc != 0)
        return -1;
      done += chunk;
    }
    return 0;
  }

  uint64_t plba = byte_off >> ns->lba_shift;
  uint32_t off = (uint32_t)(byte_off & (ns->lba_size - 1));
  uint64_t done = 0;

  if (off) {
    uint64_t len = ns->lba_size - off;
    if (len > total_bytes)
      len = total_bytes;
    int rc = nvme_io_partial(ns, plba, buf + done, off, (uint32_t)len, write,
                             user_ptr, fua);
    if (rc != 0)
      return -1;
    done += len;
    plba++;
  }

  uint64_t full = (total_bytes - done) / ns->lba_size;
  if (full) {
    int rc = nvme_io_range(ctrl, ns, plba, full, buf + done, write, fua);
    if (rc != 0)
      return -1;
    done += full * ns->lba_size;
    plba += full;
  }

  if (done < total_bytes) {
    int rc = nvme_io_partial(ns, plba, buf + done, 0,
                             (uint32_t)(total_bytes - done), write, user_ptr,
                             fua);
    if (rc != 0)
      return -1;
  }
  return 0;
}

static int nvme_ns_read(struct block_device *dev, uint64_t lba, uint32_t count,
                        void *buf) {
  struct nvme_namespace *ns = nvme_ns_from_block(dev);
  return nvme_io(ns, lba, count, (uint8_t *)buf, false, false);
}

static int nvme_ns_write(struct block_device *dev, uint64_t lba,
                         uint32_t count, const void *buf) {
  struct nvme_namespace *ns = nvme_ns_from_block(dev);
  return nvme_io(ns, lba, count, (uint8_t *)buf, true, NVME_FUA_DEFAULT);
}

static int nvme_ns_write_fua(struct block_device *dev, uint64_t lba,
                             uint32_t count, const void *buf) {
  struct nvme_namespace *ns = nvme_ns_from_block(dev);
  return nvme_io(ns, lba, count, (uint8_t *)buf, true, true);
}

static int nvme_flush_once(struct nvme_controller *ctrl,
                           struct nvme_namespace *ns) {
  struct nvme_iovq *vq = nvme_current_vq(ctrl);
  if (!vq)
    return -2;
  uint32_t generation = ctrl->generation;
  int cid = nvme_alloc_cid(ctrl, vq);
  if (cid < 0)
    return -2;

  nvme_command_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_CMD_FLUSH;
  cmd.nsid = ns->nsid;

  if (nvme_sq_submit(ctrl, vq, (uint16_t)cid, &cmd, generation) != 0) {
    if (ctrl->generation == generation)
      nvme_free_cid(vq, (uint32_t)cid);
    return -2;
  }
  return nvme_io_wait(ctrl, vq, (uint32_t)cid, generation);
}

static int nvme_ns_flush(struct block_device *dev) {
  struct nvme_namespace *ns = nvme_ns_from_block(dev);
  struct nvme_controller *ctrl = ns ? ns->ctrl : NULL;
  if (!ctrl || !ctrl->started || !ctrl->io_ready || ctrl->failed)
    return -1;

  int rc = nvme_flush_once(ctrl, ns);
#ifdef NVME_RESET_RECOVERY
  if (rc == -2 && nvme_recover(ctrl))
    rc = nvme_flush_once(ctrl, ns);
#endif
  return rc;
}

/* Driver probe ------------------------------------------------------------ */

static int nvme_probe(struct device *dev) {
  if (nvme_count >= NVME_MAX_CONTROLLERS) {
    klogf("[NVMe] too many controllers, ignoring %s\n", dev->name);
    return -1;
  }

  struct pci_device *pci = nvme_pci_for_device(dev);
  if (!pci)
    return -1;

  uint64_t bar_phys = nvme_bar0_phys(pci);
  if (!bar_phys) {
    klogf("[NVMe] %s has no usable BAR0\n", dev->name);
    return -1;
  }

  uint64_t bar_size = nvme_bar0_size(dev);
  if (bar_size < NVME_BAR_MIN_SIZE)
    bar_size = NVME_BAR_MIN_SIZE;

  if (!nvme_map_mmio(bar_phys, bar_size)) {
    klogf("[NVMe] MMIO map failed for %s\n", dev->name);
    return -1;
  }

  struct nvme_controller *ctrl = &controllers[nvme_count];
  memset(ctrl, 0, sizeof(*ctrl));
  ctrl->pdev = pci;
  ctrl->bar_phys = bar_phys;
  ctrl->bar_size = bar_size;
  ctrl->regs = (volatile nvme_regs_t *)(bar_phys + pmm_get_hhdm_offset());
  ctrl->index = (uint8_t)nvme_count;
  snprintf(ctrl->name, sizeof(ctrl->name), "nvme%u", nvme_count);
  spinlock_init(&ctrl->admin_lock);
  for (unsigned v = 0; v < NVME_MAX_QUEUES; v++) {
    struct nvme_iovq *vq = &ctrl->io[v];
    spinlock_init(&vq->lock);
    spinlock_init(&vq->cq_lock);
    for (unsigned i = 0; i < NVME_CID_MAX; i++)
      wait_queue_init(&vq->cid_wq[i]);
  }
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    spinlock_init(&ctrl->namespaces[i].rmw_state);
    wait_queue_init(&ctrl->namespaces[i].rmw_wq);
  }

  /* One I/O queue pair per CPU, up to the driver and device limits; extra
   * CPUs share queues modulo the count that was actually created. */
  uint32_t ncpu = cpu_get_count();
  if (ncpu == 0)
    ncpu = 1;
  if (ncpu > NVME_MAX_QUEUES)
    ncpu = NVME_MAX_QUEUES;
  ctrl->wanted_queues = ncpu;

  ctrl->cap = nvme_read64(ctrl, NVME_REG_CAP);
  ctrl->vs = nvme_read32(ctrl, NVME_REG_VS);
  ctrl->dstrd = (uint8_t)NVME_CAP_DSTRD(ctrl->cap);

  if (!(NVME_CAP_CSS(ctrl->cap) & 1u)) {
    klogf("[NVMe] %s lacks the NVM command set (CSS=%02x), skipping\n",
          ctrl->name, (unsigned)NVME_CAP_CSS(ctrl->cap));
    return -1;
  }
  if (NVME_CAP_MPSMIN(ctrl->cap) > 0) {
    klogf("[NVMe] %s needs >4K host pages (MPSMIN=%u), unsupported\n",
          ctrl->name, (unsigned)NVME_CAP_MPSMIN(ctrl->cap));
    return -1;
  }

  /* Memory space enable + bus mastering.  INTx starts enabled so the
   * MSI-X/MSI/INTx request chain can choose a delivery mode; the polled
   * fallback disables the pin again below. */
  uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     (uint16_t)((command | (1u << 1) | (1u << 2)) &
                                ~(1u << 10)));

  if (!nvme_controller_disable(ctrl)) {
    klogf("[NVMe] %s did not quiesce (CC=%08x CSTS=%08x); not usable\n",
          ctrl->name, nvme_read32(ctrl, NVME_REG_CC),
          nvme_read32(ctrl, NVME_REG_CSTS));
    return -1;
  }

  /* Completions: MSI-X, then MSI, then INTx, else polled.  Setup happens
   * before the first enable so the I/O CQ is created with IEN=1 when a
   * vector is available. */
  if (!nvme_irq_setup(ctrl)) {
    command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                       (uint16_t)(command | (1u << 10)));
    klogf("[ INFO ] NVMe: %s no usable interrupt; polled completions\n",
          ctrl->name);
    klogf("NVME-IRQ: %s mode=poll\n", ctrl->name);
  }

  if (nvme_controller_start(ctrl) != 0) {
    klogf("[NVMe] %s bring-up failed; controller left disabled\n", ctrl->name);
    nvme_controller_release(ctrl);
    return -1;
  }

  nvme_log_controller(ctrl);
  klogf("[  OK  ] NVMe: %s %u I/O queue(s), %u completion vector(s)\n",
        ctrl->name, ctrl->io_queue_count,
        ctrl->irq_active ? ctrl->irq.vector_count : 0);
  klogf("NVME-QUEUES: %s io=%u vectors=%u wanted=%u\n", ctrl->name,
        ctrl->io_queue_count, ctrl->irq_active ? ctrl->irq.vector_count : 0,
        ctrl->wanted_queues);
  nvme_register_namespaces(ctrl);
  nvme_count++;
  return 0;
}

static struct device_id nvme_ids[] = {
    {.type = ID_PCI,
     .pci = {.match_class = true, .class = 0x01, .subclass = 0x08}}};

static struct driver nvme_driver = {.name = "nvme",
                                    .kind = DRIVER_KERNEL,
                                    .ids = nvme_ids,
                                    .id_count = 1,
                                    .probe = nvme_probe};

int nvme_init(void) {
  nvme_count = 0;
  nvme_runtime = false;
  dm_register_driver(&nvme_driver);

  if (nvme_count == 0) {
    console_puts(KLOG_CLR_YELLOW "[ INFO ]" KLOG_CLR_RESET
                           " NVMe: no controllers found.\n");
  } else {
    unsigned namespaces = nvme_namespace_count();
    klogf("[  OK  ] NVMe: %u controller(s), %u namespace(s) initialized.\n",
          nvme_count, namespaces);
  }
  nvme_runtime = true;
  return (int)nvme_count;
}

/* Controller/namespace census, used by the boot self-test and for logging. */

unsigned nvme_controller_count(void) { return nvme_count; }

unsigned nvme_namespace_count(void) {
  unsigned total = 0;
  for (unsigned c = 0; c < nvme_count; c++)
    total += controllers[c].ns_count;
  return total;
}

/* Self-test hooks --------------------------------------------------------- */

#ifdef NVME_SELFTEST

static int nvme_test_identify_ops_on(struct nvme_controller *ctrl,
                                     unsigned ops) {
  unsigned failures = 0;
  for (unsigned i = 0; i < ops; i++) {
    uint32_t cns;
    uint32_t nsid = 0;

    if ((i & 1u) == 0 || ctrl->ns_count == 0) {
      cns = NVME_ID_CNS_CONTROLLER;
    } else {
      cns = NVME_ID_CNS_NAMESPACE;
      nsid = ctrl->namespaces[(i / 2) % ctrl->ns_count].nsid;
    }

    uint16_t sc = 0;
    if (nvme_identify(ctrl, nsid, cns, &sc) != 0 || sc != NVME_SC_SUCCESS) {
      if (failures < 4)
        klogf("[NVMe] %s identify stress failed op=%u cns=%u sc=%02x\n",
              ctrl->name, i, cns, sc);
      failures++;
    }
  }
  return (int)failures;
}

/*
 * Forced-timeout fault injection (NVME_SELFTEST_FAULT_TIMEOUT=1): wedge one
 * Flush command by suppressing its submission doorbell, verify the waiter
 * times out after the normal 5 s, that the controller fail-stopped with
 * CC.EN cleared, then bring it back up so the rest of the boot continues.
 * NVME_SELFTEST_FAULT_INDEX selects a single controller (default: all) so the
 * two-controller isolation test can wedge one and check the other.
 */
#ifdef NVME_SELFTEST_FAULT_TIMEOUT
#ifndef NVME_SELFTEST_FAULT_INDEX
#define NVME_SELFTEST_FAULT_INDEX (~0u)
#endif

static int nvme_test_fault_timeout_on(struct nvme_controller *ctrl) {
  struct nvme_namespace *ns = NULL;
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    if (ctrl->namespaces[i].present && ctrl->namespaces[i].blkdev.flush) {
      ns = &ctrl->namespaces[i];
      break;
    }
  }
  if (!ns)
    return 0; /* no namespace: nothing to flush */

  klogf("[NVMe] %s fault-timeout test: wedging one Flush\n", ctrl->name);
#ifdef NVME_RESET_RECOVERY
  uint32_t generation = ctrl->generation;
#endif
  nvme_fault_wedge = 1;
  int rc = nvme_ns_flush(&ns->blkdev);
  nvme_fault_wedge = 0;

#ifdef NVME_RESET_RECOVERY
  /* With recovery compiled in, the timeout resets the controller and the
   * retried Flush must succeed on the fresh generation. */
  if (rc != 0) {
    klogf("[NVMe] %s fault-timeout test: wedged Flush not recovered (rc=%d)\n",
          ctrl->name, rc);
    return 1;
  }
  if (ctrl->failed || !ctrl->io_ready ||
      ctrl->generation == generation) {
    klogf("[NVMe] %s fault-timeout test: recovery state wrong "
          "(failed=%d started=%d gen=%u/%u)\n",
          ctrl->name, (int)ctrl->failed, (int)ctrl->started,
          ctrl->generation, generation);
    return 1;
  }
  klogf("[  OK  ] NVMe: %s fault-timeout test: timeout recovered by reset\n",
        ctrl->name);
  return 0;
#else
  if (rc == 0) {
    klogf("[NVMe] %s fault-timeout test: wedged command unexpectedly succeeded\n",
          ctrl->name);
    return 1;
  }
  if (!ctrl->failed) {
    klogf("[NVMe] %s fault-timeout test: controller was not fail-stopped\n",
          ctrl->name);
    return 1;
  }
  if (nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) {
    klogf("[NVMe] %s fault-timeout test: CSTS.RDY still set after timeout\n",
          ctrl->name);
    return 1;
  }

  nvme_controller_stop(ctrl);
  if (nvme_controller_start(ctrl) != 0) {
    klogf("[NVMe] %s fault-timeout test: restart after fail-stop failed\n",
          ctrl->name);
    return 1;
  }
  klogf("[  OK  ] NVMe: %s fault-timeout test: clean fail-stop verified\n",
        ctrl->name);
  return 0;
#endif
}
#endif /* NVME_SELFTEST_FAULT_TIMEOUT */

int nvme_test_admin_cycles(unsigned cycles, unsigned identify_ops) {
  unsigned failures = 0;

  for (unsigned cycle = 0; cycle < cycles; cycle++) {
    for (unsigned c = 0; c < nvme_count; c++) {
      struct nvme_controller *ctrl = &controllers[c];
      nvme_controller_stop(ctrl);
      if (nvme_controller_start(ctrl) != 0) {
        klogf("[NVMe] %s restart failed at cycle %u\n", ctrl->name, cycle);
        failures++;
        continue;
      }
      if (!ctrl->started || !ctrl->io_ready || ctrl->io_queue_count == 0 ||
          ctrl->io[0].q.depth < 2) {
        klogf("[NVMe] %s inconsistent state after cycle %u\n", ctrl->name,
              cycle);
        failures++;
      }
    }
  }

  if (identify_ops) {
    for (unsigned c = 0; c < nvme_count; c++) {
      struct nvme_controller *ctrl = &controllers[c];
      if (!ctrl->started && nvme_controller_start(ctrl) != 0) {
        failures++;
        continue;
      }
      failures += nvme_test_identify_ops_on(ctrl, identify_ops);
    }
  }

#ifdef NVME_SELFTEST_FAULT_TIMEOUT
  for (unsigned c = 0; c < nvme_count; c++) {
    if (NVME_SELFTEST_FAULT_INDEX != ~0u &&
        c != (unsigned)NVME_SELFTEST_FAULT_INDEX)
      continue;
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0) {
      failures++;
      continue;
    }
    failures += nvme_test_fault_timeout_on(ctrl);

    /* Per-controller isolation: wedging one controller must not disturb any
     * other controller's queue state. */
    for (unsigned j = 0; j < nvme_count; j++) {
      if (j == c)
        continue;
      struct nvme_controller *peer = &controllers[j];
      if (!peer->started || !peer->io_ready || peer->failed) {
        klogf("[NVMe] fault isolation: %s disturbed by %s fail-stop\n",
              peer->name, ctrl->name);
        failures++;
      }
    }
  }
#endif

  /* Make sure the controllers are operational for the root mount that
   * follows the self-test. */
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0)
      failures++;
  }
  return failures ? -1 : 0;
}

/* Phase 5 self-tests ------------------------------------------------------ */

/*
 * Pattern helpers for the destructive data tests.  Compiled only with
 * NVME_SELFTEST_DATA so a normal NVME_SELFTEST build stays warning-free under
 * -Wall -Wextra (the only callers are inside that guard).
 */
#ifdef NVME_SELFTEST_DATA
static void nvme_test_fill(uint8_t *buf, size_t bytes, uint8_t seed) {
  for (size_t i = 0; i < bytes; i++)
    buf[i] = (uint8_t)(seed + (uint8_t)(i * 7u) + (uint8_t)(i >> 8));
}

static bool nvme_test_check(const uint8_t *buf, size_t bytes, uint8_t seed,
                            const char *what, const char *name) {
  for (size_t i = 0; i < bytes; i++) {
    uint8_t expect = (uint8_t)(seed + (uint8_t)(i * 7u) + (uint8_t)(i >> 8));
    if (buf[i] != expect) {
      klogf("[NVMe] %s %s: mismatch at byte %zu (got %02x want %02x)\n", name,
            what, i, buf[i], expect);
      return false;
    }
  }
  return true;
}
#endif /* NVME_SELFTEST_DATA */

/*
 * PRP chain builder check: a 4 MiB span is described by three chained list
 * pages (the layout a transfer above ~2 MiB produces).  Every data entry and
 * both link entries are validated against the buffer's physical pages.
 */
static int nvme_test_prp_chain_on(struct nvme_controller *ctrl) {
  const uint32_t bytes = 4u * 1024 * 1024;
  const uint32_t pages = bytes / NVME_PAGE_SIZE;
  void *buf_phys = pmm_alloc_pages(pages);
  if (!buf_phys) {
    klogf("[NVMe] %s PRP chain test: %u-page allocation failed\n", ctrl->name,
          pages);
    return 1;
  }

  uint64_t phys = (uint64_t)buf_phys;
  /* Deliberately 512-byte aligned but page-misaligned so PRP1 carries an
   * offset and the worst-case entry count is tested. */
  uint64_t va = phys + pmm_get_hhdm_offset() + 512;
  struct nvme_iovq *vq = &ctrl->io[0];
  int failures = 0;
  uint64_t prp1 = 0, prp2 = 0;

  if (!nvme_build_prp(vq, 0, va, bytes - 512, &prp1, &prp2)) {
    klogf("[NVMe] %s PRP chain test: build failed\n", ctrl->name);
    failures++;
  } else if (prp1 != phys + 512) {
    klogf("[NVMe] %s PRP chain test: PRP1=%llx want %llx\n", ctrl->name,
          (unsigned long long)prp1, (unsigned long long)(phys + 512));
    failures++;
  } else {
    uint32_t first_len = NVME_PAGE_SIZE - 512;
    uint32_t entries =
        ((bytes - 512) - first_len + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
    uint64_t expect = phys + 512 + first_len;
    uint64_t *list = vq->prp[0].page[0];
    unsigned page_idx = 0;
    unsigned slot = 0;
    uint32_t seen = 0;

    while (seen < entries && failures == 0) {
      if (slot == NVME_PRP_LIST_ENTRIES - 1 && seen + 1 < entries) {
        /* More entries follow: this slot must be a link to the next page. */
        uint64_t link = list[slot];
        if (!(link & 1u) || page_idx + 1 >= NVME_PRP_PAGES ||
            (link & ~1ull) != vq->prp[0].phys[page_idx + 1]) {
          klogf("[NVMe] %s PRP chain test: bad link at page %u\n", ctrl->name,
                page_idx);
          failures++;
          break;
        }
        page_idx++;
        list = vq->prp[0].page[page_idx];
        if (!list) {
          failures++;
          break;
        }
        slot = 0;
        continue;
      }
      if (list[slot] != expect) {
        klogf("[NVMe] %s PRP chain test: entry %u=%llx want %llx\n", ctrl->name,
              seen, (unsigned long long)list[slot],
              (unsigned long long)expect);
        failures++;
        break;
      }
      expect += NVME_PAGE_SIZE;
      seen++;
      slot++;
    }
    if (failures == 0)
      klogf("[  OK  ] NVMe: %s PRP chain: %u entries over %u list page(s)\n",
            ctrl->name, entries, page_idx + 1);
  }

  pmm_free_pages(buf_phys, pages);
  return failures;
}

int nvme_test_prp_chain(void) {
  int failures = 0;
  unsigned ran = 0;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0) {
      failures++;
      continue;
    }
    failures += nvme_test_prp_chain_on(ctrl);
    ran++;
  }
  return ran ? failures : -1;
}

/*
 * Graceful shutdown test: CC.SHN must complete (CSTS.SHST=2), CC.EN clears,
 * and a re-enable must bring the controller back with a fresh generation.
 */
int nvme_test_shutdown_cycle(void) {
  if (nvme_count == 0)
    return -1;

  int failures = 0;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0) {
      failures++;
      continue;
    }
    if (!nvme_controller_shutdown(ctrl)) {
      klogf("[NVMe] %s SHN test: CSTS.SHST never completed\n", ctrl->name);
      failures++;
    }
    if (nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) {
      klogf("[NVMe] %s SHN test: CSTS.RDY set after shutdown\n", ctrl->name);
      failures++;
    }
    if (nvme_controller_start(ctrl) != 0 || !ctrl->io_ready || !ctrl->started) {
      klogf("[NVMe] %s SHN test: re-enable failed\n", ctrl->name);
      failures++;
    }
  }
  if (failures == 0)
    klogf("[  OK  ] NVMe: graceful CC.SHN verified on %u controller(s)\n",
          nvme_count);
  return failures;
}

/* Destructive data tests (only for scratch media). */
#ifdef NVME_SELFTEST_DATA

/* 4Kn edge coverage: RMW preservation, cross-LBA requests, whole-LBA writes
 * and the namespace tail, on every namespace with a logical block > 512. */
static int nvme_test_4kn_ns(struct nvme_namespace *ns) {
  struct block_device *dev = &ns->blkdev;
  struct nvme_controller *ctrl = ns->ctrl;
  uint32_t spb = ns->lba_size / 512; /* virtual sectors per logical block */
  uint64_t total = ns->total_sectors;
  if (spb < 2 || total < (uint64_t)spb * 6)
    return 0;

  size_t save_bytes = (size_t)ns->lba_size * 2;
  size_t span = (size_t)ns->lba_size * 3 + 512;
  uint8_t *save = kmalloc(save_bytes);
  uint8_t *work = kmalloc(span);
  if (!save || !work) {
    kfree(save);
    kfree(work);
    klogf("[NVMe] %s 4Kn test: buffer allocation failed\n", ctrl->name);
    return 1;
  }

  int failures = 0;
  if (dev->read_sectors(dev, 0, spb * 2, save) != 0) {
    failures++;
    goto out;
  }

  /* a) one partial first sector */
  nvme_test_fill(work, 512, 0xA1);
  if (dev->write_sectors(dev, 0, 1, work) != 0 ||
      dev->read_sectors(dev, 0, 1, work) != 0 ||
      !nvme_test_check(work, 512, 0xA1, "4Kn partial write", ctrl->name))
    failures++;

  /* b) the last sector of LBA 0 must not clobber the first */
  nvme_test_fill(work, 512, 0xB2);
  if (dev->write_sectors(dev, spb - 1, 1, work) != 0) {
    failures++;
  } else {
    if (dev->read_sectors(dev, 0, 1, work) != 0 ||
        !nvme_test_check(work, 512, 0xA1, "4Kn RMW head preserved",
                         ctrl->name))
      failures++;
    if (dev->read_sectors(dev, spb - 1, 1, work) != 0 ||
        !nvme_test_check(work, 512, 0xB2, "4Kn RMW tail", ctrl->name))
      failures++;
  }

  /* c) request crossing a logical-block boundary */
  nvme_test_fill(work, 1024, 0xC3);
  if (dev->write_sectors(dev, spb - 1, 2, work) != 0 ||
      dev->read_sectors(dev, spb - 1, 2, work) != 0 ||
      !nvme_test_check(work, 1024, 0xC3, "4Kn cross-LBA", ctrl->name))
    failures++;

  /* d) aligned whole-LBA write */
  nvme_test_fill(work, ns->lba_size, 0xD4);
  if (dev->write_sectors(dev, (uint64_t)2 * spb, spb, work) != 0 ||
      dev->read_sectors(dev, (uint64_t)2 * spb, spb, work) != 0 ||
      !nvme_test_check(work, ns->lba_size, 0xD4, "4Kn full LBA", ctrl->name))
    failures++;

  /* e) misaligned span: partial head + whole blocks + partial tail */
  uint64_t start = (uint64_t)3 * spb + 1;
  nvme_test_fill(work, span, 0xE5);
  if (dev->write_sectors(dev, start, (uint32_t)(span / 512), work) != 0 ||
      dev->read_sectors(dev, start, (uint32_t)(span / 512), work) != 0 ||
      !nvme_test_check(work, span, 0xE5, "4Kn RMW span", ctrl->name))
    failures++;

  /* f) final virtual sector of the namespace */
  nvme_test_fill(work, 512, 0xF6);
  if (dev->write_sectors(dev, total - 1, 1, work) != 0 ||
      dev->read_sectors(dev, total - 1, 1, work) != 0 ||
      !nvme_test_check(work, 512, 0xF6, "4Kn last sector", ctrl->name))
    failures++;

  /* Put back the two logical blocks the test borrowed. */
  if (dev->write_sectors(dev, 0, spb * 2, save) != 0)
    failures++;

out:
  kfree(work);
  kfree(save);
  return failures;
}

#endif /* NVME_SELFTEST_DATA */

int nvme_test_4kn(void) {
#ifdef NVME_SELFTEST_DATA
  int failures = 0;
  unsigned ran = 0;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0)
      continue;
    for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
      struct nvme_namespace *ns = &ctrl->namespaces[i];
      if (!ns->present || !ns->blkdev.read_sectors || ns->lba_size <= 512)
        continue;
      ran++;
      failures += nvme_test_4kn_ns(ns);
    }
  }
  return ran ? failures : -1;
#else
  return -1; /* needs NVME_SELFTEST_DATA (destructive) */
#endif
}

/*
 * Multi-namespace isolation: give every registered namespace a unique first
 * sector and verify each one afterwards.  A command that lands on the wrong
 * namespace (queue/NSID mix-up) shows up as a mismatch.
 */
int nvme_test_multi_ns(void) {
#ifdef NVME_SELFTEST_DATA
  unsigned count = 0;
  for (unsigned c = 0; c < nvme_count; c++)
    for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++)
      if (controllers[c].namespaces[i].present &&
          controllers[c].namespaces[i].blkdev.read_sectors)
        count++;
  if (count < 2)
    return -1;

  int failures = 0;
  unsigned idx = 0;
  uint8_t *buf = kmalloc(512);
  if (!buf)
    return 1;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
      struct nvme_namespace *ns = &ctrl->namespaces[i];
      if (!ns->present || !ns->blkdev.read_sectors)
        continue;
      nvme_test_fill(buf, 512, (uint8_t)(0x40 + idx));
      if (ns->blkdev.write_sectors(&ns->blkdev, 0, 1, buf) != 0) {
        klogf("[NVMe] %s nsid=%u multi-NS write failed\n", ctrl->name, ns->nsid);
        failures++;
      }
      idx++;
    }
  }
  idx = 0;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
      struct nvme_namespace *ns = &ctrl->namespaces[i];
      if (!ns->present || !ns->blkdev.read_sectors)
        continue;
      if (ns->blkdev.read_sectors(&ns->blkdev, 0, 1, buf) != 0 ||
          !nvme_test_check(buf, 512, (uint8_t)(0x40 + idx), "multi-NS isolation",
                           ctrl->name))
        failures++;
      idx++;
    }
  }
  kfree(buf);
  if (failures == 0)
    klogf("[  OK  ] NVMe: multi-namespace isolation verified (%u namespaces)\n",
          count);
  return failures;
#else
  return -1; /* needs NVME_SELFTEST_DATA (destructive) */
#endif
}

/*
 * Reset recovery: wedge one data write so it times out; with recovery enabled
 * the driver must reset the controller, retry and produce the written data.
 */
int nvme_test_recovery(void) {
#if defined(NVME_SELFTEST_DATA) && defined(NVME_RESET_RECOVERY)
  int failures = 0;
  unsigned ran = 0;
  uint8_t *buf = kmalloc(512);
  if (!buf)
    return 1;

  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0)
      continue;
    struct nvme_namespace *ns = NULL;
    for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
      if (ctrl->namespaces[i].present && ctrl->namespaces[i].blkdev.read_sectors &&
          ctrl->namespaces[i].total_sectors >= 4096) {
        ns = &ctrl->namespaces[i];
        break;
      }
    }
    if (!ns)
      continue;

    uint32_t spb = ns->lba_size / 512;
    uint64_t vs = (ns->total_sectors / 2) & ~((uint64_t)spb - 1);
    if (vs == 0)
      continue;

    ran++;
    int rc = ns->blkdev.read_sectors(&ns->blkdev, vs, 1, buf);
    if (rc != 0) {
      failures++;
      continue;
    }
    uint8_t original[512];
    memcpy(original, buf, sizeof(original));

    nvme_test_fill(buf, 512, 0x77);
    uint32_t generation = ctrl->generation;
    klogf("[NVMe] %s recovery test: wedging one write at vsector %llu\n",
          ctrl->name, (unsigned long long)vs);
    nvme_fault_wedge = 1;
    rc = ns->blkdev.write_sectors(&ns->blkdev, vs, 1, buf);
    nvme_fault_wedge = 0;

    if (rc != 0 || ctrl->failed || !ctrl->io_ready ||
        ctrl->generation == generation) {
      klogf("[NVMe] %s recovery test: write not recovered "
            "(rc=%d failed=%d gen=%u/%u)\n",
            ctrl->name, rc, (int)ctrl->failed, ctrl->generation, generation);
      failures++;
      continue;
    }
    memset(buf, 0, 512);
    if (ns->blkdev.read_sectors(&ns->blkdev, vs, 1, buf) != 0 ||
        !nvme_test_check(buf, 512, 0x77, "recovery data", ctrl->name)) {
      failures++;
      continue;
    }
    if (ns->blkdev.write_sectors(&ns->blkdev, vs, 1, original) != 0)
      failures++;
  }

  kfree(buf);
  return ran ? failures : -1;
#else
  return -1; /* needs NVME_SELFTEST_DATA + NVME_RESET_RECOVERY */
#endif
}

/* Phase 7 audit ----------------------------------------------------------- */

/*
 * Non-destructive hardening checks.  They pin the Phase 7 fixes down with
 * executable assertions:
 *
 *  - namespace geometry (NSZE * units and the UINT64_MAX/512 byte-offset cap,
 *    LBA format and metadata rejection),
 *  - MDTS clamping (0 = unlimited, small values honored, hostile values and
 *    values at/above the shift limit capped without undefined behavior),
 *  - request bounds (past-end lba, count overflow, zero count, NULL buffer),
 *  - CID bitmap exhaustion and reuse,
 *  - PRP builder limits (zero, oversized and unresolvable transfers).
 *
 * Every request that could touch media is rejected by nvme_io() before it is
 * submitted, so the audit is safe to run on a root namespace.  Returns 0 on
 * pass and a positive failure count.
 */
static int nvme_test_audit_on(struct nvme_controller *ctrl) {
  int failures = 0;

  /* a) geometry arithmetic. */
  struct {
    uint64_t nsze;
    uint8_t lbads;
    uint16_t ms;
    bool ok;
  } geometry[] = {
      {1, 9, 0, true},
      {1, 12, 0, true},
      {4096, 9, 0, true},
      {0, 9, 0, false},        /* no blocks */
      {1, 8, 0, false},        /* LBA below 512 bytes */
      {1, 13, 0, false},       /* LBA above 4096 bytes */
      {1, 9, 8, false},        /* metadata unsupported */
      {UINT64_MAX, 12, 0, false}, /* NSZE * 8 would wrap */
      {(NVME_MAX_SECTORS / 8) + 1, 12, 0, false}, /* byte-offset cap */
  };
  for (size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); i++) {
    uint64_t sectors = 0;
    bool ok = nvme_ns_geometry(geometry[i].nsze, geometry[i].lbads,
                               geometry[i].ms, &sectors);
    if (ok != geometry[i].ok) {
      klogf("[NVMe] %s audit: geometry case %zu ok=%d want %d\n", ctrl->name, i,
            (int)ok, (int)geometry[i].ok);
      failures++;
      continue;
    }
    if (ok && sectors != (geometry[i].nsze << (geometry[i].lbads - 9u))) {
      klogf("[NVMe] %s audit: geometry case %zu sectors=%llu\n", ctrl->name, i,
            (unsigned long long)sectors);
      failures++;
    }
  }

  /* b) MDTS clamp: exact bytes for small values, cap for large/hostile. */
  if (nvme_max_transfer_for(0) != NVME_MAX_TRANSFER ||
      nvme_max_transfer_for(1) != 8u * 1024 ||
      nvme_max_transfer_for(7) != 512u * 1024 ||
      nvme_max_transfer_for(9) != 2u * 1024 * 1024 ||
      nvme_max_transfer_for(12) != NVME_MAX_TRANSFER ||
      nvme_max_transfer_for(64) != NVME_MAX_TRANSFER ||
      nvme_max_transfer_for(255) != NVME_MAX_TRANSFER) {
    klogf("[NVMe] %s audit: MDTS clamp failed (0=%u 9=%u 255=%u)\n",
          ctrl->name, nvme_max_transfer_for(0), nvme_max_transfer_for(9),
          nvme_max_transfer_for(255));
    failures++;
  }

  /* c) Request bounds on every live namespace.  All four calls are decided
   * before submission, so this never reads or writes media. */
  uint8_t *probe = kmalloc(512);
  if (!probe)
    return failures + 1;
  for (unsigned i = 0; i < NVME_MAX_NAMESPACES; i++) {
    struct nvme_namespace *ns = &ctrl->namespaces[i];
    if (!ns->present || !ns->blkdev.read_sectors)
      continue;
    if (ns->total_sectors > NVME_MAX_SECTORS) {
      klogf("[NVMe] %s nsid=%u audit: sector count above the cap\n",
            ctrl->name, ns->nsid);
      failures++;
      continue;
    }
    if (nvme_io(ns, ns->total_sectors, 1, probe, false, false) != -1) {
      klogf("[NVMe] %s nsid=%u audit: past-end read not rejected\n",
            ctrl->name, ns->nsid);
      failures++;
    }
    if (nvme_io(ns, 0, 0, probe, false, false) != 0) {
      klogf("[NVMe] %s nsid=%u audit: zero-count request not a no-op\n",
            ctrl->name, ns->nsid);
      failures++;
    }
    if ((uint64_t)UINT32_MAX > ns->total_sectors &&
        nvme_io(ns, 0, UINT32_MAX, probe, false, false) != -1) {
      klogf("[NVMe] %s nsid=%u audit: count overflow not rejected\n",
            ctrl->name, ns->nsid);
      failures++;
    }
    if (nvme_io(ns, 0, 1, NULL, false, false) != -1) {
      klogf("[NVMe] %s nsid=%u audit: NULL buffer not rejected\n", ctrl->name,
            ns->nsid);
      failures++;
    }
  }
  kfree(probe);

  /* d) CID bitmap: claim every CID, prove exhaustion is detected, then prove
   * the first CID is reusable after the bitmap is cleared. */
  struct nvme_iovq *vq = &ctrl->io[0];
  if (ctrl->io_queue_count && vq->q.valid && vq->q.depth) {
    spinlock_acquire(&vq->lock);
    uint64_t saved = vq->cid_in_use;
    vq->cid_in_use = 0;
    unsigned claimed = 0;
    while (nvme_cid_claim_locked(vq) >= 0)
      claimed++;
    bool full_detected =
        claimed >= vq->q.depth && nvme_cid_claim_locked(vq) < 0;
    vq->cid_in_use = 0;
    int reused = nvme_cid_claim_locked(vq);
    vq->cid_in_use = saved;
    spinlock_release(&vq->lock);
    if (!full_detected) {
      klogf("[NVMe] %s audit: CID exhaustion not detected (%u/%u)\n",
            ctrl->name, claimed, vq->q.depth);
      failures++;
    }
    if (reused < 0) {
      klogf("[NVMe] %s audit: CID reuse after free failed\n", ctrl->name);
      failures++;
    }
  }

  /* e) PRP builder limits: zero, oversized and unmapped va are rejected
   * before any list page or page table walk result is trusted. */
  if (ctrl->io_queue_count && vq->q.valid && ctrl->id_virt) {
    uint64_t prp1 = 0, prp2 = 0;
    if (nvme_build_prp(vq, 0, (uint64_t)ctrl->id_virt, 0, &prp1, &prp2) ||
        nvme_build_prp(vq, 0, (uint64_t)ctrl->id_virt,
                       NVME_MAX_TRANSFER + NVME_PAGE_SIZE, &prp1, &prp2) ||
        nvme_build_prp(vq, 0, 0, 512, &prp1, &prp2)) {
      klogf("[NVMe] %s audit: PRP builder accepted an invalid request\n",
            ctrl->name);
      failures++;
    }
  }

  if (failures == 0)
    klogf("[  OK  ] NVMe: %s hardening audit: geometry, MDTS, bounds, CID "
          "and PRP limits verified\n",
          ctrl->name);
  return failures;
}

int nvme_test_audit(void) {
  int failures = 0;
  unsigned ran = 0;
  for (unsigned c = 0; c < nvme_count; c++) {
    struct nvme_controller *ctrl = &controllers[c];
    if (!ctrl->started && nvme_controller_start(ctrl) != 0) {
      failures++;
      continue;
    }
    failures += nvme_test_audit_on(ctrl);
    ran++;
  }
  return ran ? failures : -1;
}

#endif /* NVME_SELFTEST */
