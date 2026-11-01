#include "mm/zram.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/tlb_shootdown.h"
#include "mm/vma.h"
#include "mm/vmm_internal.h"
#include "sched/sched.h"

static bool enabled;
static uint64_t low_pages, swapouts, swapins, scans, pressure_yields;
static uint32_t reclaim_active, target_cursor, collector_cursor;
static struct block_device device;

static bool eligible(struct vma *v) {
  return v && (v->flags & MAP_PRIVATE) && (v->flags & MAP_ANONYMOUS) &&
         !(v->flags & (MAP_SHARED | MAP_SYSV_SHM | MAP_LOCKED)) &&
         v->prot && !v->linux_vma && !v->file_node;
}
/* First interval past cursor, including ineligible VMAs. The caller skips
 * those in O(log n), charging each against its bounded work budget. */
static struct vma *next_vma(struct vma *v, uint64_t cursor) {
  if (!v)
    return NULL;
  if (v->end <= cursor)
    return next_vma(v->right, cursor);
  struct vma *left = next_vma(v->left, cursor);
  return left ? left : v;
}

/* Freeze one exclusive frame and retain a pin while the compression runs
 * off-lock. The first flush removes every stale writable translation. */
static uint64_t freeze(struct mm_struct *mm, uint64_t cr3, uint64_t va,
                       bool force) {
  struct vma *v = vma_find(&mm->vmas, va);
  if (!eligible(v) || mm->mlockall_future)
    return 0;
  uint64_t phys = 0;
  vmm_lock_acquire_for((uint64_t *)cr3, va);
  uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
  if (pte && (*pte & PAGE_FLAG_PRESENT) && (*pte & PAGE_FLAG_USER)) {
    uint64_t frame = *pte & PAGE_MASK;
    if (!force && (*pte & PAGE_FLAG_A)) {
      *pte &= ~PAGE_FLAG_A;
    } else if (!(*pte & PAGE_FLAG_MLOCKED) &&
               frame != pmm_get_zero_page_phys() && pmm_is_managed(frame) &&
               pmm_get_ref((void *)frame) == 1) {
      pmm_incref((void *)frame);
      /* COW authorizes a later write fault. Never add it to an originally
       * read-only PTE: a rejected compression must preserve protection. */
      if (*pte & PAGE_FLAG_RW)
        *pte = (*pte & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
      phys = frame;
    }
  }
  vmm_lock_release();
  return phys;
}
static bool compress_frozen(struct mm_struct *mm, uint64_t cr3, uint64_t va,
                            uint64_t phys) {
  /* Only this leaf changed. A full address-space flush here evicts unrelated
   * translations twice for every page compressed during reclaim. */
  tlb_shootdown_page_for_pcid(va, cr3, mm->pcid);
  uint32_t slot = zram_store_page(PHYS_TO_VIRT(phys));
  bool installed = false;
  if (slot) {
    spinlock_acquire(&mm->lock);
    struct vma *v = vma_find(&mm->vmas, va);
    vmm_lock_acquire_for((uint64_t *)cr3, va);
    uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
    if (eligible(v) && !mm->mlockall_future && pte &&
        (*pte & PAGE_FLAG_PRESENT) && !(*pte & PAGE_FLAG_MLOCKED) &&
        !(*pte & PAGE_FLAG_RW) && (*pte & PAGE_MASK) == phys &&
        pmm_get_ref((void *)phys) == 2) {
      *pte = zram_pte(slot);
      installed = true;
    }
    vmm_lock_release();
    spinlock_release(&mm->lock);
    if (installed) {
      /* No physical reference is released until stale readers are gone. */
      tlb_shootdown_page_for_pcid(va, cr3, mm->pcid);
      pmm_free_page((void *)phys); /* former mapping */
      __atomic_add_fetch(&swapouts, 1, __ATOMIC_RELAXED);
    } else
      zram_slot_put(slot);
  }
  pmm_free_page((void *)phys); /* scan pin */
  return installed;
}
/* Reclaim compresses pages in batches. Each batch needs one TLB flush before
 * its pages are frozen and compressed, and frames swapped out by a batch are
 * released only after the next batch's flush, which also clears their stale
 * translations. Steady state is one cross-CPU flush per batch. */
#define ZRAM_BATCH 64
#define ZRAM_SCAN_RUN 64
struct zram_batch {
  uint64_t va[ZRAM_BATCH];
  uint64_t phys[ZRAM_BATCH];
  unsigned n;
};

/* Second-chance scan of one run of pages, under one page-table lock. The old
 * per-page freeze() took the global fault lock for every page, which contended
 * with every page fault on every CPU. A page with its accessed bit set loses
 * the bit and is skipped; the others are frozen and queued. Caller holds
 * mm->lock. */
static unsigned freeze_run(struct mm_struct *mm, uint64_t cr3, struct vma *v,
                           uint64_t start, uint64_t end, struct zram_batch *b) {
  if (!eligible(v) || mm->mlockall_future)
    return 0;
  unsigned got = 0;
  vmm_lock_acquire_for((uint64_t *)cr3, start);
  for (uint64_t va = start; va < end && b->n < ZRAM_BATCH; va += PAGE_SIZE) {
    uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
    if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER))
      continue;
    if (*pte & PAGE_FLAG_A) {
      *pte &= ~PAGE_FLAG_A;
      continue;
    }
    uint64_t frame = *pte & PAGE_MASK;
    if ((*pte & PAGE_FLAG_MLOCKED) || frame == pmm_get_zero_page_phys() ||
        !pmm_is_managed(frame) || pmm_get_ref((void *)frame) != 1)
      continue;
    pmm_incref((void *)frame);
    /* COW authorizes a later write fault; the batch's flush removes stale
     * writable translations before any byte of the frame is read. */
    if (*pte & PAGE_FLAG_RW)
      *pte = (*pte & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
    b->va[b->n] = va;
    b->phys[b->n++] = frame;
    got++;
  }
  vmm_lock_release();
  return got;
}

