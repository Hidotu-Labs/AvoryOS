/* Phase 1 — imported Linux library smoke tests.
 *
 * This file is compiled with the real Linux headers (see the dedicated
 * pattern rule in kernel/GNUmakefile), so it exercises the exact upstream
 * types and signatures rather than hand-copied declarations.
 */

#include <linux/bitmap.h>
#include <linux/gcd.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/math.h>
#include <linux/rbtree.h>

#include <linuxkpi/log.h>

struct sort_elem {
  struct list_head list;
  int value;
};

static int sort_elem_cmp(void *priv, const struct list_head *a,
                         const struct list_head *b) {
  const struct sort_elem *ea = container_of(a, struct sort_elem, list);
  const struct sort_elem *eb = container_of(b, struct sort_elem, list);

  (void)priv;
  if (ea->value < eb->value)
    return -1;
  if (ea->value > eb->value)
    return 1;
  return 0;
}

static bool test_gcd(void) {
  return gcd(1071, 462) == 21 && gcd(17, 5) == 1 && gcd(0, 9) == 9 &&
         gcd(42, 42) == 42;
}

static bool test_int_sqrt(void) {
  return int_sqrt(0) == 0 && int_sqrt(1) == 1 && int_sqrt(144) == 12 &&
         int_sqrt(145) == 12 && int_sqrt(1000000) == 1000;
}

static bool test_find_bits(void) {
  unsigned long map[2] = {0x10UL, 0};    /* bit 4 set */
  unsigned long map2[2] = {0, 1UL << 1}; /* bit 65 set */
  unsigned long none[2] = {0, 0};

  return _find_first_bit(map, 128) == 4 &&
         _find_next_bit(map, 128, 5) == 128 &&
         _find_next_zero_bit(map, 128, 4) == 5 &&
         _find_first_bit(map2, 128) == 65 &&
         _find_last_bit(map2, 128) == 65 &&
         _find_first_bit(none, 128) == 128;
}

static bool test_list_sort(void) {
  static const int values[8] = {5, 1, 4, 2, 8, 6, 3, 7};
  struct sort_elem elems[8];
  LIST_HEAD(head);

  for (int i = 0; i < 8; i++) {
    elems[i].value = values[i];
    list_add_tail(&elems[i].list, &head);
  }

  list_sort(NULL, &head, sort_elem_cmp);

  int expected = 1;
  struct sort_elem *e;
  list_for_each_entry(e, &head, list) {
    if (e->value != expected)
      return false;
    expected++;
  }
  return expected == 9;
}

/* These inlines use imported Linux layouts but link to native rb_*.
 * Five ordered inserts used to break color invariants at a non-root rotation.
 * Check every mutation so a later balancing operation cannot hide the defect. */
struct rb_test_elem {
  struct rb_node node;
  unsigned int key;
};

static bool rb_test_less(struct rb_node *a, const struct rb_node *b) {
  return rb_entry(a, struct rb_test_elem, node)->key <
         rb_entry(b, struct rb_test_elem, node)->key;
}

static int rb_test_black_height(struct rb_node *node, struct rb_node *parent,
                                 unsigned int depth) {
  if (!node)
    return 1;
  if (depth >= 32 || rb_parent(node) != parent)
    return 0;
  bool black = node->__rb_parent_color & 1UL;
  if (!black && ((node->rb_left && !(node->rb_left->__rb_parent_color & 1UL)) ||
                 (node->rb_right && !(node->rb_right->__rb_parent_color & 1UL))))
    return 0;
  int left = rb_test_black_height(node->rb_left, node, depth + 1);
  int right = rb_test_black_height(node->rb_right, node, depth + 1);
  return left && left == right ? left + black : 0;
}

static bool rb_test_valid(struct rb_root_cached *root, unsigned int expected) {
  struct rb_node *node = root->rb_root.rb_node;
  if ((node && !(node->__rb_parent_color & 1UL)) ||
      !rb_test_black_height(node, NULL, 0) ||
      rb_first_cached(root) != rb_first(&root->rb_root))
    return false;
  unsigned int count = 0, previous = 0;
  for (node = rb_first_cached(root); node; node = rb_next(node)) {
    unsigned int key = rb_entry(node, struct rb_test_elem, node)->key;
    if (count >= expected || (count && key <= previous))
      return false;
    previous = key;
    count++;
  }
  return count == expected;
}

static bool test_rbtree_native_bridge(void) {
  struct rb_test_elem elems[32];
  struct rb_root_cached root = RB_ROOT_CACHED;

  for (unsigned int i = 0; i < ARRAY_SIZE(elems); i++) {
    elems[i].key = i;
    rb_add_cached(&elems[i].node, &root, rb_test_less);
    if (!rb_test_valid(&root, i + 1))
      return false;
  }
  for (unsigned int i = 0; i < ARRAY_SIZE(elems); i++) {
    /* Odd stride visits each node once, including two-child erasures. */
    unsigned int index = (i * 13) % ARRAY_SIZE(elems);
    rb_erase_cached(&elems[index].node, &root);
    if (!rb_test_valid(&root, ARRAY_SIZE(elems) - i - 1))
      return false;
  }
  return RB_EMPTY_ROOT(&root.rb_root);
}

void linuxkpi_test_phase1_libs(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"gcd", test_gcd},
      {"int_sqrt", test_int_sqrt},
      {"find_bit", test_find_bits},
      {"list_sort", test_list_sort},
      {"rbtree native bridge", test_rbtree_native_bridge},
  };

  klog_puts("[LINUXKPI] Phase 1 library import self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s (upstream) correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s (upstream) wrong result\n", tests[i].name);
  }
}
