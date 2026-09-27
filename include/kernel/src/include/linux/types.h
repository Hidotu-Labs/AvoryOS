/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_LINUX_TYPES_H
#define ASCENT_LINUX_TYPES_H

/*
 * LinuxKPI — types.h
 * Maps Linux integer typedefs onto the freestanding <stdint.h> / <stddef.h>
 * equivalents already in scope via the kernel's freestnd-c-hdrs sysroot.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Signed */
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* Unsigned */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Kernel-style aliases */
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

/* Endian-annotated types (no real byte-swap on a native-endian build) */
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

/* Misc */
typedef unsigned int  uint;
typedef unsigned long ulong;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* bool is already from <stdbool.h> */

#endif /* ASCENT_LINUX_TYPES_H */
