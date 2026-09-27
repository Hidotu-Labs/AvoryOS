#include "drivers/storage/ahci.h"
#include "arch/uaccess.h"
#include "cpu/irq.h"
#include "hal/hal.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "lib/tsc.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "drivers/manager/device.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATA_CMD_READ_DMA_EX 0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

// Private structures

struct ahci_drive {
  ahci_port_t *port;
  bool present;
  uint64_t total_sectors;
  char model[41];
  bool failed; /* Fail-stop after an uncertain command; never risk a later write. */
  struct block_device blkdev;
  spinlock_t lock; // Serializes slot ownership, issue and IRQ completion
  int port_index;
  /* Slot ownership (FREE/BUSY), changed only under drive->lock.  The thread
   * that issued the command is the only one that ever frees its slot. */
  volatile uint32_t slot_state[32];
  /* Per-command verdict: 0 pending, 1 done, -1 error.  Recorded exactly once
   * with a compare-exchange so a late IRQ cannot overwrite the result a waiter
   * already observed. */
  volatile int slot_result[32];
  wait_queue_t slot_wq[32]; /* per-slot completion waiters */
};

#define AHCI_SLOT_FREE 0u
#define AHCI_SLOT_BUSY 1u

/* PxIS / PxIE bits we care about. */
#define AHCI_PXIS_DHRS (1u << 0)
#define AHCI_PXIS_PSS (1u << 1)
#define AHCI_PXIS_DSS (1u << 2)
#define AHCI_PXIS_SDBS (1u << 3)
#define AHCI_PXIS_OFS (1u << 24)
#define AHCI_PXIS_INFS (1u << 26)
#define AHCI_PXIS_IFS (1u << 27)
#define AHCI_PXIS_HBDS (1u << 28)
#define AHCI_PXIS_HBFS (1u << 29)
#define AHCI_PXIS_TFES (1u << 30)

#define AHCI_PORT_IRQ_ENABLE                                                    \
  (AHCI_PXIS_DHRS | AHCI_PXIS_PSS | AHCI_PXIS_DSS | AHCI_PXIS_SDBS |           \
   AHCI_PXIS_OFS | AHCI_PXIS_INFS | AHCI_PXIS_IFS | AHCI_PXIS_HBDS |           \
   AHCI_PXIS_HBFS | AHCI_PXIS_TFES)

#define AHCI_TFD_BSY (1u << 7)
#define AHCI_TFD_DRQ (1u << 3)
#define AHCI_MAX_SECTORS_PER_CMD 8192u /* per command: 4 MiB */

/* Wait results. */
#define AHCI_WAIT_OK 0
#define AHCI_WAIT_ERROR (-1)
#define AHCI_WAIT_TIMEOUT (-2)     /* command wedged, port quiesced */
#define AHCI_WAIT_UNQUIESCED (-3)  /* HBA may still own the buffer */

_Static_assert(sizeof(ahci_command_table_t) == 256,
               "AHCI command table must fit the 256-byte command slot");

static ahci_hba_mem_t *hba;
static struct ahci_drive ahci_drives[32];
static int ahci_drive_count = 0;
static bool ahci_irq_active = false;

// Helpers

