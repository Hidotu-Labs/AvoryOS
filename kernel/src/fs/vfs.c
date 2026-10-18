#include "vfs.h"
#include "mount_namespace.h"
#include "../sched/capability.h"
#include "vfs_rcu.h"
#include "dirindex.h"
#include "inotify.h"
#include "../console/klog.h"
#include "../cpu/tsc.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../syscalls/sysdiag.h"
#include "hal/hal.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

vfs_node_t *fs_root = 0;

bool vfs_in_group(uint32_t gid) {
  struct thread *t = sched_get_current();
  if (!t) return gid == 0;
  if (t->fsgid == gid || t->egid == gid || t->gid == gid) return true;
  for (uint32_t i = 0; i < t->supplementary_group_count; i++)
    if (t->supplementary_groups[i] == gid) return true;
  return false;
}

bool vfs_access(vfs_node_t *node, uint32_t requested) {
  if (!node) return false;
  if ((requested & 1) && (node->flags & FS_TYPE_MASK) != FS_DIRECTORY &&
      (vfs_mount_flags(node) & 8)) return false;
  /* Fast path: every class grants `requested` (common for 0755/0777 dirs on
   * a path walk). No thread lookup, no supplementary-group loop. */
  uint32_t o = (node->mask >> 6) & 7;
  uint32_t g = (node->mask >> 3) & 7;
  uint32_t u = node->mask & 7;
  if (((o & requested) == requested) && ((g & requested) == requested) &&
      ((u & requested) == requested)) {
    if ((requested & 1) && !(node->mask & 0111) &&
        (node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return false;
    return true;
  }
  struct thread *t = sched_get_current();
  if (!t || capability_has(t, CAP_DAC_OVERRIDE) ||
      (!(requested & 2) && capability_has(t, CAP_DAC_READ_SEARCH))) {
    if ((requested & 1) && !(node->mask & 0111) &&
        (node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return false;
    return true;
  }
  uint32_t bits = node->mask & 7;
  if (t->fsuid == node->uid) bits = (node->mask >> 6) & 7;
  else if (vfs_in_group(node->gid)) bits = (node->mask >> 3) & 7;
  return (bits & requested) == requested;
}

/* Conservative ".." test matching the old strstr(path, "..") call sites: any
 * two consecutive dots mean "skip the caches". Cheaper than strstr (single
 * forward scan, no substring table), computed once per resolve and reused. */
static bool vfs_path_has_dotdot(const char *path) {
  if (!path) return false;
  char prev = '\0';
  for (const char *p = path; *p; p++) {
    if (*p == '.' && prev == '.') return true;
    prev = *p;
  }
  return false;
}

bool vfs_may_remove(vfs_node_t *parent, vfs_node_t *target) {
  struct thread *t = sched_get_current();
  if (!t || capability_has(t, CAP_FOWNER)) return true;
  if (!vfs_access(parent, 3)) return false;
  if ((parent->mask & 01000) && t->fsuid != parent->uid &&
      (!target || t->fsuid != target->uid)) return false;
  return true;
}

typedef struct vfs_mount_entry {
  uint64_t mount_id;
  vfs_node_t *mountpoint;
  vfs_node_t *target;
  char dev_name[64];
  char fs_type[32];
  /* The mountpoint as userspace sees it ("/run", "/tmp", ...).  /proc/mounts
   * must report this rather than the mountpoint node's bare name: tools like
   * OpenRC's mountinfo compare the second field against a full path, and with
   * only "run" they would mount a fresh tmpfs over the kernel's /run. */
  char path[256];
  struct vfs_mount_entry *next;
} vfs_mount_entry_t;

static vfs_mount_entry_t *vfs_mount_list = NULL;
static uint64_t vfs_next_mount_id = 18;

/* Bounded 4-way dentry cache. Positive entries own a child reference;
 * positive and negative entries both own a parent reference. */
#define VFS_DENTRY_CACHE_SIZE 16384
#define VFS_DENTRY_WAYS 4
#define VFS_DENTRY_BUCKETS (VFS_DENTRY_CACHE_SIZE / VFS_DENTRY_WAYS)
/* Bucket counts are powers of two, so indexing uses a mask (one AND) instead
 * of a modulo (a division). Same result for unsigned hashes, runs per path
 * component on every open/stat. */
#define VFS_DENTRY_BUCKET_MASK (VFS_DENTRY_BUCKETS - 1)
_Static_assert((VFS_DENTRY_BUCKETS & (VFS_DENTRY_BUCKETS - 1)) == 0,
               "VFS_DENTRY_BUCKETS must be a power of two");

typedef struct vfs_dentry_cache_entry {
  vfs_node_t *parent;
  vfs_node_t *child;
  uint32_t hash;
  uint16_t name_len;
  bool valid;
  char name[128];
} vfs_dentry_cache_entry_t;

typedef struct vfs_dentry_cache_bucket {
  vfs_dentry_cache_entry_t entries[VFS_DENTRY_WAYS];
  uint32_t seq;
  uint8_t next_victim;
  spinlock_t lock;
} vfs_dentry_cache_bucket_t;

static vfs_dentry_cache_bucket_t vfs_dentry_cache[VFS_DENTRY_BUCKETS];

/* Find cold cache pages beyond the allocating process's mapped files. Cache
 * entries already own the child references; pin under their bucket lock and
 * reclaim outside it. Never wait for a bucket held by the OOM caller. */
size_t vfs_dentry_reclaim_pages(size_t target) {
  static uint32_t cursor, active;
  if (!target || __atomic_exchange_n(&active, 1, __ATOMIC_ACQUIRE))
    return 0;
  size_t reclaimed = 0;
  for (uint32_t scanned = 0;
       scanned < VFS_DENTRY_BUCKETS && reclaimed < target; scanned++) {
    uint32_t slot = __atomic_fetch_add(&cursor, 1, __ATOMIC_RELAXED) &
                    VFS_DENTRY_BUCKET_MASK;
    vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
    if (!spinlock_try_acquire(&bucket->lock))
      continue;
    vfs_node_t *nodes[VFS_DENTRY_WAYS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_node_t *node = bucket->entries[i].child;
      if (!bucket->entries[i].valid || !node ||
          (node->flags & FS_TYPE_MASK) != FS_FILE)
        continue;
      bool duplicate = false;
      for (uint32_t j = 0; j < count; j++)
        if (nodes[j] == node) duplicate = true;
      if (!duplicate && vfs_node_tryref(node))
        nodes[count++] = node;
    }
    spinlock_release(&bucket->lock);
    for (uint32_t i = 0; i < count; i++) {
      if (reclaimed < target)
        reclaimed += vfs_cache_reclaim(nodes[i], target - reclaimed);
      vfs_close(nodes[i]);
    }
  }
  __atomic_store_n(&active, 0, __ATOMIC_RELEASE);
  return reclaimed;
}

static uint32_t vfs_dentry_hash(vfs_node_t *parent, const char *name) {
  uint64_t hash = ((uint64_t)(uintptr_t)parent >> 4) ^ 1469598103934665603ULL;
  while (*name) {
    hash ^= (uint8_t)*name++;
    hash *= 1099511628211ULL;
  }
  return (uint32_t)(hash ^ (hash >> 32));
}

/* Bench-only switch: when set, lookups take the pre-RCU locked path so
 * `vfs_bench=1` can run the old and new forms of the same lookup back to
 * back.  Never set outside the benchmark. */
static bool vfs_dentry_lookup_force_locked;

static vfs_node_t *vfs_dentry_lookup_locked(vfs_node_t *parent, const char *name,
                                            bool *negative) {
  *negative = false;
  if (!parent || !name || (parent->flags & FS_DENTRY_NOCACHE))
    return NULL;

  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash & VFS_DENTRY_BUCKET_MASK;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->name_len == len &&
        entry->parent == parent && memcmp(entry->name, name, len) == 0) {
      vfs_node_t *child = entry->child;
      if (child)
        vfs_node_ref(child);
      else
        *negative = true;
      spinlock_release(&bucket->lock);
      return child;
    }
  }
  spinlock_release(&bucket->lock);
  return NULL;
}

static vfs_node_t *vfs_dentry_lookup(vfs_node_t *parent, const char *name,
                                     bool *negative) {
  *negative = false;
  if (!parent || !name || (parent->flags & FS_DENTRY_NOCACHE))
    return NULL;

  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return NULL;

  if (vfs_dentry_lookup_force_locked || !vfs_rcu_ready())
    return vfs_dentry_lookup_locked(parent, name, negative);

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  vfs_dentry_cache_bucket_t *bucket =
      &vfs_dentry_cache[hash & VFS_DENTRY_BUCKET_MASK];

  /* Lockless seqlock read.  Writers mutate entries with `seq` odd and bump it
   * again when done, so a scan that starts and ends on the same even value
   * crossed no writer and the fields it saw are consistent.  The scan never
   * dereferences `child`; the pointer is only handed to tryref, which is safe
   * because a node's last kfree() is deferred until a grace period has passed
   * (fs/vfs_rcu.h).  The whole section runs with interrupts off so an IPI
   * cannot be acknowledged while a raw pointer is held - that is the property
   * the drainer's barrier relies on. */
  vfs_node_t *result = NULL;
  int action = 0; /* 0 = locked fallback, 1 = miss, 2 = negative, 3 = node */
  hal_irq_state_t flags = hal_irq_save();
  for (uint32_t attempt = 0; attempt < 2 && action == 0; attempt++) {
    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1)
      break; /* writer in flight: let the locked path serialize */

    uint32_t state = 0; /* 0 miss, 1 positive, 2 negative */
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
      if (!__atomic_load_n(&entry->valid, __ATOMIC_RELAXED))
        continue;
      if (__atomic_load_n(&entry->hash, __ATOMIC_RELAXED) != hash)
        continue;
      if (__atomic_load_n(&entry->parent, __ATOMIC_RELAXED) != parent)
        continue;
      uint16_t entry_len = __atomic_load_n(&entry->name_len, __ATOMIC_RELAXED);
      if (entry_len != len)
        continue;
      if (memcmp(entry->name, name, len) != 0)
        continue;
      result = __atomic_load_n(&entry->child, __ATOMIC_RELAXED);
      state = result ? 1u : 2u;
      break;
    }
    if (__atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE) != s1)
      continue; /* crossed a writer: retry once, then fall back under the lock */
    if (state == 0)
      action = 1;
    else if (state == 2)
      action = 2;
    else if (vfs_node_tryref(result))
      action = 3;
    /* tryref failed: the entry's reference dropped while scanning.  The
     * entry was invalidated concurrently; the locked lookup re-checks it and
     * either refinds it or reports the miss. */
  }
  hal_irq_restore(flags);

  if (action == 3)
    return result;
  if (action == 1)
    return NULL;
  if (action == 2) {
    *negative = true;
    return NULL;
  }
  return vfs_dentry_lookup_locked(parent, name, negative);
}

static void vfs_dentry_insert(vfs_node_t *parent, const char *name,
                              vfs_node_t *child) {
  if (!parent || !name)
    return;

  size_t len_sz = strlen(name);
  if (len_sz >= 128 || (parent->flags & FS_DENTRY_NOCACHE))
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash & VFS_DENTRY_BUCKET_MASK;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
  vfs_node_t *evicted = NULL;
  vfs_node_t *evicted_parent = NULL;

  spinlock_acquire(&bucket->lock);
  vfs_dentry_cache_entry_t *entry = NULL;
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *candidate = &bucket->entries[i];
    if (candidate->valid && candidate->hash == hash && candidate->name_len == len &&
        candidate->parent == parent && memcmp(candidate->name, name, len) == 0) {
      spinlock_release(&bucket->lock);
      return;
    }
    if (!candidate->valid && !entry)
      entry = candidate;
  }
  if (!entry) {
    entry = &bucket->entries[bucket->next_victim];
    bucket->next_victim = (bucket->next_victim + 1) % VFS_DENTRY_WAYS;
  }
  if (entry->valid) {
    evicted = entry->child;
    evicted_parent = entry->parent;
  }
  vfs_node_ref(parent);
  if (child)
    vfs_node_ref(child);

  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  entry->parent = parent;
  entry->child = child;
  entry->hash = hash;
  entry->name_len = len;
  memcpy(entry->name, name, len);
  entry->name[len] = '\0';
  entry->valid = true;
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);

  spinlock_release(&bucket->lock);

  if (evicted)
    vfs_close(evicted);
  if (evicted_parent)
    vfs_close(evicted_parent);
}

/* Fast-Path Full Path Resolution Cache (Path Dcache)
 * Maps (base_dir, path_string) -> resolved_vfs_node.
 * Directly bypasses string parsing, loops, and per-component lookup overhead
 * for high-frequency access to binaries, libs, JVM classes, and assets.
 */
#define VFS_PATH_CACHE_SIZE 4096
#define VFS_PATH_CACHE_WAYS 4
#define VFS_PATH_CACHE_BUCKETS (VFS_PATH_CACHE_SIZE / VFS_PATH_CACHE_WAYS)
#define VFS_PATH_CACHE_BUCKET_MASK (VFS_PATH_CACHE_BUCKETS - 1)
_Static_assert((VFS_PATH_CACHE_BUCKETS & (VFS_PATH_CACHE_BUCKETS - 1)) == 0,
               "VFS_PATH_CACHE_BUCKETS must be a power of two");

typedef struct vfs_path_cache_entry {
  vfs_node_t *dir;
  vfs_node_t *node;
  uint32_t hash;
  uint16_t path_len;
  bool valid;
  uint64_t gen;       // Invalidation generation this entry was last valid in
  char path[256];
} vfs_path_cache_entry_t;

