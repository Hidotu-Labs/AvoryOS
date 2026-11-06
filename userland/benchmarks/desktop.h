#ifndef AVORY_DESKTOP_BENCH_H
#define AVORY_DESKTOP_BENCH_H
#include <stddef.h>
#include <stdint.h>
extern int bench_errors;
uint64_t bench_now(void);
void bench_fail(const char *name);
void bench_skip(const char *name, const char *reason);
void bench_stats(const char *name, uint64_t *values, size_t count);
int bench_transfer(int fd, void *buf, size_t bytes, int writing);
int bench_wait(int pid);
void bench_wakeups(unsigned samples, const char *label);
int bench_scheduling(unsigned samples, unsigned workers);
int bench_storage(unsigned samples, const char *base);
int bench_apps(unsigned samples, const char *self);
int bench_graphics(unsigned frames);
int bench_browser(unsigned runs, const char *base);
int bench_command(unsigned runs, char **args);
int bench_app_child(int fd);
#endif
