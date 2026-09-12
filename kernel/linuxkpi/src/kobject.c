/* Minimal kobject/sysfs/class support for imported DRM code.
 *
 * The native kernel owns a static sysfs tree; it cannot yet attach arbitrary
 * dynamic groups.  The entry points here therefore validate their arguments,
 * remember nothing, and succeed, so drivers can complete probe and expose
 * their /dev nodes.  sysfs_emit()/show_class_attr_string()/sysfs_streq() are
 * real because attribute show() methods are compiled and exercised.
 *
 * Gap record: docs/linuxkpi-gaps.md, Phase 3. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <linuxkpi/native_sysfs.h>

/* ── attribute formatting ───────────────────────────────────────────────── */

int sysfs_emit(char *buf, const char *fmt, ...) {
  va_list args;
  int ret;

  if (!buf)
    return -EINVAL;
  va_start(args, fmt);
  ret = vsnprintf(buf, PAGE_SIZE, fmt, args);
  va_end(args);
  return ret;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...) {
  va_list args;
  int ret;

  if (!buf || at < 0 || at >= (int)PAGE_SIZE)
    return -EINVAL;
  va_start(args, fmt);
  ret = vsnprintf(buf + at, PAGE_SIZE - (size_t)at, fmt, args);
  va_end(args);
  return ret;
}

ssize_t show_class_attr_string(const struct class *class,
                               const struct class_attribute *attr, char *buf) {
  const struct class_attribute_string *cs =
      container_of(attr, struct class_attribute_string, attr);

  (void)class;
  if (!cs->str)
    return 0;
  return (ssize_t)sysfs_emit(buf, "%s\n", cs->str);
}

/* ── sysfs registration stubs ───────────────────────────────────────────── */

int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr,
                         const void *ns) {
  (void)ns;
  if (!kobj || !attr)
    return -EINVAL;
  return 0;
}

int sysfs_create_file(struct kobject *kobj, const struct attribute *attr) {
  return sysfs_create_file_ns(kobj, attr, NULL);
}

void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr,
                          const void *ns) {
  (void)kobj;
  (void)attr;
  (void)ns;
}

void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr) {
  sysfs_remove_file_ns(kobj, attr, NULL);
}

int sysfs_create_files(struct kobject *kobj,
                       const struct attribute *const *attr) {
  (void)attr;
  return kobj ? 0 : -EINVAL;
}

int sysfs_create_group(struct kobject *kobj,
                       const struct attribute_group *grp) {
  (void)grp;
  return kobj ? 0 : -EINVAL;
}

int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups) {
  (void)groups;
  return kobj ? 0 : -EINVAL;
}

void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp) {
  (void)kobj;
  (void)grp;
}

void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups) {
  (void)kobj;
  (void)groups;
}

int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr) {
  if (!kobj || !attr)
    return -EINVAL;
  return 0;
}

void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr) {
  (void)kobj;
  (void)attr;
}

int sysfs_create_link(struct kobject *kobj, struct kobject *target,
                      const char *name) {
  if (!kobj || !target || !name)
    return -EINVAL;
  return 0;
}

void sysfs_remove_link(struct kobject *kobj, const char *name) {
  (void)kobj;
  (void)name;
}

int sysfs_create_link_nowarn(struct kobject *kobj, struct kobject *target,
                             const char *name) {
  return sysfs_create_link(kobj, target, name);
}

/* ── attribute materialization ──────────────────────────────────────────── */

/* Device attribute groups (DRM connectors use these) are real: the native
 * file's read/write calls back into the attribute's show/store with the
 * owning device. */
struct kpi_dev_attr_ctx {
  struct device *dev;
  const struct device_attribute *attr;
};

static int kpi_dev_attr_show(void *ctx, char *buf, unsigned int size) {
  struct kpi_dev_attr_ctx *c = ctx;

  (void)size;
  if (!c->attr->show)
    return -5; /* EIO */
  return (int)c->attr->show(c->dev, (struct device_attribute *)c->attr, buf);
}

static int kpi_dev_attr_store(void *ctx, const char *buf, unsigned int size) {
  struct kpi_dev_attr_ctx *c = ctx;

  if (!c->attr->store)
    return -5;
  return (int)c->attr->store(c->dev, (struct device_attribute *)c->attr, buf,
                             size);
}