typedef struct vfs_path_cache_bucket {
  vfs_path_cache_entry_t entries[VFS_PATH_CACHE_WAYS];
  uint32_t seq;
  uint8_t next_victim;
  spinlock_t lock;
} vfs_path_cache_bucket_t;

static vfs_path_cache_bucket_t vfs_path_cache[VFS_PATH_CACHE_BUCKETS];

/* ── Path-cache invalidation log ─────────────────────────────────────────
 *
 * A deletion or rename used to walk all 1024 path-cache buckets, take each
 * bucket lock with interrupts masked and run strstr() on every cached path
 * string.  Deletions are common (build systems, package installs, temporary
 * files) and the cost was paid even when no cached path mentioned the removed
 * name.
 *
 * Instead, every invalidation appends the removed component name to a small
 * generation-stamped ring.  Each path-cache entry remembers the generation it
 * was last validated against; a lookup whose entry predates the current
 * generation replays only the records appended since (at most
 * VFS_PATH_INVAL_LOG_SIZE), using the same component-boundary test the old
 * full scan used.  Entries older than the ring are treated as invalid, so the
 * answer is always the conservative one the scan produced.
 *
 * Readers take the log lock only while replaying; a lookup of a
 * current-generation entry never touches it, and the invalidating writer does
 * no cache walk at all. */
#define VFS_PATH_INVAL_LOG_SIZE 32
#define VFS_PATH_INVAL_NAME_MAX 256

typedef struct vfs_path_inval_record {
  uint64_t gen;
  char name[VFS_PATH_INVAL_NAME_MAX];
} vfs_path_inval_record_t;

static vfs_path_inval_record_t vfs_path_inval_log[VFS_PATH_INVAL_LOG_SIZE];
static uint64_t vfs_path_inval_gen;
static spinlock_t vfs_path_inval_lock = SPINLOCK_INIT;

/* Bench-only switch: when set, removals take the pre-log full-table scan so
 * `vfs_bench=1` can run the old and new forms of the same syscall back to
 * back.  It is never set outside the benchmark. */
static bool vfs_path_invalidation_force_scan;

/* True when `name` appears in `path` as a whole path component.  This mirrors
 * the boundary test the old full-table invalidation scan applied. */
static bool vfs_path_component_match(const char *path, const char *name,
                                     size_t name_len) {
  const char *p = path;
  while ((p = strstr(p, name)) != NULL) {
    bool left_bound = (p == path || *(p - 1) == '/');
    bool right_bound = (p[name_len] == '\0' || p[name_len] == '/');
    if (left_bound && right_bound)
      return true;
    p++;
  }
  return false;
}

/* Append one invalidation for `name`.  O(1): no bucket is visited. */
static void vfs_path_invalidate_record(const char *name) {
  size_t name_len = strlen(name);
  if (name_len == 0 || name_len >= VFS_PATH_INVAL_NAME_MAX)
    return;

  spinlock_acquire(&vfs_path_inval_lock);
  uint64_t gen = ++vfs_path_inval_gen;
  vfs_path_inval_record_t *record =
      &vfs_path_inval_log[gen % VFS_PATH_INVAL_LOG_SIZE];
  record->gen = 0;
  memcpy(record->name, name, name_len);
  record->name[name_len] = '\0';
  record->gen = gen;
  spinlock_release(&vfs_path_inval_lock);
}

/* Validate `entry` against the invalidations recorded since its last check.
 * Runs under the entry's bucket lock and may update entry->gen. */
static bool vfs_path_entry_validate_locked(vfs_path_cache_entry_t *entry) {
  uint64_t gen = __atomic_load_n(&vfs_path_inval_gen, __ATOMIC_ACQUIRE);
  if (entry->gen == gen)
    return true;
  /* Older than the ring (or somehow ahead of it): cannot be proven. */
  if (entry->gen > gen || gen - entry->gen > VFS_PATH_INVAL_LOG_SIZE)
    return false;

  bool valid = true;
  spinlock_acquire(&vfs_path_inval_lock);
  for (uint64_t g = entry->gen + 1; g <= gen; g++) {
    vfs_path_inval_record_t *record =
        &vfs_path_inval_log[g % VFS_PATH_INVAL_LOG_SIZE];
    if (record->gen != g) {
      /* The ring moved past this record without it ever being visible. */
      valid = false;
      break;
    }
    size_t name_len = strlen(record->name);
    if (name_len && vfs_path_component_match(entry->path, record->name,
                                             name_len)) {
      valid = false;
      break;
    }
  }
  spinlock_release(&vfs_path_inval_lock);

  if (valid)
    entry->gen = gen;
  return valid;
}

static uint32_t vfs_path_hash(vfs_node_t *dir, const char *path) {
  uint64_t hash = ((uint64_t)(uintptr_t)dir >> 4) ^ 0xcbf29ce484222325ULL;
  while (*path) {
    hash ^= (uint8_t)*path++;
    hash *= 0x100000001b3ULL;
  }
  return (uint32_t)(hash ^ (hash >> 32));
}

/* Bench-only switch: same purpose as vfs_dentry_lookup_force_locked, for the
 * full-path cache.  Never set outside the benchmark. */
static bool vfs_path_lookup_force_locked;

static vfs_node_t *vfs_path_cache_lookup_locked(vfs_node_t *dir,
                                                const char *path) {
  if (!dir || !path)
    return NULL;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash & VFS_PATH_CACHE_BUCKET_MASK;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];

  /* Pin the cached node while eviction is excluded, as in dentry lookup.
   * The lockless fast path below relies on the invalidation generation; this
   * one may replay the log under the bucket lock, which also retires an entry
   * a removal has overtaken. */
  vfs_node_t *stale_node = NULL;
  vfs_node_t *stale_dir = NULL;

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->path_len == len &&
        entry->dir == dir && memcmp(entry->path, path, len) == 0) {
      if (!vfs_path_entry_validate_locked(entry)) {
        /* A deletion since this entry was cached covers it: retire the
         * entry and let the caller resolve the path from scratch. */
        stale_node = entry->node;
        stale_dir = entry->dir;
        __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
        __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
        break;
      }
      vfs_node_t *node = entry->node;
      vfs_node_ref(node);
      spinlock_release(&bucket->lock);
      return node;
    }
  }
  spinlock_release(&bucket->lock);

  if (stale_node)
    vfs_close(stale_node);
  if (stale_dir)
    vfs_close(stale_dir);
  return NULL;
}

static vfs_node_t *vfs_path_cache_lookup(vfs_node_t *dir, const char *path) {
  if (!dir || !path)
    return NULL;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return NULL;

  if (vfs_path_lookup_force_locked || !vfs_rcu_ready())
    return vfs_path_cache_lookup_locked(dir, path);

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  vfs_path_cache_bucket_t *bucket =
      &vfs_path_cache[hash & VFS_PATH_CACHE_BUCKET_MASK];

  /* Same seqlock read as the dentry cache, plus the invalidation-generation
   * test: only entries already validated against every recorded invalidation
   * are served locklessly (see vfs_path_entry_validate_locked for the replay
   * rules).  A stale entry falls back to the locked path, which replays the
   * log and re-stamps the generation, so an invalidation costs one locked
   * lookup per cache entry, not a permanent loss of the fast path. */
  vfs_node_t *result = NULL;
  int action = 0; /* 0 = locked fallback, 1 = miss, 3 = node */
  hal_irq_state_t flags = hal_irq_save();
  for (uint32_t attempt = 0; attempt < 2 && action == 0; attempt++) {
    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1)
      break;

    bool found = false;
    uint64_t entry_gen = 0;
    for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
      vfs_path_cache_entry_t *entry = &bucket->entries[i];
      if (!__atomic_load_n(&entry->valid, __ATOMIC_RELAXED))
        continue;
      if (__atomic_load_n(&entry->hash, __ATOMIC_RELAXED) != hash)
        continue;
      if (__atomic_load_n(&entry->dir, __ATOMIC_RELAXED) != dir)
        continue;
      uint16_t entry_len = __atomic_load_n(&entry->path_len, __ATOMIC_RELAXED);
      if (entry_len != len)
        continue;
      if (memcmp(entry->path, path, len) != 0)
        continue;
      result = __atomic_load_n(&entry->node, __ATOMIC_RELAXED);
      entry_gen = __atomic_load_n(&entry->gen, __ATOMIC_RELAXED);
      found = true;
      break;
    }
    if (__atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE) != s1)
      continue;
    if (!found) {
      action = 1;
      break;
    }
    /* An entry that predates the current generation needs the log replay the
     * locked path performs (and may be retired by it).  Reading the generation
     * after the scan only makes the fallback more eager. */
    if (entry_gen != __atomic_load_n(&vfs_path_inval_gen, __ATOMIC_ACQUIRE))
      break;
    if (result && vfs_node_tryref(result))
      action = 3;
    break; /* entry dropped while scanning: the locked path re-checks */
  }
  hal_irq_restore(flags);

  if (action == 3)
    return result;
  if (action == 1)
    return NULL;
  return vfs_path_cache_lookup_locked(dir, path);
}

static void vfs_path_cache_insert(vfs_node_t *dir, const char *path, vfs_node_t *node) {
  if (!dir || !node || !path || (node->flags & FS_DENTRY_NOCACHE))
    return;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash & VFS_PATH_CACHE_BUCKET_MASK;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
  vfs_node_t *evicted_node = NULL;
  vfs_node_t *evicted_dir = NULL;

  spinlock_acquire(&bucket->lock);
  vfs_path_cache_entry_t *entry = NULL;
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *candidate = &bucket->entries[i];
    if (candidate->valid && candidate->hash == hash && candidate->path_len == len &&
        candidate->dir == dir && memcmp(candidate->path, path, len) == 0) {
      spinlock_release(&bucket->lock);
      return;
    }
    if (!candidate->valid && !entry)
      entry = candidate;
  }
  if (!entry) {
    entry = &bucket->entries[bucket->next_victim];
    bucket->next_victim = (bucket->next_victim + 1) % VFS_PATH_CACHE_WAYS;
  }
  if (entry->valid) {
    evicted_node = entry->node;
    evicted_dir = entry->dir;
  }

  vfs_node_ref(dir);
  vfs_node_ref(node);
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  entry->dir = dir;
  entry->node = node;
  entry->hash = hash;
  entry->path_len = len;
  memcpy(entry->path, path, len);
  entry->path[len] = '\0';
  entry->gen = __atomic_load_n(&vfs_path_inval_gen, __ATOMIC_ACQUIRE);
  entry->valid = true;
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  spinlock_release(&bucket->lock);

  if (evicted_node)
    vfs_close(evicted_node);
  if (evicted_dir)
    vfs_close(evicted_dir);
}

/*
 * Invalidate the single dentry bucket that can hold (parent, name).
 *
 * vfs_dentry_insert() hashes on (parent, name) and only ever replaces entries
 * inside that one bucket, so an entry for a given pair can never live anywhere
 * else.  Scanning all VFS_DENTRY_BUCKETS for it is therefore pure waste, and it
 * is waste paid on every create/unlink/rename - thousands of strcmp() calls
 * against 128-byte name buffers with IRQs masked per bucket.
 */
static void vfs_dentry_bucket_invalidate(vfs_node_t *parent, const char *name) {
  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash & VFS_DENTRY_BUCKET_MASK;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
  vfs_node_t *released[VFS_DENTRY_WAYS];
  vfs_node_t *released_parents[VFS_DENTRY_WAYS];
  uint32_t released_count = 0;

  spinlock_acquire(&bucket->lock);
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->name_len == len &&
        entry->parent == parent && memcmp(entry->name, name, len) == 0) {
      released[released_count] = entry->child;
      released_parents[released_count++] = entry->parent;
      entry->valid = false;
      entry->parent = NULL;
      entry->child = NULL;
      entry->hash = 0;
      entry->name_len = 0;
      entry->name[0] = '\0';
    }
  }
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  spinlock_release(&bucket->lock);

  for (uint32_t i = 0; i < released_count; i++) {
    if (released[i])
      vfs_close(released[i]);
    vfs_close(released_parents[i]);
  }
}

/* Drop the exact (dir, path) fast-path resolution, if present. */
void vfs_path_cache_drop(vfs_node_t *dir, const char *path) {
  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash & VFS_PATH_CACHE_BUCKET_MASK;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
  vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
  vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
  uint32_t count = 0;

  spinlock_acquire(&bucket->lock);
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->path_len == len &&
        entry->dir == dir && memcmp(entry->path, path, len) == 0) {
      rel_node[count] = entry->node;
      rel_dir[count++] = entry->dir;
      entry->valid = false;
      entry->dir = NULL;
      entry->node = NULL;
      entry->hash = 0;
      entry->path_len = 0;
      entry->path[0] = '\0';
    }
  }
  __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
  spinlock_release(&bucket->lock);

  for (uint32_t i = 0; i < count; i++) {
    vfs_close(rel_node[i]);
    vfs_close(rel_dir[i]);
  }
}

void vfs_path_cache_invalidate(void) {
  for (uint32_t b = 0; b < VFS_PATH_CACHE_BUCKETS; b++) {
    vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
    vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
    uint32_t count = 0;
    vfs_path_cache_bucket_t *bucket = &vfs_path_cache[b];

    spinlock_acquire(&bucket->lock);
    __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
      vfs_path_cache_entry_t *entry = &bucket->entries[i];
      if (entry->valid) {
        rel_node[count] = entry->node;
        rel_dir[count++] = entry->dir;
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
      }
    }
    __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < count; i++) {
      vfs_close(rel_node[i]);
      vfs_close(rel_dir[i]);
    }
  }
}