static void print_uint64(uint64_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

// AHCI Control

static int find_cmdslot(ahci_port_t *port) {
  // If not set, bit isn't set in SACT and CI
  uint32_t slots = (port->sact | port->ci);
  // Hardcoded 32 slots limit
  for (int i = 0; i < 32; i++) {
    if ((slots & (1 << i)) == 0)
      return i;
  }
  return -1;
}

static void stop_cmd(ahci_port_t *port) {
  // Clear ST (Start)
  port->cmd &= ~AHCI_CMD_ST;
  // Clear FRE (FIS Receive Enable)
  port->cmd &= ~AHCI_CMD_FRE;

  // Wait until FR (FIS Receive Running) and CR (Command List Running) are
  // cleared
  while (1) {
    if (port->cmd & AHCI_CMD_FR)
      continue;
    if (port->cmd & AHCI_CMD_CR)
      continue;
    break;
  }
}

static void start_cmd(ahci_port_t *port) {
  // Wait until CR (Command List Running) is cleared
  while (port->cmd & AHCI_CMD_CR)
    ;

  // Set FRE (FIS Receive Enable) and ST (Start)
  port->cmd |= AHCI_CMD_FRE;
  port->cmd |= AHCI_CMD_ST;
}

/* Stop DMA before releasing memory named by a failed command table. */
static bool quiesce_port(ahci_port_t *port) {
  port->cmd &= ~AHCI_CMD_ST;
  uint64_t deadline = lapic_timer_get_ms() + 1000;
  while (port->cmd & AHCI_CMD_CR) {
    if (lapic_timer_get_ms() >= deadline)
      return false;
    hal_cpu_relax();
  }
  port->cmd &= ~AHCI_CMD_FRE;
  deadline = lapic_timer_get_ms() + 1000;
  while (port->cmd & AHCI_CMD_FR) {
    if (lapic_timer_get_ms() >= deadline)
      return false;
    hal_cpu_relax();
  }
  return true;
}

static void port_rebase(ahci_port_t *port) {
  stop_cmd(port);

  // Command list offset: 1K per port
  void *phys_clb = pmm_alloc(); // alloc 1 page (4K) for simplicity
  memset((void *)((uint64_t)phys_clb + pmm_get_hhdm_offset()), 0, 1024);
  port->clb = (uint32_t)(uint64_t)phys_clb;
  port->clbu = (uint32_t)((uint64_t)phys_clb >> 32);

  // FIS offset: 256 bytes per port - just use the second KB of the same page
  uint64_t phys_fb = (uint64_t)phys_clb + 1024;
  memset((void *)(phys_fb + pmm_get_hhdm_offset()), 0, 256);
  port->fb = (uint32_t)phys_fb;
  port->fbu = (uint32_t)(phys_fb >> 32);

  // Command table offset: 256 bytes per command table, 32 commands
  // We need 256 * 32 = 8K per port. Alloc 2 pages.
  void *phys_ctba_base = pmm_alloc_blocks(2);
  memset((void *)((uint64_t)phys_ctba_base + pmm_get_hhdm_offset()), 0, 8192);

  ahci_command_header_t *cmdheader =
      (ahci_command_header_t *)((uint64_t)phys_clb + pmm_get_hhdm_offset());
  for (int i = 0; i < 32; i++) {
    cmdheader[i].prdtl =
        8; // Max 8 PRDT entries per command slot for our static struct size
    uint64_t phys_ctba = (uint64_t)phys_ctba_base + (256 * i);
    cmdheader[i].ctba = (uint32_t)phys_ctba;
    cmdheader[i].ctbau = (uint32_t)(phys_ctba >> 32);
  }

  start_cmd(port);
}

// AHCI Command IO

static ahci_command_header_t *ahci_command_header(ahci_port_t *port, int slot) {
  uint64_t clb_addr = ((uint64_t)port->clbu << 32) | port->clb;
  return &((ahci_command_header_t *)(clb_addr + pmm_get_hhdm_offset()))[slot];
}

static ahci_command_table_t *ahci_command_table(ahci_command_header_t *header) {
  uint64_t ctba_addr = ((uint64_t)header->ctbau << 32) | header->ctba;
  return (ahci_command_table_t *)(ctba_addr + pmm_get_hhdm_offset());
}

/*
 * Fill PRDT entries for a physically scattered buffer.
 *
 * Walks the buffer page by page through the active page tables, coalescing
 * physically contiguous pages into single descriptors.  Each descriptor is
 * limited to 4 MiB (the 22-bit DBC field) and must not cross a 4 GiB physical
 * boundary.  The command table only has AHCI_PRDT_MAX_ENTRIES slots, so the
 * caller may need several commands for a large buffer.
 *
 * Returns the number of bytes covered, or 0 when a page is not mapped.  The
 * caller guarantees a sector-aligned buffer, so the covered byte count is
 * always a multiple of 512.
 */
static uint32_t ahci_build_prdt(ahci_command_table_t *table, uint64_t cr3,
                                uint64_t va, uint32_t max_bytes,
                                uint32_t *out_entries) {
  uint32_t covered = 0;
  uint32_t entries = 0;

  while (covered < max_bytes && entries < AHCI_PRDT_MAX_ENTRIES) {
    uint64_t cur_va = va + covered;
    uint64_t phys = vmm_virt_to_phys((uint64_t *)cr3, cur_va);
    if (!phys)
      return 0;

    uint32_t remaining = max_bytes - covered;
    uint32_t run = PAGE_SIZE - (uint32_t)(cur_va & (PAGE_SIZE - 1));
    if (run > remaining)
      run = remaining;

    /* Extend the descriptor across physically contiguous pages. */
    while (run < remaining && run < AHCI_PRDT_MAX_BYTES) {
      uint64_t end_phys = phys + run;
      if (((end_phys - 1) >> 32) != (phys >> 32))
        break; /* would cross a 4 GiB boundary */
      if (vmm_virt_to_phys((uint64_t *)cr3, cur_va + run) != end_phys)
        break; /* physically discontiguous */

      uint32_t step = PAGE_SIZE;
      if (step > remaining - run)
        step = remaining - run;
      if (step > AHCI_PRDT_MAX_BYTES - run)
        step = AHCI_PRDT_MAX_BYTES - run;
      run += step;
    }

    table->prdt_entry[entries].dba = (uint32_t)phys;
    table->prdt_entry[entries].dbau = (uint32_t)(phys >> 32);
    table->prdt_entry[entries].dbc = run - 1;
    table->prdt_entry[entries].i = 0;
    entries++;
    covered += run;
  }

  if (entries)
    table->prdt_entry[entries - 1].i = 1; /* interrupt on completion */
  *out_entries = entries;
  return covered;
}

/* HBA interrupt: wake waiters whose command slots have completed. */
static void ahci_irq_handler(struct registers *regs) {
  (void)regs;
  if (!hba)
    return;

  uint32_t hba_is = hba->is;
  if (!hba_is)
    return;

  for (int i = 0; i < 32; i++) {
    if (!(hba_is & (1u << i)))
      continue;

    ahci_port_t *port = &hba->ports[i];

    struct ahci_drive *drive = NULL;
    for (int d = 0; d < ahci_drive_count; d++) {
      if (ahci_drives[d].port_index == i) {
        drive = &ahci_drives[d];
        break;
      }
    }
    if (!drive)
      continue;

    /* Serialize with the issue path, which holds this lock across
     * alloc-slot -> fill PRDT -> set CI.  Without it the handler can observe a
     * slot between BUSY and its CI bit and attribute a stale IS from the
     * previous command to the new one. */
    spinlock_acquire(&drive->lock);

    uint32_t is = port->is;
    if (!is) {
      spinlock_release(&drive->lock);
      continue;
    }
    port->is = is; /* write-1-to-clear */

    bool error = (is & (AHCI_PXIS_TFES | AHCI_PXIS_HBDS | AHCI_PXIS_HBFS |
                        AHCI_PXIS_IFS | AHCI_PXIS_INFS | AHCI_PXIS_OFS)) ||
                 (port->tfd & 0x01);
    uint32_t ci = port->ci;

    for (int s = 0; s < 32; s++) {
      if (__atomic_load_n(&drive->slot_state[s], __ATOMIC_ACQUIRE) !=
          AHCI_SLOT_BUSY)
        continue;
      if (ci & (1u << s))
        continue; /* hardware still owns this slot */

      /* Record the verdict exactly once; the waiter may already have polled
       * CI and set it itself. */
      int expected = 0;
      __atomic_compare_exchange_n(&drive->slot_result[s], &expected,
                                  error ? -1 : 1, false, __ATOMIC_ACQ_REL,
                                  __ATOMIC_RELAXED);
      wait_queue_wake_all(&drive->slot_wq[s]);
    }

    spinlock_release(&drive->lock);
  }

  hba->is = hba_is; /* clear handled global bits */
}

/* Pick a free command slot.  Caller holds drive->lock. */
static int ahci_alloc_slot(struct ahci_drive *drive) {
  ahci_port_t *port = drive->port;
  uint32_t hw_busy = port->sact | port->ci;
  for (int i = 0; i < 32; i++) {
    if ((__atomic_load_n(&drive->slot_state[i], __ATOMIC_ACQUIRE) ==
         AHCI_SLOT_BUSY) ||
        (hw_busy & (1u << i)))
      continue;
    __atomic_store_n(&drive->slot_result[i], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&drive->slot_state[i], AHCI_SLOT_BUSY, __ATOMIC_RELEASE);
    return i;
  }
  return -1;
}

/* Program the command header and FIS for a slot, then hand it to the HBA.
 * The caller must have set header->prdtl and filled the PRDT already.
 * Caller holds drive->lock. */
static void ahci_start_slot(ahci_port_t *port, int slot, uint8_t command,
                            int is_write, uint64_t lba, uint32_t sectors) {
  ahci_command_header_t *header = ahci_command_header(port, slot);
  ahci_command_table_t *table = ahci_command_table(header);

  header->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
  header->w = is_write ? 1 : 0;

  fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&table->cfis);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1; // Command
  cmdfis->command = command;

  cmdfis->lba0 = (uint8_t)(lba & 0xFF);
  cmdfis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
  cmdfis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
  cmdfis->device = 1 << 6; // LBA mode

  cmdfis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
  cmdfis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
  cmdfis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

  cmdfis->countl = sectors & 0xFF;
  cmdfis->counth = (sectors >> 8) & 0xFF;

  /* PxIS is intentionally NOT cleared here: status bits are consumed by the
   * IRQ handler (under drive->lock) or by the waiter's CI poll.  Clearing them
   * at issue time can discard another in-flight command's error status and
   * make a failed transfer look successful. */
  port->ci = 1u << slot;
}

