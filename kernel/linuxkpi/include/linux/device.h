#ifndef __AVORY_LINUXKPI_DEVICE_H
#define __AVORY_LINUXKPI_DEVICE_H

/* Minimal Linux <linux/device.h> overlay.
 *
 * Devices are plain kernel objects here: identity, driver data, a devres
 * action list, and the printk helpers drivers use.  Bus matching, sysfs,
 * reference counting and power management are stubs until the driver-core
 * phase.  devm_* resources are real (tracked per device and released on
 * device_unregister/devres_release_all); devm_ioremap/devm_request_irq only
 * become functional with the Phase 2 VMAP/IRQ layers.
 * Implementation: linuxkpi/src/device.c. */

#include <asm/device.h>
#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/cleanup.h>
#include <linux/device/class.h>
#include <linux/kern_levels.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/irqreturn.h>
#include <linux/pm.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

struct device_node;
struct class;
struct attribute_group;
struct fwnode_handle;
struct kobj_uevent_env;
struct dev_pm_ops;
struct device_driver;

/* Minimal bus_type: the bus shims (platform.c, pci.c) match synchronously and
 * only need the name.  Upstream's <linux/device/bus.h> is deliberately not
 * pulled in because it redefines pm_message_t, which this overlay provides
 * (see below).  Recorded in docs/linuxkpi-gaps.md. */
struct bus_type {
  const char *name;
  const struct attribute_group **dev_groups;
  const struct dev_pm_ops *pm;
  int (*match)(struct device *, const struct device_driver *);
  int (*uevent)(const struct device *, struct kobj_uevent_env *);
};

#ifndef NUMA_NO_NODE
#define NUMA_NO_NODE (-1)
#endif

struct device_dma_parameters {
  unsigned int max_segment_size;
  unsigned long segment_boundary_mask;
};

/* Per-device runtime PM and wakeup state (CONFIG_PM enabled; CONFIG_PM_SLEEP
 * stays off).  Fields match the upstream <linux/pm_runtime.h> inline contract
 * and linuxkpi/src/pm_runtime.c state machine. */
struct dev_pm_info {
  spinlock_t lock;
  atomic_t usage_count;
  unsigned int disable_depth;
  enum rpm_status runtime_status;
  enum rpm_status last_status;
  enum rpm_request request;
  int runtime_error;
  int autosuspend_delay;
  u64 last_busy;
  u64 accounting_timestamp;
  u64 suspended_ns;
  bool can_wakeup : 1;
  bool should_wakeup : 1;
  bool ignore_children : 1;
  bool no_callbacks : 1;
  bool irq_safe : 1;
  bool runtime_auto : 1;
  bool use_autosuspend : 1;
  bool pm_initialized : 1;
  struct work_struct work;
  struct delayed_work autosuspend_work;
  wait_queue_head_t wait_queue;
};

/* pm_message_t and struct dev_pm_ops come from the <linux/pm.h> overlay;
 * with CONFIG_PM/CONFIG_PM_SLEEP unset the SET_*_PM_OPS helpers there
 * compile the callbacks out exactly like upstream's #else arms. */

/* Minimal <linux/device/class.h> shape.  devnode() is what drm_sysfs uses to
 * name the /dev/dri/cardN node; uevent is unused so far. */
struct device_type {
  const char *name;
  const struct attribute_group **groups;
  int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
  char *(*devnode)(const struct device *dev, umode_t *mode);
  void (*release)(struct device *dev);
  const struct dev_pm_ops *pm;
};

/* A driver-core device.  The struct is real (embedded by pci_dev etc.) but
 * bus matching, sysfs nodes and power management are stubs; imported DRM
 * drivers mostly pass it through.  Unlike upstream, of_node is always NULL
 * (the platform is x86 with a native device tree shim, not DT). */
struct device {
  struct kobject kobj; /* real kobject so sysfs_create_* can attach nodes */
  const char *init_name;
  struct device *parent;
  void *driver_data;
  void (*release)(struct device *dev);
  struct device_driver *driver;
  const struct bus_type *bus;
  struct device_node *of_node;
  struct fwnode_handle *fwnode;
  void *platform_data; /* legacy platform bus data (simpledrm) */
  struct device_type *type;
  struct class *class;
  const struct attribute_group **groups;
  dev_t devt;
  u64 dma_mask;          /* DMA addressing limit (linuxkpi/dma-mapping.c) */
  u64 coherent_dma_mask; /* coherent allocation addressing limit           */
  struct device_dma_parameters dma_parms;
  struct dev_pm_info power;
  struct dev_pm_domain *pm_domain;
  bool removable; /* dev_is_removable() consumers (stock device.h) */
  struct list_head devres_head;
  spinlock_t devres_lock;
  struct list_head kpi_list; /* global created-device list (device_destroy) */
  struct list_head kpi_bus_node;
  struct list_head kpi_class_node;
  unsigned int kpi_refs;
  char kpi_name[48];
};

/* Defined here (not in driver.h) because upstream <linux/device.h> provides
 * it through device/driver.h, and both pci.h and driver.h rely on that. */
