#include "vfs.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/tsc.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "arch/uaccess.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define CACHE_BATCH 64u
#define CACHE_NODE_LIMIT 256u

/* Bench-only: force the scatter staging path even when the frames are one
 * contiguous run, so vfs_bench=1 can measure the bounce copy this change
 * removed.  Never set outside the benchmark. */
static bool vfs_readahead_force_scratch;

static uint64_t vfs_cached_pages;

/* Access stamps used for reclaim ordering.  A single global atomic meant every
 * cache hit wrote the same cache line, so cores bounced it between themselves;
 * reclaim only needs a rough recency order, so each CPU owns a private counter
 * and the CPU id is folded into the low bits.  The counters are not globally
 * ordered and a lost increment under preemption is harmless: the stamp feeds a
 * heuristic, never correctness. */
struct vfs_cache_stamp_slot {
  uint64_t counter;
} __attribute__((aligned(64)));

static struct vfs_cache_stamp_slot vfs_cache_stamps[MAX_CPUS];

/* Logical CPU index read straight out of the per-CPU struct through GS.
 * cpu_get_current() goes through rdmsr(MSR_GS_BASE) - a VM exit under KVM -
 * and this runs on every cache hit.  Valid from the moment cpu_init() has
 * installed the GS base, which is long before any VFS traffic. */
static inline uint32_t cache_cpu_id(void) {
  uint32_t id;
  __asm__ volatile("movl %%gs:%c1, %0"
                   : "=r"(id)
                   : "i"(offsetof(struct cpu_info, cpu_id)));
  return id;
}

static uint64_t cache_stamp(void) {
  uint32_t id = cache_cpu_id();
  if (id >= MAX_CPUS)
    id = 0;
  uint64_t tick = ++vfs_cache_stamps[id].counter;
  return (tick << 6) | id;
}

static uint64_t cache_key(uint32_t offset) { return (uint64_t)(offset >> 12); }

/* Every leaf of a vfs_node page-cache tree must point at a live vfs_page_t.
 * The trees have been observed holding wild values after a kernel-heap
 * use-after-free, and the old test (`value >= 0xFFFF800000000000`) accepted
 * any high address, including unmapped ones.  Check the address really is a
 * managed-RAM page (so dereferencing it cannot page-fault) and carries the
 * live marker before touching any of its fields. */
static bool page_value_valid(const void *value) {
  const vfs_page_t *page = (const vfs_page_t *)value;
  if (!page || (uint64_t)page & 7 || !pmm_kernel_ptr_is_managed(page))
    return false;
  return page->magic == VFS_PAGE_MAGIC;
}

static uint64_t cache_invalid_values;

static void cache_report_invalid_value(const void *value) {
  uint64_t count =
      __atomic_add_fetch(&cache_invalid_values, 1, __ATOMIC_RELAXED);
  if (count <= 8) {
    klog_puts(KLOG_CLR_YELLOW
              "[VFS] WARNING: page cache tree holds an invalid entry "
              KLOG_CLR_RESET);
    klog_hex64((uint64_t)value);
    klog_puts("\n");
  }
}

size_t vfs_cache_page_count(void) {
  return (size_t)__atomic_load_n(&vfs_cached_pages, __ATOMIC_RELAXED);
}

static void cache_page_release(vfs_page_t *page) {
  pmm_free_page((void *)page->frame_phys);
  page->magic = 0;
  kfree(page);
}

vfs_page_t *vfs_cache_lookup(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return NULL;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = asc_radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    vfs_page_get(page);
    page->last_used = cache_stamp();
  }
  spinlock_release(&node->pages_lock);
  return page;
}

void vfs_cache_put(vfs_node_t *node, vfs_page_t *page) {
  if (!node || !page)
    return;
  /* No pages_lock: the packed reference word arbitrates the release, so a put
   * never has to disable interrupts just to drop a reference.  See the
   * vfs_page_t comment in vfs.h. */
  if (vfs_page_put(page))
    cache_page_release(page);
}

vfs_page_t *vfs_cache_insert(vfs_node_t *node, uint32_t offset,
                             uint64_t frame) {
  if (!node)
    return NULL;
  offset &= ~(PAGE_SIZE - 1);
  vfs_page_t *new_page = kmalloc(sizeof(*new_page));
  if (!new_page)
    return NULL;
  memset(new_page, 0, sizeof(*new_page));
  new_page->magic = VFS_PAGE_MAGIC;
  new_page->offset = offset;
  new_page->frame_phys = frame;
  new_page->uptodate = true;
  new_page->refs = 1;
  new_page->last_used = cache_stamp();

  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = asc_radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    vfs_page_get(page);
    spinlock_release(&node->pages_lock);
    kfree(new_page);
    pmm_free_page((void *)frame);
    return page;
  }
  if (asc_radix_tree_insert(&node->pages, cache_key(offset), new_page)) {
    spinlock_release(&node->pages_lock);
    kfree(new_page);
    return NULL;
  }
  __atomic_add_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  spinlock_release(&node->pages_lock);
  return new_page;
}

vfs_page_t *vfs_cache_get_or_create(vfs_node_t *node, uint32_t offset) {
  vfs_page_t *page = vfs_cache_lookup(node, offset);
  if (page) {
    while (1) {
      spinlock_acquire(&node->pages_lock);
      bool loading = page->loading;
      bool uptodate = page->uptodate;
      spinlock_release(&node->pages_lock);
      if (!loading) {
        if (uptodate)
          return page;
        vfs_cache_put(node, page);
        return NULL;
      }
      sched_yield();
    }
  }
  void *frame = pmm_alloc_page();
  if (!frame)
    return NULL;

  offset &= ~(PAGE_SIZE - 1);
  vfs_page_t *candidate = kmalloc(sizeof(*candidate));
  if (!candidate) {
    pmm_free_page(frame);
    return NULL;
  }
  memset(candidate, 0, sizeof(*candidate));
  candidate->magic = VFS_PAGE_MAGIC;
  candidate->offset = offset;
  candidate->frame_phys = (uint64_t)frame;
  candidate->loading = true;
  candidate->refs = 1;
  candidate->last_used = cache_stamp();

  bool creator = false;
  spinlock_acquire(&node->pages_lock);
  page = asc_radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    vfs_page_get(page);
  } else if (!asc_radix_tree_insert(&node->pages, cache_key(offset), candidate)) {
    page = candidate;
    creator = true;
    __atomic_add_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  }
  spinlock_release(&node->pages_lock);

  if (!creator) {
    kfree(candidate);
    pmm_free_page(frame);
    if (!page)
      return NULL;
    while (1) {
      spinlock_acquire(&node->pages_lock);
      bool loading = page->loading;
      bool uptodate = page->uptodate;
      spinlock_release(&node->pages_lock);
      if (!loading) {
        if (uptodate)
          return page;
        vfs_cache_put(node, page);
        return NULL;
      }
      sched_yield();
    }
  }

  uint32_t available = node->length > offset ? node->length - offset : 0;
  uint32_t to_read = available > PAGE_SIZE ? PAGE_SIZE : available;
  uint32_t read = 0;
  if (to_read && node->read)
    read = node->read(node, offset, to_read, PHYS_TO_VIRT((uint64_t)frame));
  if (read < PAGE_SIZE)
    memset((uint8_t *)PHYS_TO_VIRT((uint64_t)frame) + read, 0,
           PAGE_SIZE - read);

  spinlock_acquire(&node->pages_lock);
  page->uptodate = read == to_read && !vfs_page_is_evicted(page);
  page->loading = false;
  bool ok = page->uptodate;
  spinlock_release(&node->pages_lock);
  if (!ok) {
    vfs_cache_invalidate(node, offset);
    vfs_cache_put(node, page);
    return NULL;
  }
  return page;
}