/*
 * The invalidation scan this cache used before the generation log existed.
 * Unlink/rmdir/rename no longer use it: every removal now only appends to
 * vfs_path_inval_log.  It is kept so `vfs_bench=1` can measure the old and
 * new forms back to back on the same working set, the way serial_bench
 * contrasts the old tick policy with the new one.  Do not call it on the
 * live path: it walks all 1024 buckets and runs strstr() per entry.
 */
static void vfs_path_cache_invalidate_name_scan(vfs_node_t *parent,
                                                const char *name) {
  if (!name || name[0] == '\0')
    return;

  size_t nlen = strlen(name);
  if (nlen >= 256)
    return;

  for (uint32_t b = 0; b < VFS_PATH_CACHE_BUCKETS; b++) {
    vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
    vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
    uint32_t count = 0;
    vfs_path_cache_bucket_t *bucket = &vfs_path_cache[b];

    spinlock_acquire(&bucket->lock);
    for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
      vfs_path_cache_entry_t *entry = &bucket->entries[i];
      if (!entry->valid)
        continue;

      bool match = false;
      if (entry->dir == parent && entry->path_len == nlen &&
          memcmp(entry->path, name, nlen) == 0) {
        match = true;
      } else if (vfs_path_component_match(entry->path, name, nlen)) {
        match = true;
      }

      if (match) {
        rel_node[count] = entry->node;
        rel_dir[count++] = entry->dir;
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
      }
    }
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < count; i++) {
      vfs_close(rel_node[i]);
      vfs_close(rel_dir[i]);
    }
  }
}

void vfs_dentry_invalidate(vfs_node_t *parent, const char *name) {
  /* With a concrete parent and name, the dentry cache needs only the single
   * bucket holding (parent, name). The path cache gets a log record so all
   * aliases (absolute paths, subpaths) fail closed on next lookup, plus an
   * O(1) exact drop of (parent, name) so unlinked single-component temp
   * files (openat/fchmodat via dirfd) release their pinned node+pages
   * immediately instead of lingering until eviction. A NULL argument means
   * "everything", which genuinely does need the full table scan. */
  if (parent && name) {
    if (vfs_path_invalidation_force_scan)
      vfs_path_cache_invalidate_name_scan(parent, name);
    else {
      vfs_path_invalidate_record(name);
      vfs_path_cache_drop(parent, name);
    }
    vfs_dentry_bucket_invalidate(parent, name);
    return;
  }

  vfs_path_cache_invalidate();

  for (uint32_t b = 0; b < VFS_DENTRY_BUCKETS; b++) {
    vfs_node_t *released[VFS_DENTRY_WAYS];
    vfs_node_t *released_parents[VFS_DENTRY_WAYS];
    uint32_t released_count = 0;
    vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[b];

    spinlock_acquire(&bucket->lock);
    __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
      if (entry->valid && (!parent || entry->parent == parent) &&
          (!name || strcmp(entry->name, name) == 0)) {
        released[released_count] = entry->child;
        released_parents[released_count++] = entry->parent;
        entry->valid = false;
        entry->parent = NULL;
        entry->child = NULL;
        entry->hash = 0;
        entry->name_len = 0;
        entry->name[0] = '\0';
      }
    }
    __atomic_add_fetch(&bucket->seq, 1, __ATOMIC_RELEASE);
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < released_count; i++) {
      if (released[i])
        vfs_close(released[i]);
      vfs_close(released_parents[i]);
    }
  }
}


/*
 * Invalidate after a name was *created* (create/mkdir/mknod/symlink returned
 * success).
 *
 * Every filesystem in this kernel rejects these operations when the name is
 * already taken, so a successful create cannot turn an existing successful
 * resolution stale: the path cache stores only successful resolutions, and a
 * cached resolution of this very name would have made the create fail.  What
 * does have to go is the negative dentry recorded for (parent, name) by the
 * lookup that proved the name was free - otherwise the new file stays invisible
 * until that entry is evicted.
 *
 * This matters because creating a file used to throw away the entire path
 * cache and re-scan all 4096 dentry buckets, which erased the "fast open" win
 * for every binary and shared library in the session.  Removals (unlink/rmdir/
 * rename) keep the conservative full invalidation: a removed name really can
 * invalidate resolutions anchored at any of its ancestor directories.
 */
void vfs_dentry_invalidate_created(vfs_node_t *parent, const char *name) {
  if (!parent || !name) {
    vfs_dentry_invalidate(parent, name);
    return;
  }

  vfs_dentry_bucket_invalidate(parent, name);
  vfs_path_cache_drop(parent, name);
}


uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (!node)
    return 0;

  if ((node->flags & (FS_TYPE_MASK | FS_PAGE_CACHE)) ==
      (FS_FILE | FS_PAGE_CACHE))
    return vfs_cache_read(node, offset, size, buffer);

  if (node->read) {
    return node->read(node, offset, size, buffer);
  }
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  if (!node)
    return (uint32_t)-9; // EBADF

  if (size == 0)
    return 0;

  struct thread *t = sched_get_current();
  if (t && t->euid != 0 && (node->mask & 06000))
    vfs_chmod(node, (uint16_t)(node->mask & ~06000));

  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY)
    return (uint32_t)-21; // EISDIR

  if (node->write) {
    uint32_t written = node->write(node, offset, size, buffer);
    if (written && written <= size &&
        ((node->flags & FS_PAGE_CACHE) || node->mmap_shared_only))
      vfs_cache_update_or_invalidate(node, offset, written, buffer);
    return written;
  }
  if (node->flags & FS_DENTRY_NOCACHE) {
    sysdiag_record_missing_fs_node(node->name, t, -30 /* -EROFS */, "write");
  }
  return (uint32_t)-9; // EBADF: node has no write operation
}

void vfs_open(vfs_node_t *node) {
  if (!node || node == (vfs_node_t *)-1)
    return;
  vfs_node_ref(node);
  if (node->open) {
    node->open(node);
  }
}

void vfs_close(vfs_node_t *node) {
  if (!node || node == (vfs_node_t *)-1)
    return;

  bool is_pipe = (node->flags & FS_TYPE_MASK) == FS_PIPE;
  void *wq = node->wait_queue;

  if (vfs_node_unref(node)) {
    /* A namespace view's close drops its backing inode and parent path. The
     * last parent can destroy an ephemeral filesystem, so flush the view's
     * mapping cache while those references still keep the backing alive. */
    if (node->view_backing) {
      vfs_cache_sync(node);
      vfs_cache_clear(node);
    }
    if (node->close) {
      node->close(node);
    }
    vfs_cache_sync(node);
    vfs_cache_clear(node);
    spinlock_acquire(&node->ep_lock);
    INIT_LIST_HEAD(&node->ep_watchers);
    spinlock_release(&node->ep_lock);
    /* A node whose name was removed while descriptors still referenced it:
     * the last close releases the filesystem-private payload exactly once. */
    if ((node->flags & (FS_DELETED | FS_AUTODESTROY)) && node->destroy) {
      node->destroy(node);
      node->destroy = NULL;
    }
    if (!(node->flags & FS_PERSISTENT) || (node->flags & FS_DELETED)) {
      node->magic = 0;
      vfs_xattr_clear(node);
      /* The last kfree is deferred: lockless cache readers may still hold this
       * pointer between an entry scan and their tryref (fs/vfs_rcu.h). */
      vfs_rcu_defer_free(node);
    }
    return;
  }

  // For pipes, wake blocked readers/writers AFTER decrementing refcount,
  // so they re-check and detect EOF (refcount <= 1).
  // Only do this when refcount > 0 (node is still alive).
  if (is_pipe && wq) {
    extern void wait_queue_wake_all(void *wq);
    wait_queue_wake_all(wq);
  }
}

void vfs_node_retire(vfs_node_t *node) {
  if (!node)
    return;

  node->flags |= FS_DELETED;
  if (__atomic_load_n(&node->refcount, __ATOMIC_ACQUIRE) != 0)
    return; /* the last vfs_close() finishes the teardown */

  vfs_cache_clear(node);
  if (node->destroy) {
    node->destroy(node);
    node->destroy = NULL;
  }
  node->magic = 0;
  vfs_xattr_clear(node);
  /* See vfs_close(): the final free waits for a grace period. */
  vfs_rcu_defer_free(node);
}

struct dirent *vfs_readdir_buffer(void) {
  struct thread *t = sched_get_current();
  /* Before the scheduler starts, filesystem setup runs on the boot CPU. */
  static struct dirent boot_result;
  return t ? &t->readdir_result : &boot_result;
}

struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->readdir) {
    return node->readdir(node, index);
  }
  return 0;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
  if (node && (node->view_backing || vfs_namespace_active())) {
    if (node == fs_root) node = vfs_task_root();
    return vfs_namespace_finddir(node, name);
  }
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->finddir) {
    bool negative;
    vfs_node_t *cached = vfs_dentry_lookup(node, name, &negative);
    if (cached && !cached->mount_id)
      cached->mount_id = node->mount_id ? node->mount_id : 1;
    if (cached || negative)
      return cached;

    vfs_node_t *res = node->finddir(node, name);
    if (!res) {
      vfs_dentry_insert(node, name, NULL);
      return 0;
    }
    if (!res->mount_id)
      res->mount_id = node->mount_id ? node->mount_id : 1;

    /* Follow mounts transitively.  Mounts can stack (the kernel mounts a
     * ramfs at /run, then localmount mounts a tmpfs over it), and the visible
     * node is the top of the stack; stopping at the first matching entry left
     * later mounts shadowed under their own mountpoint.  The `res != target`
     * guard also terminates a mount that points back at itself.
     *
     * NOTE: do not gate this walk on FS_MOUNTPOINT. Mounts match by
     * (inode, device) value, but disk backends (ext2/ext4) hand out a fresh
     * kmalloc'd node per lookup, so the flag lives on only one instance of
     * a mountpoint while matches happen on ephemeral ones. Skipping by flag
     * hung the boot at /dev/driverctl registration; the scan is ~5 entries
     * and not worth the fragility. */
    for (int hops = 0; hops < 32; hops++) {
      vfs_node_t *next = NULL;
      vfs_mount_entry_t *curr = vfs_mount_list;
      while (curr) {
        if (curr->mountpoint && curr->mountpoint->inode == res->inode &&
            curr->mountpoint->device == res->device) {
          if (curr->target != res) {
            next = curr->target;
            break;
          }
        }
        curr = curr->next;
      }
      if (!next)
        break;
      vfs_close(res);
      vfs_open(next);
      res = next;
    }
    vfs_dentry_insert(node, name, res);
    return res;
  }
  return 0;
}

int vfs_create(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->create) {
    int result = node->create(node, name, permission);
    if (result == 0) {
      vfs_dentry_invalidate_created(node, name);
      inotify_notify(node, IN_CREATE, name, 0);
    }
    return result;
  }
  return -1;
}

int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mkdir) {
    int result = node->mkdir(node, name, permission);
    if (result == 0) {
      vfs_dentry_invalidate_created(node, name);
      inotify_notify(node, IN_CREATE | IN_ISDIR, name, 0);
    }
    return result;
  }
  return -1;
}

int vfs_unlink(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->unlink) {
    /* Keep the resolved vnode alive across dentry invalidation and the
     * backend unlink.  Filesystems can then retire the actual object after
     * removing its name, while open descriptors and mappings drain normally. */
    vfs_node_t *target = vfs_finddir(node, name);
    vfs_dentry_invalidate(node, name);
    int result = node->unlink(node, name);
    if (result == 0) {
      inotify_notify(node, IN_DELETE, name, 0);
      if (target) {
        inotify_notify(target, IN_DELETE_SELF, NULL, 0);
        vfs_node_retire(target);
      }
    }
    if (target)
      vfs_close(target);
    return result;
  }
  return -1;
}

int vfs_rmdir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rmdir) {
    vfs_node_t *target = vfs_finddir(node, name);
    vfs_dentry_invalidate(node, name);
    int rc = node->rmdir(node, name);
    if (!rc) {
      inotify_notify(node, IN_DELETE | IN_ISDIR, name, 0);
      if (target) inotify_notify(target, IN_DELETE_SELF | IN_ISDIR, NULL, 0);
    }
    if (target) vfs_close(target);
    return rc;
  }
  return -1;
}

int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (node && node->readlink) {
    return node->readlink(node, buf, size);
  }
  return -1;
}

int vfs_symlink(vfs_node_t *node, char *name, char *target) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->symlink) {
    int result = node->symlink(node, name, target);
    if (result == 0) {
      vfs_dentry_invalidate_created(node, name);
      inotify_notify(node, IN_CREATE, name, 0);
    }
    return result;
  }
  return -1;
}

static void vfs_notify_move(vfs_node_t *old_parent, const char *old_name,
                             vfs_node_t *new_parent, const char *new_name,
                             vfs_node_t *source, vfs_node_t *replaced) {
  if (!source || source == replaced || (source->inode && replaced &&
      source->inode == replaced->inode && source->device == replaced->device)) return;
  uint32_t cookie = inotify_move_cookie();
  uint32_t flags = (source->flags & FS_TYPE_MASK) == FS_DIRECTORY ? IN_ISDIR : 0;
  inotify_notify(old_parent, IN_MOVED_FROM | flags, old_name, cookie);
  inotify_notify(new_parent, IN_MOVED_TO | flags, new_name, cookie);
  inotify_notify(source, IN_MOVE_SELF | flags, NULL, 0);
  if (replaced) inotify_notify(replaced, IN_DELETE_SELF, NULL, 0);
}

int vfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rename) {
    vfs_node_t *source = vfs_finddir(node, old_name);
    vfs_node_t *replaced = vfs_finddir(node, new_name);
    vfs_dentry_invalidate(node, old_name);
    vfs_dentry_invalidate(node, new_name);
    int result = node->rename(node, old_name, new_name);
    if (result == 0) {
      /* Readers can refill either entry while the backend rename waits or
       * performs I/O. Drop pre-mutation positive/negative entries again once
       * the new namespace state is installed. */
      vfs_dentry_invalidate(node, old_name);
      vfs_dentry_invalidate(node, new_name);
    }
    bool same_inode = source && replaced && source->device == replaced->device &&
                      source->inode == replaced->inode;
    if (result == 0) vfs_notify_move(node, old_name, node, new_name, source, replaced);
    if (result == 0 && replaced && !same_inode)
      vfs_node_retire(replaced);
    if (replaced)
      vfs_close(replaced);
    if (source)
      vfs_close(source);
    return result;
  }
  return -1;
}

int vfs_rename_cross(vfs_node_t *old_parent, char *old_name,
                     vfs_node_t *new_parent, char *new_name) {
  if (!old_parent || !new_parent || !old_name || !new_name)
    return -22; // EINVAL
  /* ext2/ext4 lookup can return separate VFS handles for the same directory.
   * Pointer inequality does not imply a cross-directory move. Compare the
   * backend, mount and inode before falling back to cross-directory support. */
  extern int ext2_rename_impl(vfs_node_t *, char *, char *);
  if (old_parent->rename == ext2_rename_impl &&
      new_parent->rename == ext2_rename_impl && old_parent->device &&
      old_parent->device == new_parent->device &&
      old_parent->inode == new_parent->inode) {
    vfs_dentry_invalidate(new_parent, new_name);
    int rc = vfs_rename(old_parent, old_name, new_name);
    vfs_dentry_invalidate(new_parent, new_name);
    return rc;
  }
  if (old_parent->rename == ext2_rename_impl &&
      new_parent->rename == ext2_rename_impl && old_parent->device &&
      old_parent->device == new_parent->device) {
    extern int ext2_rename_cross(vfs_node_t *, char *, vfs_node_t *, char *);
    /* Pin the cached objects before invalidation, just as vfs_rename does.
     * Looking them up afterwards creates fresh inode aliases and can reclaim
     * the replacement while the original object still has open descriptors. */
    vfs_node_t *source = vfs_finddir(old_parent, old_name);
    vfs_node_t *replaced = vfs_finddir(new_parent, new_name);
    vfs_dentry_invalidate(old_parent, old_name);
    vfs_dentry_invalidate(new_parent, new_name);
    int rc = ext2_rename_cross(old_parent, old_name, new_parent, new_name);
    vfs_dentry_invalidate(old_parent, old_name);
    vfs_dentry_invalidate(new_parent, new_name);
    if (!rc) vfs_notify_move(old_parent, old_name, new_parent, new_name, source, replaced);
    if (!rc && replaced && (!source || source->inode != replaced->inode))
      vfs_node_retire(replaced);
    if (replaced) vfs_close(replaced);
    if (source) vfs_close(source);
    return rc;
  }
  /* Route on the parents' op table: tmpfs_rename_cross re-validates that
   * both parents really are tmpfs directories of one mount and answers
   * EXDEV for anything else, so other backends keep their old behaviour. */
  if (old_parent->rename && old_parent->rename == new_parent->rename) {
    extern int tmpfs_rename_cross(vfs_node_t *, char *, vfs_node_t *, char *);
    vfs_node_t *source = vfs_finddir(old_parent, old_name);
    vfs_node_t *replaced = vfs_finddir(new_parent, new_name);
    /* Pin event targets before invalidating. In particular, looking up an
     * absent destination records a negative dentry; it must not survive a
     * successful move (Qt renames its login socket into place this way). */
    vfs_dentry_invalidate(old_parent, old_name);
    vfs_dentry_invalidate(new_parent, new_name);
    int rc = tmpfs_rename_cross(old_parent, old_name, new_parent, new_name);
    vfs_dentry_invalidate(old_parent, old_name);
    vfs_dentry_invalidate(new_parent, new_name);
    if (!rc) vfs_notify_move(old_parent, old_name, new_parent, new_name, source, replaced);
    if (source) vfs_close(source);
    if (replaced) vfs_close(replaced);
    if (rc != -18)
      return rc;
  }
  return -18; // EXDEV
}

int vfs_chmod(vfs_node_t *node, uint16_t permission) {
  if (node && node->chmod) {
    return node->chmod(node, permission);
  }
  return -1;
}

int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  if (node && node->chown) {
    return node->chown(node, uid, gid);
  }
  return -1;
}

int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mknod) {
    int result = node->mknod(node, name, permission, flags, device);
    if (result == 0) {
      vfs_dentry_invalidate_created(node, name);
      inotify_notify(node, IN_CREATE, name, 0);
    }
    return result;
  }
  return -1;
}

int vfs_truncate(vfs_node_t *node, uint32_t size) {
  if (node && node->truncate) {
    int result = node->truncate(node, size);
    if (!result && ((node->flags & FS_PAGE_CACHE) || node->mmap_shared_only))
      vfs_cache_clear(node);
    return result;
  }
  return -1;
}

int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE)
    return -22;
  if (!len)
    return -22;
  if (len > UINT32_MAX - offset)
    return -27;
  if (node->fallocate)
    return node->fallocate(node, mode, offset, len);

  /* Resizing is not allocation: truncate may create only holes, and restoring
   * node->length after KEEP_SIZE leaves the on-disk inode enlarged. Let callers
   * use their unsupported-preallocation fallback until the backend can reserve
   * blocks without changing existing bytes or the persistent file size. */
  return -95; // EOPNOTSUPP
}

int vfs_poll(vfs_node_t *node, int events) {
  if (node && node->poll) {
    return node->poll(node, events);
  }
  uint32_t type = (node->flags & FS_TYPE_MASK);
  if (type == FS_FILE || type == FS_DIRECTORY) {
    return events & (POLLIN | POLLOUT);
  }
  return 0;
}

#define MAX_SYMLINK_DEPTH 8

/* Bench-only switch: drop the speculative walk's proved prefix so the
 * fallback re-walks it (the pre-handoff behavior).  Never set outside the
 * `vfs_bench=1` cold-leaf A/B. */
static bool vfs_resolve_force_restart;

/* Result of a stopped speculative walk (see vfs_lookup_cached_path): the
 * deepest node the walk proved reachable and the first component of the
 * original path it did not resolve.  `node` carries one reference; the
 * general walk adopts it as its current directory.  `synthetic` reports that
 * the proved prefix crossed an FS_DENTRY_NOCACHE node (only reachable
 * through a mount redirect), so the general walk keeps suppressing the
 * full-path cache insert. */
struct vfs_walk_resume {
  vfs_node_t *node;
  const char *path;
  bool synthetic;
};

/* Hand the walk's position to the resume record and return NULL: the
 * reference the walk holds (current_ref) transfers to the record, or the
 * unreferenced base directory is pinned for it.  With no record the walk
 * closes what it held, exactly as before. */
static vfs_node_t *vfs_cached_walk_abort(struct vfs_walk_resume *resume,
                                         vfs_node_t *current,
                                         vfs_node_t *current_ref,
                                         const char *resume_path,
                                         bool synthetic) {
  if (resume) {
    if (current_ref) {
      resume->node = current_ref;
    } else {
      vfs_open(current);
      resume->node = current;
    }
    resume->path = resume_path;
    resume->synthetic = synthetic;
  } else if (current_ref) {
    vfs_close(current_ref);
  }
  return NULL;
}

/*
 * Speculative cached path walker.
 *
 * Walks multi-component paths through dentry-cache hits with a reference
 * acquired under each bucket lock. Avoids the general walk's parent stack;
 * intermediate nodes remain referenced until the next component is pinned.
 *
 * If any component is a symlink, relative '..', negative cache entry, or cache miss,
 * it returns NULL, cleanly falling back to the standard refcounted walk.
 * On such a stop the deepest proved prefix is left in `resume` (when
 * non-NULL) together with the first unresolved component, so the fallback
 * continues from there: "parent directories hot, leaf not cached" is the
 * common open() shape, and re-walking the prefix takes its bucket locks
 * twice.  `resume->path` points into the caller's `path` string.
 */
static vfs_node_t *vfs_lookup_cached_path(vfs_node_t *base_dir, const char *path,
                                          struct vfs_walk_resume *resume) {
  if (resume) {
    resume->node = NULL;
    resume->path = NULL;
    resume->synthetic = false;
  }
  if (!base_dir || !path || !path[0])
    return NULL;

  /* If base has no-cache flag or path contains '..', skip cached walk. */
  if ((base_dir->flags & FS_DENTRY_NOCACHE) || vfs_path_has_dotdot(path))
    return NULL;

  const char *p = path;
  if (*p == '/') {
    while (*p == '/')
      p++;
  }
  if (*p == '\0') {
    /* Path was just "/" or empty after slashes */
    vfs_open(base_dir);
    return base_dir;
  }

  vfs_node_t *current = base_dir;
  vfs_node_t *current_ref = NULL; // Non-NULL if current holds a refcount we acquired
  bool synthetic = false;

  while (*p) {
    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    const char *comp_start = p;
    char comp[128];
    int i = 0;
    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    /* Only directories can contain another path component. */
    if ((current->flags & FS_TYPE_MASK) != FS_DIRECTORY ||
        !vfs_access(current, 1))
      return vfs_cached_walk_abort(resume, current, current_ref, comp_start,
                                   synthetic);

    if (strcmp(comp, ".") == 0)
      continue;

    bool negative = false;
    vfs_node_t *next = vfs_dentry_lookup(current, comp, &negative);
    if (!next || negative) {
      /* Cache miss or negative hit: abort cached walk and fallback. */
      return vfs_cached_walk_abort(resume, current, current_ref, comp_start,
                                   synthetic);
    }

    /* If symlink or synthetic node (procfs etc), abort cached walk. */
    if (((next->flags & FS_TYPE_MASK) == FS_SYMLINK) ||
        (next->flags & FS_DENTRY_NOCACHE)) {
      vfs_close(next);
      return vfs_cached_walk_abort(resume, current, current_ref, comp_start,
                                   synthetic);
    }

    /* Lockless mountpoint traversal */
    for (int hops = 0; hops < 32; hops++) {
      vfs_node_t *m_next = NULL;
      vfs_mount_entry_t *curr = __atomic_load_n(&vfs_mount_list, __ATOMIC_ACQUIRE);
      while (curr) {
        if (curr->mountpoint && curr->mountpoint->inode == next->inode &&
            curr->mountpoint->device == next->device) {
          if (curr->target != next) {
            m_next = curr->target;
            break;
          }
        }
        curr = __atomic_load_n(&curr->next, __ATOMIC_ACQUIRE);
      }
      if (!m_next)
        break;
      vfs_close(next);
      if (!vfs_node_is_alive(m_next) || !vfs_node_tryref(m_next)) {
        if (current_ref)
          vfs_close(current_ref);
        return NULL;
      }
      next = m_next;
      /* A mount target may be a generated (procfs-style) container even
       * when the mountpoint itself was cacheable; a walk that travels
       * through one must keep suppressing the full-path cache insert. */
      if (next->flags & FS_DENTRY_NOCACHE)
        synthetic = true;
    }

    /* Advance: release previous intermediate reference if held */
    if (current_ref)
      vfs_close(current_ref);
    current = next;
    current_ref = next;
  }

  return current_ref;
}

vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path) {
  if (vfs_namespace_active() || vfs_task_root() != fs_root)
    return vfs_namespace_resolve(dir == fs_root ? vfs_task_root() : dir, path);
  if (!path || !path[0] || !fs_root)
    return 0;

  vfs_node_t *effective_dir = (path[0] == '/') ? fs_root : (dir ? dir : fs_root);
  /* One scan reused by the cached-walk guard and both cache inserts below. */
  bool path_has_dotdot = vfs_path_has_dotdot(path);

  // 1. Fast Path Full-Path Dcache lookup:
  // Check if we already resolved this exact path string from this directory
  vfs_node_t *cached = vfs_path_cache_lookup(effective_dir, path);
  if (cached) {
    return cached;
  }

  // 2. Speculative cached walk across path components, with owned references.
  struct vfs_walk_resume resume = {0};
  vfs_node_t *cached_node = vfs_lookup_cached_path(effective_dir, path, &resume);
  if (cached_node) {
    if ((effective_dir->flags & FS_DENTRY_NOCACHE) == 0 &&
        (cached_node->flags & FS_DENTRY_NOCACHE) == 0 &&
        !path_has_dotdot) {
      vfs_path_cache_insert(effective_dir, path, cached_node);
    }
    return cached_node;
  }
  /* Bench-only switch: discard the proved prefix so the fallback re-walks
   * it, which is what every resolution used to do.  Never set outside the
   * cold-leaf A/B. */
  if (vfs_resolve_force_restart && resume.node) {
    vfs_close(resume.node);
    resume.node = NULL;
  }

  char path_buf[512];
  vfs_node_t *current;
  bool resumed = resume.node != NULL;
  bool synthetic_descent;
  char *p;
  int symlink_depth;
  int stack_top; // -1 is an empty stack

#define VFS_PARENT_STACK_DEPTH 32
  vfs_node_t *parent_stack[VFS_PARENT_STACK_DEPTH];

reset_walk:
  strncpy(path_buf, resumed ? resume.path : path, 511);
  path_buf[511] = '\0';

  if (resumed) {
    /* Continue from the node the speculative walk proved instead of
     * re-walking the prefix: adopt the walk's reference as 'current'.  The
     * prefix's ancestors are not on the parent stack, so a later '..' must
     * restart the walk (see the '..' case below). */
    current = resume.node;
    synthetic_descent = resume.synthetic ||
                        (current->flags & FS_DENTRY_NOCACHE) != 0;
  } else {
    current = effective_dir;
    vfs_open(current); // Reference for 'current'
    /* Set when this walk descends through (or starts in) a node marked
     * FS_DENTRY_NOCACHE: a synthetic container whose children are generated
     * per lookup and disappear with their task (procfs' /proc root and its
     * per-task pid dirs, drm's dynamic nodes).  Such a result is not a pure
     * function of the path string, so it must never enter the full-path
     * cache — see the insert at the end of this function. */
    synthetic_descent = (effective_dir->flags & FS_DENTRY_NOCACHE) != 0;
  }
  p = path_buf;
  symlink_depth = 0;
  stack_top = -1;

  if (path_buf[0] == '/') {
    while (*p == '/')
      p++;
  }

  while (*p) {
    char comp[128];
    int i = 0;

    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    if ((current->flags & FS_TYPE_MASK) != FS_DIRECTORY)
      goto fail;

    if (strcmp(comp, "..") == 0) {
      if (stack_top >= 0) {
        vfs_close(current);
        current = parent_stack[stack_top];
        stack_top--;
        // Ownership transferred from stack to 'current'
        continue;
      }
      if (resumed) {
        /* This '..' climbs past the resumed prefix, whose ancestors are not
         * on the stack.  Only a relative symlink target can put it here (a
         * path with a literal '..' never takes the speculative walk), so
         * redo this resolution from the top once: the restart builds the
         * full parent stack and is exactly the pre-handoff behavior. */
        vfs_close(current);
        resume.node = NULL;
        resumed = false;
        goto reset_walk;
      }
      continue;
    }

    if (strcmp(comp, ".") == 0)
      continue;

    if (!vfs_access(current, 1)) goto fail;

    vfs_node_t *next = vfs_finddir(current, comp);
    if (!next) {
      for (int j = 0; j <= stack_top; j++) {
        vfs_close(parent_stack[j]);
      }
      vfs_close(current);
      return 0;
    }

    if ((next->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      if (++symlink_depth > MAX_SYMLINK_DEPTH) {
        vfs_close(next);
        goto fail;
      }

      if (next->get_link) {
        vfs_node_t *target = next->get_link(next);
        vfs_close(next);
        if (!target)
          goto fail;
        while (*p == '/')
          p++;
        if (!*p) {
          for (int j = 0; j <= stack_top; j++)
            vfs_close(parent_stack[j]);
          vfs_close(current);
          return target;
        }
        vfs_close(current);
        current = target;
        continue;
      }

      char link_target[512];
      int len = vfs_readlink(next, link_target, 511);
      vfs_close(next);

      if (len < 0)
        goto fail;
      link_target[len] = '\0';

      char next_path[512];
      strcpy(next_path, link_target);

      if (*p) {
        int cur_len = (int)strlen(next_path);
        if (cur_len < 510) {
          bool target_ends_in_slash =
              (cur_len > 0 && next_path[cur_len - 1] == '/');
          if (!target_ends_in_slash && *p != '/')
            strcat(next_path, "/");
          else if (target_ends_in_slash && *p == '/')
            p++;
          strncat(next_path, p, 511 - strlen(next_path));
        }
      }
      next_path[511] = '\0';
      strcpy(path_buf, next_path);
      p = path_buf;

      if (path_buf[0] == '/') {
        vfs_close(current);
        for (int j = 0; j <= stack_top; j++)
          vfs_close(parent_stack[j]);
        stack_top = -1;
        current = fs_root;
        vfs_open(current);
        while (*p == '/')
          p++;
        /* Back at the root with an empty stack, the missing ancestors of a
         * resumed walk no longer matter: '..' from here is the root's own
         * parent, which the '..' case handles without a restart. */
        resumed = false;
      }
      continue;
    }

    // Descent
    if ((next->flags & FS_DENTRY_NOCACHE) != 0)
      synthetic_descent = true;
    if (stack_top < VFS_PARENT_STACK_DEPTH - 1) {
      stack_top++;
      parent_stack[stack_top] = current;
      current = next;
      // 'current' reference transferred to stack, 'next' becomes new 'current'
    } else {
      // Stack overflow - just swap current and lose history
      vfs_close(current);
      current = next;
    }
  }

  // Cleanup stack
  for (int j = 0; j <= stack_top; j++) {
    vfs_close(parent_stack[j]);
  }

  /* Cache successfully resolved path — but only for walks that never left an
   * ordinary (non-synthetic) tree.  A cached /proc entry would outlive the
   * task that produced it: the first process to open "/proc/self/statm"
   * pinned its own <tid>/statm node for every later opener, and once that
   * task was reaped the node read as EOF forever, so open()+pread() never
   * delivered three parseable fields again (Roblox logs
   * "[FLog::Output] Failed to scan size, resident, and shared" on every
   * memory poll).  /proc/self is caller-dependent on top of that: no single
   * cached node can ever be right for every caller.  Skipping the insert
   * makes each open walk the subtree again, which is also what Linux pays
   * for /proc lookups. */
  if (current && !synthetic_descent && !path_has_dotdot) {
    vfs_path_cache_insert(effective_dir, path, current);
  }

  return current;


fail:
  for (int j = 0; j <= stack_top; j++)
    vfs_close(parent_stack[j]);
  vfs_close(current);
  /* This label handles invalid traversal, permissions and symlink failures,
   * not a missing node. Probing version/connector_id beneath a regular DRM
   * version file must not be reported as an unimplemented sysfs attribute. */
  return 0;
}

vfs_node_t *vfs_resolve_path(const char *path) {
  return vfs_resolve_path_at(fs_root, path);
}

void vfs_node_init(vfs_node_t *node) {
  if (!node)
    return;
  memset(node, 0, sizeof(vfs_node_t));
  INIT_LIST_HEAD(&node->ep_watchers);
  spinlock_init(&node->ep_lock);
  asc_radix_tree_init(&node->pages);
  vfs_page_cache_tree_init(&node->pages);
  spinlock_init(&node->pages_lock);
  spinlock_init(&node->readdir_cursor_lock);
  node->refcount = 1;
  node->magic = VFS_NODE_MAGIC;
}

bool vfs_node_is_alive(const vfs_node_t *node) {
  if (!node || !pmm_kernel_ptr_is_managed(node))
    return false;
  return node->magic == VFS_NODE_MAGIC;
}

int vfs_mount_ex(vfs_node_t *mountpoint, vfs_node_t *target,
                 const char *dev_name, const char *fs_type,
                 const char *path) {
  if (!target)
    return -1;
  if (vfs_namespace_active())
    return vfs_namespace_mount(mountpoint, target, dev_name, fs_type, path, 0);

  vfs_mount_entry_t *entry = kmalloc(sizeof(vfs_mount_entry_t));
  if (!entry)
    return -1;
  entry->mountpoint = mountpoint;
  entry->target = target;
  entry->mount_id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
  target->mount_id = entry->mount_id;
  strncpy(entry->dev_name, dev_name ? dev_name : "none", 63);
  strncpy(entry->fs_type, fs_type ? fs_type : "unknown", 31);
  entry->dev_name[63] = '\0';
  entry->fs_type[31] = '\0';
  if (path && path[0]) {
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
  } else if (mountpoint) {
    strncpy(entry->path, mountpoint->name, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
  } else {
    strcpy(entry->path, "/");
  }
  entry->next = __atomic_load_n(&vfs_mount_list, __ATOMIC_ACQUIRE);
  __atomic_store_n(&vfs_mount_list, entry, __ATOMIC_RELEASE);

  /* A prior lookup may have cached the node hidden by this mount. */
  vfs_dentry_invalidate(NULL, NULL);

  target->flags |= FS_PERSISTENT;

  if (mountpoint) {
    mountpoint->flags |= FS_MOUNTPOINT | FS_PERSISTENT;
    mountpoint->ptr = target;
  }

  return 0;
}

vfs_node_t *vfs_initial_filesystem(const char *type) {
  for (vfs_mount_entry_t *m = vfs_mount_list; m; m = m->next)
    if (!strcmp(m->fs_type, type)) { vfs_open(m->target); return m->target; }
  return NULL;
}

int vfs_initial_mounts_copy(struct mount_namespace *ns) {
  for (vfs_mount_entry_t *m = vfs_mount_list; m; m = m->next) {
    int r = vfs_namespace_seed(ns, m->mountpoint, m->target, m->mount_id,
                                m->dev_name, m->fs_type, m->path);
    if (r) return r;
  }
  return 0;
}

int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target) {
  return vfs_mount_ex(mountpoint, target, "none", "unknown", NULL);
}

int vfs_statfs(vfs_node_t *node, void *buf) {
  if (node && node->statfs) {
    return node->statfs(node, buf);
  }
  return -1;
}

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count) {
  if (vfs_namespace_active()) return vfs_namespace_mounts(buffer, max_count);
  return vfs_initial_mounts_get(buffer, max_count);
}

int vfs_initial_mounts_get(vfs_mount_info_t *buffer, int max_count) {
  int count = 0;

  // Add root if it exists
  if (fs_root && max_count > 0) {
    // Try to find if root is in the mount list first
    bool found = false;
    vfs_mount_entry_t *c = vfs_mount_list;
    while (c) {
      if (!c->mountpoint ||
          (c->mountpoint && strcmp(c->mountpoint->name, "/") == 0)) {
        found = true;
        break;
      }
      c = c->next;
    }

    if (!found) {
      buffer[count].mount_id = fs_root->mount_id ? fs_root->mount_id : 1;
      buffer[count].parent_id = buffer[count].mount_id;
      buffer[count].flags = 0;
      strcpy(buffer[count].mountpoint, "/");
      strcpy(buffer[count].target, "/");
      strcpy(buffer[count].dev_name, "none");
      strcpy(buffer[count].fs_type, "ext4");
      count++;
    }
  }

  vfs_mount_entry_t *curr = vfs_mount_list;
  while (curr && count < max_count) {
    buffer[count].mount_id = curr->mount_id;
    buffer[count].parent_id = fs_root && fs_root->mount_id ? fs_root->mount_id : 1;
    buffer[count].flags = 0;
    /* Use the recorded mountpoint path, not the node's name: userspace
     * (OpenRC's mountinfo, findmnt, ...) matches full paths. */
    strncpy(buffer[count].mountpoint,
            (curr->path[0] ? curr->path : "/"), 127);
    buffer[count].mountpoint[127] = '\0';

    strncpy(buffer[count].target, curr->target->name, 127);
    buffer[count].target[127] = '\0';

    strncpy(buffer[count].dev_name, curr->dev_name, 63);
    buffer[count].dev_name[63] = '\0';

    strncpy(buffer[count].fs_type, curr->fs_type, 31);
    buffer[count].fs_type[31] = '\0';

    count++;
    curr = curr->next;
  }
  return count;
}

/* ── `vfs_selftest=1` / `vfs_bench=1` ───────────────────────────────────────
 *
 * The path cache invalidates lazily now: a removal appends the name to
 * vfs_path_inval_log and each entry revalidates on its next lookup.  The
 * selftest pins the semantics the old eager scan provided (whole-component
 * matching, unrelated paths surviving, entries older than the log window
 * failing closed), and the benchmark runs the old full-table scan and the
 * new log append back to back on the same synthetic working set - the way
 * serial_bench contrasts both tick policies.  Both run once from kmain after
 * the root filesystem is mounted, gated on the kernel command line, and clean
 * up every synthetic entry afterwards.
 */

extern const char *kernel_boot_cmdline;

#define VFS_BENCH_NODES 64u
#define VFS_BENCH_PATHS 4096u
#define VFS_BENCH_ITERS 128u

static uint32_t vfs_selftest_failures;

static void vfs_selftest_expect(bool ok, const char *what) {
  if (!ok)
    vfs_selftest_failures++;
  klog_puts(ok ? "[VFS-SELFTEST] ok   " : "[VFS-SELFTEST] FAIL ");
  klog_puts(what);
  klog_putchar('\n');
}

static vfs_node_t *vfs_probe_node_create(const char *name) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);
  strncpy(node->name, name, 127);
  node->name[127] = '\0';
  node->flags = FS_FILE;
  node->mask = 0644;
  return node;
}

/* Append `value` in decimal without pulling printf into the VFS. */
static uint32_t vfs_probe_append_uint(char *buf, uint32_t pos, uint32_t value) {
  char digits[10];
  int n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (n > 0)
    buf[pos++] = digits[--n];
  return pos;
}

static void vfs_selftest_make_name(char *buf, uint32_t index) {
  uint32_t pos = 0;
  const char prefix[] = "z_inval_";
  for (uint32_t i = 0; i < sizeof(prefix) - 1; i++)
    buf[pos++] = prefix[i];
  buf[vfs_probe_append_uint(buf, pos, index)] = '\0';
}

static void vfs_bench_make_path(char *buf, uint32_t index) {
  uint32_t pos = 0;
  const char prefix[] = "/vfsbench/d";
  for (uint32_t i = 0; i < sizeof(prefix) - 1; i++)
    buf[pos++] = prefix[i];
  pos = vfs_probe_append_uint(buf, pos, index);
  buf[pos++] = '/';
  buf[pos++] = 'f';
  pos = vfs_probe_append_uint(buf, pos, index);
  buf[pos] = '\0';
}

