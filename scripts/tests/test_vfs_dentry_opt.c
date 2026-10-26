#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <x86intrin.h>

#define VFS_DENTRY_WAYS 4
#define VFS_DENTRY_BUCKETS 4096
#define VFS_DENTRY_BUCKET_MASK (VFS_DENTRY_BUCKETS - 1)

typedef struct vfs_node {
  uint32_t flags;
  uint32_t refcount;
  uint32_t inode;
  void *device;
  uint32_t mask;
  char name[128];
} vfs_node_t;

typedef struct vfs_dentry_cache_entry {
  vfs_node_t *parent;
  vfs_node_t *child;
  uint32_t parent_inode;
  void *parent_device;
  uint32_t hash;
  uint32_t comp_hash;
  uint32_t last_used;
  uint64_t name_prefix;
  uint16_t name_len;
  bool valid;
  char name[128];
} vfs_dentry_cache_entry_t;

typedef struct vfs_dentry_cache_bucket {
  vfs_dentry_cache_entry_t entries[VFS_DENTRY_WAYS];
  uint32_t seq;
  uint32_t lru_seq;
  pthread_spinlock_t lock;
} vfs_dentry_cache_bucket_t;

static vfs_dentry_cache_bucket_t g_dcache_old[VFS_DENTRY_BUCKETS];
static vfs_dentry_cache_bucket_t g_dcache_new[VFS_DENTRY_BUCKETS];

#define VFS_PATH_INVAL_LOG_SIZE 32
#define VFS_PATH_INVAL_NAME_MAX 256

typedef struct {
  uint64_t gen;
  char name[VFS_PATH_INVAL_NAME_MAX];
} path_inval_record_t;

static path_inval_record_t g_inval_log[VFS_PATH_INVAL_LOG_SIZE];
static uint64_t g_path_inval_gen = 100;
static pthread_spinlock_t g_inval_lock;

static inline uint32_t component_hash(const char *name, uint16_t *out_len, uint64_t *out_prefix) {
  uint32_t hash = 2166136261u;
  uint64_t prefix = 0;
  uint16_t len = 0;
  while (*name && len < 127) {
    uint8_t c = (uint8_t)*name++;
    if (len < 8) prefix |= ((uint64_t)c) << (len * 8);
    hash = (hash ^ c) * 16777619u;
    len++;
  }
  if (out_len) *out_len = len;
  if (out_prefix) *out_prefix = prefix;
  return hash;
}

static inline uint32_t dentry_hash_combine(vfs_node_t *parent, uint32_t comp_hash) {
  uint64_t parent_key = ((uint64_t)parent->inode * 2654435761ULL) ^ (uint64_t)(uintptr_t)parent->device;
  uint64_t h = (parent_key ^ 1469598103934665603ULL) ^ (uint64_t)comp_hash;
  h *= 1099511628211ULL;
  return (uint32_t)(h ^ (h >> 32));
}

static bool path_component_match(const char *path, const char *name, size_t name_len) {
  const char *p = path;
  while ((p = strstr(p, name)) != NULL) {
    bool left_bound = (p == path || *(p - 1) == '/');
    bool right_bound = (p[name_len] == '\0' || p[name_len] == '/');
    if (left_bound && right_bound) return true;
    p++;
  }
  return false;
}