/* Release the two references held by each frame swapped out earlier: the
 * former mapping and the scan pin. Only after a flush that removed every
 * translation to the frame. */
static void release_pending(struct zram_batch *pend) {
  for (unsigned i = 0; i < pend->n; i++) {
    pmm_free_page((void *)pend->phys[i]); /* former mapping */
    pmm_free_page((void *)pend->phys[i]); /* scan pin */
  }
  pend->n = 0;
}

static size_t compress_batch(struct mm_struct *mm, uint64_t cr3,
                             struct zram_batch *b, struct zram_batch *pend) {
  if (!b->n)
    return 0;
  uint32_t slots[ZRAM_BATCH];
  bool installed[ZRAM_BATCH];
  /* This flush removes the stale writable translations of the frames in
   * this batch and the stale translations of the frames swapped out by the
   * previous batch. Only then may those previous frames be released. */
  tlb_shootdown_context_for(cr3);
  release_pending(pend);
  for (unsigned i = 0; i < b->n; i++) {
    installed[i] = false;
    slots[i] = zram_store_page(PHYS_TO_VIRT(b->phys[i]));
    if (!slots[i])
      continue;
    spinlock_acquire(&mm->lock);
    struct vma *v = vma_find(&mm->vmas, b->va[i]);
    vmm_lock_acquire_for((uint64_t *)cr3, b->va[i]);
    uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, b->va[i]);
    if (eligible(v) && !mm->mlockall_future && pte &&
        (*pte & PAGE_FLAG_PRESENT) && !(*pte & PAGE_FLAG_MLOCKED) &&
        !(*pte & PAGE_FLAG_RW) && (*pte & PAGE_MASK) == b->phys[i] &&
        pmm_get_ref((void *)b->phys[i]) == 2) {
      *pte = zram_pte(slots[i]);
      installed[i] = true;
    }
    vmm_lock_release();
    spinlock_release(&mm->lock);
    if (!installed[i])
      zram_slot_put(slots[i]);
  }
  size_t done = 0;
  for (unsigned i = 0; i < b->n; i++) {
    if (installed[i]) {
      /* Two references stay held until the next flush (see release_pending). */
      pend->phys[pend->n++] = b->phys[i];
      done++;
      __atomic_add_fetch(&swapouts, 1, __ATOMIC_RELAXED);
    } else {
      pmm_free_page((void *)b->phys[i]); /* scan pin only */
    }
  }
  b->n = 0;
  return done;
}