/* ── Speculative-prefix handoff (`vfs_selftest=1` / `vfs_bench=1`) ──────────
 *
 * vfs_lookup_cached_path() proves a path prefix through the dentry cache and
 * stops at the first component it cannot serve; the general walk then
 * continues from that node instead of re-walking the prefix.  The fixtures
 * below are synthetic directories served by an on-demand finddir(), so no
 * backend has to exist and the tests can pick exactly where a walk stops.
 * (Also used by vfs_cold_leaf_bench() further down.) */

#define VFS_MOCK_MAX_NODES 8u
#define VFS_MOCK_MAX_CHILDREN 4u

struct vfs_mock_node {
  vfs_node_t *node;
  const char *link_target;
  struct {
    const char *name;
    vfs_node_t *child;
  } children[VFS_MOCK_MAX_CHILDREN];
  uint32_t count;
};

static struct vfs_mock_node vfs_mock_table[VFS_MOCK_MAX_NODES];
static uint32_t vfs_mock_used;

static vfs_node_t *vfs_mock_finddir(vfs_node_t *dir, char *name) {
  struct vfs_mock_node *mock = dir->device;
  if (!mock)
    return NULL;
  for (uint32_t i = 0; i < mock->count; i++) {
    if (strcmp(mock->children[i].name, name) == 0) {
      vfs_open(mock->children[i].child);
      return mock->children[i].child;
    }
  }
  return NULL;
}

static int vfs_mock_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  struct vfs_mock_node *mock = node->device;
  if (!mock || !mock->link_target || !size)
    return -1;
  uint32_t len = (uint32_t)strlen(mock->link_target);
  if (len >= size)
    len = size - 1;
  memcpy(buf, mock->link_target, len);
  buf[len] = '\0';
  return (int)len;
}

static vfs_node_t *vfs_mock_add(const char *name, uint32_t flags,
                                uint32_t mask) {
  if (vfs_mock_used >= VFS_MOCK_MAX_NODES)
    return NULL;
  vfs_node_t *node = vfs_probe_node_create(name);
  if (!node)
    return NULL;
  struct vfs_mock_node *mock = &vfs_mock_table[vfs_mock_used++];
  memset(mock, 0, sizeof(*mock));
  mock->node = node;
  node->flags = flags;
  node->mask = mask;
  node->device = mock;
  uint32_t type = flags & FS_TYPE_MASK;
  if (type == FS_DIRECTORY)
    node->finddir = vfs_mock_finddir;
  else if (type == FS_SYMLINK)
    node->readlink = vfs_mock_readlink;
  return node;
}

static void vfs_mock_link(vfs_node_t *parent, const char *name,
                          vfs_node_t *child) {
  struct vfs_mock_node *mock = parent->device;
  if (mock && mock->count < VFS_MOCK_MAX_CHILDREN) {
    mock->children[mock->count].name = name;
    mock->children[mock->count].child = child;
    mock->count++;
  }
}

/* Release the fixture: flush the path cache, invalidate every dentry entry a
 * walk may have installed, then drop the nodes' creation references. */
static void vfs_mock_release(vfs_node_t *root) {
  if (!root)
    return;
  struct vfs_mock_node *mock = root->device;
  vfs_path_cache_invalidate();
  if (mock) {
    for (uint32_t i = 0; i < mock->count; i++) {
      vfs_dentry_invalidate(root, mock->children[i].name);
      vfs_mock_release(mock->children[i].child);
    }
  }
  vfs_close(root);
  vfs_mock_used = 0;
}

/* Close whatever the fixture created after a partial setup failure. */
static void vfs_mock_release_all(void) {
  for (uint32_t i = 0; i < vfs_mock_used; i++)
    vfs_close(vfs_mock_table[i].node);
  vfs_mock_used = 0;
}

static bool vfs_prefix_resume_selftest(void) {
  if (!fs_root)
    return false;

  uint32_t failures_before = vfs_selftest_failures;

  /* 1. Cold leaf behind a hot prefix: the walk must stop at the leaf and
   *    hand the parent back, both resolution forms must agree, and the
   *    resumed walk must still cache the full path. */
  vfs_node_t *base = vfs_mock_add("presume_base", FS_DIRECTORY, 0755);
  vfs_node_t *d1 = vfs_mock_add("presume_d1", FS_DIRECTORY, 0755);
  vfs_node_t *d2 = vfs_mock_add("presume_d2", FS_DIRECTORY, 0755);
  vfs_node_t *leaf = vfs_mock_add("presume_leaf", FS_FILE, 0644);
  if (!base || !d1 || !d2 || !leaf) {
    vfs_mock_release_all();
    return false;
  }
  vfs_mock_link(base, "presume_d1", d1);
  vfs_mock_link(d1, "presume_d2", d2);
  vfs_mock_link(d2, "presume_leaf", leaf);

  const char cold_path[] = "presume_d1/presume_d2/presume_leaf";
  vfs_node_t *warm = vfs_resolve_path_at(base, cold_path);
  vfs_selftest_expect(warm == leaf, "resume fixture resolves before the test");
  if (warm)
    vfs_close(warm);

  /* Keep the prefix hot in the dentry cache, make the leaf cold. */
  vfs_path_cache_invalidate();
  vfs_dentry_invalidate(d2, "presume_leaf");

  struct vfs_walk_resume probe = {0};
  vfs_node_t *partial = vfs_lookup_cached_path(base, cold_path, &probe);
  vfs_selftest_expect(partial == NULL && probe.node == d2 &&
                          probe.path && strcmp(probe.path, "presume_leaf") == 0,
                      "cold leaf stops the walk and hands back the prefix");
  if (probe.node)
    vfs_close(probe.node);

  vfs_dentry_invalidate(d2, "presume_leaf");
  vfs_node_t *resumed_hit = vfs_resolve_path_at(base, cold_path);

  vfs_node_t *pc_hit =
      resumed_hit == leaf ? vfs_path_cache_lookup(base, cold_path) : NULL;
  vfs_selftest_expect(pc_hit == leaf,
                      "resumed walk still caches the full path");
  if (pc_hit)
    vfs_close(pc_hit);

  vfs_path_cache_invalidate();
  vfs_dentry_invalidate(d2, "presume_leaf");
  vfs_resolve_force_restart = true;
  vfs_node_t *restart_hit = vfs_resolve_path_at(base, cold_path);
  vfs_resolve_force_restart = false;

  vfs_selftest_expect(resumed_hit == leaf && restart_hit == leaf,
                      "resumed and restart walks agree on a cold leaf");
  if (resumed_hit)
    vfs_close(resumed_hit);
  if (restart_hit)
    vfs_close(restart_hit);

  vfs_mock_release(base);

  /* 2. A relative symlink target with '..' must climb past the resume point.
   *    Only the restart from the top can do that (the ancestors are not on
   *    the stack), and it must still produce the ordinary walk's result. */
  vfs_node_t *sbase = vfs_mock_add("presume_sbase", FS_DIRECTORY, 0755);
  vfs_node_t *s1 = vfs_mock_add("presume_s1", FS_DIRECTORY, 0755);
  vfs_node_t *s2 = vfs_mock_add("presume_s2", FS_DIRECTORY, 0755);
  vfs_node_t *link = vfs_mock_add("presume_link", FS_SYMLINK, 0777);
  vfs_node_t *target = vfs_mock_add("presume_target", FS_FILE, 0644);
  if (!sbase || !s1 || !s2 || !link || !target) {
    vfs_mock_release_all();
    return false;
  }
  vfs_mock_link(sbase, "presume_s1", s1);
  vfs_mock_link(sbase, "presume_s2", s2);
  vfs_mock_link(s1, "presume_link", link);
  vfs_mock_link(s2, "presume_target", target);
  ((struct vfs_mock_node *)link->device)->link_target =
      "../presume_s2/presume_target";

  const char link_path[] = "presume_s1/presume_link";
  vfs_node_t *warm_link = vfs_resolve_path_at(sbase, link_path);
  vfs_selftest_expect(warm_link == target,
                      "symlink fixture resolves through '..' before the test");
  if (warm_link)
    vfs_close(warm_link);

  vfs_path_cache_invalidate();
  struct vfs_walk_resume lprobe = {0};
  vfs_node_t *lpartial = vfs_lookup_cached_path(sbase, link_path, &lprobe);
  vfs_selftest_expect(lpartial == NULL && lprobe.node == s1 &&
                          lprobe.path && strcmp(lprobe.path, "presume_link") == 0,
                      "symlink stops the walk and hands back the prefix");
  if (lprobe.node)
    vfs_close(lprobe.node);

  vfs_node_t *resumed_link = vfs_resolve_path_at(sbase, link_path);
  vfs_selftest_expect(resumed_link == target,
                      "resumed walk climbs '..' past the prefix");
  if (resumed_link)
    vfs_close(resumed_link);

  vfs_path_cache_invalidate();
  vfs_resolve_force_restart = true;
  vfs_node_t *restart_link = vfs_resolve_path_at(sbase, link_path);
  vfs_resolve_force_restart = false;
  vfs_selftest_expect(restart_link == target,
                      "forced restart agrees on the symlink path");
  if (restart_link)
    vfs_close(restart_link);

  vfs_mock_release(sbase);

  return vfs_selftest_failures == failures_before;
}

static bool vfs_path_cache_selftest(void) {
  if (!fs_root)
    return false;

  vfs_node_t *probe = vfs_probe_node_create("vfs_selftest_probe");
  if (!probe)
    return false;

  vfs_node_t *hit;

  /* 1. A cached resolution is served until its name is invalidated. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_probe", probe);
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_probe");
  vfs_selftest_expect(hit == probe, "cached path resolves before invalidation");
  if (hit)
    vfs_close(hit);

  /* 2. A removal retires it on the next lookup. */
  vfs_path_invalidate_record("vfs_selftest_probe");
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_probe");
  vfs_selftest_expect(!hit, "removed component invalidates the cached path");
  if (hit)
    vfs_close(hit);

  /* 3. Matching is whole-component: a name that is only a substring of the
   *    cached path's components must not invalidate it. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_probe_extra", probe);
  vfs_path_invalidate_record("vfs_selftest");
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_probe_extra");
  vfs_selftest_expect(hit == probe,
                      "substring (non-component) leaves the entry valid");
  if (hit)
    vfs_close(hit);
  vfs_path_invalidate_record("vfs_selftest_probe_extra");
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_probe_extra");
  vfs_selftest_expect(!hit, "whole component invalidates the cached path");
  if (hit)
    vfs_close(hit);

  /* 4. Removing an interior directory invalidates paths resolved through it. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_dir/child", probe);
  vfs_path_invalidate_record("vfs_selftest_dir");
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_dir/child");
  vfs_selftest_expect(!hit,
                      "interior directory component invalidates long path");
  if (hit)
    vfs_close(hit);

  /* 5. An entry older than the log window fails closed. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_ring", probe);
  for (uint32_t i = 0; i < VFS_PATH_INVAL_LOG_SIZE + 4; i++) {
    char name[32];
    vfs_selftest_make_name(name, i);
    vfs_path_invalidate_record(name);
  }
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_ring");
  vfs_selftest_expect(!hit, "entry older than the invalidation log fails closed");
  if (hit)
    vfs_close(hit);

  /* 6. Entries inserted after the wrap are usable again. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_fresh", probe);
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_fresh");
  vfs_selftest_expect(hit == probe, "entry inserted after the wrap resolves");
  if (hit)
    vfs_close(hit);

  /* 7. Repeated cached lookup acquires a caller reference. */
  vfs_path_cache_insert(fs_root, "/vfs_selftest_rcu", probe);
  hit = vfs_path_cache_lookup(fs_root, "/vfs_selftest_rcu");
  vfs_selftest_expect(hit == probe, "cached lookup resolves probe with reference");
  if (hit)
    vfs_close(hit);

  vfs_path_cache_drop(fs_root, "/vfs_selftest_probe");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_probe_extra");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_dir/child");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_ring");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_fresh");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_rcu");
  vfs_close(probe);
  return vfs_selftest_failures == 0;
}

/* `vfs_selftest=1`: the lockless lookup paths and the deferred reclamation
 * they depend on.  Every lookup that has two forms is run once lockless and
 * once with the pre-RCU locked path forced; both must agree. */