/*
 * Wait for a slot to complete.
 *
 * The completion verdict lives in drive->slot_result[] and is recorded exactly
 * once: by the IRQ handler, or by this poll when no interrupt arrives.  The
 * waiter never changes slot ownership until it has consumed the verdict, so a
 * slot cannot be reallocated while a completion is outstanding.
 *
 * The first ~100k TSC cycles are spent in an adaptive spin (most commands
 * finish in microseconds); afterwards the thread blocks on the slot wait queue
 * with a short wakeup_ticks fallback so a lost interrupt degrades to periodic
 * CI polling instead of a 5-second hang.
 */
static int ahci_wait_slot(struct ahci_drive *drive, int slot,
                          uint32_t timeout_ms) {
  ahci_port_t *port = drive->port;
  uint32_t bit = 1u << slot;
  struct thread *self = sched_get_current();
  wait_queue_entry_t wqe = { .thread = self, .next = NULL };
  uint64_t deadline = lapic_timer_get_ms() + timeout_ms;
  int result = AHCI_WAIT_ERROR;

  /* Blocking is only safe with interrupts enabled.  A caller that already
   * holds a spinlock runs with IF=0, and sleeping there would stall delivery
   * of the very completion IRQ we are waiting for, so those callers poll. */
  hal_irq_state_t iflags = hal_irq_save();
  hal_irq_restore(iflags);
  bool can_block = self && ahci_irq_active && (iflags & (1ULL << 9));

  /* Adaptive spin: cheap fast path for quick completions. */
  uint64_t spin_until = rdtsc() + 100000;
  while (rdtsc() < spin_until) {
    if (__atomic_load_n(&drive->slot_result[slot], __ATOMIC_ACQUIRE) != 0)
      break;
    if (!(port->ci & bit))
      break; /* resolved by the loop below */
    hal_cpu_relax();
  }

  for (;;) {
    int done = __atomic_load_n(&drive->slot_result[slot], __ATOMIC_ACQUIRE);
    if (done != 0) {
      result = done > 0 ? AHCI_WAIT_OK : AHCI_WAIT_ERROR;
      break;
    }

    /* CI polling also covers a missing/misrouted interrupt. */
    if (!(port->ci & bit)) {
      bool err =
          (port->is & (AHCI_PXIS_TFES | AHCI_PXIS_HBDS | AHCI_PXIS_HBFS |
                       AHCI_PXIS_IFS | AHCI_PXIS_INFS | AHCI_PXIS_OFS)) ||
          (port->tfd & 0x01);
      int expected = 0;
      __atomic_compare_exchange_n(&drive->slot_result[slot], &expected,
                                  err ? -1 : 1, false, __ATOMIC_ACQ_REL,
                                  __ATOMIC_RELAXED);
      continue;
    }

    if (lapic_timer_get_ms() >= deadline) {
      klogf("[AHCI] command timeout slot=%d ci=%08x is=%08x tfd=%08x\n", slot,
            port->ci, port->is, port->tfd);
      /* The port is wedged.  Fail-stop under the lock so no new command can
       * start, but stop DMA outside it: quiesce can take up to a second and
       * the IRQ handler must not spin on this lock for that long. */
      spinlock_acquire(&drive->lock);
      drive->failed = true;
      __atomic_store_n(&drive->slot_result[slot], 0, __ATOMIC_RELEASE);
      __atomic_store_n(&drive->slot_state[slot], AHCI_SLOT_FREE,
                       __ATOMIC_RELEASE);
      spinlock_release(&drive->lock);
      bool stopped = quiesce_port(port);
      return stopped ? AHCI_WAIT_TIMEOUT : AHCI_WAIT_UNQUIESCED;
    }

    /* No scheduler context, IRQs unwired, or IRQs masked by a caller's
     * spinlock: poll. */
    if (!can_block) {
      hal_cpu_relax();
      continue;
    }

    /* Block first, then link: a wake that lands between linking and the
     * state store must not be overwritten by a later THREAD_BLOCKED store. */
    self->state = THREAD_BLOCKED;
    wait_queue_add(&drive->slot_wq[slot], &wqe);
    if (__atomic_load_n(&drive->slot_result[slot], __ATOMIC_ACQUIRE) != 0) {
      self->state = THREAD_RUNNING;
      wait_queue_remove(&drive->slot_wq[slot], &wqe);
      continue;
    }
    self->wakeup_ticks = lapic_timer_get_ticks() + 10;
    sched_yield();
    self->wakeup_ticks = 0;
    self->state = THREAD_RUNNING;
    wait_queue_remove(&drive->slot_wq[slot], &wqe);
  }

  /* Consume the verdict and release ownership atomically against alloc. */
  spinlock_acquire(&drive->lock);
  __atomic_store_n(&drive->slot_result[slot], 0, __ATOMIC_RELEASE);
  __atomic_store_n(&drive->slot_state[slot], AHCI_SLOT_FREE, __ATOMIC_RELEASE);
  spinlock_release(&drive->lock);

  if (result == AHCI_WAIT_ERROR) {
    /* Preserve the old fail-stop semantics: one failed command takes the
     * drive out of service rather than risking a later write. */
    spinlock_acquire(&drive->lock);
    drive->failed = true;
    spinlock_release(&drive->lock);
    klogf("[AHCI] command error slot=%d ci=%08x is=%08x tfd=%08x\n", slot,
          port->ci, port->is, port->tfd);
  }

  return result;
}

