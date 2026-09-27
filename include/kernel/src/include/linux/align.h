/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_ALIGN_H
#define ASCENT_LINUX_ALIGN_H

/* LinuxKPI — align.h: pointer and size alignment helpers */

#include <linux/types.h>

/*
 * ALIGN(x, a) — round x UP to the nearest multiple of a (must be power-of-2).
 * ALIGN_DOWN(x, a) — round x DOWN.
 * IS_ALIGNED(x, a) — true if x is already aligned.
 * PTR_ALIGN(p, a) — align a pointer value.
 */
#define ALIGN(x, a)         (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)     ((typeof(p))ALIGN((uintptr_t)(p), (a)))
#define PTR_ALIGN_DOWN(p,a) ((typeof(p))ALIGN_DOWN((uintptr_t)(p), (a)))

/* Round a size up to the next page boundary */
#define PAGE_ALIGN(addr)    ALIGN(addr, 4096UL)

#endif /* ASCENT_LINUX_ALIGN_H */
