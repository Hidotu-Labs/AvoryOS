/* Real cgroup2 resource control, process membership, and PSI tracking.
 *
 * cgroup2 is mounted as a tmpfs whose directories carry a cgroupfs_group
 * (via tmpfs_dir_t.cgroup). Control files are bound to cgroupfs.c handlers
 * for live accounting and enforcement:
 *   - Core:    cgroup.procs, cgroup.threads, cgroup.events, cgroup.kill,
 *              cgroup.freeze, cgroup.controllers, cgroup.subtree_control
 *   - CPU:     cpu.weight (EEVDF), cpu.max (bandwidth throttle), cpu.stat,
 *              cpu.pressure
 *   - Memory:  memory.current, memory.min, memory.low, memory.high (reclaim +
 *              throttle), memory.max (reclaim + cgroup OOM killer),
 *              memory.oom.group, memory.events, memory.stat, memory.pressure
 *   - PIDs:    pids.current, pids.max (enforced on fork/clone), pids.events
 *   - PSI:     global (/proc/pressure/{cpu,memory,io}) and per-cgroup
 *
 * Lock order: members_lock, then tid_lock. Never take members_lock while
 * holding tid_lock or any runqueue lock. */

#include "cgroupfs.h"
#include "inotify.h"
#include "tmpfs.h"
#include "../apic/lapic_timer.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/zram.h"
#include "../sched/pid_namespace.h"
#include "../sched/sched.h"

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;
extern void signal_send(struct thread *t, int sig);

#define CGROUP_CTRL_CPU    (1U << 0)
#define CGROUP_CTRL_MEMORY (1U << 1)
#define CGROUP_CTRL_PIDS   (1U << 2)
#define CGROUP_CTRL_ALL    (CGROUP_CTRL_CPU | CGROUP_CTRL_MEMORY | CGROUP_CTRL_PIDS)

/* Fixed-point PSI EWMA constants (2-second sample window, FSHIFT=11).
 * EXP_W = round(2048 * exp(-2 / W)). */
#define PSI_FIXED_1   2048ULL
#define PSI_EXP_10    1677ULL
#define PSI_EXP_60    1981ULL
#define PSI_EXP_300   2034ULL
#define PSI_SAMPLE_NS 2000000000ULL

struct psi_state {
  uint64_t some_total_ns;
  uint64_t full_total_ns;
  uint64_t some_prev_ns;
  uint64_t full_prev_ns;
  uint32_t some_avg10_bp;  /* basis points: 10000 = 100.00% */
  uint32_t some_avg60_bp;
  uint32_t some_avg300_bp;
  uint32_t full_avg10_bp;
  uint32_t full_avg60_bp;
  uint32_t full_avg300_bp;
  uint64_t last_sample_ns;
};

struct cgroupfs_group {
  struct cgroupfs_group *parent;
  struct cgroupfs_group *next_all;
  vfs_node_t *events; /* cgroup.events, for IN_MODIFY when empty/frozen */
  bool removed;

  /* Core state */
  bool frozen;
  uint32_t subtree_control;

  /* CPU controller */
  uint32_t cpu_weight;        /* 1..10000, default 100 */
  uint64_t cpu_quota_us;      /* UINT64_MAX = "max" */
  uint64_t cpu_period_us;     /* default 100000 */
  uint64_t cpu_period_start_ns;
  uint64_t cpu_period_used_ns;
  uint64_t cpu_throttled_until_ns;
  uint64_t cpu_usage_ns;
  uint64_t cpu_user_ns;
  uint64_t cpu_system_ns;
  uint64_t cpu_nr_throttled;
  uint64_t cpu_throttled_ns;

  /* Memory controller */
  uint64_t mem_min;
  uint64_t mem_low;
  uint64_t mem_high;          /* UINT64_MAX = "max" */
  uint64_t mem_max;           /* UINT64_MAX = "max" */
  bool mem_oom_group;
  uint64_t mem_events_low;
  uint64_t mem_events_high;
  uint64_t mem_events_max;
  uint64_t mem_events_oom;
  uint64_t mem_events_oom_kill;

  /* PIDs controller */
  uint64_t pids_max;          /* UINT64_MAX = "max" */
  uint64_t pids_events_max;

  /* Per-cgroup PSI */
  struct psi_state psi[3];
};

struct cgroupfs_member {
  uint32_t tgid;
  struct cgroupfs_group *group;
  struct cgroupfs_member *next;
};

#define CGROUPFS_BUCKETS 256
static struct cgroupfs_member *members[CGROUPFS_BUCKETS];
static spinlock_t members_lock = SPINLOCK_INIT;
static struct cgroupfs_group *all_groups = NULL;
static struct psi_state global_psi[3];

static struct cgroupfs_member **bucket_of(uint32_t tgid) {
  return &members[tgid % CGROUPFS_BUCKETS];
}

static bool tgid_alive(uint32_t tgid) {
  struct sched_thread_snapshot s;
  return sched_get_thread_snapshot(tgid, &s) && s.tgid == tgid &&
         s.state != THREAD_ZOMBIE && s.state != THREAD_DEAD;
}

static bool group_within(const struct cgroupfs_group *g,
                         const struct cgroupfs_group *ancestor) {
  for (; g; g = g->parent)
    if (g == ancestor)
      return true;
  return false;
}

static bool group_is_frozen(const struct cgroupfs_group *g) {
  for (; g; g = g->parent) {
    if (__atomic_load_n(&g->frozen, __ATOMIC_RELAXED))
      return true;
  }
  return false;
}

/* Calls fn for every live member of group (or of its subtree when
 * subtree is set) and frees entries of exited processes. Caller holds
 * members_lock. Returns the number of live members visited. */
