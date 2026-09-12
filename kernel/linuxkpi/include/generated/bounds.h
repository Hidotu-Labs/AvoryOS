/* AvoryOS stand-in for Kbuild's <generated/bounds.h>.
 *
 * Upstream this file is produced by building kernel/bounds.c.  The values are
 * compile-time layout constants that imported headers only use for static
 * arrays and BUILD_BUG_ONs.  Keep them consistent with the overlay model:
 * 24 page flag bits (see page-flags.h) and the four x86 zones. */
#ifndef __AVORY_LINUXKPI_GENERATED_BOUNDS_H
#define __AVORY_LINUXKPI_GENERATED_BOUNDS_H

#define NR_PAGEFLAGS 24
#define MAX_NR_ZONES 4

#endif /* __AVORY_LINUXKPI_GENERATED_BOUNDS_H */