/* Old Lookup: executes atomic increment of bucket->lru_seq and dirty store on EVERY read hit */
static vfs_node_t *old_dentry_lookup_fast(vfs_dentry_cache_bucket_t *cache, vfs_node_t *parent,
                                         const char *name, uint16_t len, uint32_t hash,
                                         uint32_t comp_hash, uint64_t name_prefix) {
  vfs_dentry_cache_bucket_t *bucket = &cache[hash & VFS_DENTRY_BUCKET_MASK];
  vfs_node_t *result = NULL;
  int action = 0;
  vfs_dentry_cache_entry_t *hit_entry = NULL;

  for (uint32_t attempt = 0; attempt < 2 && action == 0; attempt++) {
    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) break;

    uint32_t state = 0;
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
      if (!__atomic_load_n(&entry->valid, __ATOMIC_RELAXED)) continue;
      if (__atomic_load_n(&entry->hash, __ATOMIC_RELAXED) != hash) continue;
      if (__atomic_load_n(&entry->comp_hash, __ATOMIC_RELAXED) != comp_hash) continue;
      if (__atomic_load_n(&entry->parent, __ATOMIC_RELAXED) != parent) continue;
      if (__atomic_load_n(&entry->name_len, __ATOMIC_RELAXED) != len) continue;
      if (__atomic_load_n(&entry->name_prefix, __ATOMIC_RELAXED) != name_prefix) continue;
      if (len > 8 && memcmp(entry->name + 8, name + 8, len - 8) != 0) continue;

      result = __atomic_load_n(&entry->child, __ATOMIC_RELAXED);
      hit_entry = entry;
      state = result ? 1u : 2u;
      break;
    }
    if (__atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE) != s1) continue;
    if (state == 1) {
      if (hit_entry)
        __atomic_store_n(&hit_entry->last_used,
                         __atomic_add_fetch(&bucket->lru_seq, 1, __ATOMIC_RELAXED),
                         __ATOMIC_RELAXED);
      action = 3;
    }
  }
  return (action == 3) ? result : NULL;
}

/* New Lookup: coarse-grained / check-before-write LRU (Zero cache bouncing on hit) */
static vfs_node_t *new_dentry_lookup_fast(vfs_dentry_cache_bucket_t *cache, vfs_node_t *parent,
                                         const char *name, uint16_t len, uint32_t hash,
                                         uint32_t comp_hash, uint64_t name_prefix) {
  vfs_dentry_cache_bucket_t *bucket = &cache[hash & VFS_DENTRY_BUCKET_MASK];
  vfs_node_t *result = NULL;
  int action = 0;
  vfs_dentry_cache_entry_t *hit_entry = NULL;

  for (uint32_t attempt = 0; attempt < 2 && action == 0; attempt++) {
    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) {
      asm volatile("pause");
      continue;
    }

    uint32_t state = 0;
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
      if (!__atomic_load_n(&entry->valid, __ATOMIC_RELAXED)) continue;
      if (__atomic_load_n(&entry->hash, __ATOMIC_RELAXED) != hash) continue;
      if (__atomic_load_n(&entry->comp_hash, __ATOMIC_RELAXED) != comp_hash) continue;
      if (__atomic_load_n(&entry->parent, __ATOMIC_RELAXED) != parent) continue;
      if (__atomic_load_n(&entry->name_len, __ATOMIC_RELAXED) != len) continue;
      if (__atomic_load_n(&entry->name_prefix, __ATOMIC_RELAXED) != name_prefix) continue;
      if (len > 8 && memcmp(entry->name + 8, name + 8, len - 8) != 0) continue;

      result = __atomic_load_n(&entry->child, __ATOMIC_RELAXED);
      hit_entry = entry;
      state = result ? 1u : 2u;
      break;
    }
    if (__atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE) != s1) continue;
    if (state == 1) {
      if (hit_entry) {
        uint32_t cur_lru = __atomic_load_n(&bucket->lru_seq, __ATOMIC_RELAXED);
        if (__atomic_load_n(&hit_entry->last_used, __ATOMIC_RELAXED) != cur_lru)
          __atomic_store_n(&hit_entry->last_used, cur_lru, __ATOMIC_RELAXED);
      }
      action = 3;
    }
  }
  return (action == 3) ? result : NULL;
}

/* Generation validation benchmark: Old (fallback to locked path) vs New (lockless replay) */
static bool old_validate_generation(uint64_t entry_gen, const char *path) {
  pthread_spin_lock(&g_inval_lock);
  uint64_t gen = g_path_inval_gen;
  if (entry_gen > gen || gen - entry_gen > VFS_PATH_INVAL_LOG_SIZE) {
    pthread_spin_unlock(&g_inval_lock);
    return false;
  }
  bool valid = true;
  for (uint64_t g = entry_gen + 1; g <= gen; g++) {
    path_inval_record_t *rec = &g_inval_log[g % VFS_PATH_INVAL_LOG_SIZE];
    size_t nlen = strlen(rec->name);
    if (nlen && path_component_match(path, rec->name, nlen)) {
      valid = false;
      break;
    }
  }
  pthread_spin_unlock(&g_inval_lock);
  return valid;
}