static uint32_t walk_members_locked(struct cgroupfs_group *group, bool subtree,
                                    void (*fn)(uint32_t tgid, void *ctx),
                                    void *ctx) {
  uint32_t count = 0;
  for (int b = 0; b < CGROUPFS_BUCKETS; b++) {
    struct cgroupfs_member **link = &members[b];
    while (*link) {
      struct cgroupfs_member *m = *link;
      if (!tgid_alive(m->tgid)) {
        *link = m->next;
        kfree(m);
        continue;
      }
      if (subtree ? group_within(m->group, group) : m->group == group) {
        count++;
        if (fn)
          fn(m->tgid, ctx);
      }
      link = &m->next;
    }
  }
  return count;
}

/* Sets tgid's cgroup (NULL = root). Returns the group it left. */
static struct cgroupfs_group *attach_locked(uint32_t tgid,
                                            struct cgroupfs_group *group,
                                            struct cgroupfs_member *spare) {
  struct cgroupfs_member **link = bucket_of(tgid);
  for (; *link; link = &(*link)->next) {
    if ((*link)->tgid != tgid)
      continue;
    struct cgroupfs_member *m = *link;
    struct cgroupfs_group *old = m->group;
    if (group) {
      m->group = group;
    } else {
      *link = m->next;
      kfree(m);
    }
    if (spare)
      kfree(spare);
    return old;
  }
  if (group && spare) {
    spare->tgid = tgid;
    spare->group = group;
    spare->next = *bucket_of(tgid);
    *bucket_of(tgid) = spare;
  } else if (spare) {
    kfree(spare);
  }
  return NULL;
}

static void update_tgid_threads_cgroup(uint32_t tgid,
                                       struct cgroupfs_group *group) {
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    uint32_t t_tgid = t->tgid ? t->tgid : t->tid;
    if (t_tgid == tgid)
      __atomic_store_n(&t->cgroup, group, __ATOMIC_RELEASE);
  }
  spinlock_release(&tid_lock);
  hal_irq_restore(flags);
}

/* After a move, tell watchers of every cgroup the process left that has now
 * emptied (systemd watches cgroup.events with inotify). */
static void notify_emptied(struct cgroupfs_group *left,
                           struct cgroupfs_group *joined) {
  vfs_node_t *events[16];
  int n = 0;
  spinlock_acquire(&members_lock);
  for (struct cgroupfs_group *g = left; g && n < 16; g = g->parent) {
    if (group_within(joined, g))
      break; /* still populated through the cgroup it joined */
    if (g->events && !g->removed &&
        walk_members_locked(g, true, NULL, NULL) == 0) {
      vfs_node_ref(g->events);
      events[n++] = g->events;
    }
  }
  spinlock_release(&members_lock);
  for (int i = 0; i < n; i++) {
    inotify_notify(events[i], IN_MODIFY, NULL, 0);
    vfs_close(events[i]);
  }
}

static void move_process(uint32_t tgid, struct cgroupfs_group *group) {
  struct cgroupfs_member *spare =
      group ? kmalloc(sizeof(struct cgroupfs_member)) : NULL;
  spinlock_acquire(&members_lock);
  struct cgroupfs_group *old = attach_locked(tgid, group, spare);
  spinlock_release(&members_lock);
  update_tgid_threads_cgroup(tgid, group);
  if (old && old != group)
    notify_emptied(old, group);
}

struct cgroupfs_group *cgroupfs_group_create(struct cgroupfs_group *parent) {
  struct cgroupfs_group *g = kmalloc(sizeof(*g));
  if (!g)
    return NULL;
  memset(g, 0, sizeof(*g));
  g->parent = parent;
  g->subtree_control = CGROUP_CTRL_ALL;
  g->cpu_weight = 100;
  g->cpu_quota_us = UINT64_MAX;
  g->cpu_period_us = 100000;
  g->mem_high = UINT64_MAX;
  g->mem_max = UINT64_MAX;
  g->pids_max = UINT64_MAX;

  spinlock_acquire(&members_lock);
  g->next_all = all_groups;
  all_groups = g;
  spinlock_release(&members_lock);
  return g;
}

/* Groups are never freed: control-file nodes may outlive the directory
 * through open descriptors and still point here. */
bool cgroupfs_group_remove(struct cgroupfs_group *group) {
  if (!group)
    return true;
  spinlock_acquire(&members_lock);
  bool empty = walk_members_locked(group, true, NULL, NULL) == 0;
  if (empty) {
    group->removed = true;
    group->events = NULL;
  }
  spinlock_release(&members_lock);
  return empty;
}

/* ---------- Helpers for parsing / formatting ---------- */

static uint32_t copy_window(const char *text, uint32_t len, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  if (offset >= len)
    return 0;
  if (size > len - offset)
    size = len - offset;
  memcpy(buffer, text + offset, size);
  return size;
}

static struct cgroupfs_group *group_of(vfs_node_t *node) {
  return (struct cgroupfs_group *)node->ptr;
}

static bool parse_u64_or_max(const uint8_t *buf, uint32_t size,
                             uint64_t *out) {
  uint32_t i = 0;
  while (i < size && (buf[i] == ' ' || buf[i] == '\t'))
    i++;
  if (i + 3 <= size && strncmp((const char *)buf + i, "max", 3) == 0) {
    *out = UINT64_MAX;
    return true;
  }
  if (i >= size || buf[i] < '0' || buf[i] > '9')
    return false;
  uint64_t val = 0;
  while (i < size && buf[i] >= '0' && buf[i] <= '9') {
    val = val * 10 + (uint64_t)(buf[i] - '0');
    i++;
  }
  *out = val;
  return true;
}

static uint32_t format_u64_or_max(char *buf, uint32_t cap, uint64_t val) {
  if (val == UINT64_MAX)
    return (uint32_t)snprintf(buf, cap, "max\n");
  return (uint32_t)snprintf(buf, cap, "%llu\n", (unsigned long long)val);
}

