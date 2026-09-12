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

/* io-64-nonatomic-lo-hi.h: on 64-bit builds the q accessors exist, so these
 * are direct. */
#define lo_hi_readq(addr) readq(addr)
#define lo_hi_writeq(val, addr) writeq((val), (addr))
#define lo_hi_readl(addr) readl(addr)
#define lo_hi_writel(val, addr) writel((val), (addr))

#endif /* __AVORY_LINUXKPI_IO_H */
