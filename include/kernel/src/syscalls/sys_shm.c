// Shared Memory Syscalls
// Implementation of System V Shared Memory (shmget, shmat, shmdt, shmctl)
//
// Each SHM segment owns a set of physical pages that can be mapped into
// multiple process address spaces simultaneously. The physical pages are
// reference-counted (PMM refcount system).

#include "../console/console.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/pmm.h"
#include "../fs/vfs.h"
#include "../mm/shm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define MAX_SHM_PAGES (SHM_MAX_SIZE / PAGE_SIZE)

// Per-segment descriptor

struct shm_segment {
  bool active;          // Slot in use
  bool marked_destroy;  // Marked for deletion (IPC_RMID)
  uint32_t key;         // IPC key (0 = IPC_PRIVATE)
  uint32_t shmid;       // Unique segment ID
  uint64_t size;        // Segment size (bytes)
  uint32_t num_pages;   // Number of physical pages
  uint64_t *phys_pages; // Array of physical page addresses
  uint32_t nattch;      // Number of current attaches
  uint32_t perm_mode;   // Permission bits
  uint32_t creator_pid; // PID of creator
  uint32_t creator_uid;
  uint32_t creator_gid;
  uint32_t last_pid;    // PID of last shmat/shmdt
};

// Global State

static spinlock_t shm_lock = SPINLOCK_INIT;
static struct shm_segment shm_table[SHM_MAX_SEGMENTS];
static uint32_t shm_next_id = 1;

// Backing storage for phys_pages arrays (static to avoid heap dependency loop)
static uint64_t shm_page_arrays[SHM_MAX_SEGMENTS][MAX_SHM_PAGES];

/* Called with shm_lock held after the last attachment has gone away. The
 * segment owns the initial reference returned by pmm_alloc_page(); mappings
 * hold additional references of their own. */
static void shm_destroy_segment(struct shm_segment *seg) {
  if (!seg || !seg->active || seg->nattch != 0)
    return;

  for (uint32_t i = 0; i < seg->num_pages; i++) {
    if (seg->phys_pages[i]) {
      pmm_decref((void *)seg->phys_pages[i]);
      seg->phys_pages[i] = 0;
    }
  }
  seg->active = false;
  seg->marked_destroy = false;
  seg->num_pages = 0;
  seg->size = 0;
  seg->nattch = 0;
}

// Initialization

void shm_init(void) {
  memset(shm_table, 0, sizeof(shm_table));
  memset(shm_page_arrays, 0, sizeof(shm_page_arrays));
  shm_next_id = 1;
  klog_puts("[SHM] Shared memory subsystem initialized.\n");
}

// sys_shmget

int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t a3,
                   uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (size == 0 || size > SHM_MAX_SIZE)
    return -22; // EINVAL

  spinlock_acquire(&shm_lock);

  // If key != IPC_PRIVATE, search for existing segment
  if (key != IPC_PRIVATE) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
      if (shm_table[i].active && shm_table[i].key == (uint32_t)key) {
        if (shmflg & IPC_EXCL) {
          spinlock_release(&shm_lock);
          return -17; // EEXIST
        }
        uint32_t id = shm_table[i].shmid;
        spinlock_release(&shm_lock);
        return (int64_t)id;
      }
    }
    // Key not found — must have IPC_CREAT to create
    if (!(shmflg & IPC_CREAT)) {
      spinlock_release(&shm_lock);
      return -2; // ENOENT
    }
  }

  // Find a free slot
  int slot = -1;
  for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
    if (!shm_table[i].active) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    spinlock_release(&shm_lock);
    return -28; // ENOSPC
  }

  // Allocate physical pages
  uint32_t num_pages = (uint32_t)(PAGE_ALIGN_UP(size) / PAGE_SIZE);
  if (num_pages > MAX_SHM_PAGES) {
    spinlock_release(&shm_lock);
    return -22; // EINVAL
  }

  for (uint32_t i = 0; i < num_pages; i++) {
    void *page = pmm_alloc_page();
    if (!page) {
      // Rollback
      for (uint32_t j = 0; j < i; j++) {
        pmm_free_page((void *)shm_page_arrays[slot][j]);
      }
      spinlock_release(&shm_lock);
      return -12; // ENOMEM
    }
    // Zero-fill the page via HHDM
    memset(PHYS_TO_VIRT((uint64_t)page), 0, PAGE_SIZE);
    shm_page_arrays[slot][i] = (uint64_t)page;
  }

  // Initialize segment
  struct shm_segment *seg = &shm_table[slot];
  seg->active = true;
  seg->marked_destroy = false;
  seg->key = (uint32_t)key;
  seg->shmid = shm_next_id++;
  seg->size = size;
  seg->num_pages = num_pages;
  seg->phys_pages = shm_page_arrays[slot];
  seg->nattch = 0;
  seg->perm_mode = (uint32_t)(shmflg & 0x1FF); // Low 9 bits = permissions
  seg->creator_pid = 0;

  struct thread *t = sched_get_current();
  if (t) {
    seg->creator_pid = t->tgid;
    seg->creator_uid = t->euid;
    seg->creator_gid = t->egid;
  }

  uint32_t id = seg->shmid;

  spinlock_release(&shm_lock);
  return (int64_t)id;
}

