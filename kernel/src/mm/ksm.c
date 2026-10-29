#include "ksm.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "vmm_internal.h"
#include "tlb_shootdown.h"
#include "heap.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../sched/sched.h"
#include "../apic/lapic_timer.h"

#define KSM_BUCKETS 16384
#define KSM_MAX_NODES 4096
#define KSM_MAX_TARGETS 256
/* Bound work, including absent PTEs, in each IRQ-masked mm section. */
#define KSM_MM_WORK_BUDGET 64
#define KSM_MAX_CHAIN_PROBES 16

/* Merge eagerly under memory pressure, slowly in the background otherwise.
 * Above the watermark deduplication is not urgent, but it should still
 * happen - duplicates keep accruing and merging them is the point - so the
 * daemon runs a sparse-pass cadence instead of skipping entirely.  Both
 * cadences back off when a pass merges nothing. */
#define KSM_FREE_EAGER_PAGES (32u * 1024u) /* 128 MiB */

/* Aggressive mode, in timer ticks (~1 ms each). */
#define KSM_EAGER_INTERVAL 1500u
#define KSM_MAX_EAGER_INTERVAL (KSM_EAGER_INTERVAL * 40u) /* ~60 s */

/* Background mode is deliberately sparse: avoid competing with interactive
 * launches when memory is plentiful, while still finding long-lived dupes. */
#define KSM_IDLE_INTERVAL 60000u /* ~60 s */
#define KSM_MAX_IDLE_INTERVAL 120000u /* ~2 min */

struct ksm_node {
  uint64_t phys_page;
  uint64_t va;
  /* Scanned content source when this page merged: 0 = unmerged. */
  uint64_t replacement;
  uint32_t hash;
  uint32_t next; // 0 = end of list
};

static struct ksm_node *ksm_node_pool = NULL;
static uint32_t ksm_buckets[KSM_BUCKETS];
static uint32_t ksm_node_count = 1;
/* Rotate bounded passes so the same first address spaces do not monopolize
 * the scan/pin budget. This is an index, never an unreferenced mm pointer. */
static uint32_t ksm_target_cursor;

static volatile uint32_t ksm_scan_active = 0;
static struct ksm_stats ksm_global_stats = {0};
static volatile bool ksm_enabled = true;
static struct thread *ksm_thread = NULL;

static inline uint32_t ksm_hash_page(const uint64_t *data) {
  uint32_t hash = 5381;
  for (int i = 0; i < 512; i++) {
    uint64_t v = data[i];
    hash = ((hash << 5) + hash) ^ (uint32_t)v;
    hash = ((hash << 5) + hash) ^ (uint32_t)(v >> 32);
  }
  return hash;
}

static inline bool ksm_is_page_zero(const uint64_t *data) {
  for (int i = 0; i < 512; i++) {
    if (data[i] != 0)
      return false;
  }
  return true;
}

static bool ksm_vma_eligible(struct vma *v) {
  if (!v)
    return false;
  /* Only exclusive anonymous pages can be frozen through this one PTE.
   * File-cache, shared-memory and device aliases may remain writable. */
  if (!(v->flags & MAP_PRIVATE) || !(v->flags & MAP_ANONYMOUS) ||
      !(v->prot & PROT_WRITE) ||
      (v->flags & (MAP_SHARED | MAP_SYSV_SHM)) || v->linux_vma || v->file_node)
    return false;
  return true;
}

struct ksm_target {
  struct mm_struct *mm;
  uint64_t cr3;
};

