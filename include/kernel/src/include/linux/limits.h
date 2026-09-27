/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_LIMITS_H
#define ASCENT_LINUX_LIMITS_H

/* LinuxKPI — limits.h: integer boundary constants
 * types.h MUST be included first so u8/s8/etc. are already defined
 * when the cast macros below are evaluated by the compiler. */
#include <linux/types.h>

#define U8_MAX    ((u8)0xFFU)
#define U16_MAX   ((u16)0xFFFFU)
#define U32_MAX   ((u32)0xFFFFFFFFU)
#define U64_MAX   ((u64)0xFFFFFFFFFFFFFFFFULL)

#define S8_MAX    ((s8)0x7F)
#define S8_MIN    ((s8)(-0x7F - 1))
#define S16_MAX   ((s16)0x7FFF)
#define S16_MIN   ((s16)(-0x7FFF - 1))
#define S32_MAX   ((s32)0x7FFFFFFF)
#define S32_MIN   ((s32)(-0x7FFFFFFF - 1))
#define S64_MAX   ((s64)0x7FFFFFFFFFFFFFFFLL)
#define S64_MIN   ((s64)(-0x7FFFFFFFFFFFFFFFLL - 1))

/* Classic aliases */
#define INT_MAX   S32_MAX
#define INT_MIN   S32_MIN
#define UINT_MAX  U32_MAX
#define LONG_MAX  S64_MAX
#define ULONG_MAX U64_MAX

#endif /* ASCENT_LINUX_LIMITS_H */
