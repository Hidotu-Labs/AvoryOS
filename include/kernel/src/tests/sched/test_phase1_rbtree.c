#include "console/klog.h"
#include "lib/rbtree.h"
#include "sched/eevfd_math.h"
#include <stdbool.h>
#include <stdint.h>

#define P1_ASSERT(expr)                                                       \
    do {                                                                      \
        if (!(expr)) {                                                        \
            klog_puts("[EEVFD P1] FAIL: " #expr "\n");                        \
            g_phase1_fail = 1;                                                \
            return;                                                           \
        }                                                                     \
    } while (0)

static int g_phase1_fail = 0;

/* Simple deterministic LCG pseudo-random generator */
static uint32_t p1_prng_state = 0x12345678;
static inline uint32_t p1_rand(void) {
    p1_prng_state = p1_prng_state * 1103515245 + 12345;
    return (p1_prng_state >> 16) & 0x7FFF;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 1: Basic Red-Black Tree Balancing & Ordering (1000 nodes)            */
/* ────────────────────────────────────────────────────────────────────────── */

struct basic_node {
    struct rb_node rb;
    uint32_t key;
    uint32_t val;
};

#define BASIC_TEST_COUNT 1000
static struct basic_node basic_nodes[BASIC_TEST_COUNT];

static void insert_basic_node(struct rb_root *root, struct basic_node *node) {
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;

    while (*link) {
        parent = *link;
        struct basic_node *curr = rb_entry(parent, struct basic_node, rb);
        if (node->key < curr->key)
            link = &parent->rb_left;
        else
            link = &parent->rb_right;
    }

    rb_link_node(&node->rb, parent, link);
    rb_insert_color(&node->rb, root);
}

static void test_rb_basic(void) {
    struct rb_root root = RB_ROOT;

    /* Initialize and populate nodes with pseudo-random permutation */
    for (uint32_t i = 0; i < BASIC_TEST_COUNT; i++) {
        basic_nodes[i].key = (p1_rand() << 15) | p1_rand();
        basic_nodes[i].val = i;
        insert_basic_node(&root, &basic_nodes[i]);
    }

    P1_ASSERT(rb_validate_tree(&root));

    /* Verify strictly increasing in-order iteration */
    struct rb_node *n = rb_first(&root);
    P1_ASSERT(n != NULL);
    uint32_t count = 0;
    uint32_t prev_key = 0;

    while (n) {
        struct basic_node *bn = rb_entry(n, struct basic_node, rb);
        if (count > 0) {
            P1_ASSERT(bn->key >= prev_key);
        }
        prev_key = bn->key;
        count++;
        n = rb_next(n);
    }
    P1_ASSERT(count == BASIC_TEST_COUNT);

    /* Verify reverse in-order iteration */
    n = rb_last(&root);
    P1_ASSERT(n != NULL);
    count = 0;
    prev_key = 0xFFFFFFFF;

    while (n) {
        struct basic_node *bn = rb_entry(n, struct basic_node, rb);
        if (count > 0) {
            P1_ASSERT(bn->key <= prev_key);
        }
        prev_key = bn->key;
        count++;
        n = rb_prev(n);
    }
    P1_ASSERT(count == BASIC_TEST_COUNT);

    /* Delete half the nodes (even indices) and re-verify balance */
    for (uint32_t i = 0; i < BASIC_TEST_COUNT; i += 2) {
        rb_erase(&basic_nodes[i].rb, &root);
    }

    P1_ASSERT(rb_validate_tree(&root));

    /* Verify remaining 500 nodes */
    count = 0;
    n = rb_first(&root);
    while (n) {
        count++;
        n = rb_next(n);
    }
    P1_ASSERT(count == BASIC_TEST_COUNT / 2);

    /* Delete remaining nodes */
    for (uint32_t i = 1; i < BASIC_TEST_COUNT; i += 2) {
        rb_erase(&basic_nodes[i].rb, &root);
    }

    P1_ASSERT(root.rb_node == NULL);
    P1_ASSERT(rb_validate_tree(&root));
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 2: Augmented Red-Black Tree (min_deadline propagation)               */
/* ────────────────────────────────────────────────────────────────────────── */

struct aug_entity {
    struct rb_node rb;
    uint64_t vruntime;
    uint64_t deadline;
    uint64_t min_deadline;
};

#define AUG_TEST_COUNT 500
static struct aug_entity aug_nodes[AUG_TEST_COUNT];

static void aug_update_min_deadline(struct rb_node *node) {
    if (!node)
        return;
    struct aug_entity *e = rb_entry(node, struct aug_entity, rb);
    uint64_t min_d = e->deadline;

    if (node->rb_left) {
        struct aug_entity *l = rb_entry(node->rb_left, struct aug_entity, rb);
        if (l->min_deadline < min_d)
            min_d = l->min_deadline;
    }
    if (node->rb_right) {
        struct aug_entity *r = rb_entry(node->rb_right, struct aug_entity, rb);
        if (r->min_deadline < min_d)
            min_d = r->min_deadline;
    }
    e->min_deadline = min_d;
}

static void insert_aug_entity(struct rb_root *root, struct aug_entity *e) {
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;

    while (*link) {
        parent = *link;
        struct aug_entity *curr = rb_entry(parent, struct aug_entity, rb);
        if (e->vruntime < curr->vruntime)
            link = &parent->rb_left;
        else
            link = &parent->rb_right;
    }

    e->min_deadline = e->deadline;
    rb_link_node(&e->rb, parent, link);
    rb_insert_augmented(&e->rb, root, aug_update_min_deadline);
}

static bool verify_aug_subtree(struct rb_node *node) {
    if (!node)
        return true;

    struct aug_entity *e = rb_entry(node, struct aug_entity, rb);
    uint64_t expected_min = e->deadline;

    if (node->rb_left) {
        struct aug_entity *l = rb_entry(node->rb_left, struct aug_entity, rb);
        if (l->min_deadline < expected_min)
            expected_min = l->min_deadline;
        if (!verify_aug_subtree(node->rb_left))
            return false;
    }

    if (node->rb_right) {
        struct aug_entity *r = rb_entry(node->rb_right, struct aug_entity, rb);
        if (r->min_deadline < expected_min)
            expected_min = r->min_deadline;
        if (!verify_aug_subtree(node->rb_right))
            return false;
    }

    return (e->min_deadline == expected_min);
}

static void test_rb_augmented(void) {
    struct rb_root root = RB_ROOT;

    for (uint32_t i = 0; i < AUG_TEST_COUNT; i++) {
        aug_nodes[i].vruntime = ((uint64_t)p1_rand() << 32) | ((uint64_t)p1_rand() << 16) | p1_rand();
        aug_nodes[i].deadline = ((uint64_t)p1_rand() << 32) | ((uint64_t)p1_rand() << 16) | p1_rand();
        insert_aug_entity(&root, &aug_nodes[i]);
    }

    P1_ASSERT(rb_validate_tree(&root));
    P1_ASSERT(verify_aug_subtree(root.rb_node));

    /* Erase half the augmented nodes */
    for (uint32_t i = 0; i < AUG_TEST_COUNT; i += 2) {
        rb_erase_augmented(&aug_nodes[i].rb, &root, aug_update_min_deadline);
    }

    P1_ASSERT(rb_validate_tree(&root));
    P1_ASSERT(verify_aug_subtree(root.rb_node));

    /* Erase remaining nodes */
    for (uint32_t i = 1; i < AUG_TEST_COUNT; i += 2) {
        rb_erase_augmented(&aug_nodes[i].rb, &root, aug_update_min_deadline);
    }

    P1_ASSERT(root.rb_node == NULL);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 3: Fixed-Point Arithmetic & Precision Stress Test                    */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_eevfd_math(void) {
    /* Test 1: Nice 0 identity */
    uint64_t delta_ns = 3000000ULL; /* 3ms */
    uint32_t w0 = nice_to_weight(0);
    uint32_t m0 = nice_to_wmult(0);
    P1_ASSERT(w0 == 1024);
    P1_ASSERT(calc_delta_vruntime(delta_ns, w0, m0) == delta_ns);

    /* Test 2: Monotonic weight scaling across all nice levels */
    for (int nice = -20; nice < 19; nice++) {
        uint32_t w1 = nice_to_weight(nice);
        uint32_t w2 = nice_to_weight(nice + 1);
        P1_ASSERT(w1 > w2); /* Higher priority (lower nice) = higher weight */

        uint32_t m1 = nice_to_wmult(nice);
        uint32_t m2 = nice_to_wmult(nice + 1);
        P1_ASSERT(m1 < m2); /* Higher priority = smaller vruntime multiplier */
    }

    /* Test 3: Precision comparison with 64-bit reference arithmetic */
    static const uint64_t test_slices[] = {
        100000ULL, 500000ULL, 1000000ULL, 3000000ULL, 10000000ULL, 100000000ULL
    };

    for (int nice = -20; nice <= 19; nice++) {
        uint32_t w = nice_to_weight(nice);
        uint32_t m = nice_to_wmult(nice);

        for (size_t s = 0; s < sizeof(test_slices) / sizeof(test_slices[0]); s++) {
            uint64_t dt = test_slices[s];
            uint64_t ref = (dt * 1024ULL) / w;
            uint64_t fast = calc_delta_vruntime(dt, w, m);

            int64_t diff = (int64_t)ref - (int64_t)fast;
            if (diff < 0) diff = -diff;

            /* Relative error must be strictly under 0.1% (1 in 1000) or absolute error <= 1 */
            P1_ASSERT(diff <= 1 || (uint64_t)diff * 1000ULL <= ref);
        }
    }

    /* Test 4: System virtual time calculation */
    uint64_t total_w = 1024 + 1024 + 1024; /* 3 nice-0 tasks */
    uint64_t dt_sys = calc_delta_sys_vtime(3000000ULL, total_w);
    P1_ASSERT(dt_sys == 1000000ULL); /* 3ms / 3 = 1ms */
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Phase 1 Master Runner                                                     */
/* ────────────────────────────────────────────────────────────────────────── */

void eevfd_test_phase1(void) {
    klog_puts("[TEST] EEVFD Phase 1: Red-Black Tree & Fixed-Point Math Engine...\n");
    g_phase1_fail = 0;

    test_rb_basic();
    if (g_phase1_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 1: Basic RB-tree failed!\n");
        return;
    }

    test_rb_augmented();
    if (g_phase1_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 1: Augmented RB-tree failed!\n");
        return;
    }

    test_eevfd_math();
    if (g_phase1_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 1: Fixed-point math failed!\n");
        return;
    }

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
              " EEVFD Phase 1 Passed: 1,500-node RB-Tree & Math Stress Tests.\n");
}