#define MAX_READAHEAD_PAGES 64u
#define READAHEAD_SCRATCH_CHUNK 32u

/*
 * Fill `count` consecutive cache frames starting at file offset `offset`.
 *
 * The fast path reads the whole run through one physically contiguous scratch
 * block so the filesystem/block layer issues a single large transfer.  When
 * the PMM cannot supply that block (fragmentation), fall back to smaller
 * scratch chunks before resorting to one page per call; a huge window must
 * never degrade into hundreds of tiny commands.
 *
 * Returns the number of pages that were completely filled.  On a short read
 * the caller must drop the remaining pages: caching zeros for a failed
 * transfer would let a process execute garbage.
 */
static void vfs_readahead_warn_short(uint32_t off, uint32_t got,
                                     uint32_t want) {
  static uint32_t warned;
  if (__atomic_add_fetch(&warned, 1, __ATOMIC_RELAXED) <= 4)
    klogf("[VFS] readahead short read: off=%u got=%u want=%u (pages dropped)\n",
          off, got, want);
}

/* True when pages[first..first+count-1] name one physically contiguous run.
 * Readahead takes frames as contiguous PMM blocks whenever it can, so this is
 * the common case and lets the device fill the cache pages directly. */
static bool vfs_cache_frames_contiguous(vfs_page_t **pages, uint32_t first,
                                        uint32_t count) {
  uint64_t base = pages[first]->frame_phys;
  for (uint32_t i = 1; i < count; i++) {
    if (pages[first + i]->frame_phys != base + (uint64_t)i * PAGE_SIZE)
      return false;
  }
  return true;
}

static uint32_t vfs_cache_fill_pages(vfs_node_t *node, uint32_t offset,
                                     vfs_page_t **pages, uint32_t count) {
  if (!node || !node->read || !count)
    return 0;

  uint32_t done = 0;
  while (done < count) {
    uint32_t chunk = count - done;
    if (chunk > READAHEAD_SCRATCH_CHUNK)
      chunk = READAHEAD_SCRATCH_CHUNK;

    uint32_t chunk_off = offset + done * PAGE_SIZE;
    uint32_t want = chunk * PAGE_SIZE;
    uint32_t valid = node->length > chunk_off ? node->length - chunk_off : 0;
    uint32_t ask = want < valid ? want : valid;

    /* Preferred: the frames are one contiguous run, so the device reads
     * straight into the page-cache memory - one transfer, no bounce copy.
     * The benchmark can force the staging path to price the old form. */
    if (!vfs_readahead_force_scratch && chunk > 1 &&
        vfs_cache_frames_contiguous(pages, done, chunk)) {
      uint8_t *dest = (uint8_t *)PHYS_TO_VIRT(pages[done]->frame_phys);
      uint32_t got = ask ? node->read(node, chunk_off, ask, dest) : 0;
      if (got > ask)
        got = ask;
      if (got < ask) {
        vfs_readahead_warn_short(chunk_off, got, ask);
        return done;
      }
      if (got < want)
        memset(dest + got, 0, want - got);
      done += chunk;
      continue;
    }

    /* Scattered frames: stage the run through a contiguous scratch block so
     * the filesystem/block layer still issues one transfer. */
    uint8_t *scratch = chunk > 1 ? pmm_alloc_pages(chunk) : NULL;

    if (scratch) {
      uint8_t *scratch_virt = (uint8_t *)PHYS_TO_VIRT((uint64_t)scratch);
      uint32_t got = ask ? node->read(node, chunk_off, ask, scratch_virt) : 0;
      if (got > ask)
        got = ask;
      if (got < ask) {
        vfs_readahead_warn_short(chunk_off, got, ask);
        pmm_free_pages(scratch, chunk);
        return done;
      }
      if (got < want)
        memset(scratch_virt + got, 0, want - got);

      for (uint32_t i = 0; i < chunk; i++) {
        memcpy(PHYS_TO_VIRT(pages[done + i]->frame_phys),
               scratch_virt + (size_t)i * PAGE_SIZE, PAGE_SIZE);
      }
      pmm_free_pages(scratch, chunk);
      done += chunk;
      continue;
    }

    /* One page (or an exhausted PMM): read directly into the frame. */
    uint32_t page_off = offset + done * PAGE_SIZE;
    uint32_t avail = node->length > page_off ? node->length - page_off : 0;
    uint32_t to_read = avail > PAGE_SIZE ? PAGE_SIZE : avail;
    void *frame_virt = PHYS_TO_VIRT(pages[done]->frame_phys);
    uint32_t read = to_read ? node->read(node, page_off, to_read, frame_virt) : 0;
    if (read < to_read) {
      vfs_readahead_warn_short(page_off, read, to_read);
      return done;
    }
    if (read < PAGE_SIZE)
      memset((uint8_t *)frame_virt + read, 0, PAGE_SIZE - read);
    done++;
  }
  return done;
}