static inline bool new_validate_generation(uint64_t entry_gen, const char *path) {
  uint64_t cur_gen = __atomic_load_n(&g_path_inval_gen, __ATOMIC_ACQUIRE);
  if (entry_gen == cur_gen) return true;
  if (entry_gen > cur_gen || cur_gen - entry_gen > VFS_PATH_INVAL_LOG_SIZE) return false;

  for (uint64_t g = entry_gen + 1; g <= cur_gen; g++) {
    path_inval_record_t *rec = &g_inval_log[g % VFS_PATH_INVAL_LOG_SIZE];
    uint64_t r_gen = __atomic_load_n(&rec->gen, __ATOMIC_ACQUIRE);
    if (r_gen != g) return false;
    size_t nlen = strlen(rec->name);
    if (__atomic_load_n(&rec->gen, __ATOMIC_ACQUIRE) != g) return false;
    if (nlen && path_component_match(path, rec->name, nlen)) return false;
  }
  return true;
}

#define THREADS 4
#define ITERS 1000000

typedef struct {
  int tid;
  vfs_dentry_cache_bucket_t *cache;
  vfs_node_t *parent;
  const char *name;
  uint16_t len;
  uint32_t hash;
  uint32_t comp_hash;
  uint64_t name_prefix;
  bool use_new;
  uint64_t cycles;
} worker_arg_t;

static void *worker_func(void *arg) {
  worker_arg_t *w = (worker_arg_t *)arg;
  uint64_t t0 = __rdtsc();
  if (w->use_new) {
    for (int i = 0; i < ITERS; i++) {
      vfs_node_t *n = new_dentry_lookup_fast(w->cache, w->parent, w->name,
                                            w->len, w->hash, w->comp_hash, w->name_prefix);
      (void)n;
    }
  } else {
    for (int i = 0; i < ITERS; i++) {
      vfs_node_t *n = old_dentry_lookup_fast(w->cache, w->parent, w->name,
                                            w->len, w->hash, w->comp_hash, w->name_prefix);
      (void)n;
    }
  }
  w->cycles = __rdtsc() - t0;
  return NULL;
}