/* DMA straight into a kernel buffer using scatter-gather PRDT entries.
 * `buf` must be at least sector aligned. */
static int ahci_io_direct(struct ahci_drive *drive, uint64_t lba,
                          uint32_t count, void *buf, int is_write) {
  ahci_port_t *port = drive->port;
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

  while (count > 0) {
    uint32_t chunk =
        count > AHCI_MAX_SECTORS_PER_CMD ? AHCI_MAX_SECTORS_PER_CMD : count;

    spinlock_acquire(&drive->lock);
    if (drive->failed) {
      spinlock_release(&drive->lock);
      return -1;
    }

    int slot = ahci_alloc_slot(drive);
    if (slot == -1) {
      spinlock_release(&drive->lock);
      return -1;
    }

    ahci_command_header_t *header = ahci_command_header(port, slot);
    ahci_command_table_t *table = ahci_command_table(header);
    memset(table, 0, sizeof(*table));

    uint32_t entries = 0;
    uint32_t covered =
        ahci_build_prdt(table, cr3, (uint64_t)buf, chunk * 512u, &entries);
    if (!covered || (covered & 511u)) {
      static uint32_t prdt_fail;
      if (__atomic_add_fetch(&prdt_fail, 1, __ATOMIC_RELAXED) <= 8)
        klogf("[AHCI] PRDT build failed: va=%llx bytes=%u covered=%u entries=%u\n",
              (unsigned long long)(uintptr_t)buf, chunk * 512u, covered,
              entries);
      __atomic_store_n(&drive->slot_state[slot], AHCI_SLOT_FREE,
                       __ATOMIC_RELEASE);
      spinlock_release(&drive->lock);
      return -1;
    }

    header->prdtl = entries;

    uint32_t sectors = covered / 512u;
    ahci_start_slot(port, slot,
                    is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX,
                    is_write, lba, sectors);
    spinlock_release(&drive->lock);

    if (ahci_wait_slot(drive, slot, 5000) != AHCI_WAIT_OK)
      return -1;

    lba += sectors;
    buf = (uint8_t *)buf + covered;
    count -= sectors;
  }

  return 0;
}