size_t zram_pageout(struct mm_struct *mm, uint64_t cr3, uint64_t start,
                    uint64_t end) {
  if (!enabled || !mm || !hal_irq_enabled() || start >= end)
    return 0;
  size_t freed = 0;
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    spinlock_acquire(&mm->lock);
    uint64_t phys = freeze(mm, cr3, va, true);
    spinlock_release(&mm->lock);
    if (phys)
      freed += compress_frozen(mm, cr3, va, phys);
    if ((va & 0x3ffff) == 0)
      sched_yield();
  }
  return freed;
}

bool zram_under_pressure(void) {
  return enabled && pmm_get_free_pages_including_pcp() < low_pages;
}
size_t zram_reclaim(size_t target) {
  if (!enabled || !target || !hal_irq_enabled() ||
      __atomic_exchange_n(&reclaim_active, 1, __ATOMIC_ACQUIRE))
    return 0;
  struct {
    struct mm_struct *mm;
    uint64_t cr3;
  } targets[64];
  unsigned n = 0;
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  extern uint32_t global_thread_count;
  struct thread *start = global_thread_list;
  uint32_t skip =
      global_thread_count ? collector_cursor % global_thread_count : 0;
  collector_cursor += 64;
  while (start && skip--)
    start = start->global_next;
  if (!start)
    start = global_thread_list;
  struct thread *t = start;
  bool wrapped = false;
  while (t && n < 64) {
    if (!t->is_idle && t->state != THREAD_DEAD && t->state != THREAD_ZOMBIE &&
        t->mm && t->cr3) {
      bool seen = false;
      for (unsigned j = 0; j < n; j++)
        if (targets[j].mm == t->mm)
          seen = true;
      if (!seen) {
        __atomic_add_fetch(&t->mm->ref_count, 1, __ATOMIC_ACQ_REL);
        targets[n].mm = t->mm;
        targets[n++].cr3 = t->cr3;
      }
    }
    t = t->global_next;
    if (!t && !wrapped) {
      t = global_thread_list;
      wrapped = true;
    }
    if (wrapped && t == start)
      break;
  }
  spinlock_release(&tid_lock);
  size_t freed = 0;
  unsigned first = n ? target_cursor++ % n : 0;
  for (unsigned j = 0; j < n && freed < target; j++) {
    unsigned k = (first + j) % n;
    struct mm_struct *mm = targets[k].mm;
    uint64_t cr3 = targets[k].cr3;
    struct zram_batch batch = {.n = 0};
    struct zram_batch pend = {.n = 0};
    for (unsigned work = 0; work < 32 && freed + batch.n < target; work++) {
      spinlock_acquire(&mm->lock);
      struct vma *v = next_vma(mm->vmas.root, mm->zram_scan_cursor);
      if (!v) {
        mm->zram_scan_cursor = 0;
        spinlock_release(&mm->lock);
        break;
      }
      if (!eligible(v)) {
        mm->zram_scan_cursor = v->end;
        spinlock_release(&mm->lock);
        continue;
      }
      uint64_t va =
          mm->zram_scan_cursor > v->start ? mm->zram_scan_cursor : v->start;
      uint64_t end = v->end < va + ZRAM_SCAN_RUN * PAGE_SIZE
                         ? v->end
                         : va + ZRAM_SCAN_RUN * PAGE_SIZE;
      mm->zram_scan_cursor = end;
      freeze_run(mm, cr3, v, va, end, &batch);
      spinlock_release(&mm->lock);
      __atomic_add_fetch(&scans, (end - va) / PAGE_SIZE, __ATOMIC_RELAXED);
      if (batch.n == ZRAM_BATCH)
        freed += compress_batch(mm, cr3, &batch, &pend);
    }
    freed += compress_batch(mm, cr3, &batch, &pend);
    if (pend.n) {
      tlb_shootdown_context_for(cr3);
      release_pending(&pend);
    }
    sched_yield();
  }
  for (unsigned j = 0; j < n; j++)
    mm_put(targets[j].mm, targets[j].cr3);
  __atomic_store_n(&reclaim_active, 0, __ATOMIC_RELEASE);
  return freed;
}