/* ---------- Memory accounting & OOM killer ---------- */

struct mem_sum_ctx {
  uint64_t total_bytes;
  uint64_t max_bytes;
  uint32_t max_tgid;
};

static void collect_tgid_mem_cb(uint32_t tgid, void *raw) {
  struct mem_sum_ctx *ctx = raw;
  uint64_t bytes = 0;
  if (!sched_get_mem_snapshot(tgid, NULL, &bytes))
    return;
  ctx->total_bytes += bytes;
  if (bytes >= ctx->max_bytes && tgid > 1) {
    ctx->max_bytes = bytes;
    ctx->max_tgid = tgid;
  }
}

static uint64_t compute_subtree_mem_bytes(struct cgroupfs_group *g,
                                          uint32_t *largest_tgid_out) {
  struct mem_sum_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  spinlock_acquire(&members_lock);
  walk_members_locked(g, true, collect_tgid_mem_cb, &ctx);
  spinlock_release(&members_lock);
  if (largest_tgid_out)
    *largest_tgid_out = ctx.max_tgid;
  return ctx.total_bytes;
}

struct kill_list_ctx {
  uint32_t tgids[64];
  uint32_t count;
};

static void collect_kill_tgid_cb(uint32_t tgid, void *raw) {
  struct kill_list_ctx *ctx = raw;
  if (tgid > 1 && ctx->count < 64)
    ctx->tgids[ctx->count++] = tgid;
}

static void kill_tgid_list(const uint32_t *tgids, uint32_t count) {
  if (!count)
    return;
  struct thread *targets[64];
  uint32_t n = 0;

  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t && n < 64; t = t->global_next) {
    if (t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
      continue;
    uint32_t t_tgid = t->tgid ? t->tgid : t->tid;
    if (t->tid != t_tgid)
      continue; /* send SIGKILL once per process leader */
    for (uint32_t i = 0; i < count; i++) {
      if (tgids[i] == t_tgid) {
        targets[n++] = t;
        break;
      }
    }
  }
  spinlock_release(&tid_lock);
  hal_irq_restore(flags);

  for (uint32_t i = 0; i < n; i++)
    signal_send(targets[i], SIGKILL);
}

static void kill_subtree(struct cgroupfs_group *g) {
  struct kill_list_ctx ctx;
  ctx.count = 0;
  spinlock_acquire(&members_lock);
  walk_members_locked(g, true, collect_kill_tgid_cb, &ctx);
  spinlock_release(&members_lock);
  kill_tgid_list(ctx.tgids, ctx.count);
}

int cgroupfs_mem_try_charge(struct thread *t, uint64_t bytes) {
  if (!t || !bytes)
    return 0;
  struct cgroupfs_group *leaf = __atomic_load_n(&t->cgroup, __ATOMIC_ACQUIRE);
  if (!leaf)
    return 0;

  /* Fast check: does any ancestor have memory.high or memory.max configured? */
  bool has_limit = false;
  for (struct cgroupfs_group *g = leaf; g; g = g->parent) {
    if (__atomic_load_n(&g->mem_high, __ATOMIC_RELAXED) != UINT64_MAX ||
        __atomic_load_n(&g->mem_max, __ATOMIC_RELAXED) != UINT64_MAX) {
      has_limit = true;
      break;
    }
  }
  if (!has_limit)
    return 0;

  uint32_t cur_tgid = t->tgid ? t->tgid : t->tid;
  for (struct cgroupfs_group *g = leaf; g; g = g->parent) {
    uint64_t high = __atomic_load_n(&g->mem_high, __ATOMIC_RELAXED);
    uint64_t max = __atomic_load_n(&g->mem_max, __ATOMIC_RELAXED);
    if (high == UINT64_MAX && max == UINT64_MAX)
      continue;

    uint32_t victim_tgid = 0;
    uint64_t current = compute_subtree_mem_bytes(g, &victim_tgid);

    if (high != UINT64_MAX && current + bytes > high) {
      __atomic_add_fetch(&g->mem_events_high, 1, __ATOMIC_RELAXED);
      uint64_t t0 = lapic_timer_get_ns();
      zram_reclaim(64);
      mm_file_cache_reclaim(64);
      uint64_t t1 = lapic_timer_get_ns();
      if (t1 > t0)
        psi_record_stall(PSI_MEMORY, g, true, t1 - t0);
      /* Re-check after reclaim; if still over memory.high, throttle briefly. */
      current = compute_subtree_mem_bytes(g, &victim_tgid);
      if (current + bytes > high)
        sched_yield();
    }

    if (max != UINT64_MAX && current + bytes > max) {
      uint64_t t0 = lapic_timer_get_ns();
      zram_reclaim(256);
      mm_file_cache_reclaim(256);
      uint64_t t1 = lapic_timer_get_ns();
      if (t1 > t0)
        psi_record_stall(PSI_MEMORY, g, true, t1 - t0);

      current = compute_subtree_mem_bytes(g, &victim_tgid);
      if (current + bytes > max) {
        __atomic_add_fetch(&g->mem_events_max, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g->mem_events_oom, 1, __ATOMIC_RELAXED);

        if (__atomic_load_n(&g->mem_oom_group, __ATOMIC_RELAXED)) {
          kill_subtree(g);
          __atomic_add_fetch(&g->mem_events_oom_kill, 1, __ATOMIC_RELAXED);
          return -12;
        }
        if (!victim_tgid)
          victim_tgid = cur_tgid;
        kill_tgid_list(&victim_tgid, 1);
        __atomic_add_fetch(&g->mem_events_oom_kill, 1, __ATOMIC_RELAXED);
        if (victim_tgid == cur_tgid)
          return -12;
      }
    }
  }
  return 0;
}