/* Fallback for user or unaligned buffers: bounce through a physically
 * contiguous scratch buffer.  Kernel I/O normally goes through the page cache,
 * which passes sector-aligned kernel pointers, so this path is rare. */
static int ahci_io_bounce(struct ahci_drive *drive, uint64_t lba,
                          uint32_t count, void *buf, int is_write) {
  ahci_port_t *port = drive->port;
  if (count > AHCI_MAX_SECTORS_PER_CMD)
    return -1;

  uint64_t bytes = (uint64_t)count * 512;
  uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  void *bounce_phys = pmm_alloc_blocks(pages);
  if (!bounce_phys) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                 " AHCI OOM: bounce buffer alloc failed\n");
    return -1;
  }
  void *bounce_virt =
      (void *)((uint64_t)bounce_phys + pmm_get_hhdm_offset());

  if (is_write) {
    memcpy(bounce_virt, buf, (size_t)bytes);
  }

  spinlock_acquire(&drive->lock);
  if (drive->failed) {
    spinlock_release(&drive->lock);
    pmm_free_blocks(bounce_phys, pages);
    return -1;
  }

  int slot = ahci_alloc_slot(drive);
  if (slot == -1) {
    spinlock_release(&drive->lock);
    pmm_free_blocks(bounce_phys, pages);
    return -1;
  }

  ahci_command_header_t *header = ahci_command_header(port, slot);
  ahci_command_table_t *table = ahci_command_table(header);
  memset(table, 0, sizeof(*table));

  table->prdt_entry[0].dba = (uint32_t)(uint64_t)bounce_phys;
  table->prdt_entry[0].dbau = (uint32_t)((uint64_t)bounce_phys >> 32);
  table->prdt_entry[0].dbc = (uint32_t)(bytes - 1); // Byte count - 1
  table->prdt_entry[0].i = 1;                      // Interrupt on completion
  header->prdtl = 1;

  ahci_start_slot(port, slot,
                  is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX,
                  is_write, lba, count);
  spinlock_release(&drive->lock);

  int rc = ahci_wait_slot(drive, slot, 5000);
  if (rc == AHCI_WAIT_OK && !is_write) {
    memcpy(buf, bounce_virt, (size_t)bytes);
  }

  /* If the port could not be quiesced, the HBA may still own this buffer. */
  if (rc != AHCI_WAIT_UNQUIESCED)
    pmm_free_blocks(bounce_phys, pages);

  return rc == AHCI_WAIT_OK ? 0 : -1;
}