uint32_t vfs_cache_readahead(vfs_node_t *node, uint32_t offset, uint32_t max_bytes) {
  if (!node || !node->read || !max_bytes || offset >= node->length)
    return 0;

  offset &= ~(PAGE_SIZE - 1);
  if (offset >= node->length)
    return 0;

  uint32_t available = node->length - offset;
  if (max_bytes > available)
    max_bytes = available;

  uint32_t max_pages = (max_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  if (max_pages > MAX_READAHEAD_PAGES)
    max_pages = MAX_READAHEAD_PAGES;
  if (max_pages == 0)
    return 0;

  /* Skip any already-cached leading pages, then prefetch the first run of
   * missing pages.  This matters for the async exec worker: the ELF header
   * read leaves page 0 cached, and bailing on that alone would make the whole
   * first window a no-op. */
  uint32_t start_index = 0;
  uint32_t missing_count = 0;
  spinlock_acquire(&node->pages_lock);
  for (uint32_t i = 0; i < max_pages; i++) {
    uint32_t page_off = offset + i * PAGE_SIZE;
    if (page_off >= node->length)
      break;
    vfs_page_t *existing = asc_radix_tree_lookup(&node->pages, cache_key(page_off));
    if (existing) {
      if (missing_count == 0) {
        start_index = i + 1; /* leading present page: keep looking */
        continue;
      }
      break; /* end of the first missing run */
    }
    missing_count++;
  }
  spinlock_release(&node->pages_lock);

  if (missing_count <= 1)
    return 0;

  uint32_t first_off = offset + start_index * PAGE_SIZE;

  void *frames[MAX_READAHEAD_PAGES] = {0};
  vfs_page_t *candidates[MAX_READAHEAD_PAGES] = {0};
  uint32_t allocated = 0;

  /* Take frames as contiguous PMM runs whenever possible so the fill below
   * can DMA/read straight into them; fall back to single pages when the
   * buddy allocator cannot supply a block. */
  while (allocated < missing_count) {
    uint32_t chunk = missing_count - allocated;
    if (chunk > READAHEAD_SCRATCH_CHUNK)
      chunk = READAHEAD_SCRATCH_CHUNK;

    void *run = chunk > 1 ? pmm_alloc_pages(chunk) : NULL;
    if (run) {
      /* pmm_alloc_pages rounds up to a power of two; hand the unused tail
       * pages straight back so only the frames stay allocated. */
      uint32_t run_pages = 1;
      while (run_pages < chunk)
        run_pages <<= 1;
      for (uint32_t k = chunk; k < run_pages; k++)
        pmm_free_page((void *)((uint64_t)run + (uint64_t)k * PAGE_SIZE));
    }

    uint32_t got = 0;
    for (uint32_t j = 0; j < chunk; j++) {
      void *frame = run ? (void *)((uint64_t)run + (uint64_t)j * PAGE_SIZE)
                        : pmm_alloc_page();
      if (!frame)
        break;
      vfs_page_t *candidate = kmalloc(sizeof(vfs_page_t));
      if (!candidate) {
        if (run) {
          for (uint32_t k = j; k < chunk; k++)
            pmm_free_page((void *)((uint64_t)run + (uint64_t)k * PAGE_SIZE));
        } else {
          pmm_free_page(frame);
        }
        break;
      }
      memset(candidate, 0, sizeof(vfs_page_t));
      candidate->magic = VFS_PAGE_MAGIC;
      candidate->offset = first_off + (allocated + j) * PAGE_SIZE;
      candidate->frame_phys = (uint64_t)frame;
      candidate->loading = true;
      candidate->refs = 1;
      candidate->last_used = cache_stamp();
      frames[allocated + j] = frame;
      candidates[allocated + j] = candidate;
      got++;
    }
    allocated += got;
    if (got < chunk)
      break;
  }

  if (allocated == 0)
    return 0;

  uint32_t inserted = 0;
  spinlock_acquire(&node->pages_lock);
  for (uint32_t i = 0; i < allocated; i++) {
    uint32_t page_off = first_off + i * PAGE_SIZE;
    vfs_page_t *existing = asc_radix_tree_lookup(&node->pages, cache_key(page_off));
    if (existing || asc_radix_tree_insert(&node->pages, cache_key(page_off), candidates[i])) {
      break;
    }
    __atomic_add_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
    inserted++;
  }
  spinlock_release(&node->pages_lock);

  for (uint32_t i = inserted; i < allocated; i++) {
    pmm_free_page(frames[i]);
    kfree(candidates[i]);
  }

  if (inserted == 0)
    return 0;

  /* Bulk-read the missing window into the candidate frames. */
  uint32_t filled = vfs_cache_fill_pages(node, first_off, candidates, inserted);

  spinlock_acquire(&node->pages_lock);
  for (uint32_t i = 0; i < inserted; i++) {
    candidates[i]->uptodate = (i < filled);
    candidates[i]->loading = false;
  }
  spinlock_release(&node->pages_lock);

  /* Pages that could not be read must not stay in the tree as valid zeros;
   * drop them so a later attempt can retry instead of poisoning the file. */
  for (uint32_t i = filled; i < inserted; i++) {
    vfs_cache_invalidate(node, candidates[i]->offset);
  }

  for (uint32_t i = 0; i < inserted; i++) {
    vfs_cache_put(node, candidates[i]);
  }

  return filled * PAGE_SIZE;
}

/* ── Asynchronous prefetch queue ──────────────────────────────────────────
 * execve() queues the file-backed parts of a new program here instead of
 * blocking until every window has been read.  A kernel thread runs the
 * existing synchronous vfs_cache_readahead() while the process is already
 * starting: pages are inserted with loading=true, so a demand fault on a page
 * the worker is filling waits for it instead of issuing a second read.
 */
#define VFS_PREFETCH_QUEUE_MAX 64u

struct vfs_prefetch_job {
  vfs_node_t *node;
  uint32_t offset;
  uint32_t length;
  struct vfs_prefetch_job *next;
};

static spinlock_t vfs_prefetch_lock = SPINLOCK_INIT;
static struct vfs_prefetch_job *vfs_prefetch_head;
static struct vfs_prefetch_job *vfs_prefetch_tail;
static uint32_t vfs_prefetch_depth;
static wait_queue_t vfs_prefetch_wait;
static volatile int vfs_prefetch_up;
static volatile int vfs_prefetch_starting;

static void vfs_prefetch_worker(void) {
  struct thread *self = sched_get_current();
  for (;;) {
    struct vfs_prefetch_job *job = NULL;

    spinlock_acquire(&vfs_prefetch_lock);
    if (vfs_prefetch_head) {
      job = vfs_prefetch_head;
      vfs_prefetch_head = job->next;
      if (!vfs_prefetch_head)
        vfs_prefetch_tail = NULL;
      vfs_prefetch_depth--;
    }
    spinlock_release(&vfs_prefetch_lock);

    if (!job) {
      if (!self)
        return;
      wait_queue_entry_t entry = { .thread = self, .next = NULL };

      /* Block first, then link: a wake that lands between the two must not be
       * overwritten by a later THREAD_BLOCKED store (lost-wakeup race). */
      self->state = THREAD_BLOCKED;
      wait_queue_add(&vfs_prefetch_wait, &entry);

      spinlock_acquire(&vfs_prefetch_lock);
      bool have_work = vfs_prefetch_head != NULL;
      spinlock_release(&vfs_prefetch_lock);
      if (have_work) {
        self->state = THREAD_RUNNING;
        wait_queue_remove(&vfs_prefetch_wait, &entry);
        continue;
      }

      self->wakeup_ticks = lapic_timer_get_ticks() + 100;
      sched_yield();
      self->wakeup_ticks = 0;
      self->state = THREAD_RUNNING;
      wait_queue_remove(&vfs_prefetch_wait, &entry);
      continue;
    }

    vfs_cache_readahead(job->node, job->offset, job->length);
    vfs_close(job->node);
    kfree(job);
  }
}

static void vfs_prefetch_start(void) {
  if (__atomic_load_n(&vfs_prefetch_up, __ATOMIC_ACQUIRE))
    return;

  int expected = 0;
  if (!__atomic_compare_exchange_n(&vfs_prefetch_starting, &expected, 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return;

  wait_queue_init(&vfs_prefetch_wait);
  struct thread *worker =
      sched_create_kernel_thread(vfs_prefetch_worker, NULL, true);
  if (worker) {
    strcpy(worker->comm, "vfs-prefetch");
    __atomic_store_n(&vfs_prefetch_up, 1, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&vfs_prefetch_starting, 0, __ATOMIC_RELEASE);
  }
}

void vfs_cache_prefetch_async(vfs_node_t *node, uint32_t offset,
                              uint32_t length) {
  if (!node || !length)
    return;

  vfs_prefetch_start();
  if (!__atomic_load_n(&vfs_prefetch_up, __ATOMIC_ACQUIRE)) {
    /* No worker (early boot or OOM): keep the caller synchronous. */
    vfs_cache_readahead(node, offset, length);
    return;
  }

  struct vfs_prefetch_job *job = kmalloc(sizeof(*job));
  if (!job) {
    vfs_cache_readahead(node, offset, length);
    return;
  }

  vfs_open(node);
  job->node = node;
  job->offset = offset;
  job->length = length;
  job->next = NULL;

  bool queued = false;
  spinlock_acquire(&vfs_prefetch_lock);
  if (vfs_prefetch_depth < VFS_PREFETCH_QUEUE_MAX) {
    if (vfs_prefetch_tail)
      vfs_prefetch_tail->next = job;
    else
      vfs_prefetch_head = job;
    vfs_prefetch_tail = job;
    vfs_prefetch_depth++;
    queued = true;
  }
  spinlock_release(&vfs_prefetch_lock);

  if (!queued) {
    /* Queue full: leave the pages to demand paging rather than blocking the
     * caller. */
    vfs_close(node);
    kfree(job);
    return;
  }

  wait_queue_wake_one(&vfs_prefetch_wait);
}

uint32_t vfs_cache_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  if (!node || !buffer || !size || offset >= node->length)
    return 0;
  if (size > node->length - offset)
    size = node->length - offset;

  uint32_t done = 0;
  while (done < size) {
    uint32_t current = offset + done;
    uint32_t page_offset = current & ~(PAGE_SIZE - 1);
    uint32_t in_page = current & (PAGE_SIZE - 1);
    uint32_t count = PAGE_SIZE - in_page;
    if (count > size - done)
      count = size - done;

    vfs_page_t *page = vfs_cache_lookup(node, page_offset);
    if (!page) {
      if (node->length >= 64 * 1024) {
        uint32_t ra_bytes = 128 * 1024;
        if (size - done > ra_bytes)
          ra_bytes = (size - done + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (ra_bytes > 256 * 1024)
          ra_bytes = 256 * 1024;
        vfs_cache_readahead(node, page_offset, ra_bytes);
      }
      page = vfs_cache_get_or_create(node, page_offset);
    }
    if (!page)
      break;
    if (is_user_ptr((uint64_t)buffer)) {
      unsigned long uncopied = copy_to_user(
          buffer + done,
          (uint8_t *)PHYS_TO_VIRT(page->frame_phys) + in_page, count);
      uint32_t copied = count - (uint32_t)uncopied;
      done += copied;
      vfs_cache_put(node, page);
      if (uncopied > 0)
        break;
    } else {
      memcpy(buffer + done,
             (uint8_t *)PHYS_TO_VIRT(page->frame_phys) + in_page, count);
      vfs_cache_put(node, page);
      done += count;
    }
  }
  return done;
}

/* Remove `key` from the tree and mark the page evicted.  Returns the page, or
 * NULL when it was not cached.  `releasable` is set when the caller must free
 * it: no transient reference existed and no racing put can own the final
 * release (see the packed-refcount note in vfs.h). */
static vfs_page_t *cache_delete_locked(vfs_node_t *node, uint64_t key,
                                       bool *releasable) {
  vfs_page_t *page = asc_radix_tree_delete(&node->pages, key);
  if (!page)
    return NULL;
  *releasable = vfs_page_evict(page);
  __atomic_sub_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  return page;
}

void vfs_cache_invalidate(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return;
  bool releasable = false;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = cache_delete_locked(node, cache_key(offset), &releasable);
  spinlock_release(&node->pages_lock);
  if (page && releasable)
    cache_page_release(page);
}

struct key_batch {
  uint64_t keys[CACHE_BATCH];
  uint32_t count;
  uint64_t first;
  uint64_t last;
  bool unused_only;
};

static bool collect_keys(uint64_t key, void *value, void *opaque) {
  struct key_batch *batch = opaque;
  vfs_page_t *page = value;
  if (!page_value_valid(page)) {
    cache_report_invalid_value(value);
    return true;
  }
  if (key < batch->first || key > batch->last)
    return true;
  if (batch->unused_only &&
      (vfs_page_ref_count(page) || page->dirty || page->loading ||
       page->writeback || pmm_get_ref((void *)page->frame_phys) != 1))
    return true;
  batch->keys[batch->count++] = key;
  return batch->count < CACHE_BATCH;
}

static void cache_remove_range(vfs_node_t *node, uint64_t first, uint64_t last,
                               bool unused_only) {
  while (1) {
    struct key_batch batch = {
        .first = first, .last = last, .unused_only = unused_only};
    bool complete = asc_radix_tree_for_each_range(&node->pages, first, last,
                                               collect_keys, &batch);
    if (!batch.count)
      break;
    for (uint32_t i = 0; i < batch.count; i++) {
      bool releasable = false;
      vfs_page_t *page =
          cache_delete_locked(node, batch.keys[i], &releasable);
      if (page && releasable)
        cache_page_release(page);
    }
    if (complete)
      break;
  }
}

void vfs_cache_invalidate_range(vfs_node_t *node, uint32_t offset,
                                uint32_t length) {
  if (!node || !length)
    return;
  uint64_t first = cache_key(offset);
  uint64_t end = (uint64_t)offset + length - 1;
  uint64_t last = end >> 12;
  spinlock_acquire(&node->pages_lock);
  cache_remove_range(node, first, last, false);
  spinlock_release(&node->pages_lock);
}

void vfs_cache_update_or_invalidate(vfs_node_t *node, uint32_t offset,
                                    uint32_t length, const uint8_t *buffer) {
  if (!node || !length)
    return;

  uint32_t written = 0;
  while (written < length) {
    uint32_t cur_offset = offset + written;
    uint32_t page_offset = cur_offset & (PAGE_SIZE - 1);
    uint32_t to_copy = PAGE_SIZE - page_offset;
    if (to_copy > length - written)
      to_copy = length - written;

    spinlock_acquire(&node->pages_lock);
    vfs_page_t *page = asc_radix_tree_lookup(&node->pages, cache_key(cur_offset));
    if (page && page->frame_phys && !page->loading &&
        !vfs_page_is_evicted(page)) {
      if (page->uptodate) {
        void *page_virt = PHYS_TO_VIRT(page->frame_phys);
        if (buffer) {
          if (is_user_ptr((uint64_t)buffer))
            copy_from_user((uint8_t *)page_virt + page_offset, buffer + written, to_copy);
          else
            memcpy((uint8_t *)page_virt + page_offset, buffer + written, to_copy);
        }
        page->last_used = cache_stamp();
      } else {
        cache_remove_range(node, cache_key(cur_offset), cache_key(cur_offset), false);
      }
    }
    spinlock_release(&node->pages_lock);

    written += to_copy;
  }
}

static void cache_destroy_value(void *value) {
  vfs_page_t *page = value;
  if (!page_value_valid(page)) {
    cache_report_invalid_value(value);
    return;
  }
  __atomic_sub_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  if (vfs_page_evict(page))
    cache_page_release(page);
}

void vfs_cache_clear(vfs_node_t *node) {
  if (!vfs_node_is_alive(node))
    return;
  spinlock_acquire(&node->pages_lock);
  asc_radix_tree_destroy(&node->pages, cache_destroy_value);
  spinlock_release(&node->pages_lock);
}

void vfs_cache_clear_unused(vfs_node_t *node) {
  if (!vfs_node_is_alive(node) || !(node->flags & FS_PAGE_CACHE))
    return;
  spinlock_acquire(&node->pages_lock);
  cache_remove_range(node, 0, UINT64_MAX, true);
  spinlock_release(&node->pages_lock);
}

struct reclaim_search {
  uint64_t key;
  uint64_t stamp;
  bool found;
};

static bool find_reclaimable(uint64_t key, void *value, void *opaque) {
  vfs_page_t *page = value;
  if (!page_value_valid(page)) {
    cache_report_invalid_value(value);
    return true;
  }
  struct reclaim_search *search = opaque;
  if (vfs_page_ref_count(page) || page->dirty || page->loading ||
      page->writeback || pmm_get_ref((void *)page->frame_phys) != 1)
    return true;
  if (!search->found || page->last_used < search->stamp) {
    search->found = true;
    search->key = key;
    search->stamp = page->last_used;
  }
  return true;
}

size_t vfs_cache_reclaim(vfs_node_t *node, size_t target) {
  if (!node || !target)
    return 0;
  size_t reclaimed = 0;
  spinlock_acquire(&node->pages_lock);
  while (reclaimed < target) {
    struct reclaim_search search = {0};
    asc_radix_tree_for_each(&node->pages, find_reclaimable, &search);
    if (!search.found)
      break;
    bool releasable = false;
    vfs_page_t *page = cache_delete_locked(node, search.key, &releasable);
    if (!page)
      continue;
    if (releasable)
      cache_page_release(page);
    reclaimed++;
  }
  spinlock_release(&node->pages_lock);
  return reclaimed;
}

void vfs_cache_mark_dirty(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = asc_radix_tree_lookup(&node->pages, cache_key(offset));
  if (page && page->uptodate && !vfs_page_is_evicted(page)) {
    page->dirty = true;
    page->dirty_seq++;
    page->last_used = cache_stamp();
  }
  spinlock_release(&node->pages_lock);
}

struct dirty_search {
  vfs_page_t *page;
  uint64_t dirty_seq;
};

static bool find_dirty(uint64_t key, void *value, void *opaque) {
  (void)key;
  struct dirty_search *search = opaque;
  vfs_page_t *page = value;
  if (!page_value_valid(page)) {
    cache_report_invalid_value(value);
    return true;
  }
  if (!page->dirty || page->loading || page->writeback)
    return true;
  page->writeback = true;
  vfs_page_get(page);
  search->page = page;
  search->dirty_seq = page->dirty_seq;
  return false;
}

void vfs_cache_sync(vfs_node_t *node) {
  if (!node || !node->write)
    return;
  while (1) {
    struct dirty_search search = {0};
    spinlock_acquire(&node->pages_lock);
    asc_radix_tree_for_each(&node->pages, find_dirty, &search);
    spinlock_release(&node->pages_lock);
    if (!search.page)
      break;
    uint32_t available = node->length > search.page->offset
                             ? node->length - search.page->offset
                             : 0;
    uint32_t amount = available > PAGE_SIZE ? PAGE_SIZE : available;
    uint32_t written = amount ? node->write(
                                    node, search.page->offset, amount,
                                    PHYS_TO_VIRT(search.page->frame_phys))
                              : 0;
    spinlock_acquire(&node->pages_lock);
    if (written == amount && search.page->dirty_seq == search.dirty_seq)
      search.page->dirty = false;
    search.page->writeback = false;
    bool failed = written != amount;
    spinlock_release(&node->pages_lock);
    vfs_cache_put(node, search.page);
    if (failed)
      break;
  }
}

bool vfs_cache_phase3_stress_test(void) {
  vfs_node_t node;
  vfs_node_init(&node);
  size_t cached_before = vfs_cache_page_count();
  uint64_t frames[128] = {0};
  bool pass = true;

  for (uint32_t i = 0; i < 128; i++) {
    void *frame = pmm_alloc_page();
    if (!frame) {
      pass = false;
      break;
    }
    uint32_t offset = i * 17u * PAGE_SIZE;
    frames[i] = (uint64_t)frame;
    vfs_page_t *page = vfs_cache_insert(&node, offset, (uint64_t)frame);
    if (!page || page->offset != offset) {
      if (!page)
        pmm_free_page(frame);
      pass = false;
      break;
    }
    vfs_cache_put(&node, page);
  }

  for (uint32_t operation = 0; pass && operation < 100000; operation++) {
    uint32_t slot = (operation * 73u) & 127u;
    uint32_t offset = slot * 17u * PAGE_SIZE;
    vfs_page_t *page = vfs_cache_lookup(&node, offset);
    if (!page || page->offset != offset)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
  }

  /* A duplicate insertion must preserve the existing page and consume the
   * caller's redundant frame, matching the cache insertion contract. */
  void *duplicate_frame = pmm_alloc_page();
  vfs_page_t *original = vfs_cache_lookup(&node, 126u * 17u * PAGE_SIZE);
  vfs_page_t *duplicate = duplicate_frame
      ? vfs_cache_insert(&node, 126u * 17u * PAGE_SIZE,
                         (uint64_t)duplicate_frame)
      : NULL;
  if (!original || duplicate != original ||
      pmm_get_ref(duplicate_frame) != 0)
    pass = false;
  if (duplicate)
    vfs_cache_put(&node, duplicate);
  if (original)
    vfs_cache_put(&node, original);

  /* Invalidation removes lookup visibility immediately, but a held cache
   * value and its frame remain valid until the final put. */
  vfs_page_t *held = vfs_cache_lookup(&node, 127u * 17u * PAGE_SIZE);
  if (!held) {
    pass = false;
  } else {
    uint64_t held_frame = held->frame_phys;
    vfs_cache_invalidate(&node, held->offset);
    vfs_page_t *gone = vfs_cache_lookup(&node, held->offset);
    if (gone || !vfs_page_is_evicted(held) ||
        pmm_get_ref((void *)held_frame) != 1)
      pass = false;
    if (gone)
      vfs_cache_put(&node, gone);
    vfs_cache_put(&node, held);
    if (pmm_get_ref((void *)held_frame) != 0)
      pass = false;
  }

  vfs_cache_invalidate_range(&node, 0, 64u * 17u * PAGE_SIZE);
  for (uint32_t i = 0; pass && i < 128; i++) {
    uint32_t offset = i * 17u * PAGE_SIZE;
    vfs_page_t *page = vfs_cache_lookup(&node, offset);
    bool should_exist = i >= 64 && i != 127;
    if (!!page != should_exist)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
  }

  vfs_cache_clear(&node);
  for (uint32_t i = 0; i < 128; i++) {
    if (frames[i] && pmm_get_ref((void *)frames[i]) != 0)
      pass = false;
  }
  return pass && asc_radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
}

struct phase4_file {
  uint8_t *data;
  uint32_t length;
  uint32_t read_calls;
  uint32_t write_calls;
};

static uint32_t phase4_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  struct phase4_file *file = node->device;
  file->read_calls++;
  if (offset >= file->length)
    return 0;
  if (size > file->length - offset)
    size = file->length - offset;
  memcpy(buffer, file->data + offset, size);
  return size;
}

static uint32_t phase4_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  struct phase4_file *file = node->device;
  file->write_calls++;
  if (offset >= file->length)
    return 0;
  if (size > file->length - offset)
    size = file->length - offset;
  memcpy(file->data + offset, buffer, size);
  return size;
}

bool vfs_cache_phase4_stress_test(void) {
  const uint32_t file_size = 3u * PAGE_SIZE + 123u;
  const uint32_t read_offset = PAGE_SIZE - 37u;
  const uint32_t read_size = PAGE_SIZE + 211u;
  size_t cached_before = vfs_cache_page_count();
  struct phase4_file file = {0};
  vfs_node_t node;
  bool pass = true;

  file.data = kmalloc(file_size);
  uint8_t *buffer = kmalloc(read_size);
  uint8_t *second = kmalloc(read_size);
  if (!file.data || !buffer || !second) {
    if (file.data)
      kfree(file.data);
    if (buffer)
      kfree(buffer);
    if (second)
      kfree(second);
    return false;
  }
  file.length = file_size;
  for (uint32_t i = 0; i < file_size; i++)
    file.data[i] = (uint8_t)((i * 29u + (i >> 5) + 17u) & 0xffu);

  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = file_size;
  node.device = &file;
  node.read = phase4_read;
  node.write = phase4_write;

  if (vfs_read(&node, read_offset, read_size, buffer) != read_size ||
      memcmp(buffer, file.data + read_offset, read_size))
    pass = false;
  uint32_t reads_after_fill = file.read_calls;
  for (uint32_t i = 0; pass && i < 100000u; i++) {
    if (vfs_read(&node, read_offset, read_size, second) != read_size ||
        memcmp(second, buffer, read_size))
      pass = false;
  }
  if (file.read_calls != reads_after_fill || reads_after_fill != 3u)
    pass = false;

  vfs_page_t *mapped = vfs_cache_get_or_create(&node, PAGE_SIZE);
  vfs_page_t *shared = vfs_cache_lookup(&node, PAGE_SIZE);
  if (!mapped || !shared || mapped->frame_phys != shared->frame_phys)
    pass = false;
  if (shared)
    vfs_cache_put(&node, shared);
  if (mapped)
    vfs_cache_put(&node, mapped);

  if (vfs_read(&node, file_size - 19u, 64u, second) != 19u ||
      memcmp(second, file.data + file_size - 19u, 19u))
    pass = false;
  vfs_page_t *past_eof = vfs_cache_lookup(&node, file_size + PAGE_SIZE);
  if (past_eof)
    pass = false;
  if (past_eof)
    vfs_cache_put(&node, past_eof);

  uint8_t replacement[73];
  memset(replacement, 0xa5, sizeof(replacement));
  uint32_t reads_before_write = file.read_calls;
  if (vfs_write(&node, PAGE_SIZE + 9u, sizeof(replacement), replacement) !=
          sizeof(replacement) ||
      file.write_calls != 1u ||
      vfs_read(&node, PAGE_SIZE + 9u, sizeof(replacement), second) !=
          sizeof(replacement) ||
      memcmp(second, replacement, sizeof(replacement)) ||
      file.read_calls != reads_before_write + 1u)
    pass = false;

  vfs_cache_clear(&node);
  pass = pass && asc_radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
  kfree(second);
  kfree(buffer);
  kfree(file.data);
  return pass;
}

struct phase5_file {
  uint32_t writes;
  uint32_t last_offset;
  uint8_t first_bytes[64];
  bool fail_writes;
};

static uint32_t phase5_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  (void)node;
  for (uint32_t i = 0; i < size; i++)
    buffer[i] = (uint8_t)(((offset + i) * 13u + 41u) & 0xffu);
  return size;
}

