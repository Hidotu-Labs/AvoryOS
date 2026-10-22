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
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <linuxkpi/log.h>

#define KPI_SHRINKER_MAX 32
static struct shrinker *kpi_shrinkers[KPI_SHRINKER_MAX];
static DEFINE_SPINLOCK(kpi_shrinker_lock);
static unsigned int kpi_shrinker_users[KPI_SHRINKER_MAX];
static bool kpi_shrinker_retiring[KPI_SHRINKER_MAX];
static DECLARE_WAIT_QUEUE_HEAD(kpi_shrinker_idle);

static bool kpi_shrinker_add(struct shrinker *s) {
  unsigned long flags;
  if (!s)
    return false;
  bool added = false;
  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i] == s) {
      spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
      return true;
    }
  }
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (!kpi_shrinkers[i]) {
      kpi_shrinkers[i] = s;
      added = true;
      break;
    }
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
  return added;
}

/* Entries stay occupied until every snapshotted callback drops its pin. */
static void kpi_shrinker_del(struct shrinker *s) {
  unsigned long flags;
  int slot = -1;
  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i] == s) {
      slot = i;
      kpi_shrinker_retiring[i] = true;
      break;
    }
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
  if (slot < 0)
    return;
  wait_event(kpi_shrinker_idle, READ_ONCE(kpi_shrinker_users[slot]) == 0);
  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  kpi_shrinkers[slot] = NULL;
  kpi_shrinker_retiring[slot] = false;
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
}

/* Drain cached pool pages (e.g. TTM) when PMM cannot serve `needed' pages.
 * Returns pages freed. Called without locks held; each shrinker's own
 * locking applies. Safe in sleeping (GFP_KERNEL) context only. */
unsigned long linuxkpi_shrink_reclaim(gfp_t gfp, unsigned long needed) {
  struct shrinker *snap[KPI_SHRINKER_MAX];
  int slots[KPI_SHRINKER_MAX];
  int n = 0;
  unsigned long freed = 0;
  unsigned long flags;

  spin_lock_irqsave(&kpi_shrinker_lock, flags);
  for (int i = 0; i < KPI_SHRINKER_MAX && n < KPI_SHRINKER_MAX; i++) {
    if (kpi_shrinkers[i] && !kpi_shrinker_retiring[i]) {
      slots[n] = i;
      kpi_shrinker_users[i]++;
      snap[n++] = kpi_shrinkers[i];
    }
  }
  spin_unlock_irqrestore(&kpi_shrinker_lock, flags);

  for (int i = 0; i < n; i++) {
    struct shrinker *s = snap[i];
    struct shrink_control sc = {0};
    unsigned long avail, got;

    if (freed >= needed || !s->count_objects || !s->scan_objects)
      goto put;
    sc.gfp_mask = gfp;
    sc.nid = 0;
    sc.nr_to_scan = 0;
    sc.nr_scanned = 0;
    avail = s->count_objects(s, &sc);
    if (avail == SHRINK_EMPTY || avail == SHRINK_STOP || avail == 0)
      goto put;
    sc.nr_to_scan = avail < (needed - freed) ? avail : (needed - freed);
    sc.nr_scanned = sc.nr_to_scan;
    got = s->scan_objects(s, &sc);
    if (got != SHRINK_STOP && got != SHRINK_EMPTY)
      freed += got;
put:
    spin_lock_irqsave(&kpi_shrinker_lock, flags);
    kpi_shrinker_users[slots[i]]--;
    wake_up_all(&kpi_shrinker_idle);
    spin_unlock_irqrestore(&kpi_shrinker_lock, flags);
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
  if (kpi_shrinker_add(shrinker))
    shrinker->flags |= SHRINKER_REGISTERED;
}

int register_shrinker(struct shrinker *shrinker, const char *fmt, ...) {
  (void)fmt;
  if (!shrinker)
    return -EINVAL;
  INIT_LIST_HEAD(&shrinker->list);
  if (!kpi_shrinker_add(shrinker))
    return -ENOSPC;
  shrinker->flags |= SHRINKER_REGISTERED;
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

static bool kpi_shrinkers_idle(void) {
  for (int i = 0; i < KPI_SHRINKER_MAX; i++)
    if (READ_ONCE(kpi_shrinker_users[i]))
      return false;
  return true;
}

void synchronize_shrinkers(void) {
  wait_event(kpi_shrinker_idle, kpi_shrinkers_idle());
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...) {
  (void)fmt;
  struct shrinker *s = kzalloc(sizeof(*s), GFP_KERNEL);
  if (!s)
    return NULL;
  s->flags = flags | SHRINKER_ALLOCATED;
  INIT_LIST_HEAD(&s->list);
  refcount_set(&s->refcount, 1);
  init_completion(&s->done);
  return s;
}

void shrinker_register(struct shrinker *s) {
  if (register_shrinker(s, NULL))
    klogf("[WARN] LinuxKPI: shrinker registry full; registration failed\n");
}

void shrinker_free(struct shrinker *s) {
  if (!s)
    return;
  unregister_shrinker(s);
  if (s->flags & SHRINKER_ALLOCATED)
    kfree(s);
}
#endif
