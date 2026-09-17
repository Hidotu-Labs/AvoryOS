#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"

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
  struct thread *t = sched_get_current();
  if (!t || t->euid == 0) {
    if ((requested & 1) && !(node->mask & 0111) &&
        (node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return false;
    return true;
  }
  uint32_t bits = node->mask & 7;
  if (t->fsuid == node->uid) bits = (node->mask >> 6) & 7;
  else if (vfs_in_group(node->gid)) bits = (node->mask >> 3) & 7;
  return (bits & requested) == requested;
}

bool vfs_may_remove(vfs_node_t *parent, vfs_node_t *target) {
  struct thread *t = sched_get_current();
  if (!t || t->euid == 0) return true;
  if (!vfs_access(parent, 3)) return false;
  if ((parent->mask & 01000) && t->fsuid != parent->uid &&
      (!target || t->fsuid != target->uid)) return false;
  return true;
}

typedef struct vfs_mount_entry {
  vfs_node_t *mountpoint;
  vfs_node_t *target;
  char dev_name[64];
  char fs_type[32];
  struct vfs_mount_entry *next;
} vfs_mount_entry_t;

static vfs_mount_entry_t *vfs_mount_list = NULL;

/* Bounded 4-way dentry cache. Positive entries own a child reference;
 * positive and negative entries both own a parent reference. */
#define VFS_DENTRY_CACHE_SIZE 16384
#define VFS_DENTRY_WAYS 4
#define VFS_DENTRY_BUCKETS (VFS_DENTRY_CACHE_SIZE / VFS_DENTRY_WAYS)

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
  uint8_t next_victim;
  spinlock_t lock;
} vfs_dentry_cache_bucket_t;

static vfs_dentry_cache_bucket_t vfs_dentry_cache[VFS_DENTRY_BUCKETS];

static uint32_t vfs_dentry_hash(vfs_node_t *parent, const char *name) {
  uint64_t hash = ((uint64_t)(uintptr_t)parent >> 4) ^ 1469598103934665603ULL;
  while (*name) {
    hash ^= (uint8_t)*name++;
    hash *= 1099511628211ULL;
  }
  return (uint32_t)(hash ^ (hash >> 32));
}