// sys_shmat

#define SHM_MMAP_REGION_BASE 0x7E0000000000ULL
#define SHM_MMAP_REGION_LIMIT 0x7F0000000000ULL

int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg,
                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  spinlock_acquire(&shm_lock);

  // Find segment by ID
  struct shm_segment *seg = NULL;
  for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
    if (shm_table[i].active && shm_table[i].shmid == (uint32_t)shmid) {
      seg = &shm_table[i];
      break;
    }
  }

  if (!seg) {
    spinlock_release(&shm_lock);
    return -22; // EINVAL
  }

  struct thread *t = sched_get_current();
  if (!t) {
    spinlock_release(&shm_lock);
    return -1; // EPERM
  }

  uint32_t requested = (shmflg & SHM_RDONLY) ? 4 : 6;
  if (t->euid != 0) {
    uint32_t allowed = seg->perm_mode & 7;
    if (t->euid == seg->creator_uid) allowed = (seg->perm_mode >> 6) & 7;
    else if (vfs_in_group(seg->creator_gid)) allowed = (seg->perm_mode >> 3) & 7;
    if ((allowed & requested) != requested) {
      spinlock_release(&shm_lock);
      return -13;
    }
  }

  uint64_t aligned_size = PAGE_ALIGN_UP(seg->size);

  // Determine attach address
  uint64_t vaddr;
  if (shmaddr != 0) {
    // User-specified address
    if (shmaddr & (PAGE_SIZE - 1)) {
      if (shmflg & SHM_RND) {
        shmaddr &= ~(PAGE_SIZE - 1);
      } else {
        spinlock_release(&shm_lock);
        return -22; // EINVAL
      }
    }
    vaddr = shmaddr;
  } else {
    // Auto-select address from SHM mmap region
    if (t->mm) {
      vaddr = vma_find_gap(&t->mm->vmas, aligned_size, SHM_MMAP_REGION_BASE,
                           SHM_MMAP_REGION_LIMIT);
    } else {
      vaddr = 0;
    }
    if (vaddr == 0) {
      spinlock_release(&shm_lock);
      return -12; // ENOMEM
    }
  }

  // Map each page of the segment
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (!(shmflg & SHM_RDONLY)) {
    flags |= PAGE_FLAG_RW;
  }

  for (uint32_t i = 0; i < seg->num_pages; i++) {
    uint64_t phys = seg->phys_pages[i];
    if (!vmm_map_page(pml4, vaddr + i * PAGE_SIZE, phys, flags)) {
      // Rollback mapped pages
      for (uint32_t j = 0; j < i; j++) {
        vmm_unmap_page(pml4, vaddr + j * PAGE_SIZE);
      }
      spinlock_release(&shm_lock);
      return -12; // ENOMEM
    }
    // Increment refcount so page isn't freed on process exit
    pmm_incref((void *)phys);
  }

  // Register VMA as MAP_SHARED. We avoid MAP_ANONYMOUS because this is backed
  // by an existing physical segment allocated in shmget.
  uint64_t prot = 0x1; // PROT_READ
  if (!(shmflg & SHM_RDONLY))
    prot |= 0x2; // PROT_WRITE

  if (t->mm) {
    if (vma_add(&t->mm->vmas, vaddr, vaddr + aligned_size, prot,
                MAP_SHARED | MAP_SYSV_SHM, -1, 0, NULL, 0) != 0) {
      for (uint32_t i = 0; i < seg->num_pages; i++) {
        vmm_unmap_page(pml4, vaddr + i * PAGE_SIZE);
        pmm_decref((void *)seg->phys_pages[i]);
      }
      spinlock_release(&shm_lock);
      return -12; // ENOMEM
    }
  }

  seg->nattch++;
  seg->last_pid = t->tid;

  spinlock_release(&shm_lock);
  return (int64_t)vaddr;
}

// sys_shmdt

