#include "eevfd.h"
#include "../apic/lapic_timer.h"

/* Kernel allocations all live in the higher half.  A pointer below it means a
 * stale/corrupted runqueue link; dereferencing it would turn a scheduler
 * wobble into a fatal kernel-mode fault on a user address. */
static inline bool eevfd_ptr_sane(const void *p) {
    return (uint64_t)p >= 0xFFFF800000000000ULL;
}

static void eevfd_update_min_deadline(struct rb_node *node) {
    if (!node)
        return;
    struct sched_entity *se = rb_entry(node, struct sched_entity, rb_node);
    uint64_t min_d = se->deadline;

    if (node->rb_left) {
        struct sched_entity *left = rb_entry(node->rb_left, struct sched_entity, rb_node);
        if (left->min_deadline < min_d)
            min_d = left->min_deadline;
    }
    if (node->rb_right) {
        struct sched_entity *right = rb_entry(node->rb_right, struct sched_entity, rb_node);
        if (right->min_deadline < min_d)
            min_d = right->min_deadline;
    }
    se->min_deadline = min_d;
}

void eevfd_rq_init(struct eevfd_rq *rq) {
    if (!rq)
        return;
    rq->tasks_tree = RB_ROOT;
    rq->rb_leftmost = NULL;
    rq->vtime = 0;
    rq->total_weight = 0;
    rq->nr_running = 0;
    rq->min_vruntime = 0;
    rq->curr = NULL;
}

void eevfd_entity_init(struct sched_entity *se, int nice, uint64_t slice_ns) {
    if (!se)
        return;
    se->rb_node.__rb_parent_color = 0;  /* NULL parent, RB_RED (0) */
    se->rb_node.rb_left = NULL;
    se->rb_node.rb_right = NULL;
    se->nice_value = (int8_t)nice;
    se->weight = nice_to_weight(nice);
    se->wmult = nice_to_wmult(nice);
    se->slice_ns = slice_ns ? slice_ns : eevfd_calc_slice_for_nice(nice);
    se->vruntime = 0;
    se->deadline = 0;
    se->min_deadline = 0;
    se->lag = 0;
    se->exec_start_ns = 0;
    se->prev_sum_exec_ns = 0;
    se->on_rq = false;
    se->custom_slice = (slice_ns != 0);
}

void eevfd_set_nice(struct sched_entity *se, int nice) {
    if (!se)
        return;
    se->nice_value = (int8_t)nice;
    se->weight = nice_to_weight(nice);
    se->wmult = nice_to_wmult(nice);
    if (!se->custom_slice)
        se->slice_ns = eevfd_calc_slice_for_nice(nice);
    /* Recalculate deadline */
    se->deadline = calc_deadline(se->vruntime, se->slice_ns, se->weight, se->wmult);
    se->min_deadline = se->deadline;
}

void eevfd_set_slice(struct sched_entity *se, uint64_t slice_ns) {
    if (!se)
        return;
    if (slice_ns < EEVFD_MIN_SLICE_NS)
        slice_ns = EEVFD_MIN_SLICE_NS;
    if (slice_ns > EEVFD_MAX_SLICE_NS)
        slice_ns = EEVFD_MAX_SLICE_NS;

    se->slice_ns = slice_ns;
    se->custom_slice = true;
    se->deadline = calc_deadline(se->vruntime, se->slice_ns, se->weight, se->wmult);
    se->min_deadline = se->deadline;
}

