#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#define RADIX_TREE_BITS 6u
#define RADIX_TREE_SLOTS (1u << RADIX_TREE_BITS)
#define RADIX_TREE_MAX_HEIGHT 11u

struct asc_radix_tree_node {
  void *defer[2];
  void *slots[RADIX_TREE_SLOTS];
  uint8_t count;
};

struct radix_tree {
  struct asc_radix_tree_node *root;
  uint64_t entries;
  uint64_t nodes;
  uint8_t height;
  uint32_t seq;
};

static struct asc_radix_tree_node *node_alloc(void) {
  return calloc(1, sizeof(struct asc_radix_tree_node));
}

static unsigned radix_slot(uint64_t index, uint8_t level) {
  return (unsigned)((index >> (level * RADIX_TREE_BITS)) & (RADIX_TREE_SLOTS - 1));
}

static uint8_t radix_required_height(uint64_t index) {
  uint8_t height = 1;
  while (height < RADIX_TREE_MAX_HEIGHT && (index >> (height * RADIX_TREE_BITS)) != 0)
    height++;
  return height;
}

static void tree_insert(struct radix_tree *tree, uint64_t index, void *value) {
  uint8_t needed = radix_required_height(index);
  if (!tree->root) {
    tree->root = node_alloc();
    tree->height = needed;
  }
  while (tree->height < needed) {
    struct asc_radix_tree_node *new_root = node_alloc();
    new_root->slots[0] = tree->root;
    tree->root = new_root;
    tree->height++;
  }

  struct asc_radix_tree_node *cur = tree->root;
  for (uint8_t level = tree->height - 1; level > 0; level--) {
    unsigned slot = radix_slot(index, level);
    if (!cur->slots[slot]) {
      cur->slots[slot] = node_alloc();
    }
    cur = cur->slots[slot];
  }
  cur->slots[radix_slot(index, 0)] = value;
  tree->seq += 2;
}

static void *old_lookup_lockless(const struct radix_tree *tree, uint64_t index) {
  uint32_t s1 = __atomic_load_n(&tree->seq, __ATOMIC_ACQUIRE);
  if (s1 & 1u) return NULL;

  const struct asc_radix_tree_node *node = __atomic_load_n(&tree->root, __ATOMIC_ACQUIRE);
  uint8_t height = __atomic_load_n(&tree->height, __ATOMIC_ACQUIRE);
  void *value = NULL;

  if (node && radix_required_height(index) <= height) {
    for (uint8_t level = height - 1; level > 0; level--) {
      node = __atomic_load_n(&node->slots[radix_slot(index, level)], __ATOMIC_RELAXED);
      if (!node) break;
    }
    if (node) {
      value = __atomic_load_n(&node->slots[radix_slot(index, 0)], __ATOMIC_RELAXED);
    }
  }

  if (s1 != __atomic_load_n(&tree->seq, __ATOMIC_ACQUIRE)) return NULL;
  return value;
}

static inline void *new_lookup_lockless_cached(const struct radix_tree *tree, uint64_t index,
                                              uint64_t *cached_chunk, void **cached_leaf,
                                              uint32_t *cached_seq) {
  uint32_t s1 = __atomic_load_n(&tree->seq, __ATOMIC_ACQUIRE);
  if (s1 & 1u) return NULL;

  uint64_t chunk = index >> RADIX_TREE_BITS;
  unsigned slot = (unsigned)(index & (RADIX_TREE_SLOTS - 1));

  if (*cached_chunk == chunk && *cached_leaf != NULL && *cached_seq == s1) {
    const struct asc_radix_tree_node *leaf = (const struct asc_radix_tree_node *)*cached_leaf;
    void *val = __atomic_load_n(&leaf->slots[slot], __ATOMIC_RELAXED);
    if (s1 != __atomic_load_n(&tree->seq, __ATOMIC_ACQUIRE)) return NULL;
    return val;
  }

  const struct asc_radix_tree_node *node = __atomic_load_n(&tree->root, __ATOMIC_ACQUIRE);
  uint8_t height = __atomic_load_n(&tree->height, __ATOMIC_ACQUIRE);
  void *value = NULL;

  if (node && radix_required_height(index) <= height) {
    for (uint8_t level = height - 1; level > 0; level--) {
      node = __atomic_load_n(&node->slots[radix_slot(index, level)], __ATOMIC_RELAXED);
      if (!node) break;
    }
    if (node) {
      value = __atomic_load_n(&node->slots[slot], __ATOMIC_RELAXED);
      *cached_chunk = chunk;
      *cached_leaf = (void *)node;
      *cached_seq = s1;
    }
  }

  if (s1 != __atomic_load_n(&tree->seq, __ATOMIC_ACQUIRE)) return NULL;
  return value;
}

int main(void) {
  struct radix_tree tree = {0};
  tree.seq = 2;

  // Insert into height 4 (e.g. index 300,000 = ~1.2GB)
  uint64_t base_page = 300000;
  for (uint64_t i = 0; i < 2048; i++) {
    tree_insert(&tree, base_page + i, (void *)(uintptr_t)(0x20000000ULL + i * 4096));
  }
  printf("=== Microbenchmark: Page Cache Radix Tree Lookup (Height = %u, %lu Pages) ===\n",
         tree.height, 2048UL);

  uint32_t ITERS = 10000000;
  uint64_t t0 = __rdtsc();
  uint64_t dummy = 0;
  for (uint32_t it = 0; it < ITERS; it++) {
    uint64_t p_idx = base_page + (it % 2048);
    dummy += (uintptr_t)old_lookup_lockless(&tree, p_idx);
  }
  uint64_t old_cycles = __rdtsc() - t0;
  double old_per_op = (double)old_cycles / ITERS;
  printf("Old (4-level full radix walk per page):   %.2f cycles/lookup\n", old_per_op);

  uint64_t cached_chunk = ~0ULL;
  void *cached_leaf = NULL;
  uint32_t cached_seq = 0;

  t0 = __rdtsc();
  for (uint32_t it = 0; it < ITERS; it++) {
    uint64_t p_idx = base_page + (it % 2048);
    dummy += (uintptr_t)new_lookup_lockless_cached(&tree, p_idx, &cached_chunk, &cached_leaf, &cached_seq);
  }
  uint64_t new_cycles = __rdtsc() - t0;
  double new_per_op = (double)new_cycles / ITERS;
  printf("New (Direct Level-1 Radix Leaf Cache):     %.2f cycles/lookup\n", new_per_op);
  printf("Speedup: %.2fX faster (%.1f%% latency reduction)\n",
         old_per_op / new_per_op, (1.0 - (new_per_op / old_per_op)) * 100.0);

  return (dummy == 0);
}
