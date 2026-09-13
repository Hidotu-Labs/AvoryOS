#ifndef __AVORY_LINUXKPI_PROC_FS_H
#define __AVORY_LINUXKPI_PROC_FS_H

/* AvoryOS overlay for <linux/proc_fs.h>.
 *
 * The stock header's proc_sb_info() inline dereferences struct super_block,
 * which is defined in the LinuxKPI <linux/mount.h>; upstream reaches it
 * through the fs/mount include chain.  Pull it in first so the inline sees a
 * complete type. */

#include <linux/mount.h>

#include_next <linux/proc_fs.h>

#endif /* __AVORY_LINUXKPI_PROC_FS_H */
