#ifndef __AVORY_LINUXKPI_INSTRUMENTED_H
#define __AVORY_LINUXKPI_INSTRUMENTED_H

/* AvoryOS overlay for <linux/instrumented.h>.
 *
 * Upstream's version includes the KASAN/KCSAN/KMSAN check headers even when
 * all three are disabled; those headers (kmsan.h) drag in the sanitizer page
 * API and, transitively, mm internals AvoryOS does not model.  With no
 * sanitizers configured every hook is a no-op, which is what this provides. */

#include <linux/compiler.h>
#include <linux/types.h>

static __always_inline void instrument_read(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_write(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_read_write(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_atomic_read(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_atomic_write(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_atomic_read_write(const volatile void *v, size_t size) {
  (void)v;
  (void)size;
}
static __always_inline void instrument_copy_to_user(void __user *to, const void *from,
                                                    unsigned long n) {
  (void)to;
  (void)from;
  (void)n;
}
static __always_inline void instrument_copy_from_user_before(const void *to,
                                                             const void __user *from,
                                                             unsigned long n) {
  (void)to;
  (void)from;
  (void)n;
}
static __always_inline void instrument_copy_from_user_after(const void *to,
                                                            const void __user *from,
                                                            unsigned long n,
                                                            unsigned long left) {
  (void)to;
  (void)from;
  (void)n;
  (void)left;
}

#define instrument_get_user(to) do { } while (0)

#endif /* __AVORY_LINUXKPI_INSTRUMENTED_H */
