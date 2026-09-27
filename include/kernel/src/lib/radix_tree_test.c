#include "lib/radix_tree.h"
#include "console/klog.h"
#include "mm/heap.h"
#include <stdint.h>

#define TEST_KEYS 256u

static uint64_t radix_test_random(uint64_t *state) {
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545f4914f6cdd1dULL;
}

static uint64_t radix_test_key(unsigned slot) {
  if (slot < 128)
    return slot;
  unsigned shift = (slot % RADIX_TREE_MAX_HEIGHT) * RADIX_TREE_BITS;
  uint64_t low = (uint64_t)slot * 0x9e3779b97f4a7c15ULL;
  return low ^ (1ULL << shift);
}

static void *radix_test_value(unsigned slot, uint64_t generation) {
  uintptr_t value = ((uintptr_t)generation << 16) ^ ((uintptr_t)slot << 1) ^ 1;
  return (void *)(value ? value : 1);
}

bool radix_tree_phase1_stress_test(uint64_t iterations, uint64_t seed) {
  struct radix_tree tree;
  void **reference = kcalloc(TEST_KEYS, sizeof(*reference));
  if (!reference)
    return false;
  radix_tree_init(&tree);
  static const uint64_t boundaries[] = {
      0, 1, 63, 64, 65, 4095, 4096, 4097,
      (1ULL << 30) - 1, 1ULL << 30, UINT64_MAX - 1, UINT64_MAX};
  bool pass = true;
  for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
    void *value = radix_test_value(i, 1);
    if (radix_tree_insert(&tree, boundaries[i], value) != 0 ||
        radix_tree_lookup(&tree, boundaries[i]) != value ||
        radix_tree_insert(&tree, boundaries[i], value) != -17) {
      pass = false;
      break;
    }
  }
  for (unsigned i = 0; pass && i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
    void *value = radix_tree_lookup(&tree, boundaries[i]);
    if (!value || radix_tree_delete(&tree, boundaries[i]) != value)
      pass = false;
  }
  if (pass && !radix_tree_validate(&tree))
    pass = false;

  uint64_t state = seed ? seed : 0x415343454e54524FULL;
  for (uint64_t operation = 0; pass && operation < iterations; operation++) {
    uint64_t random = radix_test_random(&state);
    unsigned slot = (unsigned)(random % TEST_KEYS);
    uint64_t key = radix_test_key(slot);
    unsigned action = (unsigned)((random >> 32) & 3);
    if (action == 0) {
      void *value = radix_test_value(slot, operation + 2);
      int result = radix_tree_insert(&tree, key, value);
      if (reference[slot]) {
        if (result != -17)
          pass = false;
      } else if (result != 0) {
        pass = false;
      } else {
        reference[slot] = value;
      }
    } else if (action == 1) {
      if (radix_tree_lookup(&tree, key) != reference[slot])
        pass = false;
    } else if (action == 2) {
      void *old = radix_tree_delete(&tree, key);
      if (old != reference[slot])
        pass = false;
      reference[slot] = NULL;
    } else if (reference[slot]) {
      void *value = radix_test_value(slot, operation + 2);
      if (radix_tree_replace(&tree, key, value) != reference[slot])
        pass = false;
      reference[slot] = value;
    } else if (radix_tree_replace(&tree, key, (void *)1) != NULL) {
      pass = false;
    }
    if ((operation & 0x3fff) == 0 && !radix_tree_validate(&tree))
      pass = false;
  }

  for (unsigned slot = 0; pass && slot < TEST_KEYS; slot++) {
    uint64_t key = radix_test_key(slot);
    if (radix_tree_lookup(&tree, key) != reference[slot])
      pass = false;
    if (reference[slot]) {
      if (radix_tree_delete(&tree, key) != reference[slot])
        pass = false;
      reference[slot] = NULL;
    }
  }
  if (!radix_tree_validate(&tree) || tree.root || tree.nodes || tree.entries)
    pass = false;
  radix_tree_destroy(&tree, NULL);
  kfree(reference);
  if (!pass) {
    klog_puts("[RADIX] Phase 1 stress failed, seed=");
    klog_hex64(seed);
    klog_puts("\n");
  }
  return pass;
}