void eevfd_place_entity(struct eevfd_rq *rq, struct sched_entity *se, bool initial_spawn) {
    if (!rq || !se || !eevfd_ptr_sane(rq) || !eevfd_ptr_sane(se))
        return;

    uint64_t slice_v = calc_slice_vruntime(se->slice_ns, se->weight, se->wmult);

    if (initial_spawn) {
        /* Newly created entity starts at current runqueue virtual time */
        se->vruntime = rq->vtime;
        se->deadline = se->vruntime + slice_v;
        se->min_deadline = se->deadline;
        se->lag = 0;
    } else {
        /* Lag-preserving placement (Linux place_entity):
         * Retain the entity's earned lag, clamped to [-slice_v, +slice_v]
         * to prevent unbounded credit/debt accumulation. */
        int64_t lag = se->lag;
        if (lag > (int64_t)slice_v)
            lag = (int64_t)slice_v;
        else if (lag < -(int64_t)slice_v)
            lag = -(int64_t)slice_v;

        /* vruntime = V - lag (positive lag = behind = gets to run sooner) */
        if ((int64_t)rq->vtime >= lag)
            se->vruntime = (uint64_t)((int64_t)rq->vtime - lag);
        else
            se->vruntime = 0;

        /* Floor: never go below min_vruntime to avoid leftmost displacement */
        if (se->vruntime < rq->min_vruntime)
            se->vruntime = rq->min_vruntime;

        se->deadline = se->vruntime + slice_v;
        se->min_deadline = se->deadline;
        se->lag = lag;
    }
}

__attribute__((optimize("O3"))) void eevfd_enqueue_entity(struct eevfd_rq *rq, struct sched_entity *se) {
    if (!rq || !se || !eevfd_ptr_sane(rq) || !eevfd_ptr_sane(se) || se->on_rq)
        return;

    /* A node with leftover linkage but on_rq == false is the signature of a
     * double insertion (or a torn dequeue); linking it again turns the tree
     * into a cycle and the next insert walks freed/arbitrary memory. */
    if (se->rb_node.__rb_parent_color || se->rb_node.rb_left || se->rb_node.rb_right)
        return;

    struct rb_node **link = &rq->tasks_tree.rb_node;
    struct rb_node *parent = NULL;
    bool leftmost = true;

    while (*link) {
        parent = *link;
        struct sched_entity *curr = rb_entry(parent, struct sched_entity, rb_node);

        if (se->vruntime < curr->vruntime) {
            link = &parent->rb_left;
        } else if (se->vruntime > curr->vruntime) {
            link = &parent->rb_right;
            leftmost = false;
        } else {
            /* Tie-breaker: earlier deadline goes left, then pointer address */
            if (se->deadline < curr->deadline || (se->deadline == curr->deadline && se < curr)) {
                link = &parent->rb_left;
            } else {
                link = &parent->rb_right;
                leftmost = false;
            }
        }
    }

    se->min_deadline = se->deadline;
    rb_link_node(&se->rb_node, parent, link);
    rb_insert_augmented(&se->rb_node, &rq->tasks_tree, eevfd_update_min_deadline);

    if (leftmost) {
        rq->rb_leftmost = &se->rb_node;
        rq->min_vruntime = se->vruntime;
        if (rq->vtime < rq->min_vruntime)
            rq->vtime = rq->min_vruntime;
    }

    rq->total_weight += se->weight;
    rq->nr_running++;
    se->on_rq = true;
}

__attribute__((optimize("O3"))) void eevfd_dequeue_entity(struct eevfd_rq *rq, struct sched_entity *se) {
    if (!rq || !se || !eevfd_ptr_sane(rq) || !eevfd_ptr_sane(se) || !se->on_rq)
        return;

    /* Snapshot lag before removal so wakeup placement can use it */
    se->lag = (int64_t)rq->vtime - (int64_t)se->vruntime;

    if (rq->rb_leftmost == &se->rb_node) {
        rq->rb_leftmost = rb_next(&se->rb_node);
    }

    rb_erase_augmented(&se->rb_node, &rq->tasks_tree, eevfd_update_min_deadline);

    rq->total_weight -= se->weight;
    rq->nr_running--;
    se->on_rq = false;

    se->rb_node.__rb_parent_color = 0;
    se->rb_node.rb_left = NULL;
    se->rb_node.rb_right = NULL;

    if (rq->rb_leftmost) {
        struct sched_entity *left = rb_entry(rq->rb_leftmost, struct sched_entity, rb_node);
        rq->min_vruntime = left->vruntime;
        if (rq->vtime < rq->min_vruntime)
            rq->vtime = rq->min_vruntime;
    }
}

