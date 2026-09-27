#include "dirindex.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lib/tsc.h"
#include "../mm/heap.h"

/* First insertion allocates this many chain heads (128 bytes, lazily, so
 * empty directories pay nothing); the table doubles whenever a child would
 * push the load past one.  Both bounds keep a bucket chain to a handful of
 * names even in the few-thousand-entry directories this exists for. */
#define VFS_DIRINDEX_MIN_BUCKETS 16

/* FNV-1a folded to 32 bits; bucket = hash & (nbuckets - 1). */
static uint32_t dirindex_hash(const char *name) {
  uint64_t h = 14695981039346656037ULL;
  while (*name) {
    h ^= (uint8_t)*name++;
    h *= 1099511628211ULL;
  }
  return (uint32_t)(h ^ (h >> 32));
}

/* Bench-only: find walks the order list with strcmp() exactly as the
 * pre-hash backends did.  See the note in dirindex.h - detach is O(1) in
 * both forms, so the scan numbers are a slight understatement of the old
 * cost. */
static bool vfs_dirindex_force_scan;

void vfs_dirindex_set_force_scan(bool force) {
  vfs_dirindex_force_scan = force;
}

void vfs_dirindex_init(vfs_dirindex_t *di) {
  di->children = NULL;
  di->cursor = NULL;
  di->cursor_index = 0;
  di->buckets = NULL;
  di->nbuckets = 0;
  di->count = 0;
  spinlock_init(&di->lock);
}

void vfs_dirindex_destroy(vfs_dirindex_t *di) {
  /* Runs from the node's destroy callback: the directory's last reference
   * is already gone, so no lock is needed and nothing else can be walking
   * the table.  The child wrappers were freed by that callback's list walk
   * before it called us. */
  vfs_child_t **buckets = di->buckets;
  di->buckets = NULL;
  di->nbuckets = 0;
  if (buckets)
    kfree(buckets);
}

vfs_child_t *vfs_dirindex_find_locked(vfs_dirindex_t *di, const char *name) {
  if (vfs_dirindex_force_scan) {
    for (vfs_child_t *c = di->children; c; c = c->next)
      if (strcmp(c->node->name, name) == 0)
        return c;
    return NULL;
  }

  if (!di->buckets)
    return NULL;
  uint32_t h = dirindex_hash(name);
  for (vfs_child_t *c = di->buckets[h & (di->nbuckets - 1)]; c; c = c->hnext)
    if (strcmp(c->node->name, name) == 0)
      return c;
  return NULL;
}

vfs_child_t *vfs_dirindex_find(vfs_dirindex_t *di, const char *name) {
  vfs_dirindex_lock(di);
  vfs_child_t *c = vfs_dirindex_find_locked(di, name);
  vfs_dirindex_unlock(di);
  return c;
}

bool vfs_dirindex_insert(vfs_dirindex_t *di, vfs_child_t *child) {
  /* Decide on and allocate a bucket array BEFORE taking the lock (see the
   * header: allocation under an IRQ-masked critical section can deadlock
   * against an interrupt handler that allocates).  The reads of buckets /
   * count here are racy hints; the adoption is re-checked under the lock. */
  vfs_child_t **fresh = NULL;
  uint32_t fresh_n = 0;
  if (!di->buckets || di->count + 1 > di->nbuckets) {
    fresh_n = di->nbuckets ? di->nbuckets * 2 : VFS_DIRINDEX_MIN_BUCKETS;
    fresh = kmalloc(fresh_n * sizeof(*fresh));
  }

  vfs_child_t **old = NULL;
  bool adopted = false;

  vfs_dirindex_lock(di);
  if (!di->buckets) {
    /* First child in this directory: the array is mandatory. */
    if (fresh) {
      for (uint32_t i = 0; i < fresh_n; i++)
        fresh[i] = NULL;
      di->buckets = fresh;
      di->nbuckets = fresh_n;
      adopted = true;
    }
  } else if (fresh && di->count + 1 > di->nbuckets) {
    /* Growth: rechain every entry into the doubled table by walking the
     * order list (the list is untouched by a rehash, so readdir cursors and
     * ordering survive it). */
    for (uint32_t i = 0; i < fresh_n; i++)
      fresh[i] = NULL;
    for (vfs_child_t *c = di->children; c; c = c->next) {
      uint32_t b = dirindex_hash(c->node->name) & (fresh_n - 1);
      c->hnext = fresh[b];
      fresh[b] = c;
    }
    old = di->buckets;
    di->buckets = fresh;
    di->nbuckets = fresh_n;
    adopted = true;
  }

  if (!di->buckets) {
    /* First insertion and its array could not be allocated. */
    vfs_dirindex_unlock(di);
    return false;
  }

  /* Order list: prepend, keeping readdir order identical to the old
   * backends (newest first). */
  child->prev = NULL;
  child->next = di->children;
  if (di->children)
    di->children->prev = child;
  di->children = child;

  uint32_t b = dirindex_hash(child->node->name) & (di->nbuckets - 1);
  child->hnext = di->buckets[b];
  di->buckets[b] = child;
  di->count++;
  vfs_dirindex_unlock(di);

  if (fresh && !adopted)
    kfree(fresh);
  if (old)
    kfree(old);
  return true;
}