/* Faulting allocators cannot race an already active compressor to the last
 * frame. Wait off all VM locks, with IRQs enabled so shootdowns can complete.
 * Unproductive passes are bounded: incompressible/full-store workloads must
 * still reach the ordinary allocator/OOM path instead of hanging forever. */
/* Background reclaim: the daemon is woken on demand when memory drops below
 * the low watermark, refills to the target, and sleeps. Faults only reclaim
 * for themselves below min_pages, so a fault seldom waits on compression. */
static struct thread *daemon_thread;
static uint32_t reclaim_wanted;
static uint64_t min_pages;

static void zram_kick(void) {
  struct thread *d = __atomic_load_n(&daemon_thread, __ATOMIC_ACQUIRE);
  if (!d)
    return;
  /* Only the 0 -> 1 transition pays for a wakeup. The daemon clears the flag
   * before it refills, so the next shortage wakes it again. */
  if (__atomic_exchange_n(&reclaim_wanted, 1, __ATOMIC_ACQ_REL))
    return;
  sched_wakeup(d);
}

void zram_balance(void) {
  if (!zram_under_pressure() || !hal_irq_enabled())
    return;
  zram_kick();
  if (pmm_get_free_pages_including_pcp() >= min_pages)
    return;
  /* Each faulting thread used to reclaim until free memory recovered, up to
   * 16 passes of 256 pages, and waiters spun in sched_yield() while another
   * CPU compressed. Every fault under pressure then paid for a large batch
   * and the cores stayed busy. Now a fault does a bounded share (only the
   * deficit, at most two passes), sleeps briefly if another reclaim is
   * running, and the zram worker keeps freeing memory in the background. */
  unsigned productive = 0;
  for (unsigned waits = 0; waits < 4 && productive < 2; waits++) {
    uint64_t free_now = pmm_get_free_pages_including_pcp();
    if (free_now >= low_pages + 512)
      return;
    if (__atomic_load_n(&reclaim_active, __ATOMIC_ACQUIRE)) {
      __atomic_add_fetch(&pressure_yields, 1, __ATOMIC_RELAXED);
      struct thread *self = sched_get_current();
      if (!self || self->is_idle)
        return;
      self->state = THREAD_SLEEPING;
      self->wakeup_ticks = lapic_timer_get_ticks() + 1;
      sched_yield();
      self->wakeup_ticks = 0;
      self->state = THREAD_RUNNING;
      continue;
    }
    uint64_t deficit = low_pages + 512 - free_now;
    size_t want = deficit < 32 ? 32 : deficit > 256 ? 256 : (size_t)deficit;
    if (!zram_reclaim(want))
      return;
    productive++;
  }
}

/* Swap-in readahead: a program that faults on a swapped page usually reads
 * its neighbours next, so the following pages that are also swapped out are
 * read back with it. Readahead never reclaims and stops below the watermark,
 * so a fault cannot wait on it beyond the copies it makes. */
#define ZRAM_READAHEAD 7
static void zram_readahead(struct mm_struct *mm, uint64_t cr3,
                           uint64_t fault_va) {
  for (unsigned d = 1; d <= ZRAM_READAHEAD; d++) {
    if (pmm_get_free_pages_including_pcp() < min_pages + 64)
      return;
    uint64_t va = fault_va + (uint64_t)d * PAGE_SIZE;
    uint64_t entry = 0;
    spinlock_acquire(&mm->lock);
    struct vma *v = vma_find(&mm->vmas, va);
    if (eligible(v)) {
      vmm_lock_acquire_for((uint64_t *)cr3, va);
      uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
      entry = pte ? *pte : 0;
      vmm_lock_release();
    }
    spinlock_release(&mm->lock);
    uint32_t slot = zram_pte_slot(entry);
    if (!slot || !zram_slot_get(slot))
      continue;
    void *frame = pmm_alloc();
    if (!frame) {
      zram_slot_put(slot);
      return;
    }
    if (zram_slot_read(slot, PHYS_TO_VIRT((uint64_t)frame))) {
      pmm_free_page(frame);
      zram_slot_put(slot);
      continue;
    }
    bool installed = false;
    spinlock_acquire(&mm->lock);
    vmm_lock_acquire_for((uint64_t *)cr3, va);
    uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
    v = vma_find(&mm->vmas, va);
    if (pte && *pte == entry && v && v->prot) {
      uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
      if (v->prot & PROT_WRITE)
        flags |= PAGE_FLAG_RW;
      if (!(v->prot & PROT_EXEC))
        flags |= PAGE_FLAG_NX;
      *pte = (uint64_t)frame | flags;
      installed = true;
    }
    vmm_lock_release();
    spinlock_release(&mm->lock);
    if (installed) {
      zram_slot_put(slot); /* former PTE */
      __atomic_add_fetch(&swapins, 1, __ATOMIC_RELAXED);
    } else
      pmm_free_page(frame);
    zram_slot_put(slot); /* reader */
  }
}

