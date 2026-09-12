#ifndef __AVORY_LINUXKPI_MM_TYPES_H
#define __AVORY_LINUXKPI_MM_TYPES_H

/* AvoryOS overlay for <linux/mm_types.h>.
 *
 * Upstream's header builds the full Linux memory model (maple trees, anon
 * vmas, per-mm counters, folios, ...).  AvoryOS keeps its own native mm
 * structures and bridges the Linux API to them, so imported code sees only
 * the subset of types and fields it actually uses:
 *
 *   struct page              -- a real (if trimmed) page descriptor backed by
 *                               the sparse mem_map in linuxkpi/src/page.c.
 *   struct vm_area_struct    -- the Linux-facing view of a native struct vma,
 *                               allocated by the mmap bridge in
 *                               linuxkpi/src/mmap.c.
 *   struct mm_struct         -- minimal placeholder; no imported code in this
 *                               phase walks an mm.
 *
 * Layout note for struct page: unlike upstream, `pfn` is stored in the page
 * itself.  That makes page_to_pfn() branch-free without replicating
 * SPARSEMEM's section-bit arithmetic in `flags`, and costs 8 bytes per page.
 * `compound_head` is a plain pointer whose PageTail bit (PG_tail) says whether
 * it is meaningful; PageHead pages carry `compound_order`.
 */

#include <asm/pgtable_types.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/types.h>

struct address_space;
struct file;
struct mm_struct;
struct page;
struct vma; /* native AvoryOS VMA (kernel/src/mm/vma.h) */
struct vm_area_struct;
struct vm_fault;

/* Upstream mm_types.h defines swp_entry_t; <linux/pgtable.h> inlines use it
 * and pgtable.h includes this header for it. */
typedef struct {
  unsigned long val;
} swp_entry_t;

/* ------------------------------------------------------------------------- */
/* struct page                                                                */
/* ------------------------------------------------------------------------- */

struct page {
  unsigned long flags;   /* PG_* bits, see <linux/page-flags.h>              */
  unsigned long pfn;     /* Avory extension: physical page number            */
  atomic_t _refcount;    /* Allocation reference count (authoritative for    */
                         /* pages handed out through the Linux API)          */
  atomic_t _mapcount;    /* Number of PTEs referencing the page              */
  /* Second cache line word doubles as the compound bookkeeping exactly like
   * upstream: a page is either on an LRU list or a compound head/tail. */
  union {
    struct list_head lru; /* Page cache / free list link                      */
    struct {
      struct page *compound_head; /* valid on PageHead and PageTail pages   */
      unsigned long compound_order; /* valid on PageHead pages              */
    };
  };
  struct address_space *mapping; /* Page cache owner, NULL when anonymous    */
  pgoff_t index;         /* Offset in the mapping, in pages                  */
  unsigned long private; /* Filesystem/driver private data (upstream type;   */
                         /* TTM stores an allocation order and a helper      */
                         /* pointer here)                                    */
} __attribute__((aligned(64)));

/* Order-0 folios: a folio is a head page plus the guarantee that its mapping
 * covers the whole folio.  AvoryOS only allocates order-0 pages for the
 * Linux-facing page cache, so a folio is the page itself; the helpers in
 * page-flags.h hide the indirection.  Code never accesses folio fields
 * directly in the current import set (checked), so the layout this simple. */
struct folio {
  struct page page;
};

/* ------------------------------------------------------------------------- */
/* struct vm_area_struct (Linux-facing half of a native struct vma)           */
/* ------------------------------------------------------------------------- */

/*
 * Native VM_* flag values (copied from upstream <linux/mm.h> so imported
 * headers that test them agree with the LinuxKPI definitions).
 */
#define VM_NONE 0x00000000
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008
#define VM_MAYREAD 0x00000010
#define VM_MAYWRITE 0x00000020
#define VM_MAYEXEC 0x00000040
#define VM_MAYSHARE 0x00000080
#define VM_GROWSDOWN 0x00000100
#define VM_UFFD_MISSING 0x00000200
#define VM_PFNMAP 0x00000400
#define VM_UFFD_WP 0x00001000
#define VM_LOCKED 0x00002000
#define VM_IO 0x00004000
#define VM_SEQ_READ 0x00008000
#define VM_RAND_READ 0x00010000
#define VM_DONTCOPY 0x00020000
#define VM_DONTEXPAND 0x00040000
#define VM_LOCKONFAULT 0x00080000
#define VM_ACCOUNT 0x00100000
#define VM_NORESERVE 0x00200000
#define VM_HUGETLB 0x00400000
#define VM_SYNC 0x00800000
#define VM_ARCH_1 0x01000000
#define VM_DONTDUMP 0x04000000
#define VM_DONTUSERF 0x08000000
#define VM_MIXEDMAP 0x10000000
#define VM_HUGEPAGE 0x20000000
#define VM_NOHUGEPAGE 0x40000000
#define VM_MERGEABLE 0x80000000

struct vm_area_struct {
  unsigned long vm_start;
  unsigned long vm_end;
  unsigned long vm_flags;
  unsigned long vm_pgoff;
  struct file *vm_file;
  void *vm_private_data;
  const struct vm_operations_struct *vm_ops;
  pgprot_t vm_page_prot; /* real pgprot_t so pgprot_writecombine() fits */
  struct mm_struct *vm_mm;
  /* Native struct vma this view belongs to (set by the bridge). */
  struct vma *native_vma;
};

/* Minimal mm: AvoryOS keeps the real mapping in the native struct vma, but
 * imported headers read tlb_flush_pending and pgtable.h's pgd_offset()
 * dereferences ->pgd.  The single instance (init_mm) is defined in
 * linuxkpi/src/mm_extra.c. */
struct mm_struct {
  atomic_t tlb_flush_pending;
  atomic_t mm_users;
  atomic_t mm_count;
  pgd_t *pgd; /* for pgd_offset()/pgd_offset_k() in pgtable.h */
};

extern struct mm_struct init_mm;

/* Upstream mm_types.h also defines enum fault_flag; <linux/mm.h>'s
 * fault_flag_allow_retry_first() and the TTM fault path use these values. */
enum fault_flag {
  FAULT_FLAG_WRITE = 1 << 0,
  FAULT_FLAG_MKWRITE = 1 << 1,
  FAULT_FLAG_ALLOW_RETRY = 1 << 2,
  FAULT_FLAG_RETRY_NOWAIT = 1 << 3,
  FAULT_FLAG_KILLABLE = 1 << 4,
  FAULT_FLAG_TRIED = 1 << 5,
  FAULT_FLAG_USER = 1 << 6,
  FAULT_FLAG_REMOTE = 1 << 7,
  FAULT_FLAG_INSTRUCTION = 1 << 8,
  FAULT_FLAG_INTERRUPTIBLE = 1 << 9,
  FAULT_FLAG_UNSHARE = 1 << 10,
  FAULT_FLAG_ORIG_PTE_VALID = 1 << 11,
  FAULT_FLAG_VMA_LOCK = 1 << 12,
};

struct vm_fault {
  struct vm_area_struct *vma;
  unsigned int flags; /* FAULT_FLAG_* */
  pgoff_t pgoff;
  unsigned long address; /* Faulting virtual address */
  void *pmd;             /* opaque: native page-table entry */
  void *pte;
  struct page *page;
  void *entry;
};

#endif /* __AVORY_LINUXKPI_MM_TYPES_H */
