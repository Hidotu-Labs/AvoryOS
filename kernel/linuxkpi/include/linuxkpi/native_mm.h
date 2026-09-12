#ifndef LINUXKPI_NATIVE_MM_H
#define LINUXKPI_NATIVE_MM_H

/* Native memory bridge for LinuxKPI implementation files.
 *
 * Native functions are declared under a distinct C name with an
 * __asm__("native_symbol") alias: the implementation can then call
 * asc_kmalloc() without colliding with the Linux kmalloc() API it is
 * implementing.  Types use compiler builtins (__SIZE_TYPE__) instead of
 * <stddef.h>/<stdint.h> so this header cannot conflict with Linux headers.
 */

extern void *asc_kmalloc(__SIZE_TYPE__ size) __asm__("kmalloc");
extern void asc_kfree(void *ptr) __asm__("kfree");
extern void *asc_kcalloc(__SIZE_TYPE__ num, __SIZE_TYPE__ size)
    __asm__("kcalloc");
extern void *asc_krealloc(void *ptr, __SIZE_TYPE__ new_size)
    __asm__("krealloc");
extern __SIZE_TYPE__ asc_heap_ksize(const void *ptr) __asm__("heap_ksize");

/* Native named object caches (kernel/src/mm/slab_cache.{c,h}).
 * kmem_cache_create(name, obj_size, alignment, ctor, dtor). */
extern void *asc_kmem_cache_create(const char *name, __SIZE_TYPE__ obj_size,
                                   __SIZE_TYPE__ alignment,
                                   void (*ctor)(void *),
                                   void (*dtor)(void *))
    __asm__("kmem_cache_create");
extern void *asc_kmem_cache_alloc(void *cache) __asm__("kmem_cache_alloc");
extern void asc_kmem_cache_free(void *cache, void *obj)
    __asm__("kmem_cache_free");
extern void asc_kmem_cache_destroy(void *cache) __asm__("kmem_cache_destroy");
extern void asc_kmem_cache_shrink(void *cache) __asm__("kmem_cache_shrink");

/* ── Physical page allocator (kernel/src/mm/pmm.c) ─────────────────────── */

extern void *asc_pmm_alloc_pages(__SIZE_TYPE__ count) __asm__("pmm_alloc_pages");
extern void *asc_pmm_alloc_pages_range(__SIZE_TYPE__ count,
                                       __UINT64_TYPE__ min_phys,
                                       __UINT64_TYPE__ max_phys)
    __asm__("pmm_alloc_pages_range");
extern void asc_pmm_free_pages(void *ptr, __SIZE_TYPE__ count)
    __asm__("pmm_free_pages");
extern __SIZE_TYPE__ asc_pmm_get_free_pages(void) __asm__("pmm_get_free_pages");
extern __SIZE_TYPE__ asc_pmm_get_free_pages_total(void)
    __asm__("pmm_get_free_pages_including_pcp");
extern __UINT64_TYPE__ asc_pmm_get_hhdm_offset(void)
    __asm__("pmm_get_hhdm_offset");
extern __UINT64_TYPE__ asc_pmm_get_total_memory(void)
    __asm__("pmm_get_total_memory");
extern _Bool asc_pmm_is_managed(__UINT64_TYPE__ phys) __asm__("pmm_is_managed");
extern __UINT64_TYPE__ asc_pmm_get_zero_page_phys(void)
    __asm__("pmm_get_zero_page_phys");

/* ── Virtual memory manager (kernel/src/mm/vmm*.c) ─────────────────────── */

/* All PML4 handles are *physical* addresses, as used by the native API. */
extern __UINT64_TYPE__ *asc_vmm_get_kernel_pml4(void)
    __asm__("vmm_get_kernel_pml4");
extern __UINT64_TYPE__ *asc_vmm_get_active_pml4(void)
    __asm__("vmm_get_active_pml4");
extern _Bool asc_vmm_map_page(__UINT64_TYPE__ *pml4,
                              __UINT64_TYPE__ virtual_addr,
                              __UINT64_TYPE__ physical_addr,
                              __UINT64_TYPE__ flags) __asm__("vmm_map_page");
extern _Bool asc_vmm_map_range(__UINT64_TYPE__ *pml4,
                               __UINT64_TYPE__ virtual_addr,
                               __UINT64_TYPE__ physical_addr,
                               __SIZE_TYPE__ pages,
                               __UINT64_TYPE__ flags) __asm__("vmm_map_range");
extern void asc_vmm_unmap_page(__UINT64_TYPE__ *pml4,
                               __UINT64_TYPE__ virtual_addr)
    __asm__("vmm_unmap_page");
extern void asc_vmm_free_empty_tables(__UINT64_TYPE__ *pml4,
                                      __UINT64_TYPE__ virtual_addr)
    __asm__("vmm_free_empty_tables");
extern __UINT64_TYPE__ asc_vmm_virt_to_phys(__UINT64_TYPE__ *pml4,
                                            __UINT64_TYPE__ virtual_addr)
    __asm__("vmm_virt_to_phys");

/* Page-table entry flags (kernel/src/mm/vmm.h). */
#define ASC_PAGE_PRESENT 0x001ULL
#define ASC_PAGE_RW 0x002ULL
#define ASC_PAGE_USER 0x004ULL
#define ASC_PAGE_PWT 0x008ULL
#define ASC_PAGE_PCD 0x010ULL
#define ASC_PAGE_PAT 0x080ULL /* bit 7 at the PTE level (PS on PD entries) */
#define ASC_PAGE_NX 0x8000000000000000ULL

static inline void asc_invlpg(__UINT64_TYPE__ va) {
  __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}

#endif /* LINUXKPI_NATIVE_MM_H */
