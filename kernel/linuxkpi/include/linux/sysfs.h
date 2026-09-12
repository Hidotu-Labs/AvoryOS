#ifndef __AVORY_LINUXKPI_SYSFS_H
#define __AVORY_LINUXKPI_SYSFS_H

/* AvoryOS overlay for <linux/sysfs.h>.
 *
 * Imported DRM code defines device/class attributes and attribute groups;
 * the native sysfs tree does not accept dynamic nodes yet, so the create/
 * remove entry points in linuxkpi/src/kobject.c are bookkeeping stubs.  The
 * attribute macros and the sysfs_emit() formatting helpers are real, because
 * show() methods are compiled and may be called from tests. */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/stringify.h>
#include <linux/types.h>

struct file;
struct device;
struct class;
struct bin_attribute;
struct vm_area_struct;

struct attribute {
  const char *name;
  umode_t mode;
};

struct attribute_group {
  const char *name;
  umode_t (*is_visible)(struct kobject *, struct attribute *, int);
  umode_t (*is_bin_visible)(struct kobject *, struct bin_attribute *, int);
  struct attribute **attrs;
  struct bin_attribute **bin_attrs;
  const struct attribute_group **groups;
};

struct bin_attribute {
  struct attribute attr;
  size_t size;
  void *private;
  ssize_t (*read)(struct file *, struct kobject *, struct bin_attribute *,
                  char *, loff_t, size_t);
  ssize_t (*write)(struct file *, struct kobject *, struct bin_attribute *,
                   char *, loff_t, size_t);
  int (*mmap)(struct file *, struct kobject *, struct bin_attribute *,
              struct vm_area_struct *);
};

struct device_attribute {
  struct attribute attr;
  ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
  ssize_t (*store)(struct device *dev, struct device_attribute *attr,
                   const char *buf, size_t count);
};

#define __ATTR(_name, _mode, _show, _store)                                   \
  {                                                                           \
    .attr = {.name = __stringify(_name), .mode = (_mode)},                    \
    .show = (_show), .store = (_store),                                       \
  }
#define __ATTR_RO(_name)                                                      \
  {                                                                           \
    .attr = {.name = __stringify(_name), .mode = 0444}, .show = _name##_show, \
  }
#define __ATTR_RW(_name)                                                      \
  {                                                                           \
    .attr = {.name = __stringify(_name), .mode = 0644},                       \
    .show = _name##_show, .store = _name##_store,                             \
  }
#define __ATTR_WO(_name)                                                      \
  {                                                                           \
    .attr = {.name = __stringify(_name), .mode = 0200},                       \
    .store = _name##_store,                                                   \
  }

#define DEVICE_ATTR(_name, _mode, _show, _store)                              \
  struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name)                                                 \
  struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_RW(_name)                                                 \
  struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_WO(_name)                                                 \
  struct device_attribute dev_attr_##_name = __ATTR_WO(_name)

int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr,
                         const void *ns);
int sysfs_create_file(struct kobject *kobj, const struct attribute *attr);
void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr,
                          const void *ns);
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);

int sysfs_create_files(struct kobject *kobj, const struct attribute *const *attr);
int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp);
int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups);
void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp);
void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups);

int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr);
void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr);

int sysfs_create_link(struct kobject *kobj, struct kobject *target,
                      const char *name);
void sysfs_remove_link(struct kobject *kobj, const char *name);
int sysfs_create_link_nowarn(struct kobject *kobj, struct kobject *target,
                             const char *name);

int sysfs_emit(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline bool sysfs_attr_mode_is_visible(umode_t mode) { return mode != 0; }

#endif /* __AVORY_LINUXKPI_SYSFS_H */
