/* Host regression test for the real VFS cache lookup/eviction code.
 * Bucket locks use host atomics; page-cache teardown is stubbed because these
 * probe nodes contain no cached pages. This does not exercise IRQ semantics.
 * Compile/run instructions are in docs/journal-benchmark.md.
 */

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef strchr
#undef strrchr
#undef strstr
#define LOCK_SPINLOCK_H
/* Zero-initialized host locks model kernel bucket exclusion without cli/sti. */
typedef struct {
  volatile uint32_t locked;
  uint64_t saved_flags;
} spinlock_t;
typedef struct {
  volatile uint32_t locked;
} rawspinlock_t;
#define SPINLOCK_INIT {0}
#define RAWSPINLOCK_INIT {0}
static void spinlock_init(spinlock_t *s) { s->locked = 0; }
static void spinlock_acquire(spinlock_t *s) {
  while (__atomic_exchange_n(&s->locked, 1, __ATOMIC_ACQUIRE)) {
  }
}
static void spinlock_release(spinlock_t *s) {
  __atomic_store_n(&s->locked, 0, __ATOMIC_RELEASE);
}
#define sched_yield kernel_sched_yield
#ifdef TEST_VFS_SOURCE
#include TEST_VFS_SOURCE
#else
#include "../../kernel/src/fs/vfs.c"
#endif
#undef sched_yield
void vfs_cache_sync(vfs_node_t *n) { (void)n; }
void vfs_cache_clear(vfs_node_t *n) { (void)n; }
void vfs_xattr_clear(vfs_node_t *n) { (void)n; }
/* A freed allocation remains inside managed RAM in the kernel as well. */
bool pmm_kernel_ptr_is_managed(const void *p) { return p != NULL; }
void wait_queue_wake_all(void *q) { (void)q; }
static unsigned long allocated_nodes, freed_nodes;
void kfree(void *p) {
  __atomic_add_fetch(&freed_nodes, 1, __ATOMIC_RELAXED);
  free(p);
}
static vfs_node_t parent;
static char collision_names[16][64];
static int mode;
static uint32_t write_probe(vfs_node_t *n, uint32_t o, uint32_t z, uint8_t *b) {
  (void)n;
  (void)o;
  (void)b;
  return z;
}
static void *stress(void *arg) {
  unsigned id = (unsigned)(uintptr_t)arg;
  for (unsigned i = 0; i < 30000; i++) {
    const char *name = collision_names[(i + id) % 16];
    vfs_node_t *n = calloc(1, sizeof(*n));
    assert(n);
    __atomic_add_fetch(&allocated_nodes, 1, __ATOMIC_RELAXED);
    n->refcount = 1;
    n->magic = VFS_NODE_MAGIC;
    n->flags = FS_FILE;
    n->write = write_probe;
    if (mode)
      vfs_path_cache_insert(&parent, name, n);
    else
      vfs_dentry_insert(&parent, name, n);
    vfs_close(n);
    bool negative = false;
    vfs_node_t *hit = mode ? vfs_path_cache_lookup(&parent, name)
                           : vfs_dentry_lookup(&parent, name, &negative);
    if (hit) {
      assert(hit->magic == VFS_NODE_MAGIC && hit->refcount > 0 &&
             hit->write == write_probe);
      vfs_close(hit);
    }
  }
  return NULL;
}

static vfs_node_t rename_source;
static vfs_node_t *rename_finddir(vfs_node_t *dir, char *name) {
  (void)dir;
  if (strcmp(name, "old") != 0)
    return NULL;
  vfs_node_ref(&rename_source);
  return &rename_source;
}

static int rename_backend(vfs_node_t *dir, char *old_name, char *new_name) {
  /* Model readers refilling the old positive and new negative cache entries
   * while the backend waits for I/O, after pre-rename invalidation. */
  vfs_dentry_insert(dir, old_name, &rename_source);
  vfs_dentry_insert(dir, new_name, NULL);
  vfs_path_cache_insert(dir, old_name, &rename_source);
  return 0;
}

static void check_rename_invalidation(void) {
  rename_source.refcount = 1;
  rename_source.magic = VFS_NODE_MAGIC;
  rename_source.flags = FS_FILE | FS_PERSISTENT;
  parent.finddir = rename_finddir;
  parent.rename = rename_backend;
  assert(vfs_rename(&parent, "old", "new") == 0);
  bool negative;
  assert(vfs_dentry_lookup(&parent, "old", &negative) == NULL && !negative);
  assert(vfs_dentry_lookup(&parent, "new", &negative) == NULL && !negative);
  assert(vfs_path_cache_lookup(&parent, "old") == NULL);
  assert(parent.refcount == 1 && rename_source.refcount == 1);
  puts("PASS rename: pre-mutation entries refilled during backend I/O are retired");
}

int main(void) {
  parent.refcount = 1;
  parent.magic = VFS_NODE_MAGIC;
  parent.flags = FS_DIRECTORY;
  for (mode = 0; mode < 2; mode++) {
    unsigned found = 0;
    for (unsigned i = 0; found < 16; i++) {
      char name[64];
      snprintf(name, sizeof(name), "collision-%u", i);
      unsigned hash =
          mode ? vfs_path_hash(&parent, name) : vfs_dentry_hash(&parent, name);
      unsigned mask =
          mode ? VFS_PATH_CACHE_BUCKET_MASK : VFS_DENTRY_BUCKET_MASK;
      if (!(hash & mask))
        strcpy(collision_names[found++], name);
    }
    pthread_t t[4];
    for (unsigned i = 0; i < 4; i++)
      assert(!pthread_create(&t[i], NULL, stress, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++)
      assert(!pthread_join(t[i], NULL));
    for (unsigned i = 0; i < 16; i++) {
      if (mode)
        vfs_path_cache_drop(&parent, collision_names[i]);
      else
        vfs_dentry_bucket_invalidate(&parent, collision_names[i]);
    }
    assert(parent.refcount == 1);
    assert(allocated_nodes == freed_nodes);
    printf("PASS %s: 120000 inserts/lookups forcing one-bucket eviction; live "
           "references, no leaks\n",
           mode ? "path" : "dentry");
  }
  check_rename_invalidation();
}