enum probe_type {
  PROBE_DEFAULT_STRATEGY,
  PROBE_PREFER_ASYNCHRONOUS,
  PROBE_FORCE_SYNCHRONOUS,
};

struct device_driver {
  struct module *owner;
  const char *name;
  const struct bus_type *bus;
  const struct of_device_id *of_match_table;
  const struct acpi_device_id *acpi_match_table;
  const struct attribute_group **dev_groups;
  const struct dev_pm_ops *pm;
  int (*probe)(struct device *dev);
  int (*remove)(struct device *dev);
  enum probe_type probe_type;
  void (*shutdown)(struct device *dev);
};

static inline const char *dev_driver_string(const struct device *dev) {
  if (dev && dev->driver && dev->driver->name)
    return dev->driver->name;
  return "none";
}

static inline const char *dev_name(const struct device *dev) {
  if (!dev)
    return "device";
  return dev->init_name ? dev->init_name : "device";
}

static inline void *dev_get_platdata(const struct device *dev) {
  return dev ? dev->platform_data : NULL;
}

/* Upstream linux/device/driver.h: registers a driver and its module init/exit
 * pair.  In a static kernel the exit half compiles to nothing. */
#define module_driver(__driver, __register, __unregister, ...)                 \
  static int __init __driver##_init(void) {                                   \
    return __register(&(__driver), ##__VA_ARGS__);                            \
  }                                                                           \
  module_init(__driver##_init);                                               \
  static void __exit __driver##_exit(void) {                                  \
    __unregister(&(__driver), ##__VA_ARGS__);                                 \
  }                                                                           \
  module_exit(__driver##_exit)

static inline struct device *kobj_to_dev(struct kobject *kobj) {
  return container_of(kobj, struct device, kobj);
}

/* <linux/property.h> (included earlier through of.h in some TUs) defines a
 * _Generic-based dev_fwnode() macro; only provide the inline when that macro
 * is absent, so either include order works. */
#ifndef dev_fwnode
static inline struct fwnode_handle *dev_fwnode(const struct device *dev) {
  return dev ? dev->fwnode : NULL;
}
#endif

static inline int dev_to_node(struct device *dev) {
  (void)dev;
  return NUMA_NO_NODE;
}

/* _once() variants: emitted once per call site, like upstream. */
#define dev_err_once(dev, fmt, ...)                                           \
  do {                                                                        \
    static bool __warned;                                                      \
    if (!__warned) {                                                           \
      __warned = true;                                                         \
      dev_err(dev, fmt, ##__VA_ARGS__);                                        \
    }                                                                          \
  } while (0)
#define dev_warn_once(dev, fmt, ...)                                          \
  do {                                                                        \
    static bool __warned;                                                      \
    if (!__warned) {                                                           \
      __warned = true;                                                         \
      dev_warn(dev, fmt, ##__VA_ARGS__);                                       \
    }                                                                          \
  } while (0)
#define dev_info_once(dev, fmt, ...)                                          \
  do {                                                                        \
    static bool __warned;                                                      \
    if (!__warned) {                                                           \
      __warned = true;                                                         \
      dev_info(dev, fmt, ##__VA_ARGS__);                                       \
    }                                                                          \
  } while (0)
#define dev_notice_once(dev, fmt, ...)                                        \
  do {                                                                        \
    static bool __noted;                                                       \
    if (!__noted) {                                                            \
      __noted = true;                                                          \
      dev_notice(dev, fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)
#define dev_dbg_once(dev, fmt, ...)                                           \
  do {                                                                        \
    static bool __warned;                                                      \
    if (!__warned) {                                                           \
      __warned = true;                                                         \
      dev_dbg(dev, fmt, ##__VA_ARGS__);                                        \
    }                                                                          \
  } while (0)

static inline bool device_is_registered(struct device *dev) {
  return dev && dev->kobj.name != NULL;
}

int bus_register(const struct bus_type *bus);
void bus_unregister(const struct bus_type *bus);
struct device *bus_find_device_by_name(const struct bus_type *bus,
                                        struct device *start, const char *name);
struct device *bus_find_device_by_fwnode(const struct bus_type *bus,
                                          const struct fwnode_handle *fwnode);
int device_bind_driver(struct device *dev);
void device_release_driver(struct device *dev);
int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);

/* Groups are accepted but not materialized yet (sysfs dynamic nodes are a
 * native-sysfs follow-up; see docs/linuxkpi-gaps.md). */
int device_add_groups(struct device *dev, const struct attribute_group **groups);
void device_remove_groups(struct device *dev,
                          const struct attribute_group **groups);

void *dev_get_drvdata(const struct device *dev);
void dev_set_drvdata(struct device *dev, void *data);

void device_initialize(struct device *dev);
void pm_runtime_init(struct device *dev);
int device_register(struct device *dev);
void device_unregister(struct device *dev);
int device_add(struct device *dev);
void device_del(struct device *dev);

struct device *get_device(struct device *dev);
void put_device(struct device *dev);

int dev_set_name(struct device *dev, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

int __kpi_dev_printk(const char *level, const struct device *dev,
                     const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define dev_printk(level, dev, fmt, ...)                                      \
  __kpi_dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...) dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...) dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...) dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define dev_dbg(dev, fmt, ...) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...)                                                 \
  ({                                                                           \
    if (0)                                                                     \
      dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__);                         \
    0;                                                                         \
  })
#endif

/* ── classes / device creation ──────────────────────────────────────────── */

struct class *class_create(const char *name);
void class_destroy(const struct class *cls);
struct device *__kpi_device_create(const struct class *cls,
                                   struct device *parent, dev_t devt,
                                   void *drvdata, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void __kpi_device_destroy(const struct class *cls, dev_t devt);

struct device *device_create_with_groups(const struct class *cls,
                                         struct device *parent, dev_t devt,
                                         void *drvdata,
                                         const struct attribute_group **groups,
                                         const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/* Native driver manager owns plain device_create()/device_destroy() with a
 * different signature; imported Linux code goes through the KPI variants. */
#define device_create(cls, parent, devt, drvdata, fmt, ...)                   \
  __kpi_device_create(cls, parent, devt, drvdata, fmt, ##__VA_ARGS__)
#define device_destroy(cls, devt) __kpi_device_destroy(cls, devt)

/* ── devres ─────────────────────────────────────────────────────────────── */

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);
void device_set_node(struct device *dev, struct fwnode_handle *fwnode);
void *devres_find(struct device *dev, dr_release_t release, dr_match_t match,
                  void *match_data);

void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp);
void devres_free(void *res);
void devres_add(struct device *dev, void *res);

void devres_release_all(struct device *dev);

/* Devres groups: the tag is the group id; open returns it (and records the
 * group start), close stops recording, release frees everything added since
 * the matching open (used by vgem/vkms around probe). */
void *devres_open_group(struct device *dev, void *id, gfp_t gfp);
void devres_close_group(struct device *dev, void *id);
void devres_release_group(struct device *dev, void *id);

int devm_add_action(struct device *dev, void (*action)(void *), void *data);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
                             void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp);
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp);
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp);
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static inline void *devm_kmalloc_array(struct device *dev, size_t n,
                                       size_t size, gfp_t flags) {
  size_t bytes;

  if (__builtin_mul_overflow(n, size, &bytes))
    return NULL;
  return devm_kmalloc(dev, bytes, flags);
}
void devm_kfree(struct device *dev, const void *p);

struct resource;
void __iomem *devm_ioremap(struct device *dev, resource_size_t offset,
                           resource_size_t size);
void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset,
                              resource_size_t size);
void __iomem *devm_ioremap_resource(struct device *dev,
                                    const struct resource *res);
void __iomem *devm_ioremap_resource_wc(struct device *dev,
                                       const struct resource *res);
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size,
                    unsigned long flags);
