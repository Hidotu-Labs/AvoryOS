#ifndef __AVORY_LINUXKPI_KOBJECT_H
#define __AVORY_LINUXKPI_KOBJECT_H

/* AvoryOS overlay for <linux/kobject.h>.
 *
 * A kobject here is a small named object; it is what sysfs_create_*() would
 * attach to.  The native kernel has its own sysfs tree, so the sysfs bridge
 * (linuxkpi/src/kobject.c) currently accepts registrations without creating
 * nodes; /sys/class/drm/cardN appears when the native sysfs grows dynamic
 * registration.  kobject_uevent() is routed to the native netlink uevent
 * broadcast where possible. */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>

/* of.h reaches this header before any numa header; keep the constant
 * available for of_node_to_nid() and friends. */
#ifndef NUMA_NO_NODE
#define NUMA_NO_NODE (-1)
#endif

struct kobject;
struct kset;
struct kobj_type;
struct kernfs_node;
struct attribute_group;
struct attribute;
struct sysfs_ops;
struct kset_uevent_ops;
struct device;

struct kobject {
  const char *name;
  struct list_head entry;
  struct kobject *parent;
  struct kset *kset;
  const struct kobj_type *ktype;
  struct kernfs_node *sd;
  unsigned int flags;
  char kpi_name[48]; /* Avory: backing store for kobject_init_and_add names */
};

/* Upstream kobj_type.  amdgpu's discovery/xgmi code initializes .release and
 * .sysfs_ops; the AvoryOS sysfs bridge uses the device_attribute path and
 * ignores ktype->sysfs_ops, so these fields are shape-complete, not wired. */
struct kobj_type {
  void (*release)(struct kobject *kobj);
  const struct sysfs_ops *sysfs_ops;
  const struct attribute_group **default_groups;
};

/* Minimal kset: a kobject plus a child list.  Registration is bookkeeping
 * only (the native sysfs tree owns the real hierarchy); list_empty(&kset->list)
 * is still meaningful to callers that check whether children were added. */
struct kset {
  struct list_head list;
  spinlock_t list_lock;
  struct kobject kobj;
  const struct kset_uevent_ops *uevent_ops;
};

#define to_kset(obj) container_of(obj, struct kset, kobj)

#define KOBJ_NAME_LEN 20

enum kobject_action {
  KOBJ_ADD,
  KOBJ_REMOVE,
  KOBJ_CHANGE,
  KOBJ_MOVE,
  KOBJ_ONLINE,
  KOBJ_OFFLINE,
  KOBJ_BIND,
  KOBJ_UNBIND,
  KOBJ_MAX
};

#define UEVENT_HELPER_PATH_LEN 256
#define UEVENT_NUM_ENVP 64
#define UEVENT_BUFFER_SIZE 2048

struct kobj_uevent_env {
  char *argv[3];
  char *envp[UEVENT_NUM_ENVP];
  int envp_idx;
  char buf[UEVENT_BUFFER_SIZE];
  int buflen;
};

static inline const char *kobject_name(const struct kobject *kobj) {
  return kobj ? kobj->name : NULL;
}

int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
                         struct kobject *parent, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt,
                ...) __attribute__((format(printf, 3, 4)));
void kobject_del(struct kobject *kobj);
void kobject_put(struct kobject *kobj);
struct kobject *kobject_get(struct kobject *kobj);

int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

extern const struct sysfs_ops kobj_sysfs_ops;

void sysfs_remove_files(struct kobject *kobj, const struct attribute *const *ptr);
int devm_device_add_group(struct device *dev, const struct attribute_group *grp);
void devm_device_remove_group(struct device *dev,
                              const struct attribute_group *grp);

int kset_register(struct kset *kset);
void kset_unregister(struct kset *kset);
struct kset *kset_create_and_add(const char *name,
                                 const struct kset_uevent_ops *uevent_ops,
                                 struct kobject *parent);

int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[]);

int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* __AVORY_LINUXKPI_KOBJECT_H */
