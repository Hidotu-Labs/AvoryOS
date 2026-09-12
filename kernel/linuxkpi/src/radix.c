/* One-time radix-tree/xarray bring-up.  See linux/radix-tree.h. */

#include <linux/radix-tree.h>

/* Creates the shared radix-tree node cache (and upstream's per-CPU preload
 * state) and initialises the xarray internals that lib/xarray.c and lib/idr.c
 * build on.  Upstream calls this from init/main.c; AvoryOS calls it from the
 * LinuxKPI bring-up in kernel.c before any xarray/idr user runs. */
void linuxkpi_radix_init(void) { radix_tree_init(); }
