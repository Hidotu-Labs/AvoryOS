#ifndef LIB_RADIX_TREE_H
#define LIB_RADIX_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lock/spinlock.h"

#define RADIX_TREE_BITS 6u
#define RADIX_TREE_SLOTS (1u << RADIX_TREE_BITS)
#define RADIX_TREE_MAX_HEIGHT 11u

struct asc_radix_tree_node;

struct asc_radix_tree_allocator {
  void *(*alloc)(size_t size, void *context);
  void (*free)(void *ptr, void *context);
  void *context;
};

/* First two words of every radix node, reserved for tree users that defer
 * node reclamation.  Layout matches fs/vfs_rcu.h's struct vfs_rcu_head (queue
 * link + reclaim callback), so a deferred-free hook may enqueue a departing
 * node through this slot without allocating.  The tree itself never reads or
 * writes it; the slot only matters while a tree has a deferred-free hook. */
struct radix_tree_defer_slot {
  void *next;
  void *reclaim;
};

typedef bool (*asc_radix_tree_iter_fn)(uint64_t index, void *value, void *context);

struct radix_tree {
  struct asc_radix_tree_node *root;
  uint64_t entries;
  uint64_t nodes;
  uint8_t height;
  /* Odd while a mutating operation is in flight.  Bumped (and returned to
   * even) by every insert/replace/delete/destroy under tree->lock, so a
   * lockless lookup can validate that `root`/`height`/slots did not change
   * across its traversal. */
  uint32_t seq;
  spinlock_t lock;
  struct asc_radix_tree_allocator allocator;
  /* Optional: when set, radix_node_free() hands every departing node to this
   * hook instead of allocator.free().  The hook must eventually free the node
   * with the tree's allocator, after a grace period covering lockless readers
   * (the page cache uses fs/vfs_rcu.c for this).  Nodes are unlinked before
   * the hook runs, so a queued node is unreachable from new lookups. */
  void (*deferred_free)(void *node, void *context);
  void *deferred_free_context;
};

void asc_radix_tree_init(struct radix_tree *tree);
void asc_radix_tree_init_with_allocator(struct radix_tree *tree,
                                    const struct asc_radix_tree_allocator *allocator);
void *asc_radix_tree_lookup(const struct radix_tree *tree, uint64_t index);

/* Lockless lookup for trees whose readers run inside an IRQ-off grace period
 * and whose frees are deferred through asc_radix_tree_set_deferred_free().
 * Sets *stable=false (and returns NULL) when a mutation overlapped the
 * traversal, in which case the caller should retry or take the locked path.
 * On a stable return the value is the same one asc_radix_tree_lookup() would
 * return.  The tree's own lock is not taken. */
void *asc_radix_tree_lookup_lockless(const struct radix_tree *tree,
                                     uint64_t index, bool *stable);

/* Route every node freed by mutations through `free_fn(node, context)`.
 * Set once after init, before the tree is shared.  The node's first two words
 * are the radix_tree_defer_slot the hook may queue through. */
void asc_radix_tree_set_deferred_free(struct radix_tree *tree,
                                      void (*free_fn)(void *node,
                                                      void *context),
                                      void *context);
int asc_radix_tree_insert(struct radix_tree *tree, uint64_t index, void *value);
void *asc_radix_tree_replace(struct radix_tree *tree, uint64_t index, void *value);
void *asc_radix_tree_delete(struct radix_tree *tree, uint64_t index);
void asc_radix_tree_destroy(struct radix_tree *tree,
                        void (*release)(void *value));
bool asc_radix_tree_for_each(struct radix_tree *tree, asc_radix_tree_iter_fn callback,
                         void *context);
/* Iteration callbacks run with the tree locked and must not mutate the tree. */
bool asc_radix_tree_for_each_range(struct radix_tree *tree, uint64_t first,
                               uint64_t last, asc_radix_tree_iter_fn callback,
                               void *context);
bool asc_radix_tree_validate(const struct radix_tree *tree);
bool asc_radix_tree_phase1_stress_test(uint64_t iterations, uint64_t seed);
bool asc_radix_tree_phase2_stress_test(void);

#endif
