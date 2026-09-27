#ifndef FS_DIRINDEX_H
#define FS_DIRINDEX_H

#include "vfs.h"
#include "../lock/spinlock.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Directory index for the in-memory filesystems (ramfs and tmpfs).
 *
 * Both backends kept their children in a singly-linked list and found them by
 * walking it with strcmp() per entry.  That is O(n) in the backend itself:
 * the VFS dentry cache hides it only until an entry goes cold, and
 * create/unlink/rename pay it unconditionally because they must check the
 * name themselves before touching the filesystem.  A directory with a few
 * thousand entries (a package staging tree, /run during boot, build output
 * in /tmp) therefore costs thousands of pointer dereferences and string
 * compares per operation, where a bucket probe costs one.
 *
 * The index keeps three views of the same entries:
 *
 *   - an order list (children/next/prev, newest first).  readdir walks it to
 *     produce stable, cheaply resumable enumeration, and the prev pointer
 *     takes a detach from "the hash found the entry" to unlinked in O(1)
 *     instead of re-walking from the head to find the predecessor.
 *   - a power-of-two bucket table chained by hnext, keyed by the child's
 *     name (FNV-1a folded to 32 bits), for O(1) finddir/unlink/rename.
 *   - a count, which answers "is this directory empty?" for rmdir in O(1).
 *
 * Every lookup and mutation runs under the index's spinlock.  Directory
 * operations were previously lock-free and lost updates when two CPUs
 * created in the same directory concurrently (both prepends computed the
 * same head); the lock fixes that, and a rehash cannot be made safe without
 * it.  Two rules keep the lock cheap and safe:
 *
 *   - allocations happen outside it.  spinlock_acquire() masks interrupts
 *     for the duration of the critical section, so kmalloc() under the lock
 *     would let an interrupt handler that itself allocates deadlock against
 *     us.  insert() allocates the first bucket array (or the doubled one for
 *     growth) before locking and adopts it under the lock; a failed growth
 *     allocation only means longer chains, never a failed insert.
 *   - no VFS callbacks run under it.  vfs_node_retire()/kfree() of a removed
 *     child happen after the unlock.  Exactly one operation nests a second
 *     index lock - rmdir takes parent, then the child directory to test
 *     emptiness - and that nesting always follows directory depth, so it
 *     cannot cycle.
 *
 * Layout contract: for these backends' directory nodes, node->device points
 * at a struct whose first member is vfs_dirindex_t (ramfs_dir_t IS the
 * index; tmpfs_dir_t starts with it).  That is what lets the shared finddir
 * callback reach the index without knowing which backend it is looking at.
 *
 * The child layout (node, next, prev, hnext) is shared for the same reason:
 * the index walks entries it did not allocate the containing struct of.
 */
typedef struct vfs_child {
  struct vfs_node *node;
  struct vfs_child *next;  /* order list: newest first (readdir order) */
  struct vfs_child *prev;  /* makes detach O(1) once the hash found it */
  struct vfs_child *hnext; /* hash bucket chain */
} vfs_child_t;

typedef struct vfs_dirindex {
  vfs_child_t *children;      /* order list head, newest first */
  vfs_child_t *cursor;        /* sequential readdir resume point */
  uint32_t cursor_index;      /* child index the cursor points at */
  vfs_child_t **buckets;      /* chain heads; NULL while the dir is empty */
  uint32_t nbuckets;          /* power of two, 0 while buckets is NULL */
  uint32_t count;             /* children currently indexed */
  spinlock_t lock;
} vfs_dirindex_t;

/* node->device for a directory of a backend using this index. */
static inline vfs_dirindex_t *vfs_dirindex_of(vfs_node_t *node) {
  return (vfs_dirindex_t *)node->device;
}

static inline void vfs_dirindex_lock(vfs_dirindex_t *di) {
  spinlock_acquire(&di->lock);
}

static inline void vfs_dirindex_unlock(vfs_dirindex_t *di) {
  spinlock_release(&di->lock);
}

