#include "rbtree.h"

static inline void __rb_rotate_left(struct rb_node *node, struct rb_root *root,
                                    rb_augment_cb augment_cb) {
    struct rb_node *right = node->rb_right;
    struct rb_node *parent = rb_parent(node);

    node->rb_right = right->rb_left;
    if (right->rb_left)
        rb_set_parent(right->rb_left, node);

    right->rb_left = node;
    rb_set_parent_color(right, parent, rb_color(node));

    if (parent) {
        if (node == parent->rb_left)
            parent->rb_left = right;
        else
            parent->rb_right = right;
    } else {
        root->rb_node = right;
    }

    rb_set_parent_color(node, right, RB_RED);

    if (augment_cb) {
        augment_cb(node);
        augment_cb(right);
    }
}

static inline void __rb_rotate_right(struct rb_node *node, struct rb_root *root,
                                     rb_augment_cb augment_cb) {
    struct rb_node *left = node->rb_left;
    struct rb_node *parent = rb_parent(node);

    node->rb_left = left->rb_right;
    if (left->rb_right)
        rb_set_parent(left->rb_right, node);

    left->rb_right = node;
    rb_set_parent_color(left, parent, rb_color(node));

    if (parent) {
        if (node == parent->rb_right)
            parent->rb_right = left;
        else
            parent->rb_left = left;
    } else {
        root->rb_node = left;
    }

    rb_set_parent_color(node, left, RB_RED);

    if (augment_cb) {
        augment_cb(node);
        augment_cb(left);
    }
}

void rb_augment_propagate(struct rb_node *node, rb_augment_cb augment_cb) {
    if (!augment_cb)
        return;
    while (node) {
        augment_cb(node);
        node = rb_parent(node);
    }
}

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         rb_augment_cb augment_cb) {
    struct rb_node *parent = rb_parent(node);
    struct rb_node *gparent;

    if (augment_cb)
        rb_augment_propagate(node, augment_cb);

    while (parent && rb_is_red(parent)) {
        gparent = rb_parent(parent);
        if (!gparent)
            break;

        if (parent == gparent->rb_left) {
            struct rb_node *uncle = gparent->rb_right;
            if (uncle && rb_is_red(uncle)) {
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(parent, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                parent = rb_parent(node);
                continue;
            }

            if (parent->rb_right == node) {
                __rb_rotate_left(parent, root, augment_cb);
                struct rb_node *tmp = parent;
                parent = node;
                node = tmp;
            }

            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            __rb_rotate_right(gparent, root, augment_cb);
        } else {
            struct rb_node *uncle = gparent->rb_left;
            if (uncle && rb_is_red(uncle)) {
                rb_set_color(uncle, RB_BLACK);
                rb_set_color(parent, RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                parent = rb_parent(node);
                continue;
            }

            if (parent->rb_left == node) {
                __rb_rotate_right(parent, root, augment_cb);
                struct rb_node *tmp = parent;
                parent = node;
                node = tmp;
            }

            rb_set_color(parent, RB_BLACK);
            rb_set_color(gparent, RB_RED);
            __rb_rotate_left(gparent, root, augment_cb);
        }
    }

    rb_set_color(root->rb_node, RB_BLACK);
}

void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    rb_insert_augmented(node, root, NULL);
}

static void __rb_erase_color(struct rb_node *node, struct rb_node *parent,
                             struct rb_root *root, rb_augment_cb augment_cb) {
    struct rb_node *other;

    while ((!node || rb_is_black(node)) && node != root->rb_node) {
        if (parent->rb_left == node) {
            other = parent->rb_right;
            if (other && rb_is_red(other)) {
                rb_set_color(other, RB_BLACK);
                rb_set_color(parent, RB_RED);
                __rb_rotate_left(parent, root, augment_cb);
                other = parent->rb_right;
            }
            if (!other) {
                node = parent;
                parent = rb_parent(node);
                continue;
            }
            if ((!other->rb_left || rb_is_black(other->rb_left)) &&
                (!other->rb_right || rb_is_black(other->rb_right))) {
                rb_set_color(other, RB_RED);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (!other->rb_right || rb_is_black(other->rb_right)) {
                    if (other->rb_left)
                        rb_set_color(other->rb_left, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_right(other, root, augment_cb);
                    other = parent->rb_right;
                }
                if (other) {
                    rb_set_color(other, rb_color(parent));
                    if (other->rb_right)
                        rb_set_color(other->rb_right, RB_BLACK);
                }
                rb_set_color(parent, RB_BLACK);
                __rb_rotate_left(parent, root, augment_cb);
                node = root->rb_node;
                break;
            }
        } else {
            other = parent->rb_left;
            if (other && rb_is_red(other)) {
                rb_set_color(other, RB_BLACK);
                rb_set_color(parent, RB_RED);
                __rb_rotate_right(parent, root, augment_cb);
                other = parent->rb_left;
            }
            if (!other) {
                node = parent;
                parent = rb_parent(node);
                continue;
            }
            if ((!other->rb_left || rb_is_black(other->rb_left)) &&
                (!other->rb_right || rb_is_black(other->rb_right))) {
                rb_set_color(other, RB_RED);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (!other->rb_left || rb_is_black(other->rb_left)) {
                    if (other->rb_right)
                        rb_set_color(other->rb_right, RB_BLACK);
                    rb_set_color(other, RB_RED);
                    __rb_rotate_left(other, root, augment_cb);
                    other = parent->rb_left;
                }
                if (other) {
                    rb_set_color(other, rb_color(parent));
                    if (other->rb_left)
                        rb_set_color(other->rb_left, RB_BLACK);
                }
                rb_set_color(parent, RB_BLACK);
                __rb_rotate_right(parent, root, augment_cb);
                node = root->rb_node;
                break;
            }
        }
    }
    if (node)
        rb_set_color(node, RB_BLACK);
}

void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        rb_augment_cb augment_cb) {
    struct rb_node *child, *parent;
    rb_color_t color;

    if (!node->rb_left) {
        child = node->rb_right;
        parent = rb_parent(node);
        color = rb_color(node);

        if (child)
            rb_set_parent(child, parent);
        if (parent) {
            if (parent->rb_left == node)
                parent->rb_left = child;
            else
                parent->rb_right = child;
        } else {
            root->rb_node = child;
        }

        if (augment_cb)
            rb_augment_propagate(parent, augment_cb);
    } else if (!node->rb_right) {
        child = node->rb_left;
        parent = rb_parent(node);
        color = rb_color(node);

        rb_set_parent(child, parent);
        if (parent) {
            if (parent->rb_left == node)
                parent->rb_left = child;
            else
                parent->rb_right = child;
        } else {
            root->rb_node = child;
        }

        if (augment_cb)
            rb_augment_propagate(parent, augment_cb);
    } else {
        struct rb_node *old = node;
        struct rb_node *left;

        node = node->rb_right;
        while ((left = node->rb_left) != NULL)
            node = left;

        child = node->rb_right;
        parent = rb_parent(node);
        color = rb_color(node);

        if (child)
            rb_set_parent(child, parent);
        if (parent) {
            if (parent->rb_left == node)
                parent->rb_left = child;
            else
                parent->rb_right = child;
        } else {
            root->rb_node = child;
        }

        if (rb_parent(node) == old)
            parent = node;

        rb_set_parent_color(node, rb_parent(old), rb_color(old));
        node->rb_right = old->rb_right;
        node->rb_left = old->rb_left;

        if (rb_parent(old)) {
            if (rb_parent(old)->rb_left == old)
                rb_parent(old)->rb_left = node;
            else
                rb_parent(old)->rb_right = node;
        } else {
            root->rb_node = node;
        }

        rb_set_parent(old->rb_left, node);
        if (old->rb_right)
            rb_set_parent(old->rb_right, node);

        if (augment_cb) {
            augment_cb(node);
            rb_augment_propagate(parent, augment_cb);
        }
    }

    if (color == RB_BLACK)
        __rb_erase_color(child, parent, root, augment_cb);
}

void rb_erase(struct rb_node *node, struct rb_root *root) {
    rb_erase_augmented(node, root, NULL);
}

void rb_replace_node(struct rb_node *victim, struct rb_node *new_node,
                     struct rb_root *root) {
    struct rb_node *parent = rb_parent(victim);

    if (parent) {
        if (victim == parent->rb_left)
            parent->rb_left = new_node;
        else
            parent->rb_right = new_node;
    } else {
        root->rb_node = new_node;
    }

    if (victim->rb_left)
        rb_set_parent(victim->rb_left, new_node);
    if (victim->rb_right)
        rb_set_parent(victim->rb_right, new_node);

    *new_node = *victim;
}

struct rb_node *rb_first(const struct rb_root *root) {
    struct rb_node *n;

    n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_left)
        n = n->rb_left;
    return n;
}

