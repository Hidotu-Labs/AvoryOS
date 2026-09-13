#ifndef __AVORY_LINUXKPI_UACCESS_H
#define __AVORY_LINUXKPI_UACCESS_H

/* Native-backed Linux <linux/uaccess.h> overlay.
 *
 * The copy primitives are the fault-tolerant assembly in
 * src/arch/x86_64/uaccess.asm; like Linux they return the number of bytes NOT
 * copied (0 on success) and validate the user range themselves.  Scalar
 * get_user()/put_user() are built on those copies: slower than Linux's
 * per-size asm, but correct and enough until the access work grows. */

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

/* Matches the bound enforced by src/arch/x86_64/uaccess.asm. */
#define TASK_SIZE_MAX 0x0000800000000000UL

static inline bool access_ok(const void __user *addr, unsigned long size) {
  unsigned long start = (unsigned long)addr;
  unsigned long end = start + size;
  return end >= start && end <= TASK_SIZE_MAX;
}

unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long clear_user(void __user *to, unsigned long n);
long strncpy_from_user(char *dst, const char __user *src, long count);
long strnlen_user(const char __user *str, long count);

/* Raw variants are the same primitives here (no user-access checks). */
static inline unsigned long __copy_from_user(void *to, const void __user *from,
                                             unsigned long n) {
  return copy_from_user(to, from, n);
}

static inline unsigned long __copy_to_user(void __user *to, const void *from,
                                           unsigned long n) {
  return copy_to_user(to, from, n);
}

/* ── scalar accessors ───────────────────────────────────────────────────── */

#define __get_user(x, ptr)                                                    \
  ({                                                                          \
    __typeof__(*(ptr)) __gu_val = (__typeof__(*(ptr)))0;                      \
    unsigned long __gu_left =                                                \
        copy_from_user(&__gu_val, (const void __user *)(ptr),                 \
                       sizeof(*(ptr)));                                       \
    (x) = __gu_val;                                                           \
    __gu_left ? -EFAULT : 0;                                                  \
  })

#define __put_user(x, ptr)                                                    \
  ({                                                                          \
    __typeof__(*(ptr)) __pu_val = (__typeof__(*(ptr)))(x);                    \
    unsigned long __pu_left =                                                \
        copy_to_user((void __user *)(ptr), &__pu_val, sizeof(*(ptr)));        \
    __pu_left ? -EFAULT : 0;                                                  \
  })

#define get_user(x, ptr) __get_user(x, ptr)
#define put_user(x, ptr) __put_user(x, ptr)

#define __get_user_error(x, ptr, err)                                         \
  do {                                                                        \
    long __gu_err = __get_user(x, ptr);                                       \
    if (__gu_err)                                                             \
      (err) = (__typeof__(err))__gu_err;                                      \
  } while (0)

#define __put_user_error(x, ptr, err)                                         \
  do {                                                                        \
    long __pu_err = __put_user(x, ptr);                                       \
    if (__pu_err)                                                             \
      (err) = (__typeof__(err))__pu_err;                                      \
  } while (0)

#define __get_user_unaligned(x, ptr) __get_user(x, ptr)
#define __put_user_unaligned(x, ptr) __put_user(x, ptr)
#define get_user_unaligned(x, ptr) get_user(x, ptr)
#define put_user_unaligned(x, ptr) put_user(x, ptr)

/* ── struct copies ──────────────────────────────────────────────────────── */

/* 1 = all bytes are zero, 0 = a non-zero byte, -EFAULT = bad user range. */
static inline int check_zeroed_user(const void __user *from, size_t size) {
  const unsigned char __user *p = from;

  while (size) {
    unsigned long word = 0;
    size_t chunk = size < sizeof(word) ? size : sizeof(word);

    if (copy_from_user(&word, p, chunk))
      return -EFAULT;
    if (word)
      return 0;
    p += chunk;
    size -= chunk;
  }
  return 1;
}

/* Linux semantics: copy min(ksize, usize) bytes, zero the kernel tail, and
 * reject (with -E2BIG) a user struct whose tail beyond ksize is not zero. */
static inline int copy_struct_from_user(void *dst, size_t ksize,
                                        const void __user *src, size_t usize) {
  size_t size = (usize < ksize) ? usize : ksize;

  if (copy_from_user(dst, src, size))
    return -EFAULT;

  if (usize < ksize) {
    memset((char *)dst + size, 0, ksize - size);
    return 0;
  }

  if (usize > ksize) {
    int ret = check_zeroed_user((const unsigned char __user *)src + ksize,
                                usize - ksize);
    if (ret <= 0)
      return ret ? ret : -E2BIG;
  }

  return 0;
}

#define u64_to_user_ptr(x) ((void __user *)(uintptr_t)(x))

/* No pagefault/migrate disable state exists: all kernel memory is permanently
 * mapped through the HHDM and there is no highmem or page migration.  Stock
 * <linux/io-mapping.h> inlines call these; keep them balanced no-ops. */
static inline void pagefault_disable(void) { }
static inline void pagefault_enable(void) { }
static inline bool pagefault_disabled(void) { return false; }

/* x86 passes addresses through unchanged (no LAM/5-level tagging to strip in
 * this kernel); stock uaccess.h defines the same for such configs. */
static inline unsigned long untagged_addr(unsigned long addr) { return addr; }

#endif /* __AVORY_LINUXKPI_UACCESS_H */