void vfs_dirindex_detach_locked(vfs_dirindex_t *di, vfs_child_t *child) {
  /* Order list, O(1) via the prev pointer the hash lookup handed us. */
  if (child->prev)
    child->prev->next = child->next;
  else
    di->children = child->next;
  if (child->next)
    child->next->prev = child->prev;
  child->prev = NULL;
  child->next = NULL;

  /* Hash chain: splice the entry out of the bucket its CURRENT name hashes
   * to. */
  if (di->buckets) {
    uint32_t b = dirindex_hash(child->node->name) & (di->nbuckets - 1);
    vfs_child_t **pp = &di->buckets[b];
    while (*pp && *pp != child)
      pp = &(*pp)->hnext;
    if (*pp == child)
      *pp = child->hnext;
  }
  child->hnext = NULL;

  if (di->count)
    di->count--;

  /* Any removal shifts the index of everything behind it, so a cursor
   * trained before it can no longer resume correctly; drop it and let the
   * next sequential getdents rescan from the head. */
  di->cursor = NULL;
  di->cursor_index = 0;
}

void vfs_dirindex_rename_locked(vfs_dirindex_t *di, vfs_child_t *child,
                                const char *new_name) {
  /* Unchain under the OLD name (the entry must still be in the bucket
   * node->name hashes to), rename, rechain.  The order list is unaffected -
   * a rename does not move the entry, so readdir ordering and any cursor
   * stay valid. */
  if (di->buckets) {
    uint32_t b = dirindex_hash(child->node->name) & (di->nbuckets - 1);
    vfs_child_t **pp = &di->buckets[b];
    while (*pp && *pp != child)
      pp = &(*pp)->hnext;
    if (*pp == child)
      *pp = child->hnext;
    child->hnext = NULL;
  }

  strncpy(child->node->name, new_name, 127);
  child->node->name[127] = '\0';

  if (di->buckets) {
    uint32_t b = dirindex_hash(child->node->name) & (di->nbuckets - 1);
    child->hnext = di->buckets[b];
    di->buckets[b] = child;
  }
}

vfs_node_t *vfs_dirindex_finddir(vfs_node_t *node, char *name) {
  if (!node || !node->device)
    return NULL;
  if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
    return node;

  vfs_dirindex_t *di = vfs_dirindex_of(node);
  vfs_dirindex_lock(di);
  vfs_child_t *c = vfs_dirindex_find_locked(di, name);
  /* Read the child's node pointer under the lock: a concurrent unlink can
   * detach and free the wrapper the moment we drop it.  The pointer is
   * returned bare - for internal existence checks only.  Backend wiring
   * must use vfs_dirindex_finddir_ref() so vfs_finddir() callers get the
   * reference they own (see dirindex.h). */
  vfs_node_t *out = c ? c->node : NULL;
  vfs_dirindex_unlock(di);
  return out;
}

vfs_node_t *vfs_dirindex_finddir_ref(vfs_node_t *node, char *name) {
  vfs_node_t *out = vfs_dirindex_finddir(node, name);
  /* "." / ".." resolve to the directory itself: take the caller's reference
   * here too, matching what every other backend does for self-returns
   * (procfs does vfs_open(node) on the same path). */
  if (out)
    vfs_open(out);
  return out;
}

