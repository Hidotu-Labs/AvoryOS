#include "lib/radix_tree.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"

#define RADIX_ENOMEM 12
#define RADIX_EEXIST 17
#define RADIX_EINVAL 22

struct radix_tree_node {
  void *slots[RADIX_TREE_SLOTS];
  uint8_t count;
};

static void *radix_default_alloc(size_t size, void *context) {
  (void)context;
  return kmalloc(size);
}

static void radix_default_free(void *ptr, void *context) {
  (void)context;
  kfree(ptr);
}

static struct radix_tree_node *radix_node_alloc(struct radix_tree *tree) {
  struct radix_tree_node *node =
      tree->allocator.alloc(sizeof(*node), tree->allocator.context);
  if (!node)
    return NULL;
  memset(node, 0, sizeof(*node));
  return node;
}

static void radix_node_free(struct radix_tree *tree,
                            struct radix_tree_node *node) {
  tree->allocator.free(node, tree->allocator.context);
}

static uint8_t radix_required_height(uint64_t index) {
  uint8_t height = 1;
  while (height < RADIX_TREE_MAX_HEIGHT &&
         (index >> (height * RADIX_TREE_BITS)) != 0)
    height++;
  return height;
}

static unsigned radix_slot(uint64_t index, uint8_t level) {
  return (unsigned)((index >> (level * RADIX_TREE_BITS)) &
                    (RADIX_TREE_SLOTS - 1));
}

/* A stored child pointer is only treated as a node when it is an aligned
 * kernel address backed by managed RAM.  Leaf values are opaque to the tree,
 * but every intermediate slot must point at a real node: the page cache's
 * trees have been observed holding wild values after a kernel-heap
 * use-after-free, and dereferencing one turns a recoverable data corruption
 * into a fatal ring-0 page fault. */
static bool radix_ptr_is_node(const void *ptr) {
  if (!ptr || ((uint64_t)ptr & (sizeof(void *) - 1)) != 0)
    return false;
  return pmm_kernel_ptr_is_managed(ptr);
}

static void radix_reset_corrupt_tree(struct radix_tree *tree) {
  tree->root = NULL;
  tree->height = 0;
  tree->entries = 0;
  tree->nodes = 0;
}

void radix_tree_init_with_allocator(
    struct radix_tree *tree, const struct radix_tree_allocator *allocator) {
  if (!tree)
    return;
  memset(tree, 0, sizeof(*tree));
  spinlock_init(&tree->lock);
  if (allocator && allocator->alloc && allocator->free)
    tree->allocator = *allocator;
  else {
    tree->allocator.alloc = radix_default_alloc;
    tree->allocator.free = radix_default_free;
  }
}

void radix_tree_init(struct radix_tree *tree) {
  radix_tree_init_with_allocator(tree, NULL);
}

static void *radix_lookup_locked(const struct radix_tree *tree,
                                 uint64_t index) {
  if (!radix_ptr_is_node(tree->root) ||
      radix_required_height(index) > tree->height)
    return NULL;
  const struct radix_tree_node *node = tree->root;
  for (uint8_t level = tree->height - 1; level > 0; level--) {
    node = node->slots[radix_slot(index, level)];
    if (!radix_ptr_is_node(node))
      return NULL;
  }
  return node->slots[radix_slot(index, 0)];
}

void *radix_tree_lookup(const struct radix_tree *tree, uint64_t index) {
  if (!tree)
    return NULL;
  struct radix_tree *mutable = (struct radix_tree *)tree;
  spinlock_acquire(&mutable->lock);
  void *value = radix_lookup_locked(tree, index);
  spinlock_release(&mutable->lock);
  return value;
}

static void radix_free_allocated(struct radix_tree *tree,
                                 struct radix_tree_node **nodes,
                                 unsigned count) {
  while (count)
    radix_node_free(tree, nodes[--count]);
}

