/* Shrinker API stubs (header: <linux/shrinker.h>).
 *
 * AvoryOS has no page-reclaim shrinker infrastructure.  Imported drivers
 * (TTM's ttm_pool registers "drm-ttm_pool") call register_shrinker() at init
 * and unregister_shrinker() at teardown; both are accepted and remembered
 * only insofar as the driver's own state goes.  Nothing ever walks the
 * shrinker chain, so reclaim scans zero objects: never less correct than a
 * shrinker that could not run, just uninvolved in memory pressure.
 *
 * When native reclaim arrives, this file must grow a registry and call
 * count_objects()/scan_objects() from the reclaimer instead of no-oping. */

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/shrinker.h>

int prealloc_shrinker(struct shrinker *shrinker, const char *fmt, ...) {
  (void)fmt;
  if (!shrinker)
    return -EINVAL;
  INIT_LIST_HEAD(&shrinker->list);
  return 0;
}

void register_shrinker_prepared(struct shrinker *shrinker) {
  if (shrinker)
    shrinker->flags |= SHRINKER_REGISTERED;
}

int register_shrinker(struct shrinker *shrinker, const char *fmt, ...) {
  (void)fmt;
  if (!shrinker)
    return -EINVAL;
  INIT_LIST_HEAD(&shrinker->list);
  shrinker->flags |= SHRINKER_REGISTERED;
  return 0;
}

void unregister_shrinker(struct shrinker *shrinker) {
  if (shrinker)
    shrinker->flags &= ~SHRINKER_REGISTERED;
}

void free_prealloced_shrinker(struct shrinker *shrinker) {
  if (shrinker)
    shrinker->flags &= ~SHRINKER_REGISTERED;
}

void synchronize_shrinkers(void) {}
