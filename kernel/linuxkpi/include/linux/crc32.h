#ifndef __AVORY_LINUXKPI_CRC32_H
#define __AVORY_LINUXKPI_CRC32_H

/* Additive Linux <linux/crc32.h> overlay.
 *
 * crc32.c's conversions (__cpu_to_le32/__le32_to_cpu) and its cacheline-aligned
 * tables relied on transitive includes from the real <linux/module.h> and
 * friends, which this layer deliberately does not have yet.  Pull those in,
 * then the unmodified upstream API header. */

#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/byteorder/generic.h>
#include <linux/cache.h>

#include_next <linux/crc32.h>

#endif /* __AVORY_LINUXKPI_CRC32_H */