static uint32_t phase5_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  struct phase5_file *file = node->device;
  file->writes++;
  file->last_offset = offset;
  uint32_t copy = size < sizeof(file->first_bytes) ? size
                                                   : sizeof(file->first_bytes);
  memcpy(file->first_bytes, buffer, copy);
  return file->fail_writes ? 0 : size;
}

bool vfs_cache_phase5_stress_test(void) {
  const uint32_t pages = CACHE_NODE_LIMIT + 64u;
  size_t cached_before = vfs_cache_page_count();
  struct phase5_file file = {0};
  vfs_node_t node;
  bool pass = true;

  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = pages * PAGE_SIZE;
  node.device = &file;
  node.read = phase5_read;
  node.write = phase5_write;

  vfs_page_t *held = vfs_cache_get_or_create(&node, 0);
  if (!held)
    pass = false;
  for (uint32_t i = 1; pass && i < pages; i++) {
    vfs_page_t *page = vfs_cache_get_or_create(&node, i * PAGE_SIZE);
    if (!page)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
    spinlock_acquire(&node.pages_lock);
    size_t excess = node.pages.entries > CACHE_NODE_LIMIT
                        ? (size_t)(node.pages.entries - CACHE_NODE_LIMIT)
                        : 0;
    spinlock_release(&node.pages_lock);
    if (excess && vfs_cache_reclaim(&node, excess) != excess)
      pass = false;
  }
  spinlock_acquire(&node.pages_lock);
  uint64_t bounded_entries = node.pages.entries;
  spinlock_release(&node.pages_lock);
  vfs_page_t *held_lookup = vfs_cache_lookup(&node, 0);
  if (bounded_entries > CACHE_NODE_LIMIT || !held_lookup)
    pass = false;
  if (held_lookup)
    vfs_cache_put(&node, held_lookup);
  if (held)
    vfs_cache_put(&node, held);

  uint32_t dirty_offset = (pages - 1u) * PAGE_SIZE;
  vfs_page_t *dirty = vfs_cache_get_or_create(&node, dirty_offset);
  uint8_t expected[64];
  memset(expected, 0x6d, sizeof(expected));
  if (!dirty) {
    pass = false;
  } else {
    memcpy(PHYS_TO_VIRT(dirty->frame_phys), expected, sizeof(expected));
    vfs_cache_mark_dirty(&node, dirty_offset);
    vfs_cache_put(&node, dirty);
  }

  file.fail_writes = true;
  vfs_cache_sync(&node);
  dirty = vfs_cache_lookup(&node, dirty_offset);
  if (!dirty || !dirty->dirty || file.writes != 1u)
    pass = false;
  if (dirty)
    vfs_cache_put(&node, dirty);

  file.fail_writes = false;
  vfs_cache_sync(&node);
  dirty = vfs_cache_lookup(&node, dirty_offset);
  if (!dirty || dirty->dirty || file.writes != 2u ||
      file.last_offset != dirty_offset ||
      memcmp(file.first_bytes, expected, sizeof(expected)))
    pass = false;
  if (dirty)
    vfs_cache_put(&node, dirty);

  spinlock_acquire(&node.pages_lock);
  size_t remaining = (size_t)node.pages.entries;
  spinlock_release(&node.pages_lock);
  if (vfs_cache_reclaim(&node, remaining) != remaining)
    pass = false;
  vfs_cache_clear(&node);
  return pass && asc_radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
}

