#ifndef __AVORY_LINUXKPI_GFP_H
#define __AVORY_LINUXKPI_GFP_H

/* Minimum viable Linux <linux/gfp.h> overlay.
 *
 * The real header (and the mmzone/page-flags web behind it) assumes a Linux
 * memory model AvoryOS does not have yet.  The flag *values* come from the
 * upstream <linux/gfp_types.h>, which is self-contained; allocation behavior
 * is implemented in linuxkpi/src/slab.c.  Only __GFP_ZERO and the DMA-zone
 * bits carry meaning today; everything else is accepted and ignored.
 */

#include <linux/types.h>
#include <linux/gfp_types.h>

static inline bool gfpflags_allow_blocking(const gfp_t gfp_flags) {
  (void)gfp_flags;
  return true;
}

static inline bool gfpflags_allow_spinning(const gfp_t gfp_flags) {
  (void)gfp_flags;
  return true;
}

/* Linux's GFP_ZONE macros pick a zone number from the low GFP bits; there are
 * no zones here, so the value is informational. */
#define gfp_zone(gfp_flags) (0)

#ifndef default_gfp
#define __kpi_default_gfp(a, b, ...) b
#define default_gfp(...) __kpi_default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#endif

#endif /* __AVORY_LINUXKPI_GFP_H */
