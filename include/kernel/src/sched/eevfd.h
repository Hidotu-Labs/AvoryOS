#ifndef SCHED_EEVFD_H
#define SCHED_EEVFD_H

#include "../lib/rbtree.h"
#include "eevfd_math.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Scheduling entity for EEVFD.
 * Embedded directly inside struct thread.
 */
struct sched_entity {
    struct rb_node rb_node;       /* Linkage into EEVFD tasks_tree */
    uint64_t vruntime;            /* Virtual runtime (progress) */
    uint64_t deadline;            /* Virtual deadline (eligibility + slice) */
    uint64_t min_deadline;        /* Augmented subtree minimum deadline */
    uint64_t slice_ns;            /* Allocated physical quantum */
    uint32_t weight;              /* Nice weight from prio_to_weight */
    uint32_t wmult;               /* Inverse multiplier from prio_to_wmult */
    int64_t  lag;                 /* Virtual lag: V_rq - vruntime */
    uint64_t exec_start_ns;       /* Start timestamp of current CPU run */
    uint64_t prev_sum_exec_ns;    /* Total execution time in nanoseconds */
    int8_t   nice_value;          /* Nice value [-20, 19] */
    bool     on_rq;               /* True if entity is currently in runqueue tree */
    bool     custom_slice;        /* True if userland/thread requested custom slice */
};

typedef struct sched_entity sched_entity_t;

/**
 * Per-CPU EEVFD Runqueue structure.
 */
struct eevfd_rq {
    struct rb_root tasks_tree;    /* Red-black tree of runnable entities */
    struct rb_node *rb_leftmost;  /* Cached entity with minimum vruntime */
    uint64_t vtime;               /* System virtual time (monotonic) */
    uint64_t total_weight;        /* Total weight of runnable entities W */
    uint32_t nr_running;          /* Number of runnable entities */
    uint64_t min_vruntime;        /* Minimum vruntime across all runnable entities */
    struct sched_entity *curr;    /* Currently running entity on this CPU */
};

typedef struct eevfd_rq eevfd_rq_t;

/* Runqueue and Entity Management API */
void eevfd_rq_init(struct eevfd_rq *rq);
void eevfd_entity_init(struct sched_entity *se, int nice, uint64_t slice_ns);
void eevfd_set_nice(struct sched_entity *se, int nice);
void eevfd_set_slice(struct sched_entity *se, uint64_t slice_ns);

/* Enqueue / Dequeue */
void eevfd_place_entity(struct eevfd_rq *rq, struct sched_entity *se, bool initial_spawn);
void eevfd_enqueue_entity(struct eevfd_rq *rq, struct sched_entity *se);
void eevfd_dequeue_entity(struct eevfd_rq *rq, struct sched_entity *se);

/* Execution accounting & context switching */
void eevfd_update_curr(struct eevfd_rq *rq, uint64_t now_ns);
struct sched_entity *eevfd_pick_next_entity(struct eevfd_rq *rq);
void eevfd_set_next_entity(struct eevfd_rq *rq, struct sched_entity *se);
void eevfd_put_prev_entity(struct eevfd_rq *rq, struct sched_entity *se);

/* Task migration across SMP cores */
void eevfd_migrate_entity(struct eevfd_rq *src_rq, struct eevfd_rq *dst_rq, struct sched_entity *se);

/* Preemption checks */
bool eevfd_check_preempt(struct eevfd_rq *rq, struct sched_entity *curr,
                         struct sched_entity *woken);

/* Invariant & consistency validation */
bool eevfd_validate_rq(const struct eevfd_rq *rq);

#endif /* SCHED_EEVFD_H */
