/* Small implementations for symbols imported DRM code references but that
 * live in upstream files AvoryOS does not compile (yet).  Everything here is
 * weak so the real imported implementation wins the moment it is added to
 * scripts/linux/files.txt; each entry is tracked in docs/linuxkpi-gaps.md. */

#include <linux/thread_info.h> /* must precede asm/smp.h (NOT_STACK et al.) */
#include <asm/pgtable_types.h>
#include <asm/processor.h>
#include <asm/set_memory.h>
#include <asm/smp.h>
#include <drm/drm_client.h>
#include <drm/drm_utils.h>
#include <linux/component.h>
#include <linux/fwnode.h>
#include <linux/ioport.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/llist.h>
#include <linux/math64.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include <linuxkpi/log.h>

/* ── component framework (drm_sysfs typec links) ────────────────────────── */

__attribute__((weak)) int component_add(struct device *dev,
                                        const struct component_ops *ops) {
  (void)dev;
  (void)ops;
  return 0;
}

__attribute__((weak)) void component_del(struct device *dev,
                                         const struct component_ops *ops) {
  (void)dev;
  (void)ops;
}

/* ── drm_client (only referenced by drm_dev_unregister paths) ───────────── */

__attribute__((weak)) void drm_client_dev_unregister(struct drm_device *dev) {
  (void)dev;
}

__attribute__((weak)) void drm_client_dev_restore(struct drm_device *dev) {
  (void)dev;
}

/* ── panel orientation quirks (no DMI tables here) ──────────────────────── */

__attribute__((weak)) int drm_get_panel_orientation_quirk(int width,
                                                          int height) {
  (void)width;
  (void)height;
  return -1; /* DRM_MODE_PANEL_ORIENTATION_UNKNOWN */
}

__attribute__((weak)) void fwnode_handle_put(struct fwnode_handle *fwnode) {
  (void)fwnode;
}

/* ── x86 memory/cpu helpers ─────────────────────────────────────────────── */

__attribute__((weak)) int set_pages_array_wb(struct page **pages,
                                             int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

__attribute__((weak)) int set_pages_array_wc(struct page **pages,
                                             int addrinarray) {
  (void)pages;
  (void)addrinarray;
  return 0;
}

__attribute__((weak)) int wbinvd_on_all_cpus(void) { return 0; }

/* Kbuild initializes this from the boot CPU; a zeroed record is enough for
 * the cache-line macros imported code uses. */
struct cpuinfo_x86 boot_cpu_data = {
    .x86_cache_alignment = 64,
};

unsigned long __default_kernel_pte_mask = ~0UL;

/* ── misc kernel globals ────────────────────────────────────────────────── */

__attribute__((weak)) bool static_key_initialized = false;
__attribute__((weak)) int oops_in_progress = 0;
__attribute__((weak)) int overflowuid = 65534;
__attribute__((weak)) int overflowgid = 65534;

struct resource iomem_resource;

__attribute__((weak)) struct resource *__devm_request_region(
    struct device *dev, struct resource *parent, resource_size_t start,
    resource_size_t n, const char *name) {
  (void)dev;
  (void)parent;
  (void)start;
  (void)n;
  (void)name;
  return NULL;
}

/* ── printk ─────────────────────────────────────────────────────────────── */

__attribute__((weak)) int _printk(const char *fmt, ...) {
  va_list args;

  /* Imported <linux/printk.h> defines printk() as a macro for _printk(), so
   * this stub must not call printk() (that recursed until the stack died).
   * Go straight to the native klog bridge instead.  Linux encodes the log
   * level as "\001<digit>" at the front of the format string; the native
   * console has no levels, so strip it like the native printk() does. */
  if (fmt && fmt[0] == '\001' && fmt[1] != '\0')
    fmt += 2;

  va_start(args, fmt);
  vklogf(fmt, args);
  va_end(args);
  return 0;
}

/* ── string / allocation helpers (upstream lib/string.c, mm/util.c) ─────── */

__attribute__((weak)) void *memchr_inv(const void *start, int c,
                                       size_t bytes) {
  const unsigned char *p = start;
  unsigned char ch = (unsigned char)c;

  while (bytes--) {
    if (*p != ch)
      return (void *)p;
    p++;
  }
  return NULL;
}

__attribute__((weak)) void *memdup_user(const void __user *src, size_t len) {
  void *p = kmalloc(len, GFP_KERNEL);

  if (!p)
    return ERR_PTR(-ENOMEM);
  if (copy_from_user(p, src, len)) {
    kfree(p);
    return ERR_PTR(-EFAULT);
  }
  return p;
}

__attribute__((weak)) char *kstrdup(const char *s, gfp_t gfp) {
  size_t len;
  char *p;

  if (!s)
    return NULL;
  len = strlen(s) + 1;
  p = kmalloc(len, gfp);
  if (p)
    memcpy(p, s, len);
  return p;
}

__attribute__((weak)) const char *kstrdup_const(const char *s, gfp_t gfp) {
  return kstrdup(s, gfp);
}

__attribute__((weak)) void kfree_const(const void *p) { kfree(p); }

__attribute__((weak)) char *kvasprintf(gfp_t gfp, const char *fmt,
                                       va_list ap) {
  char tmp[512];
  va_list copy;

  va_copy(copy, ap);
  vsnprintf(tmp, sizeof(tmp), fmt, copy);
  va_end(copy);
  return kstrdup(tmp, gfp);
}

__attribute__((weak)) char *kasprintf(gfp_t gfp, const char *fmt, ...) {
  va_list ap;
  char *p;

  va_start(ap, fmt);
  p = kvasprintf(gfp, fmt, ap);
  va_end(ap);
  return p;
}

__attribute__((weak)) struct timespec64 ns_to_timespec64(s64 nsec) {
  struct timespec64 ts;

  ts.tv_sec = div_s64_rem(nsec, NSEC_PER_SEC, (s32 *)&ts.tv_nsec);
  if (ts.tv_nsec < 0) {
    ts.tv_nsec += NSEC_PER_SEC;
    ts.tv_sec--;
  }
  return ts;
}

__attribute__((weak)) bool llist_add_batch(struct llist_node *new_first,
                                           struct llist_node *new_last,
                                           struct llist_head *head) {
  struct llist_node *first;

  do {
    first = head->first;
    new_last->next = first;
  } while (!__atomic_compare_exchange_n(&head->first, &first, new_first, false,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED));
  return first != NULL;
}

/* __sw_hweight32()/__sw_hweight64() are implemented in
 * kernel/src/arch/x86_64/hweight.asm, not here: imported x86 code calls them
 * from an inline asm with an empty clobber list and therefore requires the
 * upstream "preserve every register except the result" convention that a C
 * function cannot provide. */