/* ---------- CPU & PIDs controllers ---------- */

bool cgroupfs_can_fork(struct thread *parent, vfs_node_t *cgroup_dir) {
  struct cgroupfs_group *target = NULL;
  if (cgroup_dir) {
    target = tmpfs_cgroup_of(cgroup_dir);
    if (!target || target->removed)
      return true;
  } else if (parent) {
    target = __atomic_load_n(&parent->cgroup, __ATOMIC_ACQUIRE);
  }
  if (!target)
    return true;

  for (struct cgroupfs_group *g = target; g; g = g->parent) {
    uint64_t limit = __atomic_load_n(&g->pids_max, __ATOMIC_RELAXED);
    if (limit == UINT64_MAX)
      continue;
    spinlock_acquire(&members_lock);
    uint32_t cur = walk_members_locked(g, true, NULL, NULL);
    spinlock_release(&members_lock);
    if (cur >= limit) {
      __atomic_add_fetch(&g->pids_events_max, 1, __ATOMIC_RELAXED);
      return false;
    }
  }
  return true;
}

uint32_t cgroupfs_effective_cpu_weight(struct thread *t, uint32_t base_weight) {
  if (!t)
    return base_weight;
  struct cgroupfs_group *g = __atomic_load_n(&t->cgroup, __ATOMIC_ACQUIRE);
  if (!g)
    return base_weight;
  uint32_t cw = __atomic_load_n(&g->cpu_weight, __ATOMIC_RELAXED);
  if (cw == 100 || cw == 0)
    return base_weight;
  uint64_t scaled = ((uint64_t)base_weight * (uint64_t)cw) / 100ULL;
  if (scaled < 1)
    scaled = 1;
  if (scaled > 1000000ULL)
    scaled = 1000000ULL;
  return (uint32_t)scaled;
}

void cgroupfs_account_cpu_tick(struct thread *t, uint64_t dt_ns,
                               uint64_t now_ns, bool in_user) {
  if (!t || !dt_ns)
    return;
  struct cgroupfs_group *leaf = __atomic_load_n(&t->cgroup, __ATOMIC_ACQUIRE);
  if (!leaf)
    return;

  for (struct cgroupfs_group *g = leaf; g; g = g->parent) {
    __atomic_add_fetch(&g->cpu_usage_ns, dt_ns, __ATOMIC_RELAXED);
    if (in_user)
      __atomic_add_fetch(&g->cpu_user_ns, dt_ns, __ATOMIC_RELAXED);
    else
      __atomic_add_fetch(&g->cpu_system_ns, dt_ns, __ATOMIC_RELAXED);

    uint64_t quota_us = __atomic_load_n(&g->cpu_quota_us, __ATOMIC_RELAXED);
    if (quota_us == UINT64_MAX)
      continue;

    uint64_t period_us = __atomic_load_n(&g->cpu_period_us, __ATOMIC_RELAXED);
    if (period_us < 1000)
      period_us = 1000;
    uint64_t period_ns = period_us * 1000ULL;
    uint64_t quota_ns = quota_us * 1000ULL;

    uint64_t pstart = __atomic_load_n(&g->cpu_period_start_ns, __ATOMIC_RELAXED);
    if (pstart == 0 || now_ns >= pstart + period_ns) {
      __atomic_store_n(&g->cpu_period_start_ns, now_ns, __ATOMIC_RELAXED);
      __atomic_store_n(&g->cpu_period_used_ns, dt_ns, __ATOMIC_RELAXED);
      __atomic_store_n(&g->cpu_throttled_until_ns, 0, __ATOMIC_RELAXED);
    } else {
      uint64_t used =
          __atomic_add_fetch(&g->cpu_period_used_ns, dt_ns, __ATOMIC_RELAXED);
      if (used >= quota_ns) {
        uint64_t until = pstart + period_ns;
        uint64_t prev =
            __atomic_load_n(&g->cpu_throttled_until_ns, __ATOMIC_RELAXED);
        if (prev < until) {
          __atomic_store_n(&g->cpu_throttled_until_ns, until, __ATOMIC_RELAXED);
          __atomic_add_fetch(&g->cpu_nr_throttled, 1, __ATOMIC_RELAXED);
          if (until > now_ns) {
            uint64_t stall = until - now_ns;
            __atomic_add_fetch(&g->cpu_throttled_ns, stall, __ATOMIC_RELAXED);
            psi_record_stall(PSI_CPU, g, false, stall);
          }
        }
      }
    }
  }
}

bool cgroupfs_is_throttled_or_frozen(struct thread *t, uint64_t now_ns) {
  if (!t)
    return false;
  struct cgroupfs_group *leaf = __atomic_load_n(&t->cgroup, __ATOMIC_ACQUIRE);
  if (!leaf)
    return false;

  for (struct cgroupfs_group *g = leaf; g; g = g->parent) {
    if (__atomic_load_n(&g->frozen, __ATOMIC_RELAXED))
      return true;
    uint64_t until =
        __atomic_load_n(&g->cpu_throttled_until_ns, __ATOMIC_RELAXED);
    if (until && now_ns < until)
      return true;
  }
  return false;
}

/* ---------- PSI (Pressure Stall Information) ---------- */

static uint32_t calc_psi_ewma(uint32_t prev_bp, uint32_t sample_bp,
                              uint64_t exp_w) {
  uint64_t decayed = ((uint64_t)prev_bp * exp_w) / PSI_FIXED_1;
  uint64_t fresh = ((uint64_t)sample_bp * (PSI_FIXED_1 - exp_w)) / PSI_FIXED_1;
  uint64_t res = decayed + fresh;
  if (res > 10000)
    res = 10000;
  return (uint32_t)res;
}

