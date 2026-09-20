#ifndef CONSOLE_DEBUG_H
#define CONSOLE_DEBUG_H

/*
 * DBG() — bring-up tracing that is visible with or without a serial cable.
 *
 * Compiled out by default now that boot is healthy: define DEBUG_CONSOLE=1
 * (or pass -DDEBUG_CONSOLE=1 to the kernel build) to re-enable it.  When
 * enabled, every message is written synchronously to COM1 and rendered on the
 * framebuffer console through the console's non-blocking path, so a CPU
 * already holding the console lock cannot deadlock the trace.
 *
 * Valid once console_init() has run (the boot log phase and later).  Use it
 * for low-frequency checkpoints (AP bring-up, driver probes), not in hot
 * paths: COM1 polling costs microseconds per byte.
 *
 *   DBG("[SMP] AP %u online\n", cpu_id);
 */
#ifndef DEBUG_CONSOLE
#define DEBUG_CONSOLE 0
#endif

#if DEBUG_CONSOLE

#define DBG(fmt, ...) dbg_printf(fmt, ##__VA_ARGS__)

void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dbg_puts(const char *s);

#else /* !DEBUG_CONSOLE */

#define DBG(fmt, ...) ((void)0)
#define dbg_puts(s) ((void)0)

#endif /* DEBUG_CONSOLE */

#endif