void devm_memunmap(struct device *dev, void *addr);
int devm_request_irq(struct device *dev, unsigned int irq,
                     irqreturn_t (*handler)(int, void *), unsigned long irqflags,
                     const char *devname, void *dev_id);

/* Shape-only pieces stock <linux/device.h> provides that imported drivers
 * use.  Removability/PM flags are inert (no hotplug, no runtime PM). */
enum device_removable {
  DEVICE_REMOVABLE_NOT_SUPPORTED = 0,
  DEVICE_REMOVABLE_UNKNOWN,
  DEVICE_REMOVABLE,
};

static inline bool dev_is_removable(struct device *dev) {
  return dev->removable == DEVICE_REMOVABLE;
}

static inline void dev_pm_set_driver_flags(struct device *dev, u32 flags) {
  (void)dev;
  (void)flags;
}

#define ATTRIBUTE_GROUPS(name)                                                 \
  static const struct attribute_group name##_group = {                         \
      .attrs = name##_attrs,                                                   \
  };                                                                           \
  static const struct attribute_group *name##_groups[] = {&name##_group, NULL}

#define dev_WARN(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, condition, fmt, ...)                                \
  do {                                                                        \
    if (condition)                                                             \
      dev_warn(dev, fmt, ##__VA_ARGS__);                                       \
  } while (0)
#define dev_err_ratelimited(dev, fmt, ...) dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...) dev_dbg(dev, fmt, ##__VA_ARGS__)

int device_create_file(struct device *dev, const struct device_attribute *attr);
void device_remove_file(struct device *dev,
                        const struct device_attribute *attr);

/* Optional audio-component ordering links have no native provider. */
struct device_link;
#define DL_FLAG_STATELESS (1U << 0)
#define DL_FLAG_PM_RUNTIME (1U << 2)
static inline struct device_link *device_link_add(struct device *consumer,
    struct device *supplier, u32 flags) {
  (void)consumer; (void)supplier; (void)flags;
  return NULL;
}
static inline void device_link_del(struct device_link *link) {
  /* No link is allocated while the native runtime PM core is disabled. */
  (void)link;
}

int dev_err_probe(const struct device *dev, int err, const char *fmt, ...);

/* Match upstream device.h's inclusion order: PM wakeup helpers rely on the
 * device and power-state definitions above. */
#define _DEVICE_H_
#include <linux/pm_wakeup.h>
#undef _DEVICE_H_

#endif /* __AVORY_LINUXKPI_DEVICE_H */
