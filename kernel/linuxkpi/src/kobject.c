/* kobject/sysfs/class support for imported DRM code.
 *
 * sysfs_emit()/show_class_attr_string()/sysfs_streq() are real because
 * attribute show()/store() methods are compiled and exercised.  The
 * sysfs_create_*()/sysfs_remove_*() entry points materialize real nodes when
 * the kobject has a native directory handle (`kobj->sd`, set for class devices
 * by device_add() and for PCI wrappers at wrapper creation) and otherwise
 * accept the registration as a no-op.  Device attribute groups, binary
 * attributes, `is_visible()`/`is_bin_visible()`, named subgroups and
 * per-attribute context release are implemented; see docs/linuxkpi-gaps.md
 * (P5 C5) for the remaining divergences (no kernfs link support, no
 * refcounted kobjects). */

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

/* ── sysfs materialization ──────────────────────────────────────────────── */

/* Attribute contexts live in the native sysfs record and are freed through its
 * release callback when the file is removed.  `kobj->sd` is the opaque native
 * directory handle; kobjects without one (not yet attached to the native tree)
 * accept registrations without creating nodes, as before. */
struct kpi_dev_attr_ctx {
  struct device *dev;
  const struct device_attribute *attr;
};

static void kpi_attr_ctx_free(void *ctx) { kfree(ctx); }

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

/* Create one device attribute under `dir`.  -EEXIST is tolerated as success
 * (re-adding a group that survived), matching the class bridge. */
static int kpi_create_attr_at(struct kobject *kobj, void *dir,
                              const struct attribute *attr) {
  struct kpi_dev_attr_ctx *ctx;
  int ret;

  if (!dir || !attr || !attr->name)
    return 0;

  ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
  if (!ctx)
    return -ENOMEM;
  ctx->dev = kobj_to_dev(kobj);
  ctx->attr = container_of(attr, struct device_attribute, attr);
  ret = asc_sysfs_attr_file(dir, attr->name, attr->mode, ctx,
                            kpi_dev_attr_show, kpi_dev_attr_store,
                            kpi_attr_ctx_free);
  if (ret) {
    kfree(ctx);
    return ret == -17 ? 0 : ret;
  }
  return 0;
}

static void kpi_remove_attr_at(void *dir, const char *name) {
  if (dir && name)
    asc_sysfs_remove(dir, name);
}

/* Binary attribute contexts. */
struct kpi_bin_attr_ctx {
  struct kobject *kobj;
  const struct bin_attribute *attr;
};

static void kpi_bin_ctx_free(void *ctx) { kfree(ctx); }

static int kpi_bin_read(void *ctx, unsigned int offset, char *buf,
                        unsigned int size) {
  struct kpi_bin_attr_ctx *c = ctx;
  ssize_t ret;

  if (!c->attr->read)
    return -5;
  ret = c->attr->read(NULL, c->kobj, (struct bin_attribute *)c->attr, buf,
                      (loff_t)offset, size);
  return (int)ret;
}

static int kpi_bin_write(void *ctx, unsigned int offset, const char *buf,
                         unsigned int size) {
  struct kpi_bin_attr_ctx *c = ctx;
  ssize_t ret;

  if (!c->attr->write)
    return -5;
  ret = c->attr->write(NULL, c->kobj, (struct bin_attribute *)c->attr,
                       (char *)buf, (loff_t)offset, size);
  return (int)ret;
}

static int kpi_create_bin_at(struct kobject *kobj, void *dir,
                             const struct bin_attribute *attr) {
  struct kpi_bin_attr_ctx *ctx;
  int ret;

  if (!dir || !attr || !attr->attr.name)
    return 0;

  ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
  if (!ctx)
    return -ENOMEM;
  ctx->kobj = kobj;
  ctx->attr = attr;
  ret = asc_sysfs_bin_file(dir, attr->attr.name, attr->attr.mode, ctx,
                           kpi_bin_read, kpi_bin_write, kpi_bin_ctx_free);
  if (ret) {
    kfree(ctx);
    return ret == -17 ? 0 : ret;
  }
  return 0;
}

/* is_visible()/is_bin_visible() are honored; on failure the attributes added
 * so far in the group are rolled back by name. */
