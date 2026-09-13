/* Weak stubs for VFS/procfs functions that imported library code references
 * but that the native VFS bridge does not provide yet.  They keep
 * lib/string_helpers.c linking; when the real implementations are imported
 * (fs/proc/base.c, fs/d_path.c) their strong definitions win. */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
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

/* lib/string.c implementation; drm_dp_mst_topology uses it to build connector
 * names and more DRM code will need it. */
size_t strlcat(char *dest, const char *src, size_t count) {
  size_t dsize = strlen(dest);
  size_t len = strlen(src);
  size_t res = dsize + len;

  /* This would be a bug */
  BUG_ON(dsize >= count);

  dest += dsize;
  count -= dsize;
  if (len >= count)
    len = count - 1;
  __builtin_memcpy(dest, src, len);
  dest[len] = '\0';
  return res;
}

/* lib/ratelimit.c implementation.  Divergence: the state lock is not a
 * trylock, so contention serializes instead of suppressing the message, and
 * the "callbacks suppressed" report is emitted inline rather than deferred
 * (CONFIG_PRINTK_DEFERRED does not exist here).  Semantics otherwise match
 * upstream, which is what dev_*_ratelimited() callers rely on. */
int ___ratelimit(struct ratelimit_state *rs, const char *func) {
  int interval = rs->interval;
  int burst = rs->burst;
  unsigned long flags;
  int ret;

  if (!interval)
    return 1;

  raw_spin_lock_irqsave(&rs->lock, flags);

  if (!rs->begin)
    rs->begin = jiffies;

  if (time_is_before_jiffies(rs->begin + interval)) {
    if (rs->missed && !(rs->flags & RATELIMIT_MSG_ON_RELEASE)) {
      printk(KERN_WARNING "%s: %d callbacks suppressed\n", func, rs->missed);
      rs->missed = 0;
    }
    rs->begin = jiffies;
    rs->printed = 0;
  }
  if (burst && burst > rs->printed) {
    rs->printed++;
    ret = 1;
  } else {
    rs->missed++;
    ret = 0;
  }

  raw_spin_unlock_irqrestore(&rs->lock, flags);
  return ret;
}

/* ── lib/string.c helpers used by amdgpu ─────────────────────────────────── */

size_t strnlen(const char *s, size_t count) {
  const char *sc;

  for (sc = s; count-- && *sc; ++sc)
    ;
  return (size_t)(sc - s);
}

char *strnstr(const char *s1, const char *s2, size_t len) {
  size_t l2 = strlen(s2);

  if (!l2)
    return (char *)s1;
  while (len >= l2) {
    len--;
    if (!memcmp(s1, s2, l2))
      return (char *)s1;
    if (!*s1++)
      break;
  }
  return NULL;
}

size_t strcspn(const char *s, const char *reject) {
  const char *p;
  size_t count = 0;

  for (; *s; s++, count++)
    for (p = reject; *p; p++)
      if (*p == *s)
        return count;
  return count;
}

char *strsep(char **s, const char *ct) {
  char *sbegin = *s;
  char *end;

  if (!sbegin)
    return NULL;

  end = sbegin + strcspn(sbegin, ct);
  if (*end)
    *end++ = '\0';
  *s = end;
  return sbegin;
}

/* A small sscanf for the formats amdgpu uses: %u, %d, %x/%X, %s (with width)
 * and %n, plus literal characters/whitespace.  Unknown conversions stop the
 * scan (returning what matched so far), matching the standard's behavior for
 * a failed conversion. */
int sscanf(const char *buf, const char *fmt, ...) {
  va_list args;
  const char *s = buf;
  int matched = 0;

  va_start(args, fmt);
  while (*fmt) {
    if (*fmt == ' ' || *fmt == '\t') {
      while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
      fmt++;
      continue;
    }
    if (*fmt != '%') {
      if (*s != *fmt)
        break;
      s++;
      fmt++;
      continue;
    }
    fmt++;
    int width = 0;
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');
    char conv = *fmt ? *fmt++ : 0;
    if (conv == 'n') {
      *(va_arg(args, int *)) = (int)(s - buf);
      continue;
    }
    if (conv != 'c' && conv != '[')
      while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (!*s)
      break;
    if (conv == 's') {
      char *out = va_arg(args, char *);
      int n = 0;
      while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
             (!width || n < width))
        out[n++] = *s++;
      out[n] = '\0';
      if (!n)
        break;
      matched++;
      continue;
    }
    if (conv == 'u' || conv == 'd' || conv == 'x' || conv == 'X' ||
        conv == 'i') {
      unsigned long value = 0;
      int neg = 0, n = 0;
      if (conv == 'd' && *s == '-') {
        neg = 1;
        s++;
      }
      while (*s && (!width || n < width)) {
        int digit;
        if (conv == 'x' || conv == 'X' || conv == 'i' ||
            (conv == 'u' || conv == 'd') == 0) {
          if (*s >= '0' && *s <= '9')
            digit = *s - '0';
          else if (*s >= 'a' && *s <= 'f')
            digit = *s - 'a' + 10;
          else if (*s >= 'A' && *s <= 'F')
            digit = *s - 'A' + 10;
          else
            break;
        } else if (*s >= '0' && *s <= '9') {
          digit = *s - '0';
        } else {
          break;
        }
        value = value * ((conv == 'x' || conv == 'X' || conv == 'i') ? 16 : 10)
                + (unsigned)digit;
        s++;
        n++;
      }
      if (!n)
        break;
      *(va_arg(args, unsigned int *)) =
          (unsigned int)(neg ? -value : value);
      matched++;
      continue;
    }
    break; /* unsupported conversion */
  }
  va_end(args);
  return matched;
}

void *vmemdup_user(const void __user *src, size_t len) {
  void *p = kmalloc(len, GFP_KERNEL);

  if (!p)
    return ERR_PTR(-ENOMEM);
  if (copy_from_user(p, src, len)) {
    kfree(p);
    return ERR_PTR(-EFAULT);
  }
  return p;
}

/* ── printk ratelimit and misc kernel state ──────────────────────────────── */

struct ratelimit_state printk_ratelimit_state = {.interval = 5 * HZ,
                                                 .burst = 10};

int __printk_ratelimit(const char *func) {
  return ___ratelimit(&printk_ratelimit_state, func);
}

enum system_states system_state = SYSTEM_RUNNING;

void add_taint(unsigned flag, enum lockdep_ok lockdep_ok) {
  (void)flag;
  (void)lockdep_ok;
}

/* No userspace helper and no power management: orderly_poweroff() cannot ask
 * userland to shut down; the native kernel owns reboots. */
void orderly_poweroff(bool force) { (void)force; }

void emergency_restart(void) {}

/* Task shadows live until their thread exits (no thread-exit hook yet), so
 * the refcount-0 path is a no-op rather than a free. */
struct task_struct;
void __put_task_struct(struct task_struct *t) { (void)t; }
