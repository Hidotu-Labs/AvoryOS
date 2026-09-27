#ifndef CONSOLE_KLOG_H
#define CONSOLE_KLOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hot-path diagnostics are useful while debugging the kernel, but serial I/O
 * can noticeably stall short-lived processes/syscalls. */
#ifndef KLOG_VERBOSE
#define KLOG_VERBOSE 0
#endif

#if KLOG_VERBOSE
#define klog_debug_puts(s) klog_puts(s)
#define klog_debug_uint64(n) klog_uint64(n)
#define klog_debug_hex64(n) klog_hex64(n)
#define klog_debug_hex32(n) klog_hex32(n)
#define klog_debugf(fmt, ...) klogf(fmt, ##__VA_ARGS__)
#else
#define klog_debug_puts(s) ((void)0)
#define klog_debug_uint64(n) ((void)0)
#define klog_debug_hex64(n) ((void)0)
#define klog_debug_hex32(n) ((void)0)
#define klog_debugf(fmt, ...) ((void)0)
#endif

#define KLOG_CLR_RESET  "\x1b[0m"
#define KLOG_CLR_RED    "\x1b[31m"
#define KLOG_CLR_GREEN  "\x1b[32m"
#define KLOG_CLR_YELLOW "\x1b[33m"
#define KLOG_CLR_BLUE   "\x1b[34m"
#define KLOG_CLR_MAGENTA "\x1b[35m"
#define KLOG_CLR_CYAN   "\x1b[36m"
#define KLOG_CLR_WHITE  "\x1b[37m"

void klog_putchar(char c);
void klog_puts(const char *s);
void klog_uint64(uint64_t num);
void klog_int64(int64_t num);
void klog_hex64(uint64_t num);
void klog_hex32(uint32_t num);
void klogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vklogf(const char *fmt, va_list ap);
void klog_proc_exit(uint32_t tid, uint32_t tgid, bool is_thread, const char *comm, uint64_t status);
void klog_proc_exec(uint32_t tid, const char *path);
void klog_ramfs_free(void *ptr, uint64_t capacity, bool is_pmm, uint64_t pages);

void klog_set_screen_logging(bool enabled);

#endif