static bool vfs_lockless_selftest(void) {
  if (!fs_root)
    return false;

  uint32_t failures_before = vfs_selftest_failures;
  vfs_selftest_expect(vfs_rcu_ready(),
                      "deferred node reclamation engine is active");

  vfs_node_t *parent = vfs_probe_node_create("vfsrcu_parent");
  vfs_node_t *child = vfs_probe_node_create("vfsrcu_child");
  if (!parent || !child) {
    vfs_close(parent);
    vfs_close(child);
    return false;
  }
  parent->flags = FS_DIRECTORY;

  static const char name[] = "vfsrcu_entry";

  /* Positive hit: both forms must find the child. */
  vfs_dentry_insert(parent, name, child);
  for (uint32_t mode = 0; mode < 2; mode++) {
    vfs_dentry_lookup_force_locked = mode != 0;
    bool negative = false;
    vfs_node_t *hit = vfs_dentry_lookup(parent, name, &negative);
    vfs_selftest_expect(hit == child && !negative,
                        mode ? "locked dentry lookup hits"
                             : "lockless dentry lookup hits");
    if (hit)
      vfs_close(hit);
  }
  vfs_dentry_lookup_force_locked = false;

  /* Stable miss, then a cached negative entry, then invalidation. */
  bool negative = false;
  vfs_node_t *hit = vfs_dentry_lookup(parent, "vfsrcu_absent", &negative);
  vfs_selftest_expect(!hit && !negative,
                      "lockless dentry miss is not negative");

  vfs_dentry_insert(parent, "vfsrcu_absent", NULL);
  negative = false;
  hit = vfs_dentry_lookup(parent, "vfsrcu_absent", &negative);
  vfs_selftest_expect(!hit && negative, "lockless dentry negative entry");

  vfs_dentry_invalidate(parent, name);
  negative = false;
  hit = vfs_dentry_lookup(parent, name, &negative);
  vfs_selftest_expect(!hit && !negative,
                      "lockless dentry invalidate drops the entry");
  if (hit)
    vfs_close(hit);

  /* Full-path cache: the same agreement, plus generation invalidation. */
  static const char path[] = "/vfsrcu_selftest";
  vfs_path_cache_insert(fs_root, path, child);
  for (uint32_t mode = 0; mode < 2; mode++) {
    vfs_path_lookup_force_locked = mode != 0;
    vfs_node_t *phit = vfs_path_cache_lookup(fs_root, path);
    vfs_selftest_expect(phit == child, mode ? "locked path lookup hits"
                                            : "lockless path lookup hits");
    if (phit)
      vfs_close(phit);
  }
  vfs_path_lookup_force_locked = false;

  vfs_path_invalidate_record("vfsrcu_selftest");
  hit = vfs_path_cache_lookup(fs_root, path);
  vfs_selftest_expect(!hit, "lockless path lookup drops invalidated entry");
  if (hit)
    vfs_close(hit);
  vfs_path_cache_drop(fs_root, path);

  /* Deferred free: every victim is inserted, invalidated and closed, so the
   * last close enqueues it; the nodes must only be reclaimed after a grace
   * period, exactly once each. */
  uint64_t freed_before = vfs_rcu_freed_count();
  const uint32_t freed_cycles = 64;
  for (uint32_t i = 0; i < freed_cycles; i++) {
    vfs_node_t *victim = vfs_probe_node_create("vfsrcu_victim");
    if (!victim)
      break;
    vfs_dentry_insert(parent, "vfsrcu_victim", victim);
    vfs_dentry_invalidate(parent, "vfsrcu_victim");
    vfs_close(victim);
  }
  vfs_rcu_synchronize();
  uint64_t freed_after = vfs_rcu_freed_count();
  vfs_selftest_expect(freed_after >= freed_before + freed_cycles,
                      "deferred frees reclaim after a grace period");
  vfs_selftest_expect(vfs_rcu_queued_count() == 0,
                      "deferred free queue drains");

  vfs_dentry_invalidate(parent, "vfsrcu_absent");
  vfs_close(child);
  vfs_close(parent);
  return vfs_selftest_failures == failures_before;
}

void vfs_selftest_maybe_run(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "vfs_selftest"))
    return;

  vfs_selftest_failures = 0;
  klog_puts("[VFS-SELFTEST] path cache invalidation + page cache refs\n");
  bool path_ok = vfs_path_cache_selftest();
  bool page_ok = vfs_cache_ref_selftest();
  bool page_lockless_ok = vfs_cache_lockless_selftest();
  bool dir_ok = vfs_dirindex_selftest();
  bool lockless_ok = vfs_lockless_selftest();
  bool prefix_ok = vfs_prefix_resume_selftest();
  klog_puts("[VFS-SELFTEST] path cache: ");
  klog_puts(path_ok ? "PASS" : "FAIL");
  klog_puts(", page cache refs: ");
  klog_puts(page_ok ? "PASS" : "FAIL");
  klog_puts(", page lockless: ");
  klog_puts(page_lockless_ok ? "PASS" : "FAIL");
  klog_puts(", dirindex: ");
  klog_puts(dir_ok ? "PASS" : "FAIL");
  klog_puts(", lockless lookups: ");
  klog_puts(lockless_ok ? "PASS" : "FAIL");
  klog_puts(", prefix resume: ");
  klog_puts(prefix_ok ? "PASS" : "FAIL");
  klog_putchar('\n');
}

/* ── `vfs_selftest` for /proc resolutions ───────────────────────────────────
 *
 * Called from kmain() after procfs_init(): the checks above need the mounted
 * root, this one needs a mounted /proc.  Same `vfs_selftest` token.
 *
 * /proc nodes are generated per lookup and die with their task, and
 * /proc/self resolves differently for every caller, so no resolution under
 * /proc may be entered into the full-path cache.  When one was, the first
 * process to open "/proc/self/statm" pinned its own <tid>/statm node for
 * every later opener: once that task was reaped the node read as EOF
 * forever, so open()+pread() never delivered three parseable fields again
 * (Roblox logs "[FLog::Output] Failed to scan size, resident, and shared"
 * on every memory poll). */
void vfs_procfs_selftest(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "vfs_selftest"))
    return;

  vfs_selftest_failures = 0;

  vfs_node_t *node = vfs_resolve_path("/proc/self/statm");
  vfs_selftest_expect(node != NULL, "/proc/self/statm resolves");
  if (node) {
    uint8_t buf[64];
    vfs_selftest_expect(vfs_read(node, 0, sizeof(buf) - 1, buf) > 0,
                        "/proc/self/statm read returns data");
    vfs_close(node);
  }

  vfs_node_t *cached = vfs_path_cache_lookup(fs_root, "/proc/self/statm");
  vfs_selftest_expect(!cached,
                      "/proc/self/statm has no full-path cache entry");
  if (cached)
    vfs_close(cached);

  struct thread *self = sched_get_current();
  if (self) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/statm", self->tid);

    vfs_node_t *pid_node = vfs_resolve_path(path);
    vfs_selftest_expect(pid_node != NULL,
                        "explicit /proc/<tid>/statm resolves");
    if (pid_node) {
      uint8_t buf[64];
      vfs_selftest_expect(vfs_read(pid_node, 0, sizeof(buf) - 1, buf) > 0,
                          "explicit /proc/<tid>/statm read returns data");
      vfs_close(pid_node);
    }

    cached = vfs_path_cache_lookup(fs_root, path);
    vfs_selftest_expect(
        !cached, "explicit /proc/<tid>/statm has no full-path cache entry");
    if (cached)
      vfs_close(cached);
  }

  klog_puts("[VFS-SELFTEST] procfs paths: ");
  klog_puts(vfs_selftest_failures == 0 ? "PASS" : "FAIL");
  klog_putchar('\n');
}

static void vfs_bench_report(const char *name, uint64_t ops, uint64_t cycles) {
  uint64_t ns = tsc_cycles_to_ns(cycles);
  klog_puts("[VFS-BENCH] ");
  klog_puts(name);
  klog_puts(": ");
  klog_uint64(ops);
  klog_puts(" ops, ");
  klog_uint64(ops ? cycles / ops : 0);
  klog_puts(" cycles/op");
  if (ns) {
    klog_puts(", ");
    klog_uint64(ops ? ns / ops : 0);
    klog_puts(" ns/op");
  }
  klog_putchar('\n');
}

/* Fill the path cache with VFS_BENCH_PATHS synthetic entries backed by a pool
 * of probe nodes.  Returns the number of nodes created (0 on allocation
 * failure); the caller owns them and must flush the cache before closing. */
static uint32_t vfs_bench_fill_cache(vfs_node_t *pool[VFS_BENCH_NODES]) {
  uint32_t created = 0;

  for (uint32_t i = 0; i < VFS_BENCH_NODES; i++) {
    pool[i] = vfs_probe_node_create("vfsbench");
    if (!pool[i])
      break;
    created++;
  }
  if (!created)
    return 0;

  char path[64];
  for (uint32_t i = 0; i < VFS_BENCH_PATHS; i++) {
    vfs_bench_make_path(path, i);
    vfs_path_cache_insert(fs_root, path, pool[i % created]);
  }
  return created;
}

static void vfs_path_cache_bench(void) {
  vfs_node_t *pool[VFS_BENCH_NODES];
  uint32_t created = vfs_bench_fill_cache(pool);
  if (!created)
    return;

  /* Old form: every removal walks all 1024 buckets and runs strstr() on each
   * valid entry.  The probe name matches nothing, so all iterations pay the
   * same full cost the old unlink path paid. */
  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_BENCH_ITERS; i++)
    vfs_path_cache_invalidate_name_scan(fs_root, "z_no_such_component_xyz");
  vfs_bench_report("path-inval old-scan", VFS_BENCH_ITERS,
                   rdtsc_fence() - t0);

  /* New form: one generation-stamped log append, no bucket touched. */
  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_BENCH_ITERS; i++)
    vfs_path_invalidate_record("z_no_such_component_xyz");
  vfs_bench_report("path-inval log-append", VFS_BENCH_ITERS,
                   rdtsc_fence() - t0);

  /* Lookup A/B: identical warm entries, once with the bucket lock forced and
   * once through the lockless seqlock path.  The delta is the lock. */
  const uint32_t hit_iters = VFS_BENCH_ITERS * 16;
  uint64_t locked_cycles;
  uint64_t lockless_cycles;
  uint64_t hits = 0;

  vfs_path_lookup_force_locked = true;
  uint64_t t_hit = rdtsc_fence();
  for (uint32_t i = 0; i < hit_iters; i++) {
    char pbuf[64];
    vfs_bench_make_path(pbuf, i % VFS_BENCH_PATHS);
    vfs_node_t *h = vfs_path_cache_lookup(fs_root, pbuf);
    if (h) {
      hits++;
      vfs_close(h);
    }
  }
  locked_cycles = rdtsc_fence() - t_hit;

  vfs_path_lookup_force_locked = false;
  t_hit = rdtsc_fence();
  for (uint32_t i = 0; i < hit_iters; i++) {
    char pbuf[64];
    vfs_bench_make_path(pbuf, i % VFS_BENCH_PATHS);
    vfs_node_t *h = vfs_path_cache_lookup(fs_root, pbuf);
    if (h) {
      hits++;
      vfs_close(h);
    }
  }
  lockless_cycles = rdtsc_fence() - t_hit;

  vfs_bench_report("cached dcache-hit locked  ", hit_iters, locked_cycles);
  vfs_bench_report("cached dcache-hit lockless", hit_iters, lockless_cycles);
  if (locked_cycles > lockless_cycles)
    vfs_bench_report("cached dcache-hit saved   ", hit_iters,
                     locked_cycles - lockless_cycles);
  if (!hits)
    klog_puts("[VFS-BENCH] path-cache A/B found no entries\n");

  vfs_path_cache_invalidate();
  for (uint32_t i = 0; i < created; i++)
    vfs_close(pool[i]);
}

/* The microbenchmark above isolates invalidation; this one runs the same
 * create+unlink pair a real workload issues, with the same warm cache, once
 * with the old scan forced and once with the log.  The difference is the
 * per-syscall cost the change removed. */
static bool vfs_path_unlink_bench(void) {
  vfs_node_t *dir = vfs_resolve_path("/tmp");
  if (!dir || (dir->flags & FS_TYPE_MASK) != FS_DIRECTORY || !dir->create ||
      !dir->unlink) {
    vfs_close(dir);
    return false;
  }

  char name[] = "vfsbench.tmp";
  vfs_node_t *pool[VFS_BENCH_NODES];
  uint32_t created = vfs_bench_fill_cache(pool);
  if (!created) {
    vfs_close(dir);
    return false;
  }

  /* Warm the tmpfs inode/dentry paths so the timed loops measure the steady
   * state, not first-touch. */
  for (uint32_t i = 0; i < 16; i++) {
    if (vfs_create(dir, name, 0644) == 0)
      vfs_unlink(dir, name);
  }
  vfs_unlink(dir, name);

  const uint32_t iters = 512;

  vfs_path_invalidation_force_scan = true;
  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < iters; i++) {
    if (vfs_create(dir, name, 0644) == 0)
      vfs_unlink(dir, name);
  }
  uint64_t old_cycles = rdtsc_fence() - t0;

  vfs_path_invalidation_force_scan = false;
  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < iters; i++) {
    if (vfs_create(dir, name, 0644) == 0)
      vfs_unlink(dir, name);
  }
  uint64_t new_cycles = rdtsc_fence() - t0;

  vfs_unlink(dir, name);
  vfs_bench_report("create+unlink old-inval", iters, old_cycles);
  vfs_bench_report("create+unlink new-inval", iters, new_cycles);
  if (old_cycles > new_cycles)
    vfs_bench_report("create+unlink saved   ", iters, old_cycles - new_cycles);

  vfs_path_cache_invalidate();
  for (uint32_t i = 0; i < created; i++)
    vfs_close(pool[i]);
  vfs_close(dir);
  return true;
}

/* ── Cold-leaf resolution A/B (`vfs_bench=1`) ───────────────────────────────
 *
 * The caching shape that misses both caches: the parent directories are hot
 * in the dentry cache, the leaf resolves for the first time (or right after
 * a directory write invalidated it).  The speculative walk proves the
 * prefix, then stops at the leaf; before prefix handoff the general walk
 * re-walked every parent bucket.  Both forms here resolve the same path
 * with the same warm prefix and a freshly invalidated leaf, so the delta is
 * the duplicated prefix walk.  The fixture is synthetic directories with an
 * on-demand finddir(), so no backend I/O is in the numbers. */

#define VFS_COLD_LEAF_ITERS 20000u