static vfs_node_t *vfs_dentry_lookup(vfs_node_t *parent, const char *name,
                                     bool *negative) {
  *negative = false;
  if (!parent || !name || (parent->flags & FS_DENTRY_NOCACHE))
    return NULL;

  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
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

static void vfs_dentry_insert(vfs_node_t *parent, const char *name,
                              vfs_node_t *child) {
  if (!parent || !name)
    return;

  size_t len_sz = strlen(name);
  if (len_sz >= 128 || (parent->flags & FS_DENTRY_NOCACHE))
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
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
  entry->parent = parent;
  entry->child = child;
  entry->hash = hash;
  entry->name_len = len;
  memcpy(entry->name, name, len);
  entry->name[len] = '\0';
  entry->valid = true;
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

static vfs_node_t *vfs_path_cache_lookup(vfs_node_t *dir, const char *path) {
  if (!dir || !path)
    return NULL;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
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
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
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

static void vfs_path_cache_insert(vfs_node_t *dir, const char *path, vfs_node_t *node) {
  if (!dir || !node || !path || (node->flags & FS_DENTRY_NOCACHE))
    return;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
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
  entry->dir = dir;
  entry->node = node;
  entry->hash = hash;
  entry->path_len = len;
  memcpy(entry->path, path, len);
  entry->path[len] = '\0';
  entry->gen = __atomic_load_n(&vfs_path_inval_gen, __ATOMIC_ACQUIRE);
  entry->valid = true;
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
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
  vfs_node_t *released[VFS_DENTRY_WAYS];
  vfs_node_t *released_parents[VFS_DENTRY_WAYS];
  uint32_t released_count = 0;

  spinlock_acquire(&bucket->lock);
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
  spinlock_release(&bucket->lock);

  for (uint32_t i = 0; i < released_count; i++) {
    if (released[i])
      vfs_close(released[i]);
    vfs_close(released_parents[i]);
  }
}

/* Drop the exact (dir, path) fast-path resolution, if present. */
static void vfs_path_cache_drop(vfs_node_t *dir, const char *path) {
  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
  vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
  vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
  uint32_t count = 0;

  spinlock_acquire(&bucket->lock);
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
   * bucket holding (parent, name), and the path cache only needs a record in
   * the invalidation log - each entry rechecks it lazily on its next lookup.
   * A NULL argument means "everything", which genuinely does need the full
   * table scan (e.g. unmounts). */
  if (parent && name) {
    if (vfs_path_invalidation_force_scan)
      vfs_path_cache_invalidate_name_scan(parent, name);
    else
      vfs_path_invalidate_record(name);
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
    return 0;

  struct thread *t = sched_get_current();
  if (size && t && t->euid != 0 && (node->mask & 06000))
    vfs_chmod(node, (uint16_t)(node->mask & ~06000));

  if (node->write) {
    uint32_t written = node->write(node, offset, size, buffer);
    if (written && (node->flags & FS_PAGE_CACHE))
      vfs_cache_update_or_invalidate(node, offset, written, buffer);
    return written;
  }
  return 0;
}

void vfs_open(vfs_node_t *node) {
  if (!node)
    return;
  vfs_node_ref(node);
  if (node->open) {
    node->open(node);
  }
}

void vfs_close(vfs_node_t *node) {
  if (!node)
    return;

  bool is_pipe = (node->flags & FS_TYPE_MASK) == FS_PIPE;
  void *wq = node->wait_queue;

  if (vfs_node_unref(node)) {
    if (node->close) {
      node->close(node);
    }
    vfs_cache_sync(node);
    vfs_cache_clear(node);
    node->ep_watchers.next = NULL;
    node->ep_watchers.prev = NULL;
    if (!(node->flags & FS_PERSISTENT)) {
      node->magic = 0;
      kfree(node);
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

struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->readdir) {
    return node->readdir(node, index);
  }
  return 0;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->finddir) {
    bool negative;
    vfs_node_t *cached = vfs_dentry_lookup(node, name, &negative);
    if (cached || negative)
      return cached;

    vfs_node_t *res = node->finddir(node, name);
    if (!res) {
      vfs_dentry_insert(node, name, NULL);
      return 0;
    }

    vfs_mount_entry_t *curr = vfs_mount_list;
    while (curr) {
      if (curr->mountpoint && curr->mountpoint->inode == res->inode &&
          curr->mountpoint->device == res->device) {
        if (curr->target != res) {
          vfs_close(res);
          vfs_open(curr->target);
          vfs_dentry_insert(node, name, curr->target);
          return curr->target;
        }
      }
      curr = curr->next;
    }
    vfs_dentry_insert(node, name, res);
    return res;
  }
  return 0;
}

int vfs_create(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->create) {
    int result = node->create(node, name, permission);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mkdir) {
    int result = node->mkdir(node, name, permission);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_unlink(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->unlink) {
    vfs_dentry_invalidate(node, name);
    return node->unlink(node, name);
  }
  return -1;
}

int vfs_rmdir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rmdir) {
    vfs_dentry_invalidate(node, name);
    return node->rmdir(node, name);
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
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rename) {
    vfs_dentry_invalidate(node, old_name);
    vfs_dentry_invalidate(node, new_name);
    return node->rename(node, old_name, new_name);
  }
  return -1;
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
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_truncate(vfs_node_t *node, uint32_t size) {
  if (node && node->truncate) {
    int result = node->truncate(node, size);
    if (!result && (node->flags & FS_PAGE_CACHE))
      vfs_cache_clear(node);
    return result;
  }
  return -1;
}

int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {
  if (node && node->fallocate) {
    return node->fallocate(node, mode, offset, len);
  }
  if (node && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    // Mode 0: standard allocation (posix_fallocate)
    if (mode == 0) {
      uint32_t required = offset + len;
      if (required > node->length) {
        if (node->truncate)
          return node->truncate(node, required);
        node->length = required;
      }
      return 0;
    }
  }
  return -1;
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

vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path) {
  if (!path || !fs_root)
    return 0;

  vfs_node_t *effective_dir = (path[0] == '/') ? fs_root : (dir ? dir : fs_root);

  // 1. Fast Path Full-Path Dcache lookup:
  // Check if we already resolved this exact path string from this directory
  vfs_node_t *cached = vfs_path_cache_lookup(effective_dir, path);
  if (cached) {
    return cached;
  }

  char path_buf[512];
  strncpy(path_buf, path, 511);
  path_buf[511] = '\0';

  vfs_node_t *current = effective_dir;
  vfs_open(current); // Reference for 'current'

  int symlink_depth = 0;
  char *p = path_buf;

#define VFS_PARENT_STACK_DEPTH 32
  vfs_node_t *parent_stack[VFS_PARENT_STACK_DEPTH];
  int stack_top = -1; // Stack is empty initially

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

    if (strcmp(comp, "..") == 0) {
      if (stack_top >= 0) {
        vfs_close(current);
        current = parent_stack[stack_top];
        stack_top--;
        // Ownership transferred from stack to 'current'
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
      }
      continue;
    }

    // Descent
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

  // Cache successfully resolved path
  if (current && strstr(path, "..") == NULL) {
    vfs_path_cache_insert(effective_dir, path, current);
  }

  return current;


fail:
  for (int j = 0; j <= stack_top; j++)
    vfs_close(parent_stack[j]);
  vfs_close(current);
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
                 const char *dev_name, const char *fs_type) {
  if (!target)
    return -1;

  vfs_mount_entry_t *entry = kmalloc(sizeof(vfs_mount_entry_t));
  if (!entry)
    return -1;
  entry->mountpoint = mountpoint;
  entry->target = target;
  strncpy(entry->dev_name, dev_name ? dev_name : "none", 63);
  strncpy(entry->fs_type, fs_type ? fs_type : "unknown", 31);
  entry->next = vfs_mount_list;
  vfs_mount_list = entry;

  /* A prior lookup may have cached the node hidden by this mount. */
  vfs_dentry_invalidate(NULL, NULL);

  target->flags |= FS_PERSISTENT;

  if (mountpoint) {
    mountpoint->flags |= FS_MOUNTPOINT | FS_PERSISTENT;
    mountpoint->ptr = target;
  }

  return 0;
}

int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target) {
  return vfs_mount_ex(mountpoint, target, "none", "unknown");
}

int vfs_statfs(vfs_node_t *node, void *buf) {
  if (node && node->statfs) {
    return node->statfs(node, buf);
  }
  return -1;
}

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count) {
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
      strcpy(buffer[count].mountpoint, "/");
      strcpy(buffer[count].target, "/");
      strcpy(buffer[count].dev_name, "none");
      strcpy(buffer[count].fs_type, "ext3"); // Common default for this OS
      count++;
    }
  }

  vfs_mount_entry_t *curr = vfs_mount_list;
  while (curr && count < max_count) {
    if (curr->mountpoint) {
      strncpy(buffer[count].mountpoint, curr->mountpoint->name, 127);
      buffer[count].mountpoint[127] = '\0';
    } else {
      strcpy(buffer[count].mountpoint, "/");
    }

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

  vfs_path_cache_drop(fs_root, "/vfs_selftest_probe");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_probe_extra");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_dir/child");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_ring");
  vfs_path_cache_drop(fs_root, "/vfs_selftest_fresh");
  vfs_close(probe);
  return vfs_selftest_failures == 0;
}

void vfs_selftest_maybe_run(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "vfs_selftest"))
    return;

  vfs_selftest_failures = 0;
  klog_puts("[VFS-SELFTEST] path cache invalidation + page cache refs\n");
  bool path_ok = vfs_path_cache_selftest();
  bool page_ok = vfs_cache_ref_selftest();
  klog_puts("[VFS-SELFTEST] path cache: ");
  klog_puts(path_ok ? "PASS" : "FAIL");
  klog_puts(", page cache refs: ");
  klog_puts(page_ok ? "PASS" : "FAIL");
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

void vfs_bench_maybe_run(void) {
  if (!kernel_boot_cmdline || !strstr(kernel_boot_cmdline, "vfs_bench"))
    return;

  klog_puts("[VFS-BENCH] path cache + page cache operations (");
  klog_uint64(VFS_BENCH_PATHS);
  klog_puts(" cached paths)\n");
  vfs_path_cache_bench();
  if (!vfs_path_unlink_bench())
    klog_puts("[VFS-BENCH] create+unlink pair skipped: no writable /tmp\n");
  vfs_cache_bench();
  klog_puts("[VFS-BENCH] done\n");
}