/* ── selftest ────────────────────────────────────────────────────────────
 *
 * Pins the semantics the old linked list provided, now that lookup,
 * detach and rename all run through the hash: every inserted name finds
 * its node, a missing name does not, rename moves the entry between
 * buckets, detach removes it from both views, the count tracks both, and -
 * the part a subtle hash bug would break - the bench-only list-walk form
 * agrees with the hashed form on every query. */

static uint32_t dirindex_test_failures;

static void dirindex_expect(bool ok, const char *what) {
  if (!ok)
    dirindex_test_failures++;
  klog_puts(ok ? "[VFS-SELFTEST] ok   " : "[VFS-SELFTEST] FAIL ");
  klog_puts(what);
  klog_putchar('\n');
}

#define DIRINDEX_TEST_NODES 64u

/* A never-linked scratch node: it has no cache state, no pages and no
 * references beyond vfs_node_init()'s, so the wrapper teardown below can
 * kfree() it directly. */
static vfs_node_t *dirindex_probe_node(const char *name) {
  vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
  if (!n)
    return NULL;
  vfs_node_init(n);
  strncpy(n->name, name, 127);
  n->name[127] = '\0';
  n->flags = FS_FILE;
  return n;
}

static void dirindex_make_name(char *buf, size_t cap, const char *prefix,
                               uint32_t i) {
  char digits[10];
  int n = 0;
  do {
    digits[n++] = (char)('0' + i % 10);
    i /= 10;
  } while (i);
  size_t p = 0;
  while (prefix[p] && p + 1 < cap) {
    buf[p] = prefix[p];
    p++;
  }
  while (n > 0 && p + 1 < cap)
    buf[p++] = digits[--n];
  buf[p] = '\0';
}

bool vfs_dirindex_selftest(void) {
  dirindex_test_failures = 0;

  vfs_dirindex_t di;
  vfs_dirindex_init(&di);

  vfs_child_t *children[DIRINDEX_TEST_NODES];
  vfs_node_t *nodes[DIRINDEX_TEST_NODES];
  char name[32];

  for (uint32_t i = 0; i < DIRINDEX_TEST_NODES; i++) {
    dirindex_make_name(name, sizeof(name), "d", i);
    nodes[i] = dirindex_probe_node(name);
    children[i] = nodes[i] ? kmalloc(sizeof(vfs_child_t)) : NULL;
    if (!nodes[i] || !children[i]) {
      dirindex_expect(false, "dirindex: probe allocation");
      goto out;
    }
    children[i]->node = nodes[i];
    children[i]->next = children[i]->prev = children[i]->hnext = NULL;
    if (!vfs_dirindex_insert(&di, children[i])) {
      dirindex_expect(false, "dirindex: insert");
      goto out;
    }
  }

  dirindex_expect(di.count == DIRINDEX_TEST_NODES,
                  "dirindex: count tracks inserts");
  /* 64 children against a 16-bucket start also proves growth/rehash ran. */
  dirindex_expect(di.nbuckets >= DIRINDEX_TEST_NODES / 2,
                  "dirindex: table grew past the initial buckets");

  bool all_found = true;
  for (uint32_t i = 0; i < DIRINDEX_TEST_NODES && all_found; i++) {
    dirindex_make_name(name, sizeof(name), "d", i);
    vfs_child_t *c = vfs_dirindex_find(&di, name);
    if (!c || c->node != nodes[i])
      all_found = false;
  }
  dirindex_expect(all_found, "dirindex: every inserted name finds its node");
  dirindex_expect(vfs_dirindex_find(&di, "d_but_missing") == NULL,
                  "dirindex: missing name is not found");

  /* rename must move the entry between buckets: the old name stops
   * resolving and the new one resolves to the same node. */
  vfs_dirindex_lock(&di);
  vfs_child_t *first = vfs_dirindex_find_locked(&di, "d0");
  if (first)
    vfs_dirindex_rename_locked(&di, first, "renamed0");
  vfs_dirindex_unlock(&di);
  dirindex_expect(first != NULL &&
                      vfs_dirindex_find(&di, "d0") == NULL &&
                      vfs_dirindex_find(&di, "renamed0") == first,
                  "dirindex: rename moves the entry between buckets");

  /* The bench-only list-walk form must agree with the hash on a hit, a
   * miss and the renamed entry. */
  vfs_dirindex_set_force_scan(true);
  bool scan_ok = vfs_dirindex_find(&di, "renamed0") == first &&
                 vfs_dirindex_find(&di, "d_missing") == NULL;
  for (uint32_t i = 1; i < DIRINDEX_TEST_NODES && scan_ok; i++) {
    dirindex_make_name(name, sizeof(name), "d", i);
    if (vfs_dirindex_find(&di, name) != children[i])
      scan_ok = false;
  }
  vfs_dirindex_set_force_scan(false);
  dirindex_expect(scan_ok, "dirindex: forced list scan agrees with the hash");

  /* Detach every other entry through both views; the count and the
   * survivors' resolvability are what unlink/rmdir rely on. */
  uint32_t detached = 0;
  for (uint32_t i = 0; i < DIRINDEX_TEST_NODES; i += 2) {
    vfs_dirindex_lock(&di);
    vfs_child_t *c = (i == 0) ? first : vfs_dirindex_find_locked(&di, children[i]->node->name);
    if (c)
      vfs_dirindex_detach_locked(&di, c);
    vfs_dirindex_unlock(&di);
    if (c) {
      kfree(c->node);
      kfree(c);
      detached++;
    }
  }
  bool survivors_ok = (di.count == DIRINDEX_TEST_NODES - detached);
  for (uint32_t i = 1; i < DIRINDEX_TEST_NODES && survivors_ok; i += 2) {
    dirindex_make_name(name, sizeof(name), "d", i);
    if (vfs_dirindex_find(&di, name) != children[i])
      survivors_ok = false;
  }
  dirindex_expect(survivors_ok && vfs_dirindex_find(&di, "d0") == NULL &&
                      vfs_dirindex_find(&di, "renamed0") == NULL,
                  "dirindex: detach removes the entry from both views");

out:
  /* Detached entries were freed as they came out; whatever the order list
   * still holds is a survivor (or, on an early-failure goto, everything
   * indexed so far) - free wrappers and nodes, then the bucket array. */
  vfs_dirindex_lock(&di);
  vfs_child_t *c = di.children;
  di.children = NULL;
  vfs_dirindex_unlock(&di);
  while (c) {
    vfs_child_t *next = c->next;
    kfree(c->node);
    kfree(c);
    c = next;
  }
  vfs_dirindex_destroy(&di);
  return dirindex_test_failures == 0;
}

