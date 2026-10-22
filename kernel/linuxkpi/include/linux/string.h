#ifndef __AVORY_LINUXKPI_STRING_H
#define __AVORY_LINUXKPI_STRING_H

#include_next <linux/string.h>

/* Linux 6.12 adds an allocation-profiling macro around kmemdup. AvoryOS
 * owns allocation and has no Linux allocation tagging; retain its exported
 * function for both old and new callers, regardless of header order. */
#ifdef kmemdup
#undef kmemdup
#endif
void *kmemdup(const void *src, size_t len, gfp_t flags);

#endif