int main(void) {
  pthread_spin_init(&g_inval_lock, PTHREAD_PROCESS_PRIVATE);

  vfs_node_t parent = {.inode = 100, .device = (void *)0x1000, .flags = 0x02};
  vfs_node_t child = {.inode = 101, .device = (void *)0x1000, .flags = 0x01};

  const char *test_name = "libc.so.6";
  uint16_t len = 0;
  uint64_t prefix = 0;
  uint32_t comp_h = component_hash(test_name, &len, &prefix);
  uint32_t dhash = dentry_hash_combine(&parent, comp_h);

  uint32_t slot = dhash & VFS_DENTRY_BUCKET_MASK;

  // Init old cache
  pthread_spin_init(&g_dcache_old[slot].lock, PTHREAD_PROCESS_PRIVATE);
  g_dcache_old[slot].seq = 2;
  g_dcache_old[slot].lru_seq = 1;
  g_dcache_old[slot].entries[0].valid = true;
  g_dcache_old[slot].entries[0].parent = &parent;
  g_dcache_old[slot].entries[0].child = &child;
  g_dcache_old[slot].entries[0].hash = dhash;
  g_dcache_old[slot].entries[0].comp_hash = comp_h;
  g_dcache_old[slot].entries[0].name_prefix = prefix;
  g_dcache_old[slot].entries[0].name_len = len;
  strcpy(g_dcache_old[slot].entries[0].name, test_name);

  // Init new cache
  pthread_spin_init(&g_dcache_new[slot].lock, PTHREAD_PROCESS_PRIVATE);
  g_dcache_new[slot].seq = 2;
  g_dcache_new[slot].lru_seq = 1;
  g_dcache_new[slot].entries[0].valid = true;
  g_dcache_new[slot].entries[0].parent = &parent;
  g_dcache_new[slot].entries[0].child = &child;
  g_dcache_new[slot].entries[0].hash = dhash;
  g_dcache_new[slot].entries[0].comp_hash = comp_h;
  g_dcache_new[slot].entries[0].name_prefix = prefix;
  g_dcache_new[slot].entries[0].name_len = len;
  strcpy(g_dcache_new[slot].entries[0].name, test_name);

  printf("=== Microbenchmark 1: Multi-Core Dentry Cache Hit (%d Threads, %d Ops Each) ===\n", THREADS, ITERS);

  // Run Old
  pthread_t threads[THREADS];
  worker_arg_t args_old[THREADS];
  for (int i = 0; i < THREADS; i++) {
    args_old[i] = (worker_arg_t){
      .tid = i, .cache = g_dcache_old, .parent = &parent, .name = test_name,
      .len = len, .hash = dhash, .comp_hash = comp_h, .name_prefix = prefix,
      .use_new = false
    };
    pthread_create(&threads[i], NULL, worker_func, &args_old[i]);
  }
  uint64_t max_cycles_old = 0;
  for (int i = 0; i < THREADS; i++) {
    pthread_join(threads[i], NULL);
    if (args_old[i].cycles > max_cycles_old) max_cycles_old = args_old[i].cycles;
  }
  double old_per_op = (double)max_cycles_old / ITERS;
  printf("Old (Atomic LRU increment per hit):   %.2f cycles/lookup\n", old_per_op);

  // Run New
  worker_arg_t args_new[THREADS];
  for (int i = 0; i < THREADS; i++) {
    args_new[i] = (worker_arg_t){
      .tid = i, .cache = g_dcache_new, .parent = &parent, .name = test_name,
      .len = len, .hash = dhash, .comp_hash = comp_h, .name_prefix = prefix,
      .use_new = true
    };
    pthread_create(&threads[i], NULL, worker_func, &args_new[i]);
  }
  uint64_t max_cycles_new = 0;
  for (int i = 0; i < THREADS; i++) {
    pthread_join(threads[i], NULL);
    if (args_new[i].cycles > max_cycles_new) max_cycles_new = args_new[i].cycles;
  }
  double new_per_op = (double)max_cycles_new / ITERS;
  printf("New (Read-Mostly Lockless Dentry Hit): %.2f cycles/lookup\n", new_per_op);
  printf("Speedup: %.2fX faster (%.1f%% latency reduction)\n\n",
         old_per_op / new_per_op, (1.0 - (new_per_op / old_per_op)) * 100.0);

  printf("=== Microbenchmark 2: Generation Counter Invalidation Revalidation ===\n");
  // Set up recent invalidations in log
  for (uint64_t g = 96; g <= 100; g++) {
    path_inval_record_t *r = &g_inval_log[g % VFS_PATH_INVAL_LOG_SIZE];
    r->gen = g;
    snprintf(r->name, sizeof(r->name), "temp_file_%lu.tmp", g);
  }
  const char *test_path = "/usr/lib/x86_64-linux-gnu/libc.so";
  uint64_t stale_gen = 95; // 5 generations behind

  int val_iters = 2000000;
  uint64_t t0 = __rdtsc();
  for (int i = 0; i < val_iters; i++) {
    bool ok = old_validate_generation(stale_gen, test_path);
    (void)ok;
  }
  uint64_t old_val_cyc = __rdtsc() - t0;
  double old_val_per = (double)old_val_cyc / val_iters;
  printf("Old (Locked Log Replay under Spinlock): %.2f cycles/check\n", old_val_per);

  t0 = __rdtsc();
  for (int i = 0; i < val_iters; i++) {
    bool ok = new_validate_generation(stale_gen, test_path);
    (void)ok;
  }
  uint64_t new_val_cyc = __rdtsc() - t0;
  double new_val_per = (double)new_val_cyc / val_iters;
  printf("New (Lockless Log Replay):              %.2f cycles/check\n", new_val_per);
  printf("Speedup: %.2fX faster (%.1f%% latency reduction)\n",
         old_val_per / new_val_per, (1.0 - (new_val_per / old_val_per)) * 100.0);

  return 0;
}