static int ahci_io(ahci_port_t *port, uint64_t lba, uint32_t count, void *buf,
                   int is_write) {
  // Find the drive associated with this port
  struct ahci_drive *drive = NULL;
  for (int i = 0; i < 32; i++) {
    if (ahci_drives[i].port == port) {
      drive = &ahci_drives[i];
      break;
    }
  }

  if (!drive || !buf || count == 0 || lba >= drive->total_sectors ||
      (uint64_t)count > drive->total_sectors - lba ||
      lba > 0x0000FFFFFFFFFFFFULL) {
    return -1;
  }
  if (drive->failed) {
    static uint32_t refused;
    if (__atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED) <= 8)
      klogf("[AHCI] I/O refused: drive failed lba=%llu count=%u wr=%d\n",
            (unsigned long long)lba, count, is_write);
    return -1;
  }

  if (is_user_ptr((uint64_t)buf) || ((uint64_t)buf & 0x1FFu)) {
    return ahci_io_bounce(drive, lba, count, buf, is_write);
  }
  return ahci_io_direct(drive, lba, count, buf, is_write);
}

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count,
                     void *buf) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  return ahci_io(drive->port, lba, count, buf, 0);
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count,
                      const void *buf) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  // Cast away const for our naive physical address calculation
  return ahci_io(drive->port, lba, count, (void *)buf, 1);
}

static int ahci_flush(struct block_device *dev) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  if (!drive)
    return -1;

  ahci_port_t *port = drive->port;

  spinlock_acquire(&drive->lock);
  if (drive->failed) {
    spinlock_release(&drive->lock);
    return -1;
  }

  int slot = ahci_alloc_slot(drive);
  if (slot < 0) {
    spinlock_release(&drive->lock);
    return -1;
  }

  ahci_command_header_t *header = ahci_command_header(port, slot);
  ahci_command_table_t *table = ahci_command_table(header);
  memset(table, 0, sizeof(*table));
  header->prdtl = 0;

  ahci_start_slot(port, slot, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0);
  spinlock_release(&drive->lock);

  int rc = ahci_wait_slot(drive, slot, 5000);
  if (rc != AHCI_WAIT_OK) {
    spinlock_acquire(&drive->lock);
    drive->failed = true;
    spinlock_release(&drive->lock);
  }
  return rc == AHCI_WAIT_OK ? 0 : -1;
}