/* ── `vfs_selftest=1` / `vfs_bench=1` page-cache pieces ─────────────────────
 *
 * The selftest pins the packed-reference arbitration: a held reference keeps an
 * evicted page alive until its final put, eviction with no users frees right
 * away, and a failed load leaves nothing behind.  The benchmark contrasts the
 * lock-free put against the old lock+IRQ-mask form, and the per-CPU stamp
 * against the global atomic it replaced - same working set, back to back.
 * Entry points and gating live in vfs.c. */

bool vfs_cache_ref_selftest(void) {
  bool pass = true;
  size_t cached_before = vfs_cache_page_count();
  vfs_node_t node;
  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = 4 * PAGE_SIZE;

  /* 1. A held reference outlives eviction; the last put frees the page. */
  void *frame = pmm_alloc_page();
  if (!frame) {
    vfs_cache_clear(&node);
    return false;
  }
  vfs_page_t *page = vfs_cache_insert(&node, 0, (uint64_t)frame);
  if (!page) {
    pmm_free_page(frame);
    vfs_cache_clear(&node);
    return false;
  }
  vfs_cache_put(&node, page); /* cached, no transient users now */

  vfs_page_t *held = vfs_cache_lookup(&node, 0);
  if (!held || vfs_page_ref_count(held) != 1)
    pass = false;
  if (held) {
    uint64_t held_frame = held->frame_phys;
    vfs_cache_invalidate(&node, 0);
    if (!vfs_page_is_evicted(held) || pmm_get_ref((void *)held_frame) != 1)
      pass = false;
    vfs_page_t *gone = vfs_cache_lookup(&node, 0);
    if (gone) {
      pass = false;
      vfs_cache_put(&node, gone);
    }
    vfs_cache_put(&node, held);
  }

  /* 2. Eviction with no transient users frees immediately. */
  void *frame2 = pmm_alloc_page();
  if (!frame2) {
    pass = false;
  } else {
    vfs_page_t *page2 = vfs_cache_insert(&node, PAGE_SIZE, (uint64_t)frame2);
    if (!page2) {
      pmm_free_page(frame2);
      pass = false;
    } else {
      vfs_cache_put(&node, page2);
      vfs_cache_invalidate(&node, PAGE_SIZE);
    }
  }

  /* 3. A failed load (no read callback) must not stay cached as valid. */
  vfs_page_t *failed = vfs_cache_get_or_create(&node, 2 * PAGE_SIZE);
  if (failed) {
    pass = false;
    vfs_cache_put(&node, failed);
  }
  vfs_page_t *lingering = vfs_cache_lookup(&node, 2 * PAGE_SIZE);
  if (lingering) {
    pass = false;
    vfs_cache_put(&node, lingering);
  }

  vfs_cache_clear(&node);
  return pass && asc_radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
}

