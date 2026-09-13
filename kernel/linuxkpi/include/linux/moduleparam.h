#ifndef __AVORY_LINUXKPI_MODULEPARAM_H
#define __AVORY_LINUXKPI_MODULEPARAM_H

/* Linux <linux/moduleparam.h> overlay.
 *
 * Parameters and __setup()/early_param() handlers are collected in the
 * .kpi_params/.kpi_setups sections (see kernel/linker-scripts/x86_64.lds) and
 * applied once at boot from the Limine command line by linuxkpi_param_init()
 * (linuxkpi/src/params.c).  Arrays and callbacks (module_param_array,
 * module_param_cb) are not provided yet; add them when a driver needs one. */

#include <linux/init.h>
#include <linux/types.h>

enum kpi_param_type {
  KPI_PARAM_bool,
  KPI_PARAM_invbool,
  KPI_PARAM_byte,
  KPI_PARAM_short,
  KPI_PARAM_ushort,
  KPI_PARAM_int,
  KPI_PARAM_uint,
  KPI_PARAM_long,
  KPI_PARAM_ulong,
  KPI_PARAM_hexint,
  KPI_PARAM_charp,
  KPI_PARAM_string,
  KPI_PARAM_bint,   /* bool written as an integer (amdgpu's int params) */
  KPI_PARAM_ullong, /* unsigned long long (amdgpu's 64-bit params) */
};

struct kpi_param {
  const char *name;
  void *value;
  unsigned short type;
  unsigned short maxlen; /* for KPI_PARAM_string buffers */
  unsigned int perm;
};

struct kpi_setup {
  const char *str;
  int (*fn)(char *arg);
};

#define __kpi_param_section \
  __attribute__((used, section(".kpi_params"), aligned(8)))
#define __kpi_setup_section \
  __attribute__((used, section(".kpi_setups"), aligned(8)))

#define module_param_named(name, value, type, perm)                           \
  static const struct kpi_param __kpi_param_##name __kpi_param_section = {    \
      #name, &(value), KPI_PARAM_##type, 0, (perm)}

#define module_param(name, type, perm)                                        \
  module_param_named(name, name, type, perm)

#define module_param_named_unsafe(name, value, type, perm)                    \
  module_param_named(name, value, type, perm)

/* The "unsafe" variants exist for kernel-doc/tooling metadata upstream; the
 * runtime behavior is identical to the regular macros. */
#define module_param_unsafe(name, type, perm)                                 \
  module_param_named(name, name, type, perm)

#define core_param(name, var, type, perm)                                     \
  module_param_named(name, var, type, perm)

#define module_param_string(name, string, len, perm)                          \
  static const struct kpi_param __kpi_param_##name __kpi_param_section = {    \
      #name, (string), KPI_PARAM_string, (len), (perm)}

/* Descriptions are module metadata; with CONFIG_MODULES=n they expand to
 * nothing, matching upstream's __MODULE_INFO in a static kernel.  Must stay a
 * declaration-position no-op (used at file scope). */
#define MODULE_PARM_DESC(name, desc)

#define __kpi_setup(str, fn)                                                  \
  static const struct kpi_setup __kpi_setup_##fn __kpi_setup_section = {      \
      (str), (fn)}

#define __setup_param(str, unique_id, fn, early) __kpi_setup(str, fn)
#define __setup(str, fn) __kpi_setup(str, fn)
#define early_param(str, fn) __kpi_setup(str, fn)

/* Boot integration; linuxkpi_param_parse() is exposed for tests. */
void linuxkpi_param_init(void);
void linuxkpi_param_parse(const char *cmdline);

#endif /* __AVORY_LINUXKPI_MODULEPARAM_H */
