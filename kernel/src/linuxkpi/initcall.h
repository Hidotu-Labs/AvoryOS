#ifndef LINUXKPI_INITCALL_H
#define LINUXKPI_INITCALL_H

/* Initcall support, Phase 0.
 *
 * Upstream Linux code registers start-up functions with module_init() and
 * friends; with CONFIG_MODULES=n those expand to entries in the
 * .initcall<level>.init sections, and do_initcalls() runs them at the end of
 * boot.  AvoryOS has no module loader either, so imported drivers are built in
 * and started exactly the same way.
 *
 * The sections and the per-level symbols are defined by
 * kernel/linker-scripts/x86_64.lds; linuxkpi_run_initcalls() walks them in
 * link order (levels 0..7).  Once the real Linux <linux/init.h> is in use,
 * module_init() emits .initcall6.init entries and no extra glue is needed
 * there; this header keeps the mechanism testable before the tree lands.
 */

typedef int (*linuxkpi_initcall_fn_t)(void);

#define __linuxkpi_define_initcall(fn, id)                                 \
    static linuxkpi_initcall_fn_t                                          \
        __attribute__((used, section(".initcall" #id ".init")))            \
        __linuxkpi_initcall_##fn = (fn)

#define LINUXKPI_INITCALL(fn)      __linuxkpi_define_initcall(fn, 6)
#define LINUXKPI_LATE_INITCALL(fn) __linuxkpi_define_initcall(fn, 7)

/* Walk every registered initcall in link order (levels 0..7).  Exported for
 * the inline fallback used when the initcall kthread cannot be created; normal
 * boot reaches this through linuxkpi_run_initcalls(). */
void linuxkpi_run_initcalls_inline(void);

/* Run every registered initcall in a kernel thread and wait (bounded) for it
 * to finish.  Call once, late in boot, after the native services imported
 * drivers may depend on (console, heap, VMM, timers, scheduler, PCI, VFS) are
 * up.  Linux runs level-6 module_init in kernel_init's thread, and drivers may
 * sleep during probe; kmain_high_half is the BSP idle context, where the
 * scheduler resumes the idle thread regardless of its state. */
void linuxkpi_run_initcalls(void);

#endif /* LINUXKPI_INITCALL_H */
