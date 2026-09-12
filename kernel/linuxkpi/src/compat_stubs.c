/* Weak stubs for VFS/procfs functions that imported library code references
 * but that the native VFS bridge does not provide yet.  They keep
 * lib/string_helpers.c linking; when the real implementations are imported
 * (fs/proc/base.c, fs/d_path.c) their strong definitions win. */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

/* Upstream declares this in <linux/string.h>; the implementation normally
 * lives in mm/util.c. */
void *memdup_user_nul(const void __user *src, size_t len) {
  char *p = kmalloc(len + 1, GFP_KERNEL);

  if (!p)
    return ERR_PTR(-ENOMEM);
  if (copy_from_user(p, src, len)) {
    kfree(p);
    return ERR_PTR(-EFAULT);
  }
  p[len] = '\0';
  return p;
}

/* lib/string.c implementation, needed by sync_file and dma-buf.
 * (strscpy_pad is already provided by the imported lib/string_helpers.c.) */
ssize_t strscpy(char *dest, const char *src, size_t count) {
  size_t len = 0;

  while (len + 1 < count && src[len]) {
    dest[len] = src[len];
    len++;
  }
  if (count)
    dest[len] = '\0';

  if (src[len])
    return -E2BIG;
  return (ssize_t)len;
}

char *strndup_user(const char __user *s, long n) {
  char *p;
  long length = strnlen_user(s, n);

  if (!length)
    return ERR_PTR(-EFAULT);
  if (length > n)
    return ERR_PTR(-EINVAL);

  p = kmalloc((size_t)length, GFP_KERNEL);
  if (!p)
    return ERR_PTR(-ENOMEM);
  if (copy_from_user(p, s, (size_t)length)) {
    kfree(p);
    return ERR_PTR(-EFAULT);
  }
  p[length - 1] = '\0';
  return p;
}

__attribute__((weak)) int get_cmdline(struct task_struct *task, char *buffer,
                                      int buflen) {
  (void)task;
  if (buflen > 0)
    buffer[0] = '\0';
  return 0;
}

__attribute__((weak)) char *file_path(struct file *file, char *buf,
                                      int buflen) {
  (void)file;
  (void)buflen;
  return NULL;
}
