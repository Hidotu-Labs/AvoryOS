#ifndef __AVORY_LINUXKPI_VMALLOC_H
#define __AVORY_LINUXKPI_VMALLOC_H

/* AvoryOS overlay for <linux/vmalloc.h>.
 *
 * The VMAP window [0xFFFFC00000000000, 0xFFFFE00000000000) is managed by
 * linuxkpi/src/vmalloc.c with a first-fit address allocator; pages are mapped
 * into the kernel PML4 through the native VMM.  The window's PML4 entries are
 * pre-populated at boot (linuxkpi_vmalloc_init) so process address spaces
 * cloned later inherit them. */

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/io.h>

struct page;

void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void *vmalloc_user(unsigned long size);
void *vmalloc_node(unsigned long size, int node);
void vfree(const void *addr);

void *kvmalloc_node(size_t size, gfp_t flags, int node);
void *kvmalloc(size_t size, gfp_t flags);
void *kvzalloc(size_t size, gfp_t flags);
void *kvmalloc_array(size_t num, size_t size, gfp_t flags);
void *kvcalloc(size_t num, size_t size, gfp_t flags);
void kvfree(const void *addr);
void kvfree_sensitive(const void *addr, size_t len);

void *ioremap(resource_size_t offset, unsigned long size);
void *ioremap_wc(resource_size_t offset, unsigned long size);
void *ioremap_wt(resource_size_t offset, unsigned long size);
void *ioremap_cache(resource_size_t offset, unsigned long size);
void iounmap(volatile void __iomem *addr);

void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           pgprot_t prot);
void vunmap(const void *addr);
struct page *vmalloc_to_page(const void *addr);
unsigned long vmalloc_to_pfn(const void *addr);

void *memremap(resource_size_t offset, size_t size, unsigned long flags);
void memunmap(void *addr);

/* vmap() flags (upstream values). */
#define VM_IOREMAP 0x00000001
#define VM_ALLOC 0x00000002
#define VM_MAP 0x00000004
#define VM_USERMAP 0x00000008
#define VM_DMA_COHERENT 0x00000010
#define VM_NO_GUARD 0x00000020
#define VM_KASAN 0x00000040

/* memremap() flags (upstream values). */
#define MEMREMAP_WB (1 << 0)
#define MEMREMAP_WT (1 << 1)
#define MEMREMAP_WC (1 << 2)
#define MEMREMAP_ENC (1 << 3)
#define MEMREMAP_DEC (1 << 4)

#endif /* __AVORY_LINUXKPI_VMALLOC_H */
