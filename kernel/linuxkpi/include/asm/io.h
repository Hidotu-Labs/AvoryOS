#ifndef __AVORY_LINUXKPI_ASM_IO_H
#define __AVORY_LINUXKPI_ASM_IO_H

/* AvoryOS overlay for <asm/io.h>.
 *
 * x86 MMIO accessors use ordered volatile assembly; port I/O goes through
 * the native HAL (kernel/src/arch/x86_64/hal.c).  This overlay exists so that
 * every upstream header which includes <asm/io.h> (asm/pci.h, clocksource.h,
 * scatterlist.h, ...) sees the same definitions as the <linux/io.h> overlay
 * and there is exactly one definition of readl()/outb() in a translation unit.
 * It also avoids the upstream asm/io.h movdir64b path that newer GCCs reject
 * without special-insns glue. */

#include <linux/thread_info.h>
#include <linux/types.h>

/* ── MMIO ───────────────────────────────────────────────────────────────── */

/* Match the x86 Linux accessor contract: ordinary MMIO accesses are
 * compiler barriers for normal memory too. Volatile C alone permits the
 * compiler to eliminate/reorder GPU descriptor stores across a doorbell,
 * or reuse stale normal-memory data after observing device completion.
 * Hardware WC draining remains the caller's explicit wmb()/mb() duty. */
#define KPI_MMIO_READ(name, size, type, reg, barrier) \
static inline type name(const volatile void __iomem *addr) { \
    type value; \
    __asm__ volatile("mov" size " %1,%0" : reg(value) \
                     : "m" (*(const volatile type *)addr) barrier); \
    return value; \
}
#define KPI_MMIO_WRITE(name, size, type, reg, barrier) \
static inline void name(type value, volatile void __iomem *addr) { \
    __asm__ volatile("mov" size " %0,%1" : : reg(value), \
                     "m" (*(volatile type *)addr) barrier); \
}
KPI_MMIO_READ(readb, "b", u8, "=q", : "memory")
KPI_MMIO_READ(readw, "w", u16, "=r", : "memory")
KPI_MMIO_READ(readl, "l", u32, "=r", : "memory")
KPI_MMIO_READ(readq, "q", u64, "=r", : "memory")
KPI_MMIO_WRITE(writeb, "b", u8, "q", : "memory")
KPI_MMIO_WRITE(writew, "w", u16, "r", : "memory")
KPI_MMIO_WRITE(writel, "l", u32, "r", : "memory")
KPI_MMIO_WRITE(writeq, "q", u64, "r", : "memory")

KPI_MMIO_READ(__readb, "b", u8, "=q", )
KPI_MMIO_READ(__readw, "w", u16, "=r", )
KPI_MMIO_READ(__readl, "l", u32, "=r", )
KPI_MMIO_READ(__readq, "q", u64, "=r", )
KPI_MMIO_WRITE(__writeb, "b", u8, "q", )
KPI_MMIO_WRITE(__writew, "w", u16, "r", )
KPI_MMIO_WRITE(__writel, "l", u32, "r", )
KPI_MMIO_WRITE(__writeq, "q", u64, "r", )
#undef KPI_MMIO_READ
#undef KPI_MMIO_WRITE
#define readb_relaxed __readb
#define readw_relaxed __readw
#define readl_relaxed __readl
#define readq_relaxed __readq
#define writeb_relaxed __writeb
#define writew_relaxed __writew
#define writel_relaxed __writel
#define writeq_relaxed __writeq
#define __raw_readb __readb
#define __raw_readw __readw
#define __raw_readl __readl
#define __raw_readq __readq
#define __raw_writeb __writeb
#define __raw_writew __writew
#define __raw_writel __writel
#define __raw_writeq __writeq

static inline u8 ioread8(const volatile void __iomem *addr) { return readb(addr); }
static inline u16 ioread16(const volatile void __iomem *addr) { return readw(addr); }
static inline u32 ioread32(const volatile void __iomem *addr) { return readl(addr); }
static inline void iowrite8(u8 value, volatile void __iomem *addr) { writeb(value, addr); }
static inline void iowrite16(u16 value, volatile void __iomem *addr) { writew(value, addr); }
static inline void iowrite32(u32 value, volatile void __iomem *addr) { writel(value, addr); }

#define __io_br() __asm__ volatile("" ::: "memory")
#define __io_ar(v) __asm__ volatile("" ::: "memory")
#define __io_bw() __asm__ volatile("" ::: "memory")
#define __io_aw() __asm__ volatile("" ::: "memory")

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

/* "Slow" I/O variants: upstream x86 inserts a tiny delay after the access for
 * ISA devices; the native HAL has no slow_down_io() and the extra delay is
 * not required by the emulated devices, so these are plain aliases. */
static inline u8 inb_p(u16 port) { return inb(port); }
static inline u16 inw_p(u16 port) { return inw(port); }
static inline u32 inl_p(u16 port) { return inl(port); }
static inline void outb_p(u8 value, u16 port) { outb(value, port); }
static inline void outw_p(u16 value, u16 port) { outw(value, port); }
static inline void outl_p(u32 value, u16 port) { outl(value, port); }

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