__attribute__((optimize("O3"))) void eevfd_update_curr(struct eevfd_rq *rq, uint64_t now_ns) {
    if (!rq || !rq->curr)
        return;

    struct sched_entity *curr = rq->curr;
    if (curr->exec_start_ns == 0) {
        curr->exec_start_ns = now_ns;
        return;
    }

    if (now_ns <= curr->exec_start_ns)
        return;

    uint64_t delta_exec_ns = now_ns - curr->exec_start_ns;
    curr->exec_start_ns = now_ns;
    curr->prev_sum_exec_ns += delta_exec_ns;

    /* Advance entity virtual runtime */
    uint64_t delta_vr = calc_delta_vruntime(delta_exec_ns, curr->weight, curr->wmult);
    curr->vruntime += delta_vr;

    /* Advance runqueue virtual time V */
    uint64_t active_weight = rq->total_weight + (curr->on_rq ? 0 : curr->weight);
    uint64_t delta_vt = calc_delta_sys_vtime(delta_exec_ns, active_weight);
    rq->vtime += delta_vt;

    curr->lag = (int64_t)rq->vtime - (int64_t)curr->vruntime;

    /* Maintain monotonic floor (Linux update_min_vruntime) */
    uint64_t min_vr = curr->vruntime;
    if (rq->rb_leftmost) {
        struct sched_entity *left = rb_entry(rq->rb_leftmost, struct sched_entity, rb_node);
        if (left->vruntime < min_vr)
            min_vr = left->vruntime;
    }
    if (min_vr > rq->min_vruntime)
        rq->min_vruntime = min_vr;
    if (rq->vtime < rq->min_vruntime)
        rq->vtime = rq->min_vruntime;
}

__attribute__((optimize("O3"))) static void __pick_eevfd_candidate(struct rb_node *node, uint64_t vtime,
                                   struct sched_entity **best) {
    if (!node)
        return;

    struct sched_entity *se = rb_entry(node, struct sched_entity, rb_node);

    /* Pruning: if entire subtree's minimum deadline cannot beat best, skip it */
    if (*best && se->min_deadline >= (*best)->deadline)
        return;

    /* Explore left child (smaller vruntimes) */
    if (node->rb_left)
        __pick_eevfd_candidate(node->rb_left, vtime, best);

    /* If this node is eligible, check its deadline */
    if (se->vruntime <= vtime) {
        if (!*best || se->deadline < (*best)->deadline) {
            *best = se;
        }

        /* If this node is eligible, right subtree might contain eligible nodes */
        if (node->rb_right)
            __pick_eevfd_candidate(node->rb_right, vtime, best);
    }
    /* If se->vruntime > vtime, right subtree has vruntime > vtime (not eligible), pruned */
}

__attribute__((optimize("O3"))) struct sched_entity *eevfd_pick_next_entity(struct eevfd_rq *rq) {
    if (!rq || !rq->tasks_tree.rb_node)
        return NULL;

    /* O(1) Fast-path 1: Single runnable task */
    if (rq->nr_running == 1 && rq->rb_leftmost) {
        return rb_entry(rq->rb_leftmost, struct sched_entity, rb_node);
    }

    struct sched_entity *leftmost_se = rq->rb_leftmost ? rb_entry(rq->rb_leftmost, struct sched_entity, rb_node) : NULL;
    struct sched_entity *root_se = rb_entry(rq->tasks_tree.rb_node, struct sched_entity, rb_node);

    /* O(1) Fast-path 2: Leftmost is eligible and holds the subtree global minimum deadline */
    if (leftmost_se && leftmost_se->vruntime <= rq->vtime && leftmost_se->deadline <= root_se->min_deadline) {
        return leftmost_se;
    }

    /* Seed best with leftmost if eligible to maximize initial subtree pruning */
    struct sched_entity *best = (leftmost_se && leftmost_se->vruntime <= rq->vtime) ? leftmost_se : NULL;
    __pick_eevfd_candidate(rq->tasks_tree.rb_node, rq->vtime, &best);

    /* Fallback: if no strictly eligible entity found, pick leftmost entity */
    if (!best && leftmost_se) {
        best = leftmost_se;
        /* Advance vtime to make leftmost eligible */
        if (rq->vtime < best->vruntime)
            rq->vtime = best->vruntime;
    }

    return best;
}