static int kpi_create_group_at(struct kobject *kobj, void *dir,
                               const struct attribute_group *grp) {
  int i, j, ret;

  if (!dir || !grp)
    return 0;

  if (grp->attrs) {
    for (i = 0; grp->attrs[i]; i++) {
      const struct attribute *attr = grp->attrs[i];

      if (grp->is_visible &&
          !grp->is_visible(kobj, (struct attribute *)attr, i))
        continue;
      ret = kpi_create_attr_at(kobj, dir, attr);
      if (ret) {
        for (j = 0; j < i; j++) {
          if (!grp->attrs[j])
            continue;
          if (grp->is_visible &&
              !grp->is_visible(kobj, (struct attribute *)grp->attrs[j], j))
            continue;
          kpi_remove_attr_at(dir, grp->attrs[j]->name);
        }
        return ret;
      }
    }
  }

  if (grp->bin_attrs) {
    for (i = 0; grp->bin_attrs[i]; i++) {
      const struct bin_attribute *bin = grp->bin_attrs[i];

      if (grp->is_bin_visible &&
          !grp->is_bin_visible(kobj, (struct bin_attribute *)bin, i))
        continue;
      ret = kpi_create_bin_at(kobj, dir, bin);
      if (ret) {
        for (j = 0; j < i; j++) {
          if (grp->bin_attrs[j])
            kpi_remove_attr_at(dir, grp->bin_attrs[j]->attr.name);
        }
        return ret;
      }
    }
  }

  if (grp->groups) {
    for (i = 0; grp->groups[i]; i++) {
      const struct attribute_group *sub = grp->groups[i];
      void *subdir = dir;

      if (sub->name) {
        subdir = asc_sysfs_device_dir(dir, sub->name);
        if (!subdir)
          return -ENOMEM;
      }
      ret = kpi_create_group_at(kobj, subdir, sub);
      if (ret)
        return ret;
    }
  }
  return 0;
}

static void kpi_remove_group_at(struct kobject *kobj, void *dir,
                                const struct attribute_group *grp) {
  int i;

  if (!dir || !grp)
    return;

  if (grp->attrs) {
    for (i = 0; grp->attrs[i]; i++) {
      if (grp->is_visible &&
          !grp->is_visible(kobj, (struct attribute *)grp->attrs[i], i))
        continue;
      kpi_remove_attr_at(dir, grp->attrs[i]->name);
    }
  }
  if (grp->bin_attrs) {
    for (i = 0; grp->bin_attrs[i]; i++) {
      if (grp->is_bin_visible &&
          !grp->is_bin_visible(kobj, (struct bin_attribute *)grp->bin_attrs[i],
                               i))
        continue;
      kpi_remove_attr_at(dir, grp->bin_attrs[i]->attr.name);
    }
  }
  if (grp->groups) {
    for (i = 0; grp->groups[i]; i++) {
      const struct attribute_group *sub = grp->groups[i];

      if (sub->name)
        kpi_remove_attr_at(dir, sub->name);
      else
        kpi_remove_group_at(kobj, dir, sub);
    }
  }
}

int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr,
                         const void *ns) {
  (void)ns;
  if (!kobj || !attr)
    return -EINVAL;
  return kpi_create_attr_at(kobj, kobj->sd, attr);
}

int sysfs_create_file(struct kobject *kobj, const struct attribute *attr) {
  return sysfs_create_file_ns(kobj, attr, NULL);
}

void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr,
                          const void *ns) {
  (void)ns;
  if (kobj && attr)
    kpi_remove_attr_at(kobj->sd, attr->name);
}

void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr) {
  sysfs_remove_file_ns(kobj, attr, NULL);
}

int sysfs_create_files(struct kobject *kobj,
                       const struct attribute *const *attr) {
  int i;

  if (!kobj || !attr)
    return -EINVAL;
  for (i = 0; attr[i]; i++) {
    int ret = kpi_create_attr_at(kobj, kobj->sd, attr[i]);

    if (ret) {
      while (--i >= 0)
        kpi_remove_attr_at(kobj->sd, attr[i]->name);
      return ret;
    }
  }
  return 0;
}

int sysfs_create_group(struct kobject *kobj,
                       const struct attribute_group *grp) {
  void *dir;

  if (!kobj || !grp)
    return -EINVAL;
  dir = kobj->sd;
  if (!dir)
    return 0;
  if (grp->name) {
    dir = asc_sysfs_device_dir(dir, grp->name);
    if (!dir)
      return -ENOMEM;
  }
  return kpi_create_group_at(kobj, dir, grp);
}

int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups) {
  int i;

  if (!kobj || !groups)
    return -EINVAL;
  for (i = 0; groups[i]; i++) {
    int ret = sysfs_create_group(kobj, groups[i]);

    if (ret) {
      while (--i >= 0)
        sysfs_remove_group(kobj, groups[i]);
      return ret;
    }
  }
  return 0;
}

void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp) {
  if (!kobj || !grp || !kobj->sd)
    return;
  if (grp->name)
    kpi_remove_attr_at(kobj->sd, grp->name);
  else
    kpi_remove_group_at(kobj, kobj->sd, grp);
}

void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups) {
  int i;

  if (!kobj || !groups)
    return;
  for (i = 0; groups[i]; i++)
    sysfs_remove_group(kobj, groups[i]);
}

int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr) {
  if (!kobj || !attr)
    return -EINVAL;
  return kpi_create_bin_at(kobj, kobj->sd, attr);
}