static void psi_sample_state(struct psi_state *s, uint64_t now_ns) {
  uint64_t last = __atomic_load_n(&s->last_sample_ns, __ATOMIC_RELAXED);
  if (last == 0) {
    __atomic_store_n(&s->last_sample_ns, now_ns, __ATOMIC_RELAXED);
    s->some_prev_ns = __atomic_load_n(&s->some_total_ns, __ATOMIC_RELAXED);
    s->full_prev_ns = __atomic_load_n(&s->full_total_ns, __ATOMIC_RELAXED);
    return;
  }
  if (now_ns < last + PSI_SAMPLE_NS)
    return;
  uint64_t elapsed = now_ns - last;
  __atomic_store_n(&s->last_sample_ns, now_ns, __ATOMIC_RELAXED);

  uint64_t cur_some = __atomic_load_n(&s->some_total_ns, __ATOMIC_RELAXED);
  uint64_t cur_full = __atomic_load_n(&s->full_total_ns, __ATOMIC_RELAXED);
  uint64_t d_some = cur_some >= s->some_prev_ns ? cur_some - s->some_prev_ns : 0;
  uint64_t d_full = cur_full >= s->full_prev_ns ? cur_full - s->full_prev_ns : 0;
  s->some_prev_ns = cur_some;
  s->full_prev_ns = cur_full;

  uint32_t some_bp = (uint32_t)((d_some * 10000ULL) / elapsed);
  uint32_t full_bp = (uint32_t)((d_full * 10000ULL) / elapsed);
  if (some_bp > 10000) some_bp = 10000;
  if (full_bp > 10000) full_bp = 10000;

  s->some_avg10_bp = calc_psi_ewma(s->some_avg10_bp, some_bp, PSI_EXP_10);
  s->some_avg60_bp = calc_psi_ewma(s->some_avg60_bp, some_bp, PSI_EXP_60);
  s->some_avg300_bp = calc_psi_ewma(s->some_avg300_bp, some_bp, PSI_EXP_300);
  s->full_avg10_bp = calc_psi_ewma(s->full_avg10_bp, full_bp, PSI_EXP_10);
  s->full_avg60_bp = calc_psi_ewma(s->full_avg60_bp, full_bp, PSI_EXP_60);
  s->full_avg300_bp = calc_psi_ewma(s->full_avg300_bp, full_bp, PSI_EXP_300);
}

void psi_record_stall(enum psi_resource res, struct cgroupfs_group *g,
                      bool full, uint64_t stall_ns) {
  if (res > PSI_IO || !stall_ns)
    return;
  __atomic_add_fetch(&global_psi[res].some_total_ns, stall_ns, __ATOMIC_RELAXED);
  if (full)
    __atomic_add_fetch(&global_psi[res].full_total_ns, stall_ns, __ATOMIC_RELAXED);
  for (; g; g = g->parent) {
    __atomic_add_fetch(&g->psi[res].some_total_ns, stall_ns, __ATOMIC_RELAXED);
    if (full)
      __atomic_add_fetch(&g->psi[res].full_total_ns, stall_ns, __ATOMIC_RELAXED);
  }
}

