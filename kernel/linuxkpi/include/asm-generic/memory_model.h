#ifndef __AVORY_LINUXKPI_ASM_GENERIC_MEMORY_MODEL_H
#define __AVORY_LINUXKPI_ASM_GENERIC_MEMORY_MODEL_H

/* AvoryOS overlay for <asm-generic/memory_model.h>.
 *
 * Upstream selects pfn_to_page()/page_to_pfn() from the configured memory
 * model (FLATMEM/SPARSEMEM) and #defines the two names accordingly.  AvoryOS
 * has its own sparse mem_map and exports both as real functions
 * (linuxkpi/src/page.c), so the upstream macros must not be defined.  This
 * header deliberately expands to nothing. */

#endif /* __AVORY_LINUXKPI_ASM_GENERIC_MEMORY_MODEL_H */
