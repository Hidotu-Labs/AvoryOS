#ifndef __AVORY_LINUXKPI_KGDB_H
#define __AVORY_LINUXKPI_KGDB_H

/* AvoryOS overlay for <linux/kgdb.h>.
 *
 * The upstream header drags in kprobes/ftrace/ptrace for debugging
 * integrations that do not exist here.  Imported DRM code only asks
 * in_dbg_master() (drm_util.h's assert helper); AvoryOS has no kgdb. */

#include <linux/types.h>

static inline bool in_dbg_master(void) { return false; }

#endif /* __AVORY_LINUXKPI_KGDB_H */
