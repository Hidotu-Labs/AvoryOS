/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_CACHE_H
#define ASCENT_LINUX_CACHE_H

/* LinuxKPI — cache.h: cache-line size and alignment attributes */

/* x86-64: L1 cache line is 64 bytes */
#define SMP_CACHE_BYTES         64
#define L1_CACHE_BYTES          SMP_CACHE_BYTES
#define L1_CACHE_SHIFT          6

/* Pad a struct field / local variable to an entire cache line */
#define ____cacheline_aligned   __attribute__((__aligned__(SMP_CACHE_BYTES)))
#define __cacheline_aligned     ____cacheline_aligned

/* Place a variable in its own cache line, preventing false sharing */
#define ____cacheline_aligned_in_smp ____cacheline_aligned
#define __cacheline_aligned_in_smp   ____cacheline_aligned

/* Read-mostly data: GCC hint to place in a cache-friendly section */
#define __read_mostly           /* no-op in a monolithic kernel */

#endif /* ASCENT_LINUX_CACHE_H */