// Identify & Setup

static bool ahci_identify(ahci_port_t *port, struct ahci_drive *drive) {
  int slot = find_cmdslot(port);
  if (slot == -1)
    return false;

  // Use page allocator for temp buffer (must be physical!)
  void *phys_buf = pmm_alloc();
  void *virt_buf = (void *)((uint64_t)phys_buf + pmm_get_hhdm_offset());
  memset(virt_buf, 0, 4096);

  uint64_t clb_addr = ((uint64_t)port->clbu << 32) | port->clb;
  ahci_command_header_t *cmdheader =
      (ahci_command_header_t *)(clb_addr + pmm_get_hhdm_offset());

  cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
  cmdheader[slot].w = 0;
  cmdheader[slot].prdtl = 1;

  uint64_t ctba_addr =
      ((uint64_t)cmdheader[slot].ctbau << 32) | cmdheader[slot].ctba;
  ahci_command_table_t *cmdtbl =
      (ahci_command_table_t *)(ctba_addr + pmm_get_hhdm_offset());
  memset(cmdtbl, 0, sizeof(ahci_command_table_t));

  cmdtbl->prdt_entry[0].dba = (uint32_t)(uint64_t)phys_buf;
  cmdtbl->prdt_entry[0].dbau = (uint32_t)((uint64_t)phys_buf >> 32);
  cmdtbl->prdt_entry[0].dbc = 511; // 512 bytes
  cmdtbl->prdt_entry[0].i = 1;

  fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&cmdtbl->cfis);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = ATA_CMD_IDENTIFY;

  while (port->tfd & (0x80 | 0x08))
    ;

  port->ci = 1 << slot;

  while (1) {
    if ((port->ci & (1 << slot)) == 0)
      break;
    if (port->is & (1 << 30))
      return false;
  }

  uint16_t *identify_data = (uint16_t *)virt_buf;

  uint64_t lba48_sectors = (uint64_t)identify_data[100] |
                           ((uint64_t)identify_data[101] << 16) |
                           ((uint64_t)identify_data[102] << 32) |
                           ((uint64_t)identify_data[103] << 48);

  uint32_t lba28_sectors =
      (uint32_t)identify_data[60] | ((uint32_t)identify_data[61] << 16);

  drive->total_sectors = lba48_sectors ? lba48_sectors : lba28_sectors;

  for (int i = 0; i < 20; i++) {
    drive->model[i * 2] = (char)(identify_data[27 + i] >> 8);
    drive->model[i * 2 + 1] = (char)(identify_data[27 + i] & 0xFF);
  }
  drive->model[40] = '\0';

  for (int i = 39; i >= 0; i--) {
    if (drive->model[i] == ' ')
      drive->model[i] = '\0';
    else
      break;
  }

  pmm_free(phys_buf);
  return true;
}