int radix_tree_insert(struct radix_tree *tree, uint64_t index, void *value) {
  if (!tree || !value)
    return -RADIX_EINVAL;
  spinlock_acquire(&tree->lock);
  if (tree->root && !radix_ptr_is_node(tree->root))
    radix_reset_corrupt_tree(tree);
  if (radix_lookup_locked(tree, index)) {
    spinlock_release(&tree->lock);
    return -RADIX_EEXIST;
  }

  uint8_t needed = radix_required_height(index);
  struct radix_tree_node *fresh[RADIX_TREE_MAX_HEIGHT * 2];
  unsigned fresh_count = 0;
  unsigned wrapper_count = 0;
  unsigned branch_count = 0;
  struct radix_tree_node *attach_parent = NULL;
  unsigned attach_slot = 0;

  if (!tree->root) {
    branch_count = needed;
  } else if (needed > tree->height) {
    wrapper_count = needed - tree->height;
    branch_count = needed - 1;
  } else {
    struct radix_tree_node *node = tree->root;
    for (uint8_t level = tree->height - 1; level > 0; level--) {
      unsigned slot = radix_slot(index, level);
      struct radix_tree_node *child = node->slots[slot];
      if (!radix_ptr_is_node(child)) {
        /* Missing or corrupt branch: attach the fresh subtree here, exactly
         * as for an empty slot.  A wild pointer can never be followed. */
        attach_parent = node;
        attach_slot = slot;
        branch_count = level;
        break;
      }
      node = child;
    }
    if (!branch_count) {
      unsigned slot = radix_slot(index, 0);
      node->slots[slot] = value;
      node->count++;
      tree->entries++;
      spinlock_release(&tree->lock);
      return 0;
    }
  }

  unsigned total = wrapper_count + branch_count;
  for (; fresh_count < total; fresh_count++) {
    fresh[fresh_count] = radix_node_alloc(tree);
    if (!fresh[fresh_count]) {
      radix_free_allocated(tree, fresh, fresh_count);
      spinlock_release(&tree->lock);
      return -RADIX_ENOMEM;
    }
  }

  if (!tree->root) {
    for (unsigned i = 0; i + 1 < branch_count; i++) {
      uint8_t level = needed - 1 - i;
      fresh[i]->slots[radix_slot(index, level)] = fresh[i + 1];
      fresh[i]->count = 1;
    }
    struct radix_tree_node *leaf = fresh[branch_count - 1];
    leaf->slots[radix_slot(index, 0)] = value;
    leaf->count = 1;
    tree->root = fresh[0];
    tree->height = needed;
  } else if (wrapper_count) {
    for (unsigned i = 0; i < wrapper_count; i++) {
      fresh[i]->slots[0] =
          (i + 1 < wrapper_count) ? (void *)fresh[i + 1] : tree->root;
      fresh[i]->count = 1;
    }
    struct radix_tree_node *new_root = fresh[0];
    struct radix_tree_node *branch = fresh[wrapper_count];
    unsigned top_slot = radix_slot(index, needed - 1);
    new_root->slots[top_slot] = branch;
    new_root->count++;
    for (unsigned i = 0; i + 1 < branch_count; i++) {
      uint8_t level = needed - 2 - i;
      branch->slots[radix_slot(index, level)] = fresh[wrapper_count + i + 1];
      branch->count = 1;
      branch = fresh[wrapper_count + i + 1];
    }
    branch->slots[radix_slot(index, 0)] = value;
    branch->count = 1;
    tree->root = new_root;
    tree->height = needed;
  } else {
    struct radix_tree_node *branch = fresh[0];
    attach_parent->slots[attach_slot] = branch;
    attach_parent->count++;
    for (unsigned i = 0; i + 1 < branch_count; i++) {
      uint8_t level = branch_count - 1 - i;
      branch->slots[radix_slot(index, level)] = fresh[i + 1];
      branch->count = 1;
      branch = fresh[i + 1];
    }
    branch->slots[radix_slot(index, 0)] = value;
    branch->count = 1;
  }
  tree->nodes += total;
  tree->entries++;
  spinlock_release(&tree->lock);
  return 0;
}

void *radix_tree_replace(struct radix_tree *tree, uint64_t index, void *value) {
  if (!tree || !value)
    return NULL;
  spinlock_acquire(&tree->lock);
  if (!radix_ptr_is_node(tree->root) ||
      radix_required_height(index) > tree->height) {
    spinlock_release(&tree->lock);
    return NULL;
  }
  struct radix_tree_node *node = tree->root;
  for (uint8_t level = tree->height - 1; level > 0; level--) {
    node = node->slots[radix_slot(index, level)];
    if (!radix_ptr_is_node(node)) {
      spinlock_release(&tree->lock);
      return NULL;
    }
  }
  unsigned slot = radix_slot(index, 0);
  void *old = node->slots[slot];
  if (old)
    node->slots[slot] = value;
  spinlock_release(&tree->lock);
  return old;
}

void *radix_tree_delete(struct radix_tree *tree, uint64_t index) {
  if (!tree)
    return NULL;
  spinlock_acquire(&tree->lock);
  if (tree->root && !radix_ptr_is_node(tree->root))
    radix_reset_corrupt_tree(tree);
  if (!radix_ptr_is_node(tree->root) ||
      radix_required_height(index) > tree->height) {
    spinlock_release(&tree->lock);
    return NULL;
  }
  struct radix_tree_node *path[RADIX_TREE_MAX_HEIGHT];
  unsigned slots[RADIX_TREE_MAX_HEIGHT];
  struct radix_tree_node *node = tree->root;
  path[tree->height - 1] = node;
  for (uint8_t level = tree->height - 1; level > 0; level--) {
    unsigned slot = radix_slot(index, level);
    slots[level] = slot;
    node = node->slots[slot];
    if (!radix_ptr_is_node(node)) {
      spinlock_release(&tree->lock);
      return NULL;
    }
    path[level - 1] = node;
  }
  unsigned leaf_slot = radix_slot(index, 0);
  void *old = node->slots[leaf_slot];
  if (!old) {
    spinlock_release(&tree->lock);
    return NULL;
  }
  node->slots[leaf_slot] = NULL;
  node->count--;
  tree->entries--;
  for (uint8_t level = 0; level + 1 < tree->height; level++) {
    if (path[level]->count)
      break;
    struct radix_tree_node *parent = path[level + 1];
    parent->slots[slots[level + 1]] = NULL;
    parent->count--;
    radix_node_free(tree, path[level]);
    tree->nodes--;
  }
  while (tree->height > 1 && tree->root->count == 1 &&
         tree->root->slots[0]) {
    struct radix_tree_node *old_root = tree->root;
    tree->root = old_root->slots[0];
    tree->height--;
    radix_node_free(tree, old_root);
    tree->nodes--;
  }
  if (!tree->entries) {
    radix_node_free(tree, tree->root);
    tree->root = NULL;
    tree->height = 0;
    tree->nodes--;
  }
  spinlock_release(&tree->lock);
  return old;
}

