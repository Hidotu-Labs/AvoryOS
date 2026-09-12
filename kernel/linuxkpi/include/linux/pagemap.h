#ifndef __AVORY_LINUXKPI_PAGEMAP_H
#define __AVORY_LINUXKPI_PAGEMAP_H

/* AvoryOS overlay for <linux/pagemap.h>.
 *
 * Upstream's page-cache API is built on the Linux address_space/folio model,
 * which AvoryOS overlays on its native page cache.  Imported DRM code includes
 * this header for the mapping GFP accessors and page pinning helpers, so the
 * subset lives here; the page-cache operations AvoryOS implements are in
 * linuxkpi/src/shmem.c (shmem-backed GEM objects). */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagevec.h>

/* Mapping GFP helpers (upstream pagemap.h). */
static inline gfp_t mapping_gfp_mask(const struct address_space *mapping) {
  return mapping->gfp_mask;
}

static inline void mapping_set_gfp_mask(struct address_space *mapping,
                                        gfp_t mask) {
  mapping->gfp_mask = mask;
}

#define mapping_gfp_constraint(mapping, GFP_CONSTRAINT)                       \
  (mapping_gfp_mask(mapping) & (GFP_CONSTRAINT))

static inline bool mapping_unevictable(const struct address_space *mapping) {
  return test_bit(0, &mapping->flags);
}

static inline void mapping_set_unevictable(struct address_space *mapping) {
  set_bit(0, &mapping->flags);
}

static inline void mapping_clear_unevictable(struct address_space *mapping) {
  clear_bit(0, &mapping->flags);
}

unsigned long invalidate_mapping_pages(struct address_space *mapping,
                                       pgoff_t start, pgoff_t end);

#endif /* __AVORY_LINUXKPI_PAGEMAP_H */