/* Zero a fresh index.  The bucket array is allocated lazily on the first
 * insert, so an empty directory costs no extra memory at all. */
void vfs_dirindex_init(vfs_dirindex_t *di);

/* Free the bucket array of a directory that is already unreachable (its
 * destroy callback runs when the last reference dropped).  The child
 * wrappers themselves are owned and freed by the backend. */
void vfs_dirindex_destroy(vfs_dirindex_t *di);

/* Lookup primitives.  The _locked variants assume the caller holds di->lock
 * and are what composite operations (rename, rmdir's empty check) use so a
 * whole sequence runs under one acquisition.  A found child is returned
 * WITHOUT a reference - the pointer is only meaningful while the lock is
 * held (or, for find_locked callers, until the next detach). */
vfs_child_t *vfs_dirindex_find_locked(vfs_dirindex_t *di, const char *name);
vfs_child_t *vfs_dirindex_find(vfs_dirindex_t *di, const char *name);

/* Link `child` into the order list and the hash.  Allocates the bucket
 * array on first use and doubles it when the load passes one, both outside
 * the lock; returns false only when the very first array could not be
 * allocated (the child is then not indexed and remains caller-owned). */
bool vfs_dirindex_insert(vfs_dirindex_t *di, vfs_child_t *child);

/* Unlink `child` from the order list and the hash and retire the readdir
 * cursor (any removal shifts the indices that follow it, so a resume from
 * it could skip or repeat entries; the next getdents rescans instead).
 * Caller holds di->lock and frees the wrapper afterwards. */
void vfs_dirindex_detach_locked(vfs_dirindex_t *di, vfs_child_t *child);

/* Move `child` to `new_name`: unchains under its current name, renames,
 * rechains.  Caller holds di->lock and has already checked that `child`
 * really carries the name being replaced. */
void vfs_dirindex_rename_locked(vfs_dirindex_t *di, vfs_child_t *child,
                                const char *new_name);

/* Shared ->finddir callback for any node whose device starts with a
 * vfs_dirindex_t.  Preserves the backend's old contract exactly: "." and
 * ".." resolve to the node itself, and a found child is returned bare (no
 * reference taken).  Internal existence checks use this form and must NOT
 * vfs_close() the result.
 *
 * vfs_finddir() callers, on the other hand, own a reference on what the
 * backend returns: the dentry-cache insert takes its own reference for the
 * cache, and the caller drops its own with vfs_close().  Every other
 * backend upholds that (fresh nodes arrive with vfs_node_init()'s
 * reference, shared ones are vfs_open()ed).  A bare dirindex pointer handed
 * to vfs_finddir therefore steals the cache's reference: the entry dangles,
 * and a later invalidate/evict closes a reference that was never taken,
 * tearing the node down early and freeing it a second time on the real
 * last close.  Backend wiring MUST use vfs_dirindex_finddir_ref() below. */
vfs_node_t *vfs_dirindex_finddir(vfs_node_t *node, char *name);

/* Reference-taking form of the above, for ->finddir wiring only: bare
 * lookup plus vfs_open() on a hit, so the result honours the same
 * caller-owns-a-reference contract as every other backend. */
vfs_node_t *vfs_dirindex_finddir_ref(vfs_node_t *node, char *name);

/* Bench only (`vfs_bench=1`): force find to walk the order list the way the
 * pre-hash implementation did, so old and new forms can be timed back to
 * back on the same working set - the same trick as
 * vfs_path_invalidation_force_scan.  Detach stays O(1) in both forms, so the
 * scan numbers slightly UNDERSTATE what the old code cost.  Never set
 * outside the benchmark. */
void vfs_dirindex_set_force_scan(bool force);

/* Boot-time check + benchmark, wired into vfs_selftest_maybe_run() and
 * vfs_bench_maybe_run(). */
bool vfs_dirindex_selftest(void);
void vfs_dirindex_bench(void);

#endif
