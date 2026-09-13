#ifndef __AVORY_LINUXKPI_MMZONE_H
#define __AVORY_LINUXKPI_MMZONE_H

/* AvoryOS overlay for <linux/mmzone.h>.
 *
 * The stock header is used as-is; this adds the page <-> zone/node helpers
 * that upstream defines in <linux/mm.h>.  AvoryOS replaces mm.h with a
 * trimmed model, but stock headers that include mmzone.h directly (swap.h,
 * vmstat.h, bio.h, mm_inline.h) still expect the helpers, so they are
 * supplied here as macros.  Macros (guarded, not inlines) so a translation
 * unit that also includes linux/mm.h cannot hit a redefinition. */

#include_next <linux/mmzone.h>

/* swap.h embeds struct plist_node right after including mmzone.h; upstream
 * reaches plist.h through its include chain. */
#include <linux/plist.h>

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
#ifndef page_to_nid
/* No NUMA: stock mmzone.h only defines page_to_nid under CONFIG_NUMA. */
#define page_to_nid(page) 0
#endif
#ifndef pfn_to_nid
#define pfn_to_nid(pfn) 0
#endif

#endif /* __AVORY_LINUXKPI_MMZONE_H */