struct rb_node *rb_last(const struct rb_root *root) {
    struct rb_node *n;

    n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_right)
        n = n->rb_right;
    return n;
}

struct rb_node *rb_next(const struct rb_node *node) {
    struct rb_node *parent;

    /* A node that is its own parent is an empty/detached node sentinel. */
    if (rb_parent(node) == node)
        return NULL;

    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node *)node;
    }

    while ((parent = rb_parent(node)) && node == parent->rb_right)
        node = parent;

    return parent;
}

struct rb_node *rb_prev(const struct rb_node *node) {
    struct rb_node *parent;

    /* A node that is its own parent is an empty/detached node sentinel. */
    if (rb_parent(node) == node)
        return NULL;

    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right)
            node = node->rb_right;
        return (struct rb_node *)node;
    }

    while ((parent = rb_parent(node)) && node == parent->rb_left)
        node = parent;

    return parent;
}

static int __rb_validate_subtree(const struct rb_node *node, int *black_height_out) {
    if (!node) {
        *black_height_out = 1; /* NIL leaves are black */
        return 1;
    }

    /* Property: If node is RED, both children must be BLACK */
    if (rb_is_red(node)) {
        if (node->rb_left && rb_is_red(node->rb_left))
            return 0;
        if (node->rb_right && rb_is_red(node->rb_right))
            return 0;
    }

    /* Property: Parent pointers must be symmetric */
    if (node->rb_left && rb_parent(node->rb_left) != node)
        return 0;
    if (node->rb_right && rb_parent(node->rb_right) != node)
        return 0;

    int left_bh = 0, right_bh = 0;
    if (!__rb_validate_subtree(node->rb_left, &left_bh))
        return 0;
    if (!__rb_validate_subtree(node->rb_right, &right_bh))
        return 0;

    /* Property: Black height must be identical across left and right subtrees */
    if (left_bh != right_bh)
        return 0;

    *black_height_out = left_bh + (rb_is_black(node) ? 1 : 0);
    return 1;
}

bool rb_validate_tree(const struct rb_root *root) {
    if (!root || !root->rb_node)
        return true;

    /* Property: Root must be BLACK */
    if (!rb_is_black(root->rb_node))
        return false;

    int bh = 0;
    return __rb_validate_subtree(root->rb_node, &bh) != 0;
}
