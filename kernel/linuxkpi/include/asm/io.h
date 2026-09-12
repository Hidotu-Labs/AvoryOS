#ifndef __AVORY_LINUXKPI_ASM_IO_H
#define __AVORY_LINUXKPI_ASM_IO_H

/* AvoryOS overlay for <asm/io.h>.
 *
 * x86 MMIO accessors are plain volatile loads/stores; port I/O goes through
 * the native HAL (kernel/src/arch/x86_64/hal.c).  This overlay exists so that
 * every upstream header which includes <asm/io.h> (asm/pci.h, clocksource.h,
 * scatterlist.h, ...) sees the same definitions as the <linux/io.h> overlay
 * and there is exactly one definition of readl()/outb() in a translation unit.
 * It also avoids the upstream asm/io.h movdir64b path that newer GCCs reject
 * without special-insns glue. */

#include <linux/thread_info.h>
#include <linux/types.h>

/* ── MMIO ───────────────────────────────────────────────────────────────── */

static inline u8 readb(const volatile void __iomem *addr) {
  return *(const volatile u8 *)addr;
}
static inline u16 readw(const volatile void __iomem *addr) {
  return *(const volatile u16 *)addr;
}
static inline u32 readl(const volatile void __iomem *addr) {
  return *(const volatile u32 *)addr;
}
static inline u64 readq(const volatile void __iomem *addr) {
  return *(const volatile u64 *)addr;
}

static inline void writeb(u8 value, volatile void __iomem *addr) {
  *(volatile u8 *)addr = value;
}
static inline void writew(u16 value, volatile void __iomem *addr) {
  *(volatile u16 *)addr = value;
}
static inline void writel(u32 value, volatile void __iomem *addr) {
  *(volatile u32 *)addr = value;
}
static inline void writeq(u64 value, volatile void __iomem *addr) {
  *(volatile u64 *)addr = value;
}

/* Relaxed variants are identical here (volatile accesses are ordered enough
 * for the strongly-ordered x86 MMIO model). */
#define readb_relaxed readb
#define readw_relaxed readw
#define readl_relaxed readl
#define readq_relaxed readq
#define writeb_relaxed writeb
#define writew_relaxed writew
#define writel_relaxed writel
#define writeq_relaxed writeq

#define __raw_readb readb
#define __raw_readw readw
#define __raw_readl readl
#define __raw_readq readq
#define __raw_writeb writeb
#define __raw_writew writew
#define __raw_writel writel
#define __raw_writeq writeq

static inline u8 ioread8(const volatile void __iomem *addr) { return readb(addr); }
static inline u16 ioread16(const volatile void __iomem *addr) { return readw(addr); }
static inline u32 ioread32(const volatile void __iomem *addr) { return readl(addr); }
static inline void iowrite8(u8 value, volatile void __iomem *addr) { writeb(value, addr); }
static inline void iowrite16(u16 value, volatile void __iomem *addr) { writew(value, addr); }
static inline void iowrite32(u32 value, volatile void __iomem *addr) { writel(value, addr); }

/* Barrier hooks upstream places around MMIO; no-ops on x86. */
#define __io_br() do { } while (0)
#define __io_ar(v) do { } while (0)
#define __io_bw() do { } while (0)
#define __io_aw() do { } while (0)

/* ── port I/O through the native HAL (src/arch/x86_64/hal.c) ────────────── */

extern u8 asc_port_read8(u16 port) __asm__("hal_port_read8");
extern u16 asc_port_read16(u16 port) __asm__("hal_port_read16");
extern u32 asc_port_read32(u16 port) __asm__("hal_port_read32");
extern void asc_port_write8(u16 port, u8 value) __asm__("hal_port_write8");
extern void asc_port_write16(u16 port, u16 value) __asm__("hal_port_write16");
extern void asc_port_write32(u16 port, u32 value) __asm__("hal_port_write32");

static inline u8 inb(u16 port) { return asc_port_read8(port); }
static inline u16 inw(u16 port) { return asc_port_read16(port); }
static inline u32 inl(u16 port) { return asc_port_read32(port); }
static inline void outb(u8 value, u16 port) { asc_port_write8(port, value); }
static inline void outw(u16 value, u16 port) { asc_port_write16(port, value); }
static inline void outl(u32 value, u16 port) { asc_port_write32(port, value); }

static inline void memcpy_fromio(void *dst, const volatile void __iomem *src,
                                 size_t n) {
  u8 *d = dst;
  const volatile u8 *s = src;
  while (n--)
    *d++ = *s++;
}

static inline void memcpy_toio(volatile void __iomem *dst, const void *src,
                               size_t n) {
  volatile u8 *d = dst;
  const u8 *s = src;
  while (n--)
    *d++ = *s++;
}

static inline void memset_io(volatile void __iomem *dst, int value, size_t n) {
  volatile u8 *d = dst;
  while (n--)
    *d++ = (u8)value;
}

/* Upstream linux/io.h. */
#define IOMEM_ERR_PTR(err) (void __iomem *)ERR_PTR(err)

#endif /* __AVORY_LINUXKPI_ASM_IO_H */
