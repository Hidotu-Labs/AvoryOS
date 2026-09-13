#ifndef __AVORY_LINUXKPI_DEBUGFS_H
#define __AVORY_LINUXKPI_DEBUGFS_H

/* AvoryOS overlay for <linux/debugfs.h>.
 *
 * CONFIG_DEBUG_FS is off, so the stock header's entry points are stubs; it is
 * kept because amdgpu headers include it for the seq_file/attribute shapes.
 * The stock header does not pull wait/workqueue itself, and amdgpu_ih.h /
 * amdgpu_ras.h embed wait_queue_head_t/delayed_work fields right after
 * including it, so make the chain explicit here. */

#include <linux/wait.h>
#include <linux/workqueue.h>

#include_next <linux/debugfs.h>

#endif /* __AVORY_LINUXKPI_DEBUGFS_H */
