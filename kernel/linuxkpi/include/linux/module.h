#ifndef __AVORY_LINUXKPI_MODULE_H
#define __AVORY_LINUXKPI_MODULE_H

/* Minimal Linux <linux/module.h> overlay.
 *
 * AvoryOS is a statically linked kernel with no loadable modules: module
 * metadata is dropped and module references always succeed.  The EXPORT_SYMBOL
 * family comes from the upstream <linux/export.h>, which is already a no-op
 * without CONFIG_MODULES. */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

struct module;

#define THIS_MODULE ((struct module *)0)
#define KBUILD_MODNAME "avoryos"
#define KBUILD_BASENAME "avoryos"

#define MODULE_LICENSE(_license)
#define MODULE_AUTHOR(_author)
#define MODULE_DESCRIPTION(_description)
#define MODULE_VERSION(_version)
#define MODULE_ALIAS(_alias)
#define MODULE_ALIAS_CHARDEV(_major, _minor)
#define MODULE_DEVICE_TABLE(_type, _name)
#define MODULE_FIRMWARE(_firmware)
#define MODULE_INFO(_tag, _info)
#define MODULE_PARM_DESC(_parm, _desc)
#define MODULE_SOFTDEP(_dep)
#define MODULE_IMPORT_NS(_ns)
#define MODULE_EXPORT(_name)

/* Upstream defines these in <linux/module.h> (this overlay shadows it); with
 * CONFIG_MODULES=n they are plain initcall registration. */
#define module_init(x) __initcall(x)
#define module_exit(x) __exitcall(x)

static inline bool try_module_get(struct module *module) {
  (void)module;
  return true;
}

static inline void module_put(struct module *module) { (void)module; }

#define __module_get(module) do { (void)(module); } while (0)
#define __module_get_notrace(module) __module_get(module)

#endif /* __AVORY_LINUXKPI_MODULE_H */
