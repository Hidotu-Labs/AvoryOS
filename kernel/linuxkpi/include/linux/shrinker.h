#ifndef __AVORY_LINUXKPI_SHRINKER_H
#define __AVORY_LINUXKPI_SHRINKER_H
#include_next <linux/shrinker.h>

/* Retain embedded-shrinker registration for native tests and compatibility
 * users while newer imported drivers use shrinker_alloc/register/free. */
int register_shrinker(struct shrinker *shrinker, const char *fmt, ...);
void unregister_shrinker(struct shrinker *shrinker);
#endif
