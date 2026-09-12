#ifndef __AVORY_LINUXKPI_MMAN_H
#define __AVORY_LINUXKPI_MMAN_H

/* AvoryOS overlay for <linux/mman.h>: the user-visible mmap constants only.
 * The upstream header drags percpu_counters and mm internals the bridge does
 * not need.  MAP_FAILED is userspace-only upstream; the bridge uses it as
 * the f_op->mmap failure sentinel, so it is defined here. */

#include <linux/types.h>
#include <uapi/linux/mman.h>

#define MAP_FAILED ((void *)-1)

#endif /* __AVORY_LINUXKPI_MMAN_H */