void eevfd_set_next_entity(struct eevfd_rq *rq, struct sched_entity *se) {
    if (!rq || !se)
        return;
    eevfd_dequeue_entity(rq, se);
    rq->curr = se;
    se->exec_start_ns = 0;
}

void eevfd_put_prev_entity(struct eevfd_rq *rq, struct sched_entity *se) {
    if (!rq || !se)
        return;
    se->exec_start_ns = 0;
    if (rq->curr == se)
        rq->curr = NULL;
}

void eevfd_migrate_entity(struct eevfd_rq *src_rq, struct eevfd_rq *dst_rq, struct sched_entity *se) {
    if (!se || !dst_rq)
        return;

    uint64_t slice_v = calc_slice_vruntime(se->slice_ns, se->weight, se->wmult);

    int64_t lag = 0;
    if (src_rq && src_rq->vtime > 0) {
        lag = (int64_t)src_rq->vtime - (int64_t)se->vruntime;
    }

    /* Bound lag to [-slice_v, +slice_v] to prevent debt/credit hoarding across cores */
    if (lag < -(int64_t)slice_v) {
        lag = -(int64_t)slice_v;
    } else if (lag > (int64_t)slice_v) {
        lag = (int64_t)slice_v;
    }

    se->lag = lag;

    /* Map to destination virtual time */
    if ((int64_t)dst_rq->vtime >= lag) {
        se->vruntime = (uint64_t)((int64_t)dst_rq->vtime - lag);
    } else {
        se->vruntime = 0;
    }

    /* Compute new deadline */
    se->deadline = calc_deadline(se->vruntime, se->slice_ns, se->weight, se->wmult);
    se->min_deadline = se->deadline;
}

bool eevfd_check_preempt(struct eevfd_rq *rq, struct sched_entity *curr,
                         struct sched_entity *woken) {
    if (!rq || !curr || !woken)
        return false;

    /* Condition 0: Always preempt idle thread */
    if (curr->nice_value == 19 && curr->weight == 15)
        return true;

    /* Condition 1: Current task ran past its allocated slice or deadline */
    if (curr->vruntime >= curr->deadline)
        return true;

    /* Determine if woken entity is interactive / latency-sensitive:
     * - higher priority (more weight)
     * - negative nice value
     * - shorter allocated slice */
    bool interactive = (woken->weight > curr->weight ||
                        woken->nice_value < 0 ||
                        woken->slice_ns < curr->slice_ns);

    uint64_t min_gran = interactive ? EEVFD_INTERACTIVE_MIN_GRANULARITY_NS
                                    : EEVFD_MIN_GRANULARITY_NS;

    /* Enforce minimum execution granularity floor before allowing preemption */
    uint64_t now_ns = lapic_timer_get_ns();
    if (curr->exec_start_ns && now_ns > curr->exec_start_ns) {
        uint64_t exec_delta = now_ns - curr->exec_start_ns;
        if (exec_delta < min_gran)
            return false;
    }

    /* For interactive/higher-priority tasks, drop the wakeup granularity barrier */
    uint64_t gran_v = (interactive && woken->deadline <= curr->deadline)
                          ? 0
                          : calc_slice_vruntime(EEVFD_WAKEUP_GRANULARITY_NS, woken->weight, woken->wmult);

    /* Condition 2: Woken task is eligible and its deadline is earlier than curr */
    if (woken->vruntime <= rq->vtime && (woken->deadline + gran_v) <= curr->deadline)
        return true;

    return false;
}

bool eevfd_validate_rq(const struct eevfd_rq *rq) {
    if (!rq)
        return false;

    if (!rb_validate_tree(&rq->tasks_tree))
        return false;

    if (rq->tasks_tree.rb_node == NULL)
        return (rq->nr_running == 0 && rq->total_weight == 0 && rq->rb_leftmost == NULL);

    uint32_t count = 0;
    uint64_t weight_sum = 0;
    struct rb_node *n = rb_first(&rq->tasks_tree);

    if (n != rq->rb_leftmost)
        return false;

    while (n) {
        struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
        if (!se->on_rq)
            return false;
        count++;
        weight_sum += se->weight;
        n = rb_next(n);
    }

    return (count == rq->nr_running && weight_sum == rq->total_weight);
}