static void vfs_cache_bench_report(const char *name, uint64_t ops,
                                   uint64_t cycles) {
  uint64_t ns = tsc_cycles_to_ns(cycles);
  klog_puts("[VFS-BENCH] ");
  klog_puts(name);
  klog_puts(": ");
  klog_uint64(ops);
  klog_puts(" ops, ");
  klog_uint64(ops ? cycles / ops : 0);
  klog_puts(" cycles/op");
  if (ns) {
    klog_puts(", ");
    klog_uint64(ops ? ns / ops : 0);
    klog_puts(" ns/op");
  }
  klog_putchar('\n');
}

/* ── Multi-CPU lookup+put scaling ───────────────────────────────────────────
 * The single-CPU pair above cannot attribute contention: several CPUs on the
 * same file can serialize on pages_lock, on the page's refcount line, or not
 * at all.  Run the same fan-out in three shapes and compare each with the
 * single-CPU cycles/op reported above:
 *
 *   same page   - all workers on one page: worst case for both lines
 *   spread page - one page per worker on one node: isolates pages_lock
 *   per node    - one node per worker: the no-sharing baseline
 *
 * `sched_create_kernel_thread` takes no argument, so each worker reads its
 * context from the shared array. */
#define VFS_PC_BENCH_THREADS 3
#define VFS_PC_BENCH_CPUS (VFS_PC_BENCH_THREADS + 1)
#define VFS_PC_BENCH_ITERS 200000