void psi_tick(uint64_t now_ns) {
  static uint64_t last_global_tick_ns = 0;
  uint64_t prev = __atomic_load_n(&last_global_tick_ns, __ATOMIC_RELAXED);
  if (now_ns < prev + PSI_SAMPLE_NS)
    return;
  if (!__atomic_compare_exchange_n(&last_global_tick_ns, &prev, now_ns, false,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    return;

  for (int r = 0; r < 3; r++)
    psi_sample_state(&global_psi[r], now_ns);

  if (spinlock_try_acquire(&members_lock)) {
    for (struct cgroupfs_group *g = all_groups; g; g = g->next_all) {
      if (g->removed)
        continue;
      for (int r = 0; r < 3; r++)
        psi_sample_state(&g->psi[r], now_ns);
    }
    spinlock_release(&members_lock);
  }
}

uint32_t psi_format(enum psi_resource res, struct cgroupfs_group *g,
                    char *buf, uint32_t cap) {
  if (res > PSI_IO || !buf || !cap)
    return 0;
  const struct psi_state *s = g ? &g->psi[res] : &global_psi[res];
  uint64_t some_us = __atomic_load_n(&s->some_total_ns, __ATOMIC_RELAXED) / 1000ULL;
  uint64_t full_us = __atomic_load_n(&s->full_total_ns, __ATOMIC_RELAXED) / 1000ULL;

  if (res == PSI_CPU && !g) {
    return (uint32_t)snprintf(
        buf, cap,
        "some avg10=%u.%02u avg60=%u.%02u avg300=%u.%02u total=%llu\n",
        s->some_avg10_bp / 100, s->some_avg10_bp % 100,
        s->some_avg60_bp / 100, s->some_avg60_bp % 100,
        s->some_avg300_bp / 100, s->some_avg300_bp % 100,
        (unsigned long long)some_us);
  }
  return (uint32_t)snprintf(
      buf, cap,
      "some avg10=%u.%02u avg60=%u.%02u avg300=%u.%02u total=%llu\n"
      "full avg10=%u.%02u avg60=%u.%02u avg300=%u.%02u total=%llu\n",
      s->some_avg10_bp / 100, s->some_avg10_bp % 100,
      s->some_avg60_bp / 100, s->some_avg60_bp % 100,
      s->some_avg300_bp / 100, s->some_avg300_bp % 100,
      (unsigned long long)some_us,
      s->full_avg10_bp / 100, s->full_avg10_bp % 100,
      s->full_avg60_bp / 100, s->full_avg60_bp % 100,
      s->full_avg300_bp / 100, s->full_avg300_bp % 100,
      (unsigned long long)full_us);
}

/* ---------- VFS read/write handlers ---------- */

struct procs_buf {
  char *out;
  uint32_t len, cap;
};

static void append_pid(uint32_t tgid, void *ctx) {
  struct procs_buf *b = ctx;
  uint32_t visible = pid_namespace_visible(sched_get_current(), tgid);
  if (!visible || b->len + 12 > b->cap)
    return;
  b->len += (uint32_t)snprintf(b->out + b->len, b->cap - b->len, "%u\n",
                               visible);
}

static uint32_t cgroupfs_procs_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  spinlock_acquire(&members_lock);
  uint32_t count = walk_members_locked(g, false, NULL, NULL);
  spinlock_release(&members_lock);

  struct procs_buf b = {.len = 0, .cap = (count + 8) * 12};
  b.out = kmalloc(b.cap);
  if (!b.out)
    return 0;
  spinlock_acquire(&members_lock);
  walk_members_locked(g, false, append_pid, &b);
  spinlock_release(&members_lock);
  uint32_t n = copy_window(b.out, b.len, offset, size, buffer);
  kfree(b.out);
  return n;
}

static uint32_t cgroupfs_procs_write(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  if (!g || g->removed)
    return size;
  uint32_t pid = 0;
  for (uint32_t i = 0; i < size && buffer[i] >= '0' && buffer[i] <= '9'; i++)
    pid = pid * 10 + (uint32_t)(buffer[i] - '0');

  struct thread *current = sched_get_current();
  uint32_t tgid = 0;
  if (pid == 0) {
    tgid = current ? (current->tgid ? current->tgid : current->tid) : 0;
  } else {
    struct sched_thread_snapshot s;
    if (sched_get_thread_snapshot(pid, &s))
      tgid = s.tgid ? s.tgid : s.tid;
  }
  if (tgid)
    move_process(tgid, g);
  return size;
}

static uint32_t cgroupfs_events_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  spinlock_acquire(&members_lock);
  bool populated = walk_members_locked(g, true, NULL, NULL) != 0;
  spinlock_release(&members_lock);
  bool frozen = group_is_frozen(g);
  char text[48];
  int len = snprintf(text, sizeof(text), "populated %d\nfrozen %d\n",
                     populated ? 1 : 0, frozen ? 1 : 0);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_kill_write(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (g && parse_u64_or_max(buffer, size, &val) && val == 1)
    kill_subtree(g);
  return size;
}

static uint32_t cgroupfs_freeze_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[8];
  int len = snprintf(text, sizeof(text), "%d\n",
                     __atomic_load_n(&g->frozen, __ATOMIC_RELAXED) ? 1 : 0);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_freeze_write(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (!g || !parse_u64_or_max(buffer, size, &val))
    return size;
  bool next = (val != 0);
  bool prev = __atomic_exchange_n(&g->frozen, next, __ATOMIC_ACQ_REL);
  if (prev != next && g->events)
    inotify_notify(g->events, IN_MODIFY, NULL, 0);
  return size;
}

static uint32_t cgroupfs_controllers_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  (void)node;
  const char *text = "cpu memory pids\n";
  return copy_window(text, (uint32_t)strlen(text), offset, size, buffer);
}