static void vfs_cold_leaf_bench(void) {
  vfs_node_t *base = vfs_mock_add("coldbase", FS_DIRECTORY, 0755);
  vfs_node_t *c1 = vfs_mock_add("c1", FS_DIRECTORY, 0755);
  vfs_node_t *c2 = vfs_mock_add("c2", FS_DIRECTORY, 0755);
  vfs_node_t *c3 = vfs_mock_add("c3", FS_DIRECTORY, 0755);
  vfs_node_t *leaf = vfs_mock_add("leaf", FS_FILE, 0644);
  if (!base || !c1 || !c2 || !c3 || !leaf) {
    vfs_mock_release_all();
    return;
  }
  vfs_mock_link(base, "c1", c1);
  vfs_mock_link(c1, "c2", c2);
  vfs_mock_link(c2, "c3", c3);
  vfs_mock_link(c3, "leaf", leaf);

  const char path[] = "c1/c2/c3/leaf";
  vfs_node_t *warm = vfs_resolve_path_at(base, path);
  if (warm)
    vfs_close(warm);
  if (warm != leaf) {
    klog_puts("[VFS-BENCH] cold-leaf fixture failed to resolve\n");
    vfs_mock_release(base);
    return;
  }

  uint64_t hits = 0;
  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_COLD_LEAF_ITERS; i++) {
    vfs_dentry_invalidate(c3, "leaf");
    vfs_node_t *hit = vfs_resolve_path_at(base, path);
    if (hit) {
      hits++;
      vfs_close(hit);
    }
  }
  uint64_t resume_cycles = rdtsc_fence() - t0;

  vfs_resolve_force_restart = true;
  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_COLD_LEAF_ITERS; i++) {
    vfs_dentry_invalidate(c3, "leaf");
    vfs_node_t *hit = vfs_resolve_path_at(base, path);
    if (hit) {
      hits++;
      vfs_close(hit);
    }
  }
  uint64_t restart_cycles = rdtsc_fence() - t0;
  vfs_resolve_force_restart = false;

  if (hits != (uint64_t)VFS_COLD_LEAF_ITERS * 2)
    klog_puts("[VFS-BENCH] cold-leaf: resolutions failed, numbers not "
              "comparable\n");

  vfs_bench_report("cold-leaf resume-prefix", VFS_COLD_LEAF_ITERS,
                   resume_cycles);
  vfs_bench_report("cold-leaf restart      ", VFS_COLD_LEAF_ITERS,
                   restart_cycles);
  if (restart_cycles > resume_cycles)
    vfs_bench_report("cold-leaf saved        ", VFS_COLD_LEAF_ITERS,
                     restart_cycles - resume_cycles);

  vfs_mock_release(base);
}

/* ── Lockless dentry lookup A/B (`vfs_bench=1`) ────────────────────────────
 *
 * The dentry cache serves every path component; the change these numbers
 * measure removed its per-bucket lock from the read side.  The single-CPU
 * rounds show the lock's own cost on a warm hit.  The multi-CPU phase has
 * every worker hammer the same bucket - the shared-library case - which is
 * also where the lock bounced the bucket cacheline between cores; the
 * stress phase churns that entry (insert/invalidate/free) under the workers
 * so the deferred-reclamation path runs at full tilt.  Both forms read the
 * same warm entry, so the delta is the lookup code, not the working set. */

#define VFS_DENTRY_BENCH_ITERS 200000u
#define VFS_DENTRY_BENCH_THREADS 3u
#define VFS_DENTRY_BENCH_STRESS_ITERS 200000u
#define VFS_DENTRY_BENCH_MUTATIONS 2000u

static vfs_node_t *vfs_dentry_bench_parent;
static vfs_node_t *vfs_dentry_bench_child;
static const char vfs_dentry_bench_name[] = "vfsbench_lookup";
static uint32_t vfs_dentry_bench_iters;
static volatile uint32_t vfs_dentry_bench_go;
static volatile uint32_t vfs_dentry_bench_done;
static volatile uint32_t vfs_dentry_bench_hits;
static volatile uint32_t vfs_dentry_bench_bad;

static void vfs_dentry_bench_worker(void) {
  while (!__atomic_load_n(&vfs_dentry_bench_go, __ATOMIC_ACQUIRE))
    sched_yield();
  uint32_t hits = 0;
  uint32_t bad = 0;
  for (uint32_t i = 0; i < vfs_dentry_bench_iters; i++) {
    bool negative = false;
    vfs_node_t *hit = vfs_dentry_lookup(
        vfs_dentry_bench_parent, vfs_dentry_bench_name, &negative);
    if (hit) {
      hits++;
      /* A returned reference must name a live node; a clobbered marker means
       * the reclamation protocol handed out a freed node. */
      if (hit->magic != VFS_NODE_MAGIC)
        bad++;
      vfs_close(hit);
    }
  }
  __atomic_add_fetch(&vfs_dentry_bench_hits, hits, __ATOMIC_RELAXED);
  __atomic_add_fetch(&vfs_dentry_bench_bad, bad, __ATOMIC_RELAXED);
  __atomic_add_fetch(&vfs_dentry_bench_done, 1, __ATOMIC_ACQ_REL);
}

static void vfs_dentry_bench_worker0(void) { vfs_dentry_bench_worker(); }
static void vfs_dentry_bench_worker1(void) { vfs_dentry_bench_worker(); }
static void vfs_dentry_bench_worker2(void) { vfs_dentry_bench_worker(); }

static uint64_t vfs_dentry_bench_single(bool force_locked) {
  vfs_dentry_lookup_force_locked = force_locked;
  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_DENTRY_BENCH_ITERS; i++) {
    bool negative = false;
    vfs_node_t *hit = vfs_dentry_lookup(
        vfs_dentry_bench_parent, vfs_dentry_bench_name, &negative);
    if (hit)
      vfs_close(hit);
  }
  uint64_t cycles = rdtsc_fence() - t0;
  vfs_dentry_lookup_force_locked = false;
  return cycles;
}

static void vfs_dentry_bench_report(const char *name, uint64_t ops,
                                    uint64_t cycles, uint64_t single_cycles) {
  klog_puts("[VFS-BENCH] ");
  klog_puts(name);
  klog_puts(": ");
  klog_uint64(ops);
  klog_puts(" ops, ");
  uint64_t per_op = ops ? cycles / ops : 0;
  klog_uint64(per_op);
  klog_puts(" cycles/op");
  if (single_cycles && per_op) {
    klog_puts(", ");
    klog_uint64(per_op * 100 / single_cycles);
    klog_puts("% of 1cpu");
  }
  klog_putchar('\n');
}

/* Start the workers and return how many came up. */
static uint32_t vfs_dentry_bench_start_workers(uint32_t iters) {
  static void (*const entries[VFS_DENTRY_BENCH_THREADS])(void) = {
      vfs_dentry_bench_worker0, vfs_dentry_bench_worker1,
      vfs_dentry_bench_worker2};

  __atomic_store_n(&vfs_dentry_bench_hits, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vfs_dentry_bench_bad, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vfs_dentry_bench_done, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vfs_dentry_bench_go, 0, __ATOMIC_RELEASE);
  vfs_dentry_bench_iters = iters;

  uint32_t started = 0;
  for (uint32_t i = 0; i < VFS_DENTRY_BENCH_THREADS; i++) {
    /* One worker per AP: a NULL explicit CPU would put them all on the BSP,
     * which is exactly the serialization these phases exist to expose. */
    struct cpu_info *target = cpu_get_info(i + 1);
    if (!target || target->status == CPU_STATUS_OFFLINE)
      break;
    if (!sched_create_kernel_thread(entries[i], target, true))
      break;
    started++;
  }
  if (started)
    __atomic_store_n(&vfs_dentry_bench_go, 1, __ATOMIC_RELEASE);
  return started;
}

static void vfs_dentry_bench_wait_workers(uint32_t started, uint64_t t0) {
  uint64_t deadline = tsc_get_freq_khz() * 500;
  while (__atomic_load_n(&vfs_dentry_bench_done, __ATOMIC_ACQUIRE) < started) {
    if (deadline && rdtsc_fence() - t0 > deadline)
      break;
    sched_yield();
  }
}

static void vfs_dentry_bench_run_phase(const char *name, bool force_locked,
                                       uint64_t single_cycles) {
  vfs_dentry_lookup_force_locked = force_locked;
  uint32_t started = vfs_dentry_bench_start_workers(VFS_DENTRY_BENCH_ITERS);
  if (!started) {
    vfs_dentry_lookup_force_locked = false;
    return;
  }

  uint64_t ops = 0;
  uint64_t t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_DENTRY_BENCH_ITERS; i++) {
    bool negative = false;
    vfs_node_t *hit = vfs_dentry_lookup(
        vfs_dentry_bench_parent, vfs_dentry_bench_name, &negative);
    if (hit)
      vfs_close(hit);
    ops++;
  }
  vfs_dentry_bench_wait_workers(started, t0);
  uint64_t cycles = rdtsc_fence() - t0;
  vfs_dentry_lookup_force_locked = false;

  ops += (uint64_t)started * VFS_DENTRY_BENCH_ITERS;
  vfs_dentry_bench_report(name, ops, cycles, single_cycles);
}

/* Churn the benchmark entry - fresh node, insert, invalidate, last close -
 * while the workers read it.  Every hit must name a live node; the deferred
 * free path is what keeps that true. */
static void vfs_dentry_bench_stress_phase(void) {
  uint32_t started =
      vfs_dentry_bench_start_workers(VFS_DENTRY_BENCH_STRESS_ITERS);
  if (!started)
    return;

  uint64_t t0 = rdtsc_fence();
  uint32_t mutations = 0;
  for (uint32_t i = 0; i < VFS_DENTRY_BENCH_MUTATIONS; i++) {
    vfs_node_t *victim = vfs_probe_node_create("vfsbench_lookup");
    if (!victim)
      break;
    vfs_dentry_insert(vfs_dentry_bench_parent, vfs_dentry_bench_name, victim);
    vfs_dentry_invalidate(vfs_dentry_bench_parent, vfs_dentry_bench_name);
    vfs_close(victim);
    mutations++;
  }
  vfs_dentry_bench_wait_workers(started, t0);
  uint64_t cycles = rdtsc_fence() - t0;

  uint64_t hits = __atomic_load_n(&vfs_dentry_bench_hits, __ATOMIC_RELAXED);
  uint64_t bad = __atomic_load_n(&vfs_dentry_bench_bad, __ATOMIC_RELAXED);
  klog_puts("[VFS-BENCH] 4cpu stress+churn: ");
  klog_uint64(mutations);
  klog_puts(" mutations, ");
  klog_uint64(hits);
  klog_puts(" hits, ");
  klog_uint64(bad);
  klog_puts(" dead, ");
  klog_uint64(cycles);
  klog_puts(" cycles\n");
}

static void vfs_dentry_lockless_bench(void) {
  vfs_dentry_bench_parent = vfs_probe_node_create("vfsbench_parent");
  vfs_dentry_bench_child = vfs_probe_node_create("vfsbench_lookup");
  if (!vfs_dentry_bench_parent || !vfs_dentry_bench_child) {
    vfs_close(vfs_dentry_bench_parent);
    vfs_close(vfs_dentry_bench_child);
    return;
  }
  vfs_dentry_bench_parent->flags = FS_DIRECTORY;
  vfs_dentry_insert(vfs_dentry_bench_parent, vfs_dentry_bench_name,
                    vfs_dentry_bench_child);

  /* Warm the entry so both A/B rounds measure the steady state. */
  for (uint32_t i = 0; i < 64; i++) {
    bool negative = false;
    vfs_node_t *hit = vfs_dentry_lookup(
        vfs_dentry_bench_parent, vfs_dentry_bench_name, &negative);
    if (hit)
      vfs_close(hit);
  }

  uint64_t locked = vfs_dentry_bench_single(true);
  uint64_t lockless = vfs_dentry_bench_single(false);
  vfs_bench_report("dentry hit locked  ", VFS_DENTRY_BENCH_ITERS, locked);
  vfs_bench_report("dentry hit lockless", VFS_DENTRY_BENCH_ITERS, lockless);
  if (locked > lockless)
    vfs_bench_report("dentry hit saved   ", VFS_DENTRY_BENCH_ITERS,
                     locked - lockless);

  if (cpu_get_count() >= 2) {
    klog_puts("[VFS-BENCH] multi-cpu dentry lookup, same bucket\n");
    vfs_dentry_bench_run_phase("4cpu dentry locked  ", true, locked);
    vfs_dentry_bench_run_phase("4cpu dentry lockless", false, lockless);
    vfs_dentry_bench_stress_phase();
  } else {
    klog_puts("[VFS-BENCH] multi-cpu dentry lookup skipped: single CPU\n");
  }

  vfs_dentry_invalidate(vfs_dentry_bench_parent, vfs_dentry_bench_name);
  vfs_close(vfs_dentry_bench_child);
  vfs_close(vfs_dentry_bench_parent);
}

void vfs_bench_maybe_run(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "vfs_bench"))
    return;

  klog_puts("[VFS-BENCH] path cache + page cache operations (");
  klog_uint64(VFS_BENCH_PATHS);
  klog_puts(" cached paths)\n");
  vfs_path_cache_bench();
  vfs_dentry_lockless_bench();
  vfs_cold_leaf_bench();
  if (!vfs_path_unlink_bench())
    klog_puts("[VFS-BENCH] create+unlink pair skipped: no writable /tmp\n");
  vfs_cache_bench();
  /* Last: the dirindex A/B creates and removes its own files under /tmp, so
   * it runs after the caches above have taken their measurements. */
  vfs_dirindex_bench();
  klog_puts("[VFS-BENCH] done\n");
}
