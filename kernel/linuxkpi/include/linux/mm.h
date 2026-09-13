#ifndef __AVORY_LINUXKPI_MM_H
#define __AVORY_LINUXKPI_MM_H

/* AvoryOS overlay for <linux/mm.h>.
 *
 * Provides the Linux page/memory API surface on top of the native PMM and
 * VMM.  The page model itself lives in <linux/mm_types.h> + page.c; vmalloc,
 * the mmap bridge and the fault plumbing live in vmalloc.c / mmap.c. */

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/align.h>
#include <linux/pfn.h>
#include <linux/shrinker.h>
#include <linux/mmap_lock.h>
#include <linux/uaccess.h>
#include <asm/page.h>
#include <asm/pgtable.h>

struct task_struct;
struct sysinfo;

/* Upstream mm.h provides these; imported TTM/DRM code uses them.  There is no
 * init-on-free poisoning in AvoryOS, so want_init_on_free() is constant. */
static inline bool want_init_on_free(void) { return false; }

/* Page <-> zone/node helpers: upstream keeps them in mm.h, which AvoryOS
 * replaces; stock mmzone.h-based headers expect them (see mmzone.h overlay).
 * Guarded macros so both paths agree without redefinition. */
#ifndef folio_page_idx
#define folio_page_idx(folio, p) ((p) - &(folio)->page)
#endif
#ifndef page_zone
#define page_zone(page)                                                        \
  (&NODE_DATA(page_to_nid(page))->node_zones[page_zonenum(page)])
#endif
#ifndef page_pgdat
#define page_pgdat(page) NODE_DATA(page_to_nid(page))
#endif
#ifndef folio_zone
#define folio_zone(folio) page_zone(&(folio)->page)
#endif
#ifndef folio_pgdat
#define folio_pgdat(folio) page_pgdat(&(folio)->page)
#endif

bool set_page_dirty(struct page *page);      /* linuxkpi/src/mm_extra.c */
void mark_page_accessed(struct page *page);  /* linuxkpi/src/mm_extra.c */
void si_meminfo(struct sysinfo *val);        /* linuxkpi/src/mm_extra.c */

/* Upstream takes enum fault_flag; struct vm_fault::flags is unsigned int in
 * both trees, so keep the wider type to avoid implicit-conversion warnings. */
static inline bool fault_flag_allow_retry_first(unsigned int flags) {
  return (flags & FAULT_FLAG_ALLOW_RETRY) && (flags & FAULT_FLAG_TRIED);
}

/* Upstream helper used by page iterators; the mem_map is flat here. */
#ifndef nth_page
#define nth_page(page, n) ((page) + (n))
#endif

#define PAGE_ALIGN(x) ALIGN(x, PAGE_SIZE)
#define PAGE_ALIGNED(x) IS_ALIGNED((x), PAGE_SIZE)
#define offset_in_page(p) ((unsigned long)(p) & (PAGE_SIZE - 1))

/* ------------------------------------------------------------------------- */
/* pfn <-> page <-> virtual                                                   */
/* ------------------------------------------------------------------------- */

struct page *pfn_to_page(unsigned long pfn);
unsigned long page_to_pfn(const struct page *page);
phys_addr_t page_to_phys(struct page *page);
struct page *phys_to_page(phys_addr_t phys);
void *page_address(const struct page *page);

#include <linux/sizes.h>

static inline unsigned long folio_pfn(struct folio *folio) {
  return page_to_pfn(&folio->page);
}

/* asm/page.h defines virt_to_page()/virt_addr_valid() in terms of __pa();
 * the pfn helpers below are the AvoryOS implementations called by them. */
#define virt_to_pfn(kaddr) (__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn) __va((phys_addr_t)(pfn) << PAGE_SHIFT)

/* Upstream helpers: page_to_virt() is page_address() for kernel pages, and
 * VM_ACCESS_FLAGS is the read/write/exec set (amdgpu_gem uses it). */
#define page_to_virt(page) page_address(page)
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)

/* Upstream mm.h helper (slab/bio paths use it); must come after pfn_to_page
 * is declared because asm/page.h's virt_to_page() expands to it. */
static inline struct page *virt_to_head_page(const void *x) {
  return compound_head(virt_to_page(x));
}

bool is_vmalloc_addr(const void *x);

/* RAM checks and the global page counters (page_is_ram is used by amdgpu's
 * GMC setup; all PMM-managed RAM is "RAM" here). */
int page_is_ram(unsigned long pfn);
unsigned long totalram_pages(void);

/* ------------------------------------------------------------------------- */
/* kmap: all kernel memory is permanently mapped through the HHDM             */
/* ------------------------------------------------------------------------- */

/* The kmap family lives in the highmem.h overlay (as upstream); mm.h pulls it
 * in so code that included mm.h for kmap keeps compiling. */
#include <linux/highmem.h>

/* ------------------------------------------------------------------------- */
/* Page allocation                                                            */
/* ------------------------------------------------------------------------- */

#define MAX_PAGE_ORDER 10

struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)
void __free_pages(struct page *page, unsigned int order);
#define __free_page(page) __free_pages((page), 0)
void free_pages(unsigned long addr, unsigned int order);
void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);
void split_page(struct page *page, unsigned int order);
struct page *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order);

/* ------------------------------------------------------------------------- */
/* Page reference counting                                                    */
/* ------------------------------------------------------------------------- */

static inline int page_ref_count(const struct page *page) {
  return atomic_read(&page->_refcount);
}