struct vfs_pc_bench_ctx {
  vfs_node_t *node;
  uint32_t offset;
  uint64_t iters;
};

static struct vfs_pc_bench_ctx vfs_pc_bench_ctxs[VFS_PC_BENCH_THREADS];
static volatile uint32_t vfs_pc_bench_go;
static volatile uint32_t vfs_pc_bench_done;

static void vfs_pc_bench_worker(struct vfs_pc_bench_ctx *ctx) {
  while (!__atomic_load_n(&vfs_pc_bench_go, __ATOMIC_ACQUIRE))
    sched_yield();
  for (uint64_t i = 0; i < ctx->iters; i++) {
    vfs_page_t *page = vfs_cache_lookup(ctx->node, ctx->offset);
    if (page)
      vfs_cache_put(ctx->node, page);
  }
  __atomic_add_fetch(&vfs_pc_bench_done, 1, __ATOMIC_ACQ_REL);
}

static void vfs_pc_bench_worker0(void) {
  vfs_pc_bench_worker(&vfs_pc_bench_ctxs[0]);
}
static void vfs_pc_bench_worker1(void) {
  vfs_pc_bench_worker(&vfs_pc_bench_ctxs[1]);
}
static void vfs_pc_bench_worker2(void) {
  vfs_pc_bench_worker(&vfs_pc_bench_ctxs[2]);
}

static uint32_t vfs_readahead_bench_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  memset(buffer, 0x5a, size);
  return size;
}

/* Readahead pair: a 256 KiB window (the 64-page cap) read from a synthetic
 * node, once with the frames taken as PMM runs and filled directly, once with
 * the staging path forced.  The difference is the per-page bounce copy the
 * direct path removed. */
static void vfs_cache_readahead_bench(void) {
  const uint32_t file_bytes = 256u * 1024u;
  const uint32_t rounds = 32;

  klog_puts("[VFS-BENCH] readahead ");
  klog_uint64(file_bytes / 1024);
  klog_puts(" KiB x ");
  klog_uint64(rounds);
  klog_puts(" rounds\n");

  for (uint32_t mode = 0; mode < 2; mode++) {
    vfs_readahead_force_scratch = (mode == 1);

    vfs_node_t node;
    vfs_node_init(&node);
    node.flags = FS_FILE | FS_PAGE_CACHE;
    node.length = file_bytes;
    node.read = vfs_readahead_bench_read;

    uint64_t t0 = rdtsc_fence();
    for (uint32_t round = 0; round < rounds; round++) {
      vfs_cache_readahead(&node, 0, file_bytes);
      vfs_cache_clear(&node);
    }
    uint64_t cycles = rdtsc_fence() - t0;
    vfs_cache_bench_report(mode ? "readahead staged" : "readahead direct",
                           rounds, cycles);
    vfs_cache_clear(&node);
  }

  vfs_readahead_force_scratch = false;
}

static void vfs_pc_bench_print_x100(uint64_t x100) {
  klog_uint64(x100 / 100);
  klog_putchar('.');
  uint64_t frac = x100 % 100;
  if (frac < 10)
    klog_putchar('0');
  klog_uint64(frac);
}