uint64_t ksm_scan_pass(void) {
  /* A masked caller cannot wait for remote TLB acknowledgements. */
  if (!hal_irq_enabled())
    return 0;

  /* Prevent concurrent scans without disabling interrupts on the scanning CPU */
  if (__atomic_exchange_n(&ksm_scan_active, 1, __ATOMIC_ACQUIRE) != 0)
    return 0;

  if (!ksm_node_pool) {
    __atomic_store_n(&ksm_scan_active, 0, __ATOMIC_RELEASE);
    return 0;
  }

  memset(ksm_buckets, 0, sizeof(ksm_buckets));
  ksm_node_count = 1;
  uint64_t freed_pages = 0;

  /* Step 1: Collect active unique address spaces under tid_lock */
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;

  struct ksm_target targets[KSM_MAX_TARGETS];
  int ntargets = 0;

  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t && ntargets < KSM_MAX_TARGETS;
       t = t->global_next) {
    if (t->is_idle || t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
      continue;
    if (!t->mm || !t->cr3)
      continue;
    bool seen = false;
    for (int i = 0; i < ntargets; i++) {
      if (targets[i].mm == t->mm) {
        seen = true;
        break;
      }
    }
    if (seen)
      continue;

    /* Reference the mm so it cannot be freed during our scan */
    __atomic_add_fetch(&t->mm->ref_count, 1, __ATOMIC_ACQ_REL);
    targets[ntargets].mm = t->mm;
    targets[ntargets].cr3 = t->cr3;
    ntargets++;
  }
  spinlock_release(&tid_lock);

  uint64_t zero_page_phys = pmm_get_zero_page_phys();
  uint64_t vdso_phys = vmm_get_vsyscall_page_phys();

  /* Freeze a bounded set of exclusive anonymous pages in each mm.  The
   * extra reference forces a concurrent CoW fault to copy, even if this was
   * the only mapping.  Keep these pins through the final shootdown: neither
   * old translations nor hash-table entries may outlive their frames. */
  uint32_t target_start = ntargets ? ksm_target_cursor % ntargets : 0;
  for (int t_idx = 0; t_idx < ntargets && ksm_node_count < KSM_MAX_NODES; t_idx++) {
    uint32_t target_idx = (target_start + t_idx) % ntargets;
    ksm_target_cursor = (target_idx + 1) % ntargets;
    struct mm_struct *mm = targets[target_idx].mm;
    uint64_t cr3 = targets[target_idx].cr3;
    uint32_t first = ksm_node_count;
    unsigned work = 0;
    bool needs_initial_flush = false;
    spinlock_acquire(&mm->lock);
    struct vma *stack[64];
    int sp = 0;
    struct vma *cur = mm->vmas.root;
    while ((cur || sp > 0) && work < KSM_MM_WORK_BUDGET &&
           ksm_node_count < KSM_MAX_NODES) {
      while (cur && sp < 64) {
        if (cur->end <= mm->ksm_scan_cursor) {
          cur = cur->right;
          continue;
        }
        stack[sp++] = cur;
        cur = cur->left;
      }
      if (cur) /* Do not revisit a subtree if the traversal stack overflows. */
        break;
      if (!sp)
        break;
      struct vma *v = stack[--sp];
      cur = v->right;
      work++;
      if (!ksm_vma_eligible(v)) {
        mm->ksm_scan_cursor = v->end;
        continue;
      }
      uint64_t start = v->start > mm->ksm_scan_cursor ? v->start : mm->ksm_scan_cursor;
      for (uint64_t va = start;
           va < v->end && work < KSM_MM_WORK_BUDGET &&
           ksm_node_count < KSM_MAX_NODES; va += PAGE_SIZE) {
        work++;
        mm->ksm_scan_cursor = va + PAGE_SIZE;
        uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, va);
        if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER))
          continue;
        uint64_t entry = *pte;
        uint64_t phys = entry & PAGE_MASK;
        if (phys == zero_page_phys || phys == vdso_phys ||
            !pmm_is_managed(phys) || pmm_get_ref((void *)phys) != 1)
          continue;
        /* Already-COW pages have no writable translation to revoke: they
         * were published read-only by fork or an earlier KSM pass. Reusing
         * them as candidates avoids flushing the whole mm on every scan. */
        if (!(entry & PAGE_FLAG_RW) && !(entry & PAGE_FLAG_COW))
          continue;
        pmm_incref((void *)phys);
        if (entry & PAGE_FLAG_RW) {
          *pte = (entry & ~PAGE_FLAG_RW) | PAGE_FLAG_COW;
          needs_initial_flush = true;
        }
        ksm_node_pool[ksm_node_count++] = (struct ksm_node){
            .phys_page = phys, .va = va};
      }
    }
    if (!cur && !sp && work < KSM_MM_WORK_BUDGET &&
        ksm_node_count < KSM_MAX_NODES)
      mm->ksm_scan_cursor = 0; /* Reached the end: wrap on the next pass. */
    spinlock_release(&mm->lock);

    if (first == ksm_node_count) {
      sched_yield();
      continue;
    }
    /* Spinlocks mask IRQs, so the synchronous flush MUST be outside them.
     * No page contents may be read until stale writable mappings are gone. */
    if (needs_initial_flush)
      tlb_shootdown_context_for(cr3);

    /* Hash and compare WITHOUT mm->lock.  Contents are stable here: the
     * frames are frozen (no writable mapping survives the flush above) and
     * our pin keeps a CoW fault copying to a fresh frame instead of
     * dirtying the frozen one.  Holding mm->lock across the hashing and
     * 4 KiB memcmps starved every page fault in this address space - the
     * fault path takes the same lock - for milliseconds each pass.  The
     * hash table is scan-local (ksm_scan_active excludes other scanners),
     * so no other synchronization is needed in this phase. */
    for (uint32_t idx = first; idx < ksm_node_count; idx++) {
      struct ksm_node *snapshot = &ksm_node_pool[idx];
      snapshot->replacement = 0;
      const uint64_t *words = PHYS_TO_VIRT(snapshot->phys_page);
      ksm_global_stats.pages_scanned++;
      if (zero_page_phys && ksm_is_page_zero(words)) {
        snapshot->replacement = zero_page_phys;
        ksm_global_stats.zero_pages_merged++;
        continue;
      }
      uint32_t hash = ksm_hash_page(words);
      snapshot->hash = hash;
      uint32_t bucket = hash % KSM_BUCKETS;
      unsigned probes = 0;
      for (uint32_t curr = ksm_buckets[bucket];
           curr && probes++ < KSM_MAX_CHAIN_PROBES;
           curr = ksm_node_pool[curr].next) {
        struct ksm_node *candidate = &ksm_node_pool[curr];
        if (candidate->hash == hash && candidate->phys_page != snapshot->phys_page &&
            memcmp(PHYS_TO_VIRT(candidate->phys_page), words, PAGE_SIZE) == 0) {
          snapshot->replacement = candidate->phys_page;
          pmm_incref((void *)snapshot->replacement); /* New mapping's reference. */
          ksm_global_stats.pages_sharing++;
          break;
        }
      }
      if (!snapshot->replacement) {
        /* Unmatched pages - including ones whose PTE will turn out
         * CoW-broken below, since a pinned frame's contents survive any
         * break - feed the table as candidates for the rest of this pass. */
        snapshot->next = ksm_buckets[bucket];
        ksm_buckets[bucket] = idx;
        ksm_global_stats.pages_shared++;
      }
    }

    /* Apply the decisions: O(1) PTE revalidate + write per page under the
     * lock, no content hashing or comparisons inside it.  A failed
     * revalidation means the mm CoW-broke or unmapped the page while the
     * lock was dropped; replace and insert decisions for it are discarded
     * (its frozen frame still served as a correct content candidate). */
    spinlock_acquire(&mm->lock);
    uint32_t replaced_pages = 0;
    for (uint32_t idx = first; idx < ksm_node_count; idx++) {
      struct ksm_node *snapshot = &ksm_node_pool[idx];
      uint64_t phys = snapshot->phys_page;
      uint64_t *pte = vmm_get_pte_ptr((uint64_t *)cr3, snapshot->va);
      bool valid = pte && (*pte & PAGE_FLAG_PRESENT) &&
                   (*pte & PAGE_MASK) == phys && !(*pte & PAGE_FLAG_RW);
      if (valid && snapshot->replacement) {
        *pte = snapshot->replacement |
               ((*pte & ~PAGE_MASK & ~PAGE_FLAG_RW) | PAGE_FLAG_COW);
        /* The scan pin on the old frame keeps it alive until the final
         * shootdown and release below. */
        pmm_free_page((void *)phys);
        freed_pages++;
        replaced_pages++;
      } else if (snapshot->replacement && snapshot->replacement != zero_page_phys) {
        /* The comparison reserved a reference for a new mapping. A CoW or
         * unmap during hashing cancels that mapping, so return its reference.
         * The candidate's scan pin still protects hash-table readers. */
        pmm_free_page((void *)snapshot->replacement);
        snapshot->replacement = 0;
      }
    }
    spinlock_release(&mm->lock);
    if (replaced_pages)
      tlb_shootdown_context_for(cr3);
    sched_yield();
  }

  /* Every replaced PTE has completed its remote invalidation. Hash-table
   * references can now be discarded and the old frames returned to PMM. */
  for (uint32_t idx = 1; idx < ksm_node_count; idx++)
    pmm_free_page((void *)ksm_node_pool[idx].phys_page);
  for (int t_idx = 0; t_idx < ntargets; t_idx++)
    mm_put(targets[t_idx].mm, targets[t_idx].cr3);

  ksm_node_count = 1;
  ksm_global_stats.scan_cycles++;

  if (freed_pages > 0) {
    klog_puts("[KSM] scan pass: merged ");
    klog_uint64(freed_pages);
    klog_puts(" pages (");
    klog_uint64((freed_pages * 4) / 1024);
    klog_puts(" MB RAM freed, zero=");
    klog_uint64(ksm_global_stats.zero_pages_merged);
    klog_puts(" sharing=");
    klog_uint64(ksm_global_stats.pages_sharing);
    klog_puts(")\n");
  }

  __atomic_store_n(&ksm_scan_active, 0, __ATOMIC_RELEASE);
  return freed_pages;
}