static void radix_destroy_node(struct radix_tree *tree,
                               struct radix_tree_node *node, uint8_t level,
                               void (*release)(void *)) {
  if (!radix_ptr_is_node(node))
    return;
  for (unsigned i = 0; i < RADIX_TREE_SLOTS; i++) {
    if (!node->slots[i])
      continue;
    if (!level) {
      if (release)
        release(node->slots[i]);
    } else {
      radix_destroy_node(tree, node->slots[i], level - 1, release);
    }
  }
  radix_node_free(tree, node);
}

void radix_tree_destroy(struct radix_tree *tree,
                        void (*release)(void *value)) {
  if (!tree)
    return;
  spinlock_acquire(&tree->lock);
  if (tree->root && !radix_ptr_is_node(tree->root))
    radix_reset_corrupt_tree(tree);
  if (tree->root)
    radix_destroy_node(tree, tree->root, tree->height - 1, release);
  tree->root = NULL;
  tree->height = 0;
  tree->nodes = 0;
  tree->entries = 0;
  spinlock_release(&tree->lock);
}

static bool radix_iter_node(struct radix_tree_node *node, uint8_t level,
                            uint64_t prefix, uint64_t first, uint64_t last,
                            radix_tree_iter_fn callback, void *context) {
  if (!radix_ptr_is_node(node))
    return false;
  for (unsigned i = 0; i < RADIX_TREE_SLOTS; i++) {
    if (!node->slots[i])
      continue;
    uint64_t key = prefix | ((uint64_t)i << (level * RADIX_TREE_BITS));
    if (!level) {
      if (key >= first && key <= last && !callback(key, node->slots[i], context))
        return false;
    } else {
      if (!radix_ptr_is_node(node->slots[i]))
        continue;
      if (!radix_iter_node(node->slots[i], level - 1, key, first, last,
                           callback, context)) {
        return false;
      }
    }
  }
  return true;
}

bool radix_tree_for_each_range(struct radix_tree *tree, uint64_t first,
                               uint64_t last, radix_tree_iter_fn callback,
                               void *context) {
  if (!tree || !callback || first > last)
    return false;
  spinlock_acquire(&tree->lock);
  bool completed = !tree->root ||
      radix_iter_node(tree->root, tree->height - 1, 0, first, last,
                      callback, context);
  spinlock_release(&tree->lock);
  return completed;
}

bool radix_tree_for_each(struct radix_tree *tree, radix_tree_iter_fn callback,
                         void *context) {
  return radix_tree_for_each_range(tree, 0, UINT64_MAX, callback, context);
}

static bool radix_validate_node(const struct radix_tree_node *node,
                                uint8_t level, uint64_t *nodes,
                                uint64_t *entries) {
  if (!radix_ptr_is_node(node))
    return false;
  unsigned count = 0;
  for (unsigned i = 0; i < RADIX_TREE_SLOTS; i++) {
    if (!node->slots[i])
      continue;
    count++;
    if (!level)
      (*entries)++;
    else if (!radix_validate_node(node->slots[i], level - 1, nodes, entries))
      return false;
  }
  (*nodes)++;
  return count == node->count && count != 0;
}

bool radix_tree_validate(const struct radix_tree *tree) {
  if (!tree)
    return false;
  struct radix_tree *mutable = (struct radix_tree *)tree;
  spinlock_acquire(&mutable->lock);
  bool valid;
  if (!tree->root) {
    valid = tree->height == 0 && tree->nodes == 0 && tree->entries == 0;
  } else if (!tree->height || tree->height > RADIX_TREE_MAX_HEIGHT ||
             !tree->entries) {
    valid = false;
  } else {
    uint64_t nodes = 0, entries = 0;
    valid = radix_validate_node(tree->root, tree->height - 1, &nodes,
                                &entries) &&
            nodes == tree->nodes && entries == tree->entries;
  }
  spinlock_release(&mutable->lock);
  return valid;
}
