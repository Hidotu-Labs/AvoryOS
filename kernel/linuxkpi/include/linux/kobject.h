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
void kobject_del(struct kobject *kobj);
void kobject_put(struct kobject *kobj);
struct kobject *kobject_get(struct kobject *kobj);

int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[]);

int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* __AVORY_LINUXKPI_KOBJECT_H */