void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr) {
  if (kobj && attr)
    kpi_remove_attr_at(kobj->sd, attr->attr.name);
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

/* ── device groups ──────────────────────────────────────────────────────── */

int device_add_groups(struct device *dev, const struct attribute_group **groups) {
  int i;

  if (!dev || !groups)
    return 0;
  for (i = 0; groups[i]; i++) {
    int ret = sysfs_create_group(&dev->kobj, groups[i]);

    if (ret) {
      while (--i >= 0)
        sysfs_remove_group(&dev->kobj, groups[i]);
      return ret;
    }
  }
  return 0;
}

void device_remove_groups(struct device *dev,
                          const struct attribute_group **groups) {
  int i;

  if (!dev || !groups)
    return;
  for (i = 0; groups[i]; i++)
    sysfs_remove_group(&dev->kobj, groups[i]);
}

/* Stock device.h entry points over a device's kobject. */
int device_create_file(struct device *dev,
                       const struct device_attribute *attr) {
  if (!dev || !attr)
    return -EINVAL;
  return sysfs_create_file(&dev->kobj, &attr->attr);
}

void device_remove_file(struct device *dev,
                        const struct device_attribute *attr) {
  if (dev && attr)
    sysfs_remove_file(&dev->kobj, &attr->attr);
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

void kobject_init(struct kobject *kobj, const struct kobj_type *ktype) {
  if (!kobj)
    return;
  kobj->ktype = ktype;
  INIT_LIST_HEAD(&kobj->entry);
}

int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt,
                ...) {
  va_list args;

  if (!kobj)
    return -EINVAL;
  kobj->parent = parent;
  va_start(args, fmt);
  vsnprintf(kobj->kpi_name, sizeof(kobj->kpi_name), fmt, args);
  va_end(args);
  kobj->name = kobj->kpi_name;
  return 0;
}

/* Upstream lib/kobject.c provides kobj_sysfs_ops for kernfs-backed kobjects;
 * AvoryOS kobjects are shapes, so the vtable is present but unused. */
const struct sysfs_ops kobj_sysfs_ops = {NULL, NULL};

void sysfs_remove_files(struct kobject *kobj,
                        const struct attribute *const *ptr) {
  int i;

  if (!kobj || !ptr)
    return;
  for (i = 0; ptr[i]; i++)
    sysfs_remove_file(kobj, ptr[i]);
}

/* Group-relative file registration: the native sysfs bridge places files on
 * the kobject directory, so the group name is not modeled (documented). */
int sysfs_add_file_to_group(struct kobject *kobj, const struct attribute *attr,
                            const char *group) {
  (void)group;
  return sysfs_create_file(kobj, attr);
}

void sysfs_remove_file_from_group(struct kobject *kobj,
                                  const struct attribute *attr,
                                  const char *group) {
  (void)group;
  sysfs_remove_file(kobj, attr);
}

/* Managed group add/remove: the add is a plain sysfs_create_group and the
 * remove is available explicitly; devres auto-removal is not wired because
 * the native sysfs bridge owns attribute lifetimes (P5 C5 divergence). */
int devm_device_add_group(struct device *dev,
                          const struct attribute_group *grp) {
  if (!dev || !grp)
    return -EINVAL;
  return sysfs_create_group(&dev->kobj, grp);
}

void devm_device_remove_group(struct device *dev,
                              const struct attribute_group *grp) {
  if (dev && grp)
    sysfs_remove_group(&dev->kobj, grp);
}

void kobject_del(struct kobject *kobj) {
  (void)kobj;
}

void kobject_put(struct kobject *kobj) {
  (void)kobj;
}

struct kobject *kobject_get(struct kobject *kobj) { return kobj; }

int kobject_set_name(struct kobject *kobj, const char *fmt, ...) {
  va_list args;

  if (!kobj)
    return -EINVAL;
  va_start(args, fmt);
  vsnprintf(kobj->kpi_name, sizeof(kobj->kpi_name), fmt, args);
  va_end(args);
  kobj->name = kobj->kpi_name;
  return 0;
}

/* kset bookkeeping: see the struct comment in the overlay.  amdgpu's
 * discovery sysfs tree is created with these; nothing is materialized in the
 * native sysfs tree, so registration only prepares the child list. */
int kset_register(struct kset *kset) {
  if (!kset)
    return -EINVAL;
  INIT_LIST_HEAD(&kset->list);
  spin_lock_init(&kset->list_lock);
  return 0;
}

void kset_unregister(struct kset *kset) {
  if (kset)
    INIT_LIST_HEAD(&kset->list);
}

struct kset *kset_create_and_add(const char *name,
                                 const struct kset_uevent_ops *uevent_ops,
                                 struct kobject *parent) {
  struct kset *kset = kzalloc(sizeof(*kset), GFP_KERNEL);

  if (!kset)
    return NULL;
  kset->uevent_ops = uevent_ops;
  kset->kobj.parent = parent;
  kset->kobj.name = name;
  if (kset_register(kset)) {
    kfree(kset);
    return NULL;
  }
  return kset;
}

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
                            kpi_class_attr_show, kpi_class_attr_store,
                            kpi_attr_ctx_free);
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
