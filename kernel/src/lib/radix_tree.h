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

typedef bool (*asc_radix_tree_iter_fn)(uint64_t index, void *value, void *context);

struct radix_tree {
  struct asc_radix_tree_node *root;
  uint64_t entries;
  uint64_t nodes;
  uint8_t height;
  spinlock_t lock;
  struct asc_radix_tree_allocator allocator;
};

void asc_radix_tree_init(struct radix_tree *tree);
void asc_radix_tree_init_with_allocator(struct radix_tree *tree,
                                    const struct asc_radix_tree_allocator *allocator);
void *asc_radix_tree_lookup(const struct radix_tree *tree, uint64_t index);
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
