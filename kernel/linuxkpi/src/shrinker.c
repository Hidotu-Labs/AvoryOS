/* Shrinker registry (header: <linux/shrinker.h>).
 *
 * AvoryOS has no page-reclaim daemon. Imported drivers (TTM's ttm_pool
 * registers "drm-ttm_pool") still expect their cached pool pages to be
 * reclaimable under pressure: without this, Minecraft texture churn pins
 * freed GTT/VRAM pages in TTM's pool while new high-order (2 MiB, order-9)
 * allocations fail on a fragmented PMM, surfacing as green/empty textures
 * for ~1 s until a later retry. The registry below remembers every
 * registered shrinker; alloc_pages() drains them once before giving up.
 * Nothing here touches imported amdgpu/TTM sources. */

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/shrinker.h>
#include <linux/spinlock.h>
#include <linuxkpi/log.h>

#define KPI_SHRINKER_MAX 32
static struct shrinker *kpi_shrinkers[KPI_SHRINKER_MAX];
static DEFINE_SPINLOCK(kpi_shrinker_lock);

static void kpi_shrinker_add(struct shrinker *s) {
  unsigned long flags;
  if (!s)
    return;
  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i] == s) {
      spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
      return;
    }
  }
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (!kpi_shrinkers[i]) {
      kpi_shrinkers[i] = s;
      break;
    }
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
}

static void kpi_shrinker_del(struct shrinker *s) {
  unsigned long flags;
  if (!s)
    return;
  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i] == s)
      kpi_shrinkers[i] = NULL;
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
}

/* Drain cached pool pages (e.g. TTM) when PMM cannot serve `needed' pages.
 * Returns pages freed. Called without locks held; each shrinker's own
 * locking applies. Safe in sleeping (GFP_KERNEL) context only. */
unsigned long linuxkpi_shrink_reclaim(gfp_t gfp, unsigned long needed) {
  struct shrinker *snap[KPI_SHRINKER_MAX];
  int n = 0;
  unsigned long freed = 0;
  unsigned long flags;

  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX && n < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i])
      snap[n++] = kpi_shrinkers[i];
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);

  for (int i = 0; i < n && freed < needed; i++) {
    struct shrinker *s = snap[i];
    struct shrink_control sc = {0};
    unsigned long avail, got;

    if (!s->count_objects || !s->scan_objects)
      continue;
    sc.gfp_mask = gfp;
    sc.nid = 0;
    sc.nr_to_scan = 0;
    sc.nr_scanned = 0;
    avail = s->count_objects(s, &sc);
    if (avail == SHRINK_EMPTY || avail == SHRINK_STOP || avail == 0)
      continue;
    sc.nr_to_scan = avail < (needed - freed) ? avail : (needed - freed);
    sc.nr_scanned = sc.nr_to_scan;
    got = s->scan_objects(s, &sc);
    if (got == SHRINK_STOP || got == SHRINK_EMPTY)
      continue;
    freed += got;
  }
  return freed;
}

int prealloc_shrinker(struct shrinker *shrinker, const char *fmt, ...) {
  (void)fmt;
  if (!shrinker)
    return -EINVAL;
  INIT_LIST_HEAD(&shrinker->list);
  return 0;
}

void register_shrinker_prepared(struct shrinker *shrinker) {
  if (!shrinker)
    return;
  shrinker->flags |= SHRINKER_REGISTERED;
  kpi_shrinker_add(shrinker);
}

int register_shrinker(struct shrinker *shrinker, const char *fmt, ...) {
  (void)fmt;
  if (!shrinker)
    return -EINVAL;
  INIT_LIST_HEAD(&shrinker->list);
  shrinker->flags |= SHRINKER_REGISTERED;
  kpi_shrinker_add(shrinker);
  return 0;
}

void unregister_shrinker(struct shrinker *shrinker) {
  if (!shrinker)
    return;
  kpi_shrinker_del(shrinker);
  shrinker->flags &= ~SHRINKER_REGISTERED;
}

void free_prealloced_shrinker(struct shrinker *shrinker) {
  if (!shrinker)
    return;
  kpi_shrinker_del(shrinker);
  shrinker->flags &= ~SHRINKER_REGISTERED;
}

void synchronize_shrinkers(void) {}