static void vfs_pc_bench_run_phase(const char *name,
                                   vfs_node_t *const nodes[VFS_PC_BENCH_CPUS],
                                   const uint32_t offsets[VFS_PC_BENCH_CPUS],
                                   uint64_t single_cycles) {
  static void (*const entries[VFS_PC_BENCH_THREADS])(void) = {
      vfs_pc_bench_worker0, vfs_pc_bench_worker1, vfs_pc_bench_worker2};
  uint32_t started = 0;

  __atomic_store_n(&vfs_pc_bench_go, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vfs_pc_bench_done, 0, __ATOMIC_RELEASE);

  for (uint32_t i = 0; i < VFS_PC_BENCH_THREADS; i++) {
    /* Pin one worker per AP: a NULL explicit CPU would put them all on the
     * BSP, which is exactly the serialization the test exists to expose. */
    struct cpu_info *target = cpu_get_info(i + 1);
    if (!target || target->status == CPU_STATUS_OFFLINE)
      break;
    vfs_pc_bench_ctxs[i].node = nodes[i + 1];
    vfs_pc_bench_ctxs[i].offset = offsets[i + 1];
    vfs_pc_bench_ctxs[i].iters = VFS_PC_BENCH_ITERS;
    if (!sched_create_kernel_thread(entries[i], target, true))
      break;
    started++;
  }
  if (!started)
    return;

  __atomic_store_n(&vfs_pc_bench_go, 1, __ATOMIC_RELEASE);

  uint64_t ops = 0;
  uint64_t t0 = rdtsc_fence();
  for (uint64_t i = 0; i < VFS_PC_BENCH_ITERS; i++) {
    vfs_page_t *page = vfs_cache_lookup(nodes[0], offsets[0]);
    if (!page)
      break;
    vfs_cache_put(nodes[0], page);
    ops++;
  }

  /* Bounded wait: a worker that never gets scheduled must not wedge boot. */
  uint64_t deadline = tsc_get_freq_khz() * 500;
  while (__atomic_load_n(&vfs_pc_bench_done, __ATOMIC_ACQUIRE) < started) {
    if (deadline && rdtsc_fence() - t0 > deadline)
      break;
    sched_yield();
  }
  uint64_t cycles = rdtsc_fence() - t0;

  ops += (uint64_t)started * VFS_PC_BENCH_ITERS;
  uint64_t per_op = ops ? cycles / ops : 0;
  klog_puts("[VFS-BENCH] ");
  klog_puts(name);
  klog_puts(": ");
  klog_uint64(ops);
  klog_puts(" ops, ");
  klog_uint64(per_op);
  klog_puts(" cycles/op, ");
  if (per_op) {
    vfs_pc_bench_print_x100(single_cycles * 100 / per_op);
    klog_puts("x 1cpu\n");
  } else {
    klog_puts("n/a\n");
  }
}

static void vfs_pc_bench_scaling(uint64_t single_cycles) {
  if (cpu_get_count() < 2) {
    klog_puts("[VFS-BENCH] multi-cpu lookup+put skipped: single CPU\n");
    return;
  }

  vfs_node_t shared;
  vfs_node_t separate[VFS_PC_BENCH_CPUS];
  bool shared_ready = false;
  uint32_t separate_ready = 0;

  vfs_node_init(&shared);
  shared.flags = FS_FILE | FS_PAGE_CACHE;
  shared.length = VFS_PC_BENCH_CPUS * PAGE_SIZE;
  shared_ready = true;
  for (uint32_t i = 0; i < VFS_PC_BENCH_CPUS; i++) {
    void *frame = pmm_alloc_page();
    if (!frame)
      goto cleanup;
    vfs_page_t *page = vfs_cache_insert(&shared, i * PAGE_SIZE, (uint64_t)frame);
    if (!page) {
      pmm_free_page(frame);
      goto cleanup;
    }
    vfs_cache_put(&shared, page);
  }

  for (uint32_t i = 0; i < VFS_PC_BENCH_CPUS; i++) {
    vfs_node_init(&separate[i]);
    separate[i].flags = FS_FILE | FS_PAGE_CACHE;
    separate[i].length = PAGE_SIZE;
    separate_ready++;
    void *frame = pmm_alloc_page();
    if (!frame)
      goto cleanup;
    vfs_page_t *page = vfs_cache_insert(&separate[i], 0, (uint64_t)frame);
    if (!page) {
      pmm_free_page(frame);
      goto cleanup;
    }
    vfs_cache_put(&separate[i], page);
  }

  vfs_node_t *same_page[VFS_PC_BENCH_CPUS] = {&shared, &shared, &shared, &shared};
  uint32_t same_page_off[VFS_PC_BENCH_CPUS] = {0, 0, 0, 0};
  vfs_node_t *spread[VFS_PC_BENCH_CPUS];
  uint32_t spread_off[VFS_PC_BENCH_CPUS];
  vfs_node_t *per_node[VFS_PC_BENCH_CPUS];
  uint32_t per_node_off[VFS_PC_BENCH_CPUS];
  for (uint32_t i = 0; i < VFS_PC_BENCH_CPUS; i++) {
    spread[i] = &shared;
    spread_off[i] = i * PAGE_SIZE;
    per_node[i] = &separate[i];
    per_node_off[i] = 0;
  }

  klog_puts("[VFS-BENCH] multi-cpu lookup+put (single-cpu ");
  klog_uint64(single_cycles);
  klog_puts(" cycles/op)\n");
  vfs_pc_bench_run_phase("4cpu same page  ", same_page, same_page_off,
                         single_cycles);
  vfs_pc_bench_run_phase("4cpu spread page", spread, spread_off, single_cycles);
  vfs_pc_bench_run_phase("4cpu per node  ", per_node, per_node_off,
                         single_cycles);

cleanup:
  if (shared_ready)
    vfs_cache_clear(&shared);
  for (uint32_t i = 0; i < separate_ready; i++)
    vfs_cache_clear(&separate[i]);
}

void vfs_cache_bench(void) {
  const uint64_t iters = 200000;
  vfs_node_t node;
  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = PAGE_SIZE;

  void *frame = pmm_alloc_page();
  if (!frame) {
    vfs_cache_clear(&node);
    return;
  }
  vfs_page_t *page = vfs_cache_insert(&node, 0, (uint64_t)frame);
  if (!page) {
    pmm_free_page(frame);
    vfs_cache_clear(&node);
    return;
  }
  vfs_cache_put(&node, page);

  /* New form: lookup plus the lock-free packed put. */
  uint64_t ops = 0;
  uint64_t t0 = rdtsc_fence();
  for (uint64_t i = 0; i < iters; i++) {
    vfs_page_t *p = vfs_cache_lookup(&node, 0);
    if (!p)
      break;
    vfs_cache_put(&node, p);
    ops++;
  }
  uint64_t atomic_cycles = rdtsc_fence() - t0;
  vfs_cache_bench_report("lookup+put atomic ", ops, atomic_cycles);
  uint64_t single_cycles = ops ? atomic_cycles / ops : 0;

  /* Old form: the put side took the page lock with interrupts masked. */
  ops = 0;
  t0 = rdtsc_fence();
  for (uint64_t i = 0; i < iters; i++) {
    vfs_page_t *p = vfs_cache_lookup(&node, 0);
    if (!p)
      break;
    spinlock_acquire(&node.pages_lock);
    __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    spinlock_release(&node.pages_lock);
    ops++;
  }
  vfs_cache_bench_report("lookup+put locked ", ops, rdtsc_fence() - t0);

  /* Stamp pair: one global atomic cache line vs the per-CPU counter. */
  uint64_t global_clock = 0;
  t0 = rdtsc_fence();
  for (uint64_t i = 0; i < iters; i++)
    __atomic_add_fetch(&global_clock, 1, __ATOMIC_RELAXED);
  vfs_cache_bench_report("stamp global-atomic", iters, rdtsc_fence() - t0);

  t0 = rdtsc_fence();
  for (uint64_t i = 0; i < iters; i++)
    cache_stamp();
  vfs_cache_bench_report("stamp per-cpu      ", iters, rdtsc_fence() - t0);

  vfs_cache_readahead_bench();

  vfs_pc_bench_scaling(single_cycles);

  vfs_cache_clear(&node);
}