/* ── bench ──────────────────────────────────────────────────────────────
 *
 * Times the operation idea #1 is about - a name lookup against a real,
 * large directory - twice on the same working set: through the bucket
 * table, and with the forced list walk the old backends did.  The lookup
 * goes straight to ->finddir, bypassing the VFS dentry/path caches, so the
 * numbers are the backend itself, which is the thing that changed. */

#define VFS_DIRINDEX_BENCH_FILES 4096u
#define VFS_DIRINDEX_BENCH_HITS 2u    /* full passes over the directory */
#define VFS_DIRINDEX_BENCH_MISSES 256u

static void dirindex_bench_report(const char *name, uint64_t ops,
                                  uint64_t cycles) {
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

/* "f0123" - prefix plus four decimal digits, no printf in the VFS. */
static void dirindex_bench_name(char *buf, char prefix, uint32_t i) {
  buf[0] = prefix;
  buf[1] = (char)('0' + (i / 1000) % 10);
  buf[2] = (char)('0' + (i / 100) % 10);
  buf[3] = (char)('0' + (i / 10) % 10);
  buf[4] = (char)('0' + i % 10);
  buf[5] = '\0';
}

void vfs_dirindex_bench(void) {
  vfs_node_t *parent = vfs_resolve_path("/tmp");
  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY ||
      !parent->create || !parent->unlink || !parent->mkdir ||
      parent->finddir != vfs_dirindex_finddir) {
    klog_puts("[VFS-BENCH] dirindex skipped: /tmp is not a dirindex-backed "
              "directory\n");
    vfs_close(parent);
    return;
  }

  char name[8];
  if (vfs_mkdir(parent, "dirbench", 0777) != 0) {
    klog_puts("[VFS-BENCH] dirindex skipped: cannot create /tmp/dirbench\n");
    vfs_close(parent);
    return;
  }
  vfs_node_t *dir = vfs_resolve_path("/tmp/dirbench");
  if (!dir || (dir->finddir != vfs_dirindex_finddir &&
               dir->finddir != vfs_dirindex_finddir_ref)) {
    klog_puts("[VFS-BENCH] dirindex skipped: /tmp/dirbench resolved without "
              "the index\n");
    vfs_close(dir);
    vfs_close(parent);
    return;
  }

  uint32_t made = 0;
  for (uint32_t i = 0; i < VFS_DIRINDEX_BENCH_FILES; i++) {
    dirindex_bench_name(name, 'f', i);
    if (vfs_create(dir, name, 0644) == 0)
      made++;
  }

  /* One warm pass so both forms run against the same cache-hot list.
   * The bench times the raw backend, so it calls the bare form directly:
   * the wired ->finddir takes a reference per hit that would have to be
   * closed (and billed) inside the timed loop. */
  uint32_t found = 0;
  for (uint32_t i = 0; i < made; i++) {
    dirindex_bench_name(name, 'f', i);
    if (vfs_dirindex_finddir(dir, name))
      found++;
  }

  const uint64_t hit_ops = (uint64_t)made * VFS_DIRINDEX_BENCH_HITS;
  uint64_t t0 = rdtsc_fence();
  for (uint32_t pass = 0; pass < VFS_DIRINDEX_BENCH_HITS; pass++)
    for (uint32_t i = 0; i < made; i++) {
      dirindex_bench_name(name, 'f', i);
      if (!vfs_dirindex_finddir(dir, name))
        found = 0; /* count mismatches without paying a compare per op */
    }
  uint64_t hash_cycles = rdtsc_fence() - t0;
  dirindex_bench_report("dirindex find  hit hash", hit_ops, hash_cycles);

  vfs_dirindex_set_force_scan(true);
  t0 = rdtsc_fence();
  for (uint32_t pass = 0; pass < VFS_DIRINDEX_BENCH_HITS; pass++)
    for (uint32_t i = 0; i < made; i++) {
      dirindex_bench_name(name, 'f', i);
      if (!vfs_dirindex_finddir(dir, name))
        found = 0;
    }
  uint64_t scan_cycles = rdtsc_fence() - t0;
  vfs_dirindex_set_force_scan(false);
  dirindex_bench_report("dirindex find  hit scan", hit_ops, scan_cycles);
  if (scan_cycles > hash_cycles)
    dirindex_bench_report("dirindex find  saved  ", hit_ops,
                          scan_cycles - hash_cycles);

  /* Misses: the hash stops at an empty/short chain, the scan walks the
   * whole directory - the largest gap of the two forms. */
  const uint64_t miss_ops = VFS_DIRINDEX_BENCH_MISSES;
  uint32_t missed = 0;
  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_DIRINDEX_BENCH_MISSES; i++) {
    dirindex_bench_name(name, 'm', i);
    if (vfs_dirindex_finddir(dir, name))
      missed++;
  }
  hash_cycles = rdtsc_fence() - t0;
  dirindex_bench_report("dirindex find  miss hash", miss_ops, hash_cycles);

  vfs_dirindex_set_force_scan(true);
  t0 = rdtsc_fence();
  for (uint32_t i = 0; i < VFS_DIRINDEX_BENCH_MISSES; i++) {
    dirindex_bench_name(name, 'm', i);
    if (vfs_dirindex_finddir(dir, name))
      missed++;
  }
  scan_cycles = rdtsc_fence() - t0;
  vfs_dirindex_set_force_scan(false);
  dirindex_bench_report("dirindex find  miss scan", miss_ops, scan_cycles);
  if (scan_cycles > hash_cycles)
    dirindex_bench_report("dirindex miss  saved  ", miss_ops,
                          scan_cycles - hash_cycles);

  if (found == 0 || missed)
    klog_puts("[VFS-BENCH] dirindex WARNING: lookups did not agree\n");

  for (uint32_t i = 0; i < made; i++) {
    dirindex_bench_name(name, 'f', i);
    vfs_unlink(dir, name);
  }
  vfs_rmdir(parent, "dirbench");
  vfs_close(dir);
  vfs_close(parent);
}