int device_add_groups(struct device *dev, const struct attribute_group **groups) {
  if (!dev || !dev->kobj.sd || !groups)
    return 0;

  for (int g = 0; groups[g]; g++) {
    const struct attribute_group *grp = groups[g];

    if (!grp->attrs)
      continue;
    for (int i = 0; grp->attrs[i]; i++) {
      const struct attribute *attr = grp->attrs[i];
      struct kpi_dev_attr_ctx *ctx;

      if (grp->is_visible &&
          !grp->is_visible(&dev->kobj, (struct attribute *)attr, i))
        continue;

      ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
      if (!ctx)
        return -ENOMEM;
      ctx->dev = dev;
      ctx->attr = container_of(attr, struct device_attribute, attr);
      if (asc_sysfs_attr_file(dev->kobj.sd, attr->name, attr->mode, ctx,
                              kpi_dev_attr_show, kpi_dev_attr_store))
        kfree(ctx);
    }
  }
  return 0;
}

void device_remove_groups(struct device *dev,
                          const struct attribute_group **groups) {
  (void)dev;
  (void)groups;
}

/* ── kobjects ───────────────────────────────────────────────────────────── */

int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
                         struct kobject *parent, const char *fmt, ...) {
  va_list args;

  if (!kobj)
    return -EINVAL;
  kobj->ktype = ktype;
  kobj->parent = parent;
  va_start(args, fmt);
  vsnprintf(kobj->kpi_name, sizeof(kobj->kpi_name), fmt, args);
  va_end(args);
  kobj->name = kobj->kpi_name;
  return 0;
}

void kobject_del(struct kobject *kobj) {
  (void)kobj;
}

void kobject_put(struct kobject *kobj) {
  (void)kobj;
}

struct kobject *kobject_get(struct kobject *kobj) { return kobj; }

/* ── uevents ────────────────────────────────────────────────────────────── */

int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...) {
  va_list args;
  int len;

  if (!env || env->buflen >= (int)sizeof(env->buf))
    return -ENOMEM;
  if (env->envp_idx >= UEVENT_NUM_ENVP - 1)
    return -ENOMEM;

  va_start(args, format);
  len = vsnprintf(env->buf + env->buflen,
                  sizeof(env->buf) - (size_t)env->buflen, format, args);
  va_end(args);
  if (len < 0)
    return len;
  if (env->buflen + len >= (int)sizeof(env->buf))
    return -ENOMEM;

  env->envp[env->envp_idx] = env->buf + env->buflen;
  env->envp_idx++;
  env->buflen += len + 1;
  return 0;
}

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[]) {
  /* The native netlink uevent broadcast is wired for the native DRM card;
   * the Linux DRM class bridge reuses it when device nodes land.  For now
   * the event is accepted and dropped (documented gap). */
  (void)kobj;
  (void)action;
  (void)envp;
  return 0;
}

int kobject_uevent(struct kobject *kobj, enum kobject_action action) {
  return kobject_uevent_env(kobj, action, NULL);
}

/* ── classes ────────────────────────────────────────────────────────────── */

/* class_create/class_destroy live in linuxkpi/src/device.c. */

/* Class attributes (DRM's "version") are materialized under the class dir. */
struct kpi_class_attr_ctx {
  const struct class *class;
  const struct class_attribute *attr;
};

static int kpi_class_attr_show(void *ctx, char *buf, unsigned int size) {
  struct kpi_class_attr_ctx *c = ctx;

  (void)size;
  if (!c->attr->show)
    return -5;
  return (int)c->attr->show(c->class, (struct class_attribute *)c->attr, buf);
}

static int kpi_class_attr_store(void *ctx, const char *buf, unsigned int size) {
  struct kpi_class_attr_ctx *c = ctx;

  if (!c->attr->store)
    return -5;
  return (int)c->attr->store(c->class, (struct class_attribute *)c->attr, buf,
                             size);
}

int class_create_file_ns(const struct class *class,
                         const struct class_attribute *attr, const void *ns) {
  struct kpi_class_attr_ctx *ctx;
  void *dir;
  int ret;

  (void)ns;
  if (!class || !attr)
    return -EINVAL;

  dir = class->kpi_dir;
  if (!dir && class->name)
    dir = asc_sysfs_class_dir(class->name);
  if (!dir)
    return 0; /* sysfs not up yet: accept without a node */

  ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
  if (!ctx)
    return -ENOMEM;
  ctx->class = class;
  ctx->attr = attr;
  ret = asc_sysfs_attr_file(dir, attr->attr.name, attr->attr.mode, ctx,
                            kpi_class_attr_show, kpi_class_attr_store);
  if (ret) {
    kfree(ctx);
    return ret == -17 ? 0 : ret;
  }
  return 0;
}

void class_remove_file_ns(const struct class *class,
                          const struct class_attribute *attr, const void *ns) {
  (void)ns;
  if (!class || !attr)
    return;
  if (class->kpi_dir)
    asc_sysfs_remove(class->kpi_dir, attr->attr.name);
}
