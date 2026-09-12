#ifndef __AVORY_LINUXKPI_DEVICE_CLASS_H
#define __AVORY_LINUXKPI_DEVICE_CLASS_H

/* AvoryOS overlay for <linux/device/class.h> (6.6).
 *
 * The class model is bookkeeping: class_create()/class_destroy() register a
 * name with the native device manager, and class attributes (the DRM version
 * attribute) are accepted without creating sysfs nodes. */

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/types.h>

struct device;
struct class;
struct class_interface;

struct class {
  const char *name;
  const struct attribute_group **dev_groups;
  char *(*devnode)(const struct device *dev, umode_t *mode);
  struct kobject *dev_kobj;
  struct device *(*get_device)(struct device *dev);
  void *kpi_dir; /* native /sys/class/<name> vfs_node_t (Avory) */
};

struct class_attribute {
  struct attribute attr;
  ssize_t (*show)(const struct class *class, const struct class_attribute *attr,
                  char *buf);
  ssize_t (*store)(const struct class *class,
                   const struct class_attribute *attr, const char *buf,
                   size_t count);
};

#define CLASS_ATTR_RW(_name)                                                  \
  struct class_attribute class_attr_##_name = __ATTR_RW(_name)
#define CLASS_ATTR_RO(_name)                                                  \
  struct class_attribute class_attr_##_name = __ATTR_RO(_name)
#define CLASS_ATTR_WO(_name)                                                  \
  struct class_attribute class_attr_##_name = __ATTR_WO(_name)

struct class_attribute_string {
  struct class_attribute attr;
  char *str;
};

#define _CLASS_ATTR_STRING(_name, _mode, _str)                                 \
  {__ATTR(_name, _mode, show_class_attr_string, NULL), _str}
#define CLASS_ATTR_STRING(_name, _mode, _str)                                  \
  struct class_attribute_string class_attr_##_name =                           \
      _CLASS_ATTR_STRING(_name, _mode, _str)

ssize_t show_class_attr_string(const struct class *class,
                               const struct class_attribute *attr, char *buf);

int __must_check class_create_file_ns(const struct class *class,
                                      const struct class_attribute *attr,
                                      const void *ns);
void class_remove_file_ns(const struct class *class,
                          const struct class_attribute *attr, const void *ns);

static inline int __must_check class_create_file(
    const struct class *class, const struct class_attribute *attr) {
  return class_create_file_ns(class, attr, NULL);
}

static inline void class_remove_file(const struct class *class,
                                     const struct class_attribute *attr) {
  class_remove_file_ns(class, attr, NULL);
}

struct class * __must_check class_create(const char *name);
void class_destroy(const struct class *cls);

int class_register(const struct class *cls);
void class_unregister(const struct class *cls);

#endif /* __AVORY_LINUXKPI_DEVICE_CLASS_H */