static uint32_t cgroupfs_subtree_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  uint32_t m = __atomic_load_n(&g->subtree_control, __ATOMIC_RELAXED);
  char text[64];
  int len = 0;
  if (m & CGROUP_CTRL_CPU)
    len += snprintf(text + len, sizeof(text) - (size_t)len, "%scpu",
                    len ? " " : "");
  if (m & CGROUP_CTRL_MEMORY)
    len += snprintf(text + len, sizeof(text) - (size_t)len, "%smemory",
                    len ? " " : "");
  if (m & CGROUP_CTRL_PIDS)
    len += snprintf(text + len, sizeof(text) - (size_t)len, "%spids",
                    len ? " " : "");
  len += snprintf(text + len, sizeof(text) - (size_t)len, "\n");
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_subtree_write(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return size;
  uint32_t m = __atomic_load_n(&g->subtree_control, __ATOMIC_RELAXED);
  uint32_t i = 0;
  while (i < size) {
    while (i < size && (buffer[i] == ' ' || buffer[i] == '\t' ||
                        buffer[i] == '\n' || buffer[i] == '\r'))
      i++;
    if (i >= size)
      break;
    bool add = true;
    if (buffer[i] == '+') {
      add = true;
      i++;
    } else if (buffer[i] == '-') {
      add = false;
      i++;
    }
    uint32_t start = i;
    while (i < size && buffer[i] != ' ' && buffer[i] != '\t' &&
           buffer[i] != '\n' && buffer[i] != '\r')
      i++;
    uint32_t len = i - start;
    uint32_t bit = 0;
    if (len == 3 && strncmp((const char *)buffer + start, "cpu", 3) == 0)
      bit = CGROUP_CTRL_CPU;
    else if (len == 6 && strncmp((const char *)buffer + start, "memory", 6) == 0)
      bit = CGROUP_CTRL_MEMORY;
    else if (len == 4 && strncmp((const char *)buffer + start, "pids", 4) == 0)
      bit = CGROUP_CTRL_PIDS;
    if (add)
      m |= bit;
    else
      m &= ~bit;
  }
  __atomic_store_n(&g->subtree_control, m, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_cpu_weight_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[24];
  int len = snprintf(text, sizeof(text), "%u\n",
                     __atomic_load_n(&g->cpu_weight, __ATOMIC_RELAXED));
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_cpu_weight_write(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (!g || !parse_u64_or_max(buffer, size, &val))
    return size;
  if (val < 1)
    val = 1;
  if (val > 10000)
    val = 10000;
  __atomic_store_n(&g->cpu_weight, (uint32_t)val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_cpu_max_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  uint64_t q = __atomic_load_n(&g->cpu_quota_us, __ATOMIC_RELAXED);
  uint64_t p = __atomic_load_n(&g->cpu_period_us, __ATOMIC_RELAXED);
  char text[64];
  int len;
  if (q == UINT64_MAX)
    len = snprintf(text, sizeof(text), "max %llu\n", (unsigned long long)p);
  else
    len = snprintf(text, sizeof(text), "%llu %llu\n", (unsigned long long)q,
                   (unsigned long long)p);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_cpu_max_write(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return size;
  uint64_t quota = UINT64_MAX;
  if (!parse_u64_or_max(buffer, size, &quota))
    return size;
  /* Check for optional second field: period_us */
  uint32_t i = 0;
  while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
    i++;
  while (i < size && buffer[i] != ' ' && buffer[i] != '\t' &&
         buffer[i] != '\n' && buffer[i] != '\r')
    i++;
  while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
    i++;
  if (i < size && buffer[i] >= '0' && buffer[i] <= '9') {
    uint64_t period = 0;
    if (parse_u64_or_max(buffer + i, size - i, &period) &&
        period != UINT64_MAX) {
      if (period < 1000)
        period = 1000;
      if (period > 1000000)
        period = 1000000;
      __atomic_store_n(&g->cpu_period_us, period, __ATOMIC_RELAXED);
    }
  }
  if (quota != UINT64_MAX && quota < 1000)
    quota = 1000;
  __atomic_store_n(&g->cpu_quota_us, quota, __ATOMIC_RELAXED);
  __atomic_store_n(&g->cpu_throttled_until_ns, 0, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_cpu_stat_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[256];
  int len = snprintf(
      text, sizeof(text),
      "usage_usec %llu\nuser_usec %llu\nsystem_usec %llu\n"
      "nr_periods 0\nnr_throttled %llu\nthrottled_usec %llu\n",
      (unsigned long long)(__atomic_load_n(&g->cpu_usage_ns, __ATOMIC_RELAXED) / 1000ULL),
      (unsigned long long)(__atomic_load_n(&g->cpu_user_ns, __ATOMIC_RELAXED) / 1000ULL),
      (unsigned long long)(__atomic_load_n(&g->cpu_system_ns, __ATOMIC_RELAXED) / 1000ULL),
      (unsigned long long)__atomic_load_n(&g->cpu_nr_throttled, __ATOMIC_RELAXED),
      (unsigned long long)(__atomic_load_n(&g->cpu_throttled_ns, __ATOMIC_RELAXED) / 1000ULL));
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_mem_current_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  uint64_t bytes = compute_subtree_mem_bytes(g, NULL);
  char text[32];
  int len = snprintf(text, sizeof(text), "%llu\n", (unsigned long long)bytes);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_mem_min_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  char text[32];
  uint32_t len = g ? format_u64_or_max(text, sizeof(text), g->mem_min) : 0;
  return copy_window(text, len, offset, size, buffer);
}
static uint32_t cgroupfs_mem_min_write(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->mem_min, val == UINT64_MAX ? 0 : val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_mem_low_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  char text[32];
  uint32_t len = g ? format_u64_or_max(text, sizeof(text), g->mem_low) : 0;
  return copy_window(text, len, offset, size, buffer);
}
static uint32_t cgroupfs_mem_low_write(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->mem_low, val == UINT64_MAX ? 0 : val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_mem_high_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  char text[32];
  uint32_t len = g ? format_u64_or_max(text, sizeof(text),
                                       __atomic_load_n(&g->mem_high, __ATOMIC_RELAXED))
                   : 0;
  return copy_window(text, len, offset, size, buffer);
}
static uint32_t cgroupfs_mem_high_write(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = UINT64_MAX;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->mem_high, val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_mem_max_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  char text[32];
  uint32_t len = g ? format_u64_or_max(text, sizeof(text),
                                       __atomic_load_n(&g->mem_max, __ATOMIC_RELAXED))
                   : 0;
  return copy_window(text, len, offset, size, buffer);
}
static uint32_t cgroupfs_mem_max_write(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = UINT64_MAX;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->mem_max, val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_mem_oom_group_read(vfs_node_t *node, uint32_t offset,
                                            uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[8];
  int len = snprintf(text, sizeof(text), "%d\n",
                     __atomic_load_n(&g->mem_oom_group, __ATOMIC_RELAXED) ? 1 : 0);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}
static uint32_t cgroupfs_mem_oom_group_write(vfs_node_t *node, uint32_t offset,
                                             uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = 0;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->mem_oom_group, val != 0, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_mem_events_read(vfs_node_t *node, uint32_t offset,
                                         uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[160];
  int len = snprintf(
      text, sizeof(text),
      "low %llu\nhigh %llu\nmax %llu\noom %llu\noom_kill %llu\n",
      (unsigned long long)__atomic_load_n(&g->mem_events_low, __ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&g->mem_events_high, __ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&g->mem_events_max, __ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&g->mem_events_oom, __ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&g->mem_events_oom_kill, __ATOMIC_RELAXED));
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_mem_stat_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  uint64_t bytes = compute_subtree_mem_bytes(g, NULL);
  char text[64];
  int len = snprintf(text, sizeof(text), "anon %llu\n",
                     (unsigned long long)bytes);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_pids_current_read(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  spinlock_acquire(&members_lock);
  uint32_t cur = walk_members_locked(g, true, NULL, NULL);
  spinlock_release(&members_lock);
  char text[24];
  int len = snprintf(text, sizeof(text), "%u\n", cur);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_pids_max_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  char text[32];
  uint32_t len = g ? format_u64_or_max(text, sizeof(text),
                                       __atomic_load_n(&g->pids_max, __ATOMIC_RELAXED))
                   : 0;
  return copy_window(text, len, offset, size, buffer);
}
static uint32_t cgroupfs_pids_max_write(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  uint64_t val = UINT64_MAX;
  if (g && parse_u64_or_max(buffer, size, &val))
    __atomic_store_n(&g->pids_max, val, __ATOMIC_RELAXED);
  return size;
}

static uint32_t cgroupfs_pids_events_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  char text[32];
  int len = snprintf(
      text, sizeof(text), "max %llu\n",
      (unsigned long long)__atomic_load_n(&g->pids_events_max, __ATOMIC_RELAXED));
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

static uint32_t cgroupfs_cpu_pressure_read(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
  char text[256];
  uint32_t len = psi_format(PSI_CPU, group_of(node), text, sizeof(text));
  return copy_window(text, len, offset, size, buffer);
}

static uint32_t cgroupfs_mem_pressure_read(vfs_node_t *node, uint32_t offset,
                                           uint32_t size, uint8_t *buffer) {
  char text[256];
  uint32_t len = psi_format(PSI_MEMORY, group_of(node), text, sizeof(text));
  return copy_window(text, len, offset, size, buffer);
}

static uint32_t cgroupfs_io_pressure_read(vfs_node_t *node, uint32_t offset,
                                          uint32_t size, uint8_t *buffer) {
  char text[256];
  uint32_t len = psi_format(PSI_IO, group_of(node), text, sizeof(text));
  return copy_window(text, len, offset, size, buffer);
}

void cgroupfs_bind_file(vfs_node_t *file, struct cgroupfs_group *group,
                        enum cgroupfs_file kind) {
  file->ptr = (vfs_node_t *)group;
  file->write = NULL;
  switch (kind) {
  case CGROUPFS_PROCS:
  case CGROUPFS_THREADS:
    file->read = cgroupfs_procs_read;
    file->write = cgroupfs_procs_write;
    break;
  case CGROUPFS_EVENTS:
    file->read = cgroupfs_events_read;
    group->events = file;
    break;
  case CGROUPFS_KILL:
    file->write = cgroupfs_kill_write;
    break;
  case CGROUPFS_FREEZE:
    file->read = cgroupfs_freeze_read;
    file->write = cgroupfs_freeze_write;
    break;
  case CGROUPFS_CONTROLLERS:
    file->read = cgroupfs_controllers_read;
    break;
  case CGROUPFS_SUBTREE_CONTROL:
    file->read = cgroupfs_subtree_read;
    file->write = cgroupfs_subtree_write;
    break;
  case CGROUPFS_CPU_WEIGHT:
    file->read = cgroupfs_cpu_weight_read;
    file->write = cgroupfs_cpu_weight_write;
    break;
  case CGROUPFS_CPU_MAX:
    file->read = cgroupfs_cpu_max_read;
    file->write = cgroupfs_cpu_max_write;
    break;
  case CGROUPFS_CPU_STAT:
    file->read = cgroupfs_cpu_stat_read;
    break;
  case CGROUPFS_CPU_PRESSURE:
    file->read = cgroupfs_cpu_pressure_read;
    break;
  case CGROUPFS_MEM_CURRENT:
    file->read = cgroupfs_mem_current_read;
    break;
  case CGROUPFS_MEM_MIN:
    file->read = cgroupfs_mem_min_read;
    file->write = cgroupfs_mem_min_write;
    break;
  case CGROUPFS_MEM_LOW:
    file->read = cgroupfs_mem_low_read;
    file->write = cgroupfs_mem_low_write;
    break;
  case CGROUPFS_MEM_HIGH:
    file->read = cgroupfs_mem_high_read;
    file->write = cgroupfs_mem_high_write;
    break;
  case CGROUPFS_MEM_MAX:
    file->read = cgroupfs_mem_max_read;
    file->write = cgroupfs_mem_max_write;
    break;
  case CGROUPFS_MEM_OOM_GROUP:
    file->read = cgroupfs_mem_oom_group_read;
    file->write = cgroupfs_mem_oom_group_write;
    break;
  case CGROUPFS_MEM_EVENTS:
    file->read = cgroupfs_mem_events_read;
    break;
  case CGROUPFS_MEM_STAT:
    file->read = cgroupfs_mem_stat_read;
    break;
  case CGROUPFS_MEM_PRESSURE:
    file->read = cgroupfs_mem_pressure_read;
    break;
  case CGROUPFS_PIDS_CURRENT:
    file->read = cgroupfs_pids_current_read;
    break;
  case CGROUPFS_PIDS_MAX:
    file->read = cgroupfs_pids_max_read;
    file->write = cgroupfs_pids_max_write;
    break;
  case CGROUPFS_PIDS_EVENTS:
    file->read = cgroupfs_pids_events_read;
    break;
  case CGROUPFS_IO_PRESSURE:
    file->read = cgroupfs_io_pressure_read;
    break;
  }
}

void cgroupfs_fork(struct thread *parent, struct thread *child) {
  if (!parent || !child)
    return;
  uint32_t ptgid = parent->tgid ? parent->tgid : parent->tid;
  uint32_t ctgid = child->tgid ? child->tgid : child->tid;
  struct cgroupfs_member *spare = kmalloc(sizeof(struct cgroupfs_member));
  spinlock_acquire(&members_lock);
  struct cgroupfs_group *group = NULL;
  for (struct cgroupfs_member *m = *bucket_of(ptgid); m; m = m->next) {
    if (m->tgid == ptgid) {
      group = m->group;
      break;
    }
  }
  attach_locked(ctgid, group, spare);
  spinlock_release(&members_lock);
  __atomic_store_n(&child->cgroup, group, __ATOMIC_RELEASE);
}

int cgroupfs_clone_into(struct thread *child, vfs_node_t *dir) {
  struct cgroupfs_group *group = tmpfs_cgroup_of(dir);
  if (!group || group->removed)
    return -9; /* EBADF: not a cgroup2 directory */
  move_process(child->tgid ? child->tgid : child->tid, group);
  return 0;
}