struct radix_fault_context {
  uint32_t fail_at;
  uint32_t calls;
};

static void *radix_fault_alloc(size_t size, void *opaque) {
  struct radix_fault_context *context = opaque;
  context->calls++;
  if (context->fail_at && context->calls == context->fail_at)
    return NULL;
  return kmalloc(size);
}

static void radix_fault_free(void *ptr, void *opaque) {
  (void)opaque;
  kfree(ptr);
}

struct radix_iteration_context {
  uint64_t previous;
  uint64_t first;
  uint64_t last;
  uint32_t count;
  bool valid;
};

static bool radix_iteration_check(uint64_t index, void *value, void *opaque) {
  struct radix_iteration_context *context = opaque;
  if (!value || index < context->first || index > context->last ||
      (context->count && index <= context->previous))
    context->valid = false;
  context->previous = index;
  context->count++;
  return context->valid;
}

static bool radix_phase2_fault_test(void) {
  struct radix_fault_context fault;
  struct radix_tree_allocator allocator = {
      .alloc = radix_fault_alloc,
      .free = radix_fault_free,
      .context = &fault,
  };

  /* Growing a height-one tree to UINT64_MAX needs exactly twenty nodes. */
  for (uint32_t fail_at = 1; fail_at <= 20; fail_at++) {
    struct radix_tree tree;
    fault.fail_at = 0;
    fault.calls = 0;
    radix_tree_init_with_allocator(&tree, &allocator);
    if (radix_tree_insert(&tree, 0, (void *)1))
      return false;
    uint64_t nodes = tree.nodes;
    uint64_t entries = tree.entries;
    uint8_t height = tree.height;
    fault.calls = 0;
    fault.fail_at = fail_at;
    if (radix_tree_insert(&tree, UINT64_MAX, (void *)3) != -12 ||
        tree.nodes != nodes || tree.entries != entries ||
        tree.height != height || radix_tree_lookup(&tree, 0) != (void *)1 ||
        radix_tree_lookup(&tree, UINT64_MAX) || !radix_tree_validate(&tree)) {
      radix_tree_destroy(&tree, NULL);
      return false;
    }
    radix_tree_destroy(&tree, NULL);
  }
  return true;
}

static bool radix_phase2_iteration_test(void) {
  static const uint64_t keys[] = {
      UINT64_MAX, 4096, 64, 1, 0, 4095, 65, 1024, 999999, 63};
  struct radix_tree tree;
  radix_tree_init(&tree);
  for (uint32_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (radix_tree_insert(&tree, keys[i], (void *)(uintptr_t)(i + 1))) {
      radix_tree_destroy(&tree, NULL);
      return false;
    }
  }
  struct radix_iteration_context all = {
      .first = 0, .last = UINT64_MAX, .valid = true};
  struct radix_iteration_context range = {
      .first = 64, .last = 4096, .valid = true};
  bool pass = radix_tree_for_each(&tree, radix_iteration_check, &all) &&
              all.valid && all.count == 10 &&
              radix_tree_for_each_range(&tree, 64, 4096,
                                        radix_iteration_check, &range) &&
              range.valid && range.count == 5;
  radix_tree_destroy(&tree, NULL);
  return pass;
}

bool radix_tree_phase2_stress_test(void) {
  if (!radix_phase2_fault_test() || !radix_phase2_iteration_test())
    return false;
  /* Keep the live set bounded: the heap intentionally retains empty slab
   * pages, so a huge sparse set would permanently inflate reported usage. */
  return radix_tree_phase1_stress_test(1000000,
                                       0x5048415345325244ULL);
}