int64_t sys_shmdt(uint64_t shmaddr, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (shmaddr == 0 || (shmaddr & (PAGE_SIZE - 1)))
    return -22; // EINVAL

  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  // Find the VMA for this address
  struct vma *v = NULL;
  if (t->mm) {
    v = vma_find(&t->mm->vmas, shmaddr);
  }

  /* An attach must be detached at its original address.  Requiring the
   * internal SHM marker also prevents shmdt from consuming an unrelated
   * MAP_SHARED mapping. */
  if (!v || v->start != shmaddr || !(v->flags & MAP_SYSV_SHM))
    return -22; // EINVAL

  uint64_t size = v->end - v->start;
  uint32_t num_pages = size / PAGE_SIZE;

  spinlock_acquire(&shm_lock);

  // Find which segment this belongs to (by matching physical pages)
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t first_phys = vmm_virt_to_phys(pml4, shmaddr);
  first_phys &= 0x000FFFFFFFFFF000ULL;

  struct shm_segment *seg = NULL;
  for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
    if (!shm_table[i].active)
      continue;
    if (shm_table[i].num_pages > 0 &&
        shm_table[i].phys_pages[0] == first_phys) {
      seg = &shm_table[i];
      break;
    }
  }

  // Unmap pages and decrement refcounts
  for (uint32_t i = 0; i < num_pages; i++) {
    uint64_t va = shmaddr + i * PAGE_SIZE;
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys) {
      phys &= 0x000FFFFFFFFFF000ULL;
      vmm_unmap_page(pml4, va);
      pmm_decref((void *)phys);
    }
  }

  // Remove VMA
  if (t->mm) {
    vma_remove(&t->mm->vmas, shmaddr, shmaddr + size);
  }

  if (seg) {
    if (seg->nattch > 0)
      seg->nattch--;
    seg->last_pid = t->tid;

    // If marked for destruction and no more attaches, free the segment
    if (seg->marked_destroy && seg->nattch == 0) {
      shm_destroy_segment(seg);
    }
  }

  spinlock_release(&shm_lock);
  return 0;
}

// sys_shmctl

int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t a3,
                   uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  spinlock_acquire(&shm_lock);

  struct shm_segment *seg = NULL;
  for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
    if (shm_table[i].active && shm_table[i].shmid == (uint32_t)shmid) {
      seg = &shm_table[i];
      break;
    }
  }

  if (!seg) {
    spinlock_release(&shm_lock);
    return -22; // EINVAL
  }

  if (cmd == IPC_RMID) {
    struct thread *caller = sched_get_current();
    if (!caller || (caller->euid != 0 && caller->euid != seg->creator_uid)) {
      spinlock_release(&shm_lock);
      return -1;
    }
    if (seg->nattch == 0) {
      // No attaches — free immediately
      shm_destroy_segment(seg);
    } else {
      // Mark for deferred destruction
      seg->marked_destroy = true;
    }
    spinlock_release(&shm_lock);
    return 0;
  }

  if (cmd == IPC_STAT && buf != 0) {
    struct shmid_ds *ds = (struct shmid_ds *)buf;
    memset(ds, 0, sizeof(struct shmid_ds));

    ds->shm_perm.__key = seg->key;
    ds->shm_perm.uid = seg->creator_uid;
    ds->shm_perm.gid = seg->creator_gid;
    ds->shm_perm.mode = seg->perm_mode;

    ds->shm_segsz = PAGE_ALIGN_UP(seg->size);
    ds->shm_nattch = seg->nattch;
    ds->shm_cpid = seg->creator_pid;
    ds->shm_lpid = seg->last_pid;

    spinlock_release(&shm_lock);
    return 0;
  }

  spinlock_release(&shm_lock);
  return -22; // EINVAL
}

// Shell Introspection

void shm_print_status(void) {
  spinlock_acquire(&shm_lock);

  klog_puts("\n═══ System V Shared Memory Segments ═══\n");
  klog_puts("ID    Key       Size      Pages  Attach  Status\n");
  klog_puts("────  ────────  ────────  ─────  ──────  ──────\n");

  int found = 0;
  for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
    if (!shm_table[i].active)
      continue;
    found++;
    struct shm_segment *seg = &shm_table[i];

    klog_uint64(seg->shmid);
    klog_puts("     ");
    klog_uint64(seg->key);
    klog_puts("        ");
    klog_uint64(seg->size);
    klog_puts("       ");
    klog_uint64(seg->num_pages);
    klog_puts("     ");
    klog_uint64(seg->nattch);
    klog_puts("      ");
    klog_puts(seg->marked_destroy ? "DESTROY" : "ACTIVE");
    klog_puts("\n");
  }

  if (!found) {
    klog_puts("  (no segments)\n");
  }
  klog_puts("════════════════════════════════════════\n\n");

  spinlock_release(&shm_lock);
}

// Registration

void syscall_register_shm(void) {
  syscall_register(SYS_SHMGET, (syscall_handler_t)sys_shmget);
  syscall_register(SYS_SHMAT, (syscall_handler_t)sys_shmat);
  syscall_register(SYS_SHMDT, (syscall_handler_t)sys_shmdt);
  syscall_register(SYS_SHMCTL, (syscall_handler_t)sys_shmctl);
  klog_puts("[SHM] Syscalls registered (shmget=29, shmat=30, shmctl=31, "
            "shmdt=67).\n");
}