static void probe_port(ahci_port_t *port, int portno) {
  uint32_t ssts = port->ssts;

  uint8_t ipm = (ssts >> 8) & 0x0F;
  uint8_t det = ssts & 0x0F;

  if (det != AHCI_PORT_DET_PRESENT || ipm != AHCI_PORT_IPM_ACTIVE)
    return; // Device not present or not active

  // Check signature
  if (port->sig == SATA_SIG_ATAPI) {
    console_puts("     Port ");
    console_putchar('0' + portno);
    console_puts(": ATAPI drive found. Skipping.\n");
  } else if (port->sig == SATA_SIG_SEMB) {
    // Enclosure management
  } else if (port->sig == SATA_SIG_PM) {
    // Port multiplier
  } else if (port->sig == SATA_SIG_ATA) {
    console_puts("     Port ");
    console_putchar('0' + portno);
    console_puts(": SATA drive found.\n");
    port_rebase(port);

    struct ahci_drive *drive = &ahci_drives[ahci_drive_count];
    drive->port = port;
    drive->port_index = portno;
    drive->lock = (spinlock_t)SPINLOCK_INIT;
    for (int s = 0; s < 32; s++) {
      __atomic_store_n(&drive->slot_state[s], AHCI_SLOT_FREE,
                       __ATOMIC_RELEASE);
      __atomic_store_n(&drive->slot_result[s], 0, __ATOMIC_RELEASE);
      wait_queue_init(&drive->slot_wq[s]);
    }

    if (ahci_identify(port, drive)) {
      drive->present = true;
      uint64_t size_mb = (drive->total_sectors * 512) / (1024 * 1024);

      console_puts("     sata");
      console_putchar('0' + ahci_drive_count);
      console_puts(": ");
      console_puts(drive->model);
      console_puts(" (");
      print_uint64(size_mb);
      console_puts(" MB, ");
      print_uint64(drive->total_sectors);
      console_puts(" sectors)\n");

      struct block_device *blk = &drive->blkdev;
      blk->name[0] = 's';
      blk->name[1] = 'a';
      blk->name[2] = 't';
      blk->name[3] = 'a';
      blk->name[4] = '0' + ahci_drive_count;
      blk->name[5] = '\0';
      blk->sector_size = 512;
      blk->total_sectors = drive->total_sectors;
      blk->read_sectors = ahci_read;
      blk->write_sectors = ahci_write;
      blk->flush = ahci_flush;
      blk->driver_data = drive;

      block_register(blk);
      ahci_drive_count++;

      /* Command completions wake waiters instead of being polled. */
      port->is = port->is;
      port->ie = AHCI_PORT_IRQ_ENABLE;
    }
  }
}

// Initialization

static int ahci_probe(struct device *dev) {
  uint32_t abar = 0;
  // Get ABAR from BAR5
  // We should ideally have BARs in dev->resources, but for now we'll do legacy PCI read
  struct pci_device *pci_dev = pci_find_device_by_id(dev->vendor_id, dev->device_id);
  if (!pci_dev) return -1;
  
  abar = pci_dev->bar[5];
  if (abar == 0) return -1;

  uint64_t phys_abar = abar & 0xFFFFFFF0;
  uint64_t virt_abar = phys_abar + pmm_get_hhdm_offset();
  
  // The kernel accesses PCI MMIO through the existing HHDM mapping. Do not
  // reinstall it here: remapping during SMP early boot requires a global TLB
  // shootdown and can deadlock idle APs.

  hba = (ahci_hba_mem_t *)virt_abar;
  hba->ghc |= (1 << 31); // AHCI Enable

  uint32_t pi = hba->pi;
  for (int i = 0; i < 32; i++) {
    if (pi & (1 << i)) {
      probe_port(&hba->ports[i], i);
    }
  }

  if (ahci_drive_count > 0) {
    /* Ensure bus mastering is on and legacy INTx is not disabled. */
    uint16_t cmd =
        pci_config_read16(pci_dev->bus, pci_dev->slot, pci_dev->func, 0x04);
    pci_config_write16(pci_dev->bus, pci_dev->slot, pci_dev->func, 0x04,
                       (uint16_t)((cmd | (1u << 2)) & ~(1u << 10)));

    if (pci_dev->irq_line != 0 &&
        irq_install_handler(pci_dev->irq_line, ahci_irq_handler, 0x000F)) {
      hba->is = hba->is;     /* clear stale port interrupt bits */
      hba->ghc |= (1u << 1); /* GHC.IE: enable HBA interrupts */
      ahci_irq_active = true;
    } else {
      console_puts(KLOG_CLR_YELLOW "[ WARN ]" KLOG_CLR_RESET
                   " AHCI: IRQ unavailable, using polled completion\n");
    }
  }
  return 0;
}

static struct device_id ahci_ids[] = {
    { .type = ID_PCI, .pci = { .match_class = true, .class = 0x01, .subclass = 0x06 } }
};

static struct driver ahci_driver = {
    .name = "ahci",
    .kind = DRIVER_KERNEL,
    .ids = ahci_ids,
    .id_count = 1,
    .probe = ahci_probe
};

int ahci_init(void) {
  ahci_drive_count = 0;
  dm_register_driver(&ahci_driver);

  if (ahci_drive_count == 0) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " No SATA drives successfully initialized.\n");
  } else {
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " AHCI: ");
    print_uint64(ahci_drive_count);
    console_puts(" drive(s) registered.\n");
  }
  return ahci_drive_count;
}
