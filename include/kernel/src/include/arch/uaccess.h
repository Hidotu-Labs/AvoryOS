#ifndef ARCH_UACCESS_H
#define ARCH_UACCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef USER_SPACE_LIMIT
#define USER_SPACE_LIMIT 0x00007FFFFFFFFFFFULL
#endif

#ifndef USER_ADDR_MAX
#define USER_ADDR_MAX USER_SPACE_LIMIT
#endif

static inline bool is_user_range(const void *addr, size_t size) {
  uint64_t start = (uint64_t)addr;
  if (start > USER_SPACE_LIMIT)
    return false;
  if (size > 0x800000000000ULL - start)
    return false;
  return true;
}

static inline bool is_user_ptr(uint64_t ptr) {
  return ptr <= USER_SPACE_LIMIT;
}

// Low-level assembly primitives with exception table protection:
unsigned long copy_from_user(void *to, const void *from, unsigned long n);
unsigned long copy_to_user(void *to, const void *from, unsigned long n);
unsigned long clear_user(void *to, unsigned long n);
long strncpy_from_user(char *dst, const char *src, long count);
long strnlen_user(const char *src, long maxlen);

#endif