int zram_fault(struct mm_struct *mm, uint64_t cr3, uint64_t va, bool write,
               bool exec) {
  if (!enabled)
    return 0;
  va &= ~(PAGE_SIZE - 1ULL);
  spinlock_acquire(&mm->lock);
  vmm_lock_acquire_for((uint64_t *)cr3, va);
  uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
  uint64_t entry = pte ? *pte : 0;
  uint32_t slot = zram_pte_slot(entry);
  if (!slot) {
    vmm_lock_release();
    spinlock_release(&mm->lock);
    return 0;
  }
  struct vma *v = vma_find(&mm->vmas, va);
  bool permitted = v && v->prot && (!write || (v->prot & PROT_WRITE)) &&
                   (!exec || (v->prot & PROT_EXEC));
  bool pinned = permitted && zram_slot_get(slot);
  vmm_lock_release();
  spinlock_release(&mm->lock);
  if (!permitted)
    return -13;
  if (!pinned)
    return -5;
  void *frame = pmm_alloc();
  if (!frame) {
    /* One frame is needed, not a batch: reclaim a single compression batch.
     * The global per-CPU cache drain (every CPU's cache into buddy, under
     * every zone lock) is the last resort, not the first, because it stalls
     * every allocator in the machine. */
    zram_reclaim(32);
    frame = pmm_alloc();
  }
  if (!frame) {
    pmm_pcp_drain_all();
    frame = pmm_alloc();
  }
  if (!frame) {
    zram_slot_put(slot);
    return -12;
  }
  if (zram_slot_read(slot, PHYS_TO_VIRT((uint64_t)frame))) {
    pmm_free_page(frame);
    zram_slot_put(slot);
    return -5;
  }
  bool installed = false;
  spinlock_acquire(&mm->lock);
  vmm_lock_acquire_for((uint64_t *)cr3, va);
  pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
  v = vma_find(&mm->vmas, va);
  if (pte && *pte == entry && v && v->prot &&
      (!write || (v->prot & PROT_WRITE)) && (!exec || (v->prot & PROT_EXEC))) {
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (v->prot & PROT_WRITE)
      flags |= PAGE_FLAG_RW;
    if (!(v->prot & PROT_EXEC))
      flags |= PAGE_FLAG_NX;
    *pte = (uint64_t)frame | flags;
    installed = true;
  }
  vmm_lock_release();
  spinlock_release(&mm->lock);
  if (installed) {
    zram_slot_put(slot); /* former PTE */
    __atomic_add_fetch(&swapins, 1, __ATOMIC_RELAXED);
  } else
    pmm_free_page(frame);
  zram_slot_put(slot); /* reader */
  if (installed)
    zram_readahead(mm, cr3, va);
  return 1; /* retry instruction, including a racing unmap/protection change */
}
int zram_copy_swapped(uint64_t *pml4, uint64_t va, void *page) {
  uint64_t *pte = vmm_get_pte_ptr(pml4, va);
  uint32_t slot = pte ? zram_pte_slot(*pte) : 0;
  if (!slot)
    return 0;
  if (!zram_slot_get(slot))
    return -1;
  int ret = zram_slot_read(slot, page);
  zram_slot_put(slot);
  return ret ? -1 : 1;
}
bool zram_is_enabled(void) { return enabled; }
void zram_get_stats(struct zram_stats *out) {
  zram_store_stats(out);
  out->enabled = enabled;
  out->swapouts = __atomic_load_n(&swapouts, __ATOMIC_RELAXED);
  out->swapins = __atomic_load_n(&swapins, __ATOMIC_RELAXED);
  out->scans = __atomic_load_n(&scans, __ATOMIC_RELAXED);
  out->pressure_yields = __atomic_load_n(&pressure_yields, __ATOMIC_RELAXED);
}
static int read_blocks(struct block_device *dev, uint64_t lba, uint32_t n,
                       void *buf) {
  if (!buf || !n || lba >= dev->total_sectors || n > dev->total_sectors - lba)
    return -1;
  for (unsigned i = 0; i < n; i++) {
    uint32_t slot = (uint32_t)(lba + i + 1);
    void *page = (uint8_t *)buf + i * PAGE_SIZE;
    if (!zram_slot_get(slot))
      memset(page, 0, PAGE_SIZE);
    else {
      int r = zram_slot_read(slot, page);
      zram_slot_put(slot);
      if (r)
        return -1;
    }
  }
  block_stat_read(dev, n);
  return 0;
}
static void worker(void) {
  struct thread *self = sched_get_current();
  for (;;) {
    __atomic_store_n(&reclaim_wanted, 0, __ATOMIC_RELEASE);
    /* Refill to the target in one pass while memory is short. */
    while (zram_under_pressure() &&
           pmm_get_free_pages_including_pcp() < low_pages + 512) {
      if (!zram_reclaim(256))
        break;
    }
    bool pressure = zram_under_pressure();
    self->state = THREAD_SLEEPING;
    self->wakeup_ticks = lapic_timer_get_ticks() + (pressure ? 5 : 250);
    sched_yield();
    self->wakeup_ticks = 0;
    self->state = THREAD_RUNNING;
  }
}
static uint64_t option(const char *key, uint64_t fallback) {
  extern const char *kernel_boot_cmdline;
  const char *s = kernel_boot_cmdline;
  if (!s)
    return fallback;
  size_t n = strlen(key);
  while ((s = strstr(s, key))) {
    if (s != kernel_boot_cmdline && s[-1] != ' ') {
      s += n;
      continue;
    }
    s += n;
    uint64_t v = 0;
    if (*s < '0' || *s > '9')
      return fallback;
    while (*s >= '0' && *s <= '9') {
      if (v > (UINT64_MAX - 9) / 10)
        return fallback;
      v = v * 10 + (*s++ - '0');
    }
    return (*s && *s != ' ') ? fallback : v;
  }
  return fallback;
}
void zram_init(void) {
  if (enabled || !option("zram=", 1))
    return;
  uint64_t ram = pmm_get_total_memory();
  uint64_t mb = option("zram_mb=", ram / (2 * 1024 * 1024));
  if (!mb)
    return;
  if (mb > 2048)
    mb = 2048;
  uint64_t limit_mb = option("zram_limit_mb=", ram / (4 * 1024 * 1024));
  if (limit_mb > ram / (2 * 1024 * 1024))
    limit_mb = ram / (2 * 1024 * 1024);
  if (!zram_store_init((uint32_t)(mb * 256), limit_mb * 1024 * 1024)) {
    klog_puts("[ZRAM] Unable to initialize compressed swap\n");
    return;
  }
  low_pages = ram / (16 * PAGE_SIZE);
  if (low_pages < 4096)
    low_pages = 4096;
  if (low_pages > 32768)
    low_pages = 32768;
  min_pages = low_pages / 2;
  if (min_pages < 512)
    min_pages = 512;
  enabled = true;
  strcpy(device.name, "zram0");
  device.sector_size = PAGE_SIZE;
  device.total_sectors = mb * 256;
  device.read_sectors = read_blocks;
  /* Auto-active swap is intentionally read-only through raw block I/O:
   * userspace must never overwrite an object referenced by a process PTE. */
  block_register(&device);
  struct thread *daemon = sched_create_kernel_thread(worker, NULL, true);
  if (daemon) {
    strcpy(daemon->comm, "zramd");
    /* Background nice 19 starved this thread behind the game, so reclaim ran
     * late and faults paid for it. Run at normal priority instead. */
    sched_set_priority(daemon, SCHED_PRIORITY_DEFAULT, 0);
    __atomic_store_n(&daemon_thread, daemon, __ATOMIC_RELEASE);
  }
  klogf("[ZRAM] Automatic LZ4 swap: %llu MiB logical, %llu MiB storage limit\n",
        (unsigned long long)mb, (unsigned long long)limit_mb);
}