static void ksm_worker(void) {
  struct thread *self = sched_get_current();
  uint32_t interval = KSM_IDLE_INTERVAL;
  bool idle_mode = true;
  for (;;) {
    /* Eager under memory pressure, sparse in the background: both scan,
     * at different cadences, and each backs off on unproductive passes. */
    bool low_memory = pmm_get_free_pages() < KSM_FREE_EAGER_PAGES;

    if (low_memory != idle_mode) {
      idle_mode = low_memory;
      klog_puts(low_memory
          ? "[KSM] memory pressure: eager scan mode (1.5 s base, max 60 s)\n"
          : "[KSM] plenty of RAM: background dedup (60 s base, max 2 min)\n");
      interval = low_memory ? KSM_EAGER_INTERVAL : KSM_IDLE_INTERVAL;
    }

    if (!ksm_enabled) {
      interval = low_memory ? KSM_EAGER_INTERVAL : KSM_IDLE_INTERVAL;
    } else if (low_memory) {
      if (ksm_scan_pass())
        interval = KSM_EAGER_INTERVAL; /* Productive: stay at full rate. */
      else if (interval < KSM_MAX_EAGER_INTERVAL)
        interval *= 2; /* Nothing to merge: back off. */
    } else {
      __atomic_add_fetch(&ksm_global_stats.passes_idle, 1, __ATOMIC_RELAXED);
      if (ksm_scan_pass())
        interval = KSM_IDLE_INTERVAL; /* Productive: stay at idle cadence. */
      else if (interval < KSM_MAX_IDLE_INTERVAL)
        interval *= 2; /* Nothing to merge: back off. */
    }

    self->state = THREAD_SLEEPING;
    self->wakeup_ticks = lapic_timer_get_ticks() + interval;
    sched_yield();
    self->wakeup_ticks = 0;
    self->state = THREAD_RUNNING;
  }
}

