#ifndef LIB_RBTREE_H
#define LIB_RBTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RB_RED = 0,
    RB_BLACK = 1
} rb_color_t;

struct rb_node {
    unsigned long __rb_parent_color;  /* parent ptr | color in low bit */
    struct rb_node *rb_left;
    struct rb_node *rb_right;
};

#define rb_parent(r)   ((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define rb_color(r)    ((rb_color_t)((r)->__rb_parent_color & 1UL))
#define rb_is_red(r)   (!rb_color(r))
#define rb_is_black(r) (rb_color(r))
#define rb_set_parent(r, p)  do { \
    (r)->__rb_parent_color = rb_color(r) | (unsigned long)(p); \
} while (0)
#define rb_set_color(r, c)   do { \
    (r)->__rb_parent_color = ((r)->__rb_parent_color & ~1UL) | (c); \
} while (0)
#define rb_set_parent_color(r, p, c) do { \
    (r)->__rb_parent_color = (unsigned long)(p) | (c); \
} while (0)

typedef struct rb_node rb_node_t;

struct rb_root {
    struct rb_node *rb_node;
};

typedef struct rb_root rb_root_t;

#define RB_ROOT (struct rb_root){ NULL }

#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define rb_entry_safe(ptr, type, member) \
    ((ptr) ? rb_entry(ptr, type, member) : NULL)

/**
 * Link a newly allocated node into the tree at the given link position.
 * Must be followed by rb_insert_color() (or rb_insert_augmented()).
 */
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **rb_link) {
    node->__rb_parent_color = (unsigned long)parent;
    node->rb_left = NULL;
    node->rb_right = NULL;
    *rb_link = node;
}

/* Red-Black Tree core operations */
void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
void rb_replace_node(struct rb_node *victim, struct rb_node *new_node,
                     struct rb_root *root);

/* Tree navigation */
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

/* Subtree augmented callback support */
typedef void (*rb_augment_cb)(struct rb_node *node);

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         rb_augment_cb augment_cb);
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        rb_augment_cb augment_cb);
void rb_augment_propagate(struct rb_node *node, rb_augment_cb augment_cb);

/* Validation helpers (used for kernel-init stress testing) */
bool rb_validate_tree(const struct rb_root *root);

#endif /* LIB_RBTREE_H */
