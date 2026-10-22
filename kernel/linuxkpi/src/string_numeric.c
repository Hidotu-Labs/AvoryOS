/* SPDX-License-Identifier: GPL-2.0-only */
/* Small Linux string/number entry points; importing all of vsprintf.c or
 * string.c would duplicate AvoryOS's formatting and memory primitives. */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/limits.h>
#include "kstrtox.h"

/* Linux lib/vsprintf.c's legacy parser, using the imported radix parser. */
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base) {
  unsigned long long result;
  unsigned int consumed;
  cp = _parse_integer_fixup_radix(cp, &base);
  consumed = _parse_integer_limit(cp, base, &result, INT_MAX);
  if (endp)
    *endp = (char *)cp + (consumed & ~KSTRTOX_OVERFLOW);
  return result;
}

unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base) {
  return simple_strtoull(cp, endp, base);
}

long simple_strtol(const char *cp, char **endp, unsigned int base) {
  if (*cp == '-')
    return -simple_strtoul(cp + 1, endp, base);
  return simple_strtoul(cp, endp, base);
}

/* Linux lib/string.c, including NUL as a searchable character. */
char *strnchr(const char *s, size_t count, int c) {
  while (count--) {
    if (*s == (char)c)
      return (char *)s;
    if (*s++ == '\0')
      break;
  }
  return NULL;
}