void ksm_init(void) {
  /* Initialization owns the only daemon. Never allocate threads in the
   * timer ISR, which can run before scheduler/KSM initialization. */
  if (ksm_node_pool)
    return;
  ksm_node_pool = kmalloc(KSM_MAX_NODES * sizeof(struct ksm_node));
  if (!ksm_node_pool) {
    klog_puts("[KSM] ERROR: failed to allocate node pool\n");
    return;
  }
  memset(ksm_buckets, 0, sizeof(ksm_buckets));
  ksm_node_count = 1;
  ksm_enabled = true;

  ksm_thread = sched_create_kernel_thread(ksm_worker, NULL, true);
  if (ksm_thread) {
    strcpy(ksm_thread->comm, "ksmd");
    sched_set_priority(ksm_thread, SCHED_PRIORITY_BACKGROUND, 19);
    klog_puts("[KSM] Kernel Samepage Merging daemon (ksmd) initialized.\n");
  } else {
    klog_puts("[KSM] Kernel Samepage Merging initialized (manual scan mode).\n");
  }
}

void ksm_get_stats(struct ksm_stats *out) {
  if (!out)
    return;
  *out = ksm_global_stats;
  out->running = ksm_enabled;
}

void ksm_set_running(bool run) {
  ksm_enabled = run;
  if (run)
    ksm_scan_pass();
}

bool ksm_is_running(void) {
  return ksm_enabled;
}
