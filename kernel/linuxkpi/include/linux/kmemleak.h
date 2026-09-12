#ifndef __AVORY_LINUXKPI_KMEMLEAK_H
#define __AVORY_LINUXKPI_KMEMLEAK_H

/* Minimal Linux <linux/kmemleak.h> overlay.
 *
 * Kmemleak is a debugging facility; there is none here.  Upstream's header
 * unconditionally pulls <linux/vmalloc.h> (and through it asm/processor.h),
 * which AvoryOS does not have yet, so every hook is a no-op.  radix-tree.c
 * uses these hooks on its node cache. */

#define kmemleak_init() do { } while (0)
#define kmemleak_alloc(ptr, size, min_count, gfp) do { } while (0)
#define kmemleak_alloc_percpu(ptr, size, gfp) do { } while (0)
#define kmemleak_vmalloc(area, size, gfp) do { } while (0)
#define kmemleak_free(ptr) do { } while (0)
#define kmemleak_free_part(ptr, size) do { } while (0)
#define kmemleak_free_percpu(ptr) do { } while (0)
#define kmemleak_update_trace(ptr) do { } while (0)
#define kmemleak_not_leak(ptr) do { } while (0)
#define kmemleak_transient_leak(ptr) do { } while (0)
#define kmemleak_ignore(ptr) do { } while (0)
#define kmemleak_scan_area(ptr, length, gfp) do { } while (0)
#define kmemleak_no_scan(ptr) do { } while (0)
#define kmemleak_erase(var_ptr) do { } while (0)
#define kmemleak_erase_on_stack(var_ptr) do { } while (0)

#endif /* __AVORY_LINUXKPI_KMEMLEAK_H */