static inline void set_page_count(struct page *page, int v) {
  atomic_set(&page->_refcount, v);
}

static inline void page_ref_inc(struct page *page) {
  atomic_inc(&page->_refcount);
}

static inline void page_ref_dec(struct page *page) {
  atomic_dec(&page->_refcount);
}

static inline bool page_ref_add_unless(struct page *page, int nr, int u) {
  return atomic_add_unless(&page->_refcount, nr, u);
}

static inline void get_page(struct page *page) {
  struct page *head = compound_head(page);
  page_ref_inc(head);
}

void put_page(struct page *page);
void put_pages_list(struct list_head *pages);

static inline int page_count(struct page *page) {
  return page_ref_count(compound_head(page));
}

/* ------------------------------------------------------------------------- */
/* VMA helpers (upstream definitions kept close to upstream)                  */
/* ------------------------------------------------------------------------- */

static inline unsigned long vma_pages(struct vm_area_struct *vma) {
  return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
}

/* Protection helpers (upstream mm.h/mmap.c).  The native fault path derives
 * real PTE flags from the native VMA, so these are informational values that
 * keep imported drivers' vm_page_prot arithmetic type-correct. */
static inline pgprot_t vm_get_page_prot(unsigned long vm_flags) {
  return __pgprot(vm_flags);
}

static inline void vm_flags_set(struct vm_area_struct *vma,
                                unsigned long flags) {
  vma->vm_flags |= flags;
}

static inline void vm_flags_clear(struct vm_area_struct *vma,
                                  unsigned long flags) {
  vma->vm_flags &= ~flags;
}

static inline void vm_flags_reset(struct vm_area_struct *vma,
                                  unsigned long flags) {
  vma->vm_flags = flags;
}

static inline void vm_flags_mod(struct vm_area_struct *vma,
                                unsigned long set, unsigned long clear) {
  vma->vm_flags = (vma->vm_flags | set) & ~clear;
}

static inline bool vma_is_io(struct vm_area_struct *vma) {
  return !!(vma->vm_flags & (VM_IO | VM_PFNMAP));
}

static inline int vma_is_anonymous(struct vm_area_struct *vma) {
  return !vma->vm_ops;
}

static inline bool is_cow_mapping(unsigned long vm_flags) {
  return (vm_flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}

struct file;
void vma_set_file(struct vm_area_struct *vma, struct file *file);

/* ------------------------------------------------------------------------- */
/* vm_operations_struct / fault insertion                                     */
/* ------------------------------------------------------------------------- */

typedef int vm_fault_t;

#define VM_FAULT_RETRY 0x000001
#define VM_FAULT_NOPAGE 0x000100
#define VM_FAULT_LOCKED 0x000200
#define VM_FAULT_SIGBUS 0x000004
#define VM_FAULT_SIGSEGV 0x000008
#define VM_FAULT_OOM 0x000002
#define VM_FAULT_WRITE 0x000010
#define VM_FAULT_HWPOISON 0x001000
#define VM_FAULT_FALLBACK 0x008000
#define VM_FAULT_DONE_COW 0x010000
#define VM_FAULT_NEEDDSYNC 0x020000
#define VM_FAULT_COMPLETED 0x040000
#define VM_FAULT_ERROR                                                        \
  (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON |   \
   VM_FAULT_FALLBACK)

#define FAULT_FLAG_WRITE 0x00000001
#define FAULT_FLAG_RETRY_NOWAIT 0x00000008
#define FAULT_FLAG_KILLABLE 0x00000010
#define FAULT_FLAG_USER 0x00000040
#define FAULT_FLAG_REMOTE 0x00000080
#define FAULT_FLAG_INSTRUCTION 0x00000100
#define FAULT_FLAG_INTERRUPTIBLE 0x00000200
#define FAULT_FLAG_UNSHARE 0x00000400
#define FAULT_FLAG_ORIG_PTE_VALID 0x00000800

struct vm_operations_struct {
  void (*open)(struct vm_area_struct *area);
  void (*close)(struct vm_area_struct *area);
  int (*may_split)(struct vm_area_struct *area, unsigned long addr);
  int (*mremap)(struct vm_area_struct *area);
  vm_fault_t (*fault)(struct vm_fault *vmf);
  vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
  vm_fault_t (*map_pages)(struct vm_fault *vmf, pgoff_t start_pgoff,
                          pgoff_t end_pgoff);
  vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
  vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
  int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf,
                int len, int write);
  const char *(*name)(struct vm_area_struct *vma);
  struct page *(*find_special_page)(struct vm_area_struct *vma,
                                    unsigned long addr);
};

/* Implemented in linuxkpi/src/mmap.c. */
vm_fault_t vmf_insert_page(struct vm_fault *vmf, struct page *page);
vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
                          unsigned long pfn);
vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr,
                            unsigned long pfn);
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
                               unsigned long pfn, pgprot_t prot);
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
                    unsigned long pfn, unsigned long size, pgprot_t prot);
int remap_pfn_range_notrack(struct vm_area_struct *vma, unsigned long addr,
                            unsigned long pfn, unsigned long size,
                            pgprot_t prot);
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
                   struct page *page);
void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
                  unsigned long size);
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
                         loff_t const holelen, int even_cows);

/* ------------------------------------------------------------------------- */
/* Misc                                                                        */
/* ------------------------------------------------------------------------- */

int get_cmdline(struct task_struct *task, char *buffer, int buflen);

/* struct address_space is defined in the fs.h overlay (as upstream). */

#endif /* __AVORY_LINUXKPI_MM_H */
