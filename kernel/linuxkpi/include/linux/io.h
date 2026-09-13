#ifndef __AVORY_LINUXKPI_IO_H
#define __AVORY_LINUXKPI_IO_H

/* AvoryOS overlay for <linux/io.h>.
 *
 * The accessors themselves live in the asm/io.h overlay (included first, so
 * every include path agrees); this header adds the ioremap family. */

#include <asm/io.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/types.h>

/* Phase 2 provides the real mapping (linuxkpi/src/vmalloc.c); declaring it
 * here makes imported code fail loudly at link time instead of silently using
 * an invalid pointer. */
struct resource;
void __iomem *ioremap(resource_size_t offset, unsigned long size);
void __iomem *ioremap_wc(resource_size_t offset, unsigned long size);
void __iomem *ioremap_prot(resource_size_t offset, unsigned long size,
                           unsigned long prot);
void iounmap(volatile void __iomem *addr);

/* pci_iomap()/pci_iounmap() declarations.  Upstream x86 reaches these through
 * asm-generic/io.h, which the asm/io.h overlay replaces; the implementations
 * are in linuxkpi/src/pci.c (Phase 4 C4). */
#include <asm-generic/pci_iomap.h>

/* MTRR/PAT helpers amdgpu calls for write-combining VRAM mappings.  AvoryOS
 * does not program MTRRs or PAT, so these are accepted bookkeeping no-ops
 * (documented in the gap log). */
static inline int arch_phys_wc_add(unsigned long base, unsigned long size) {
  (void)base;
  (void)size;
  return 0;
}
static inline void arch_phys_wc_del(int handle) { (void)handle; }
static inline int arch_io_reserve_memtype_wc(resource_size_t start,
                                             resource_size_t size) {
  (void)start;
  (void)size;
  return 0;
}
static inline void arch_io_free_memtype_wc(resource_size_t start,
                                           resource_size_t size) {
  (void)start;
  (void)size;
}

/* Big-endian accessors: stock io-64-nonatomic-lo-hi.h's _be variants use
 * them.  x86 has no native BE MMIO ops, so byteswap around the LE access. */
static inline u32 ioread32be(const void __iomem *addr) {
  return __builtin_bswap32(readl(addr));
}
static inline void iowrite32be(u32 val, void __iomem *addr) {
  writel(__builtin_bswap32(val), addr);
}

/* io-64-nonatomic-lo-hi.h compatibility: upstream's linux/io.h does not
 * define the lo_hi_* accessors; they live in the nonatomic headers, which
 * include linux/io.h first.  Pulling that header in at the end gives
 * linux/io.h-only users the accessors without the macro/function clash that
 * a local #define caused. */
#include <linux/io-64-nonatomic-lo-hi.h>

#endif /* __AVORY_LINUXKPI_IO_H */
