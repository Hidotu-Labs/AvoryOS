#include "console/klog.h"
#include "sched/eevfd.h"
#include <stdbool.h>
#include <stdint.h>

#define P2_ASSERT(expr)                                                       \
    do {                                                                      \
        if (!(expr)) {                                                        \
            klog_puts("[EEVFD P2] FAIL: " #expr "\n");                        \
            g_phase2_fail = 1;                                                \
            return;                                                           \
        }                                                                     \
    } while (0)

static int g_phase2_fail = 0;

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 1: Deterministic Multi-Task CPU Share Convergence (20,000 slices)    */
/* ────────────────────────────────────────────────────────────────────────── */

#define NUM_SIM_TASKS 6
static struct sched_entity sim_tasks[NUM_SIM_TASKS];
static const int sim_nices[NUM_SIM_TASKS] = { -10, 0, 0, 5, 10, -5 };

static void test_eevfd_convergence(void) {
    struct eevfd_rq rq;
    eevfd_rq_init(&rq);

    uint64_t sim_now_ns = 1000000ULL;
    const uint64_t slice_step_ns = EEVFD_BASE_SLICE_NS; /* 3 ms per slice quantum */

    for (int i = 0; i < NUM_SIM_TASKS; i++) {
        eevfd_entity_init(&sim_tasks[i], sim_nices[i], slice_step_ns);
        eevfd_place_entity(&rq, &sim_tasks[i], true);
        eevfd_enqueue_entity(&rq, &sim_tasks[i]);
    }

    P2_ASSERT(eevfd_validate_rq(&rq));
    P2_ASSERT(rq.nr_running == NUM_SIM_TASKS);

    /* Simulate 20,000 scheduling rounds */
    const uint32_t total_rounds = 20000;

    for (uint32_t round = 0; round < total_rounds; round++) {
        struct sched_entity *next = eevfd_pick_next_entity(&rq);
        P2_ASSERT(next != NULL);

        /* Eligibility check: task must be eligible or leftmost */
        bool is_leftmost = (&next->rb_node == rq.rb_leftmost);
        P2_ASSERT(next->vruntime <= rq.vtime || is_leftmost);

        eevfd_set_next_entity(&rq, next);
        next->exec_start_ns = sim_now_ns;

        sim_now_ns += slice_step_ns;
        eevfd_update_curr(&rq, sim_now_ns);

        /* Recalculate deadline for next slice */
        next->deadline = calc_deadline(next->vruntime, next->slice_ns, next->weight, next->wmult);
        next->min_deadline = next->deadline;

        eevfd_put_prev_entity(&rq, next);
        eevfd_enqueue_entity(&rq, next);

        if ((round & 0x3FF) == 0) {
            P2_ASSERT(eevfd_validate_rq(&rq));
        }
    }

    /* Verify proportional share convergence against nice 0 task (Task 1) */
    uint64_t t_ref = sim_tasks[1].prev_sum_exec_ns;
    uint32_t w_ref = sim_tasks[1].weight;
    P2_ASSERT(t_ref > 0);

    for (int i = 0; i < NUM_SIM_TASKS; i++) {
        uint64_t t_actual = sim_tasks[i].prev_sum_exec_ns;
        uint32_t w_actual = sim_tasks[i].weight;

        /* Expected ratio: (t_actual / t_ref) ~= (w_actual / w_ref) */
        /* Rearranged: t_actual * w_ref ~= t_ref * w_actual */
        uint64_t lhs = t_actual * (uint64_t)w_ref;
        uint64_t rhs = t_ref * (uint64_t)w_actual;

        int64_t diff = (int64_t)lhs - (int64_t)rhs;
        if (diff < 0) diff = -diff;

        /* Error must be strictly under 0.5% */
        P2_ASSERT((uint64_t)diff * 200ULL <= rhs);
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 2: Bounded Lag & Anti-Hoarding Wakeup Semantics                      */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_eevfd_lag_bounding(void) {
    struct eevfd_rq rq;
    eevfd_rq_init(&rq);

    struct sched_entity t_active, t_sleeper;
    eevfd_entity_init(&t_active, 0, EEVFD_BASE_SLICE_NS);
    eevfd_entity_init(&t_sleeper, 0, EEVFD_BASE_SLICE_NS);

    eevfd_place_entity(&rq, &t_active, true);
    eevfd_place_entity(&rq, &t_sleeper, true);
    eevfd_enqueue_entity(&rq, &t_active);

    /* Run t_active for 100 seconds of simulated time while t_sleeper sleeps */
    uint64_t sim_now = 10000000ULL;
    t_active.exec_start_ns = 0;
    rq.curr = &t_active;

    for (int step = 0; step < 1000; step++) {
        t_active.exec_start_ns = sim_now;
        sim_now += 10000000ULL; /* 10ms per step */
        eevfd_update_curr(&rq, sim_now);
    }

    P2_ASSERT(rq.vtime > 50000000ULL);

    /* Wake up t_sleeper after long sleep */
    eevfd_place_entity(&rq, &t_sleeper, false);

    uint64_t slice_v = calc_slice_vruntime(t_sleeper.slice_ns, t_sleeper.weight, t_sleeper.wmult);

    /* Assert that sleeper's vruntime is clamped to [rq.vtime - slice_v, rq.vtime + slice_v] */
    P2_ASSERT(t_sleeper.vruntime >= (rq.vtime - slice_v));
    P2_ASSERT(t_sleeper.vruntime <= (rq.vtime + slice_v));

    /* Lag cannot exceed 1 virtual slice */
    int64_t lag = (int64_t)rq.vtime - (int64_t)t_sleeper.vruntime;
    P2_ASSERT(lag >= -(int64_t)slice_v && lag <= (int64_t)slice_v);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 3: Latency-Sensitive Preemption & Early Deadline Selection           */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_eevfd_latency_preemption(void) {
    struct eevfd_rq rq;
    eevfd_rq_init(&rq);

    struct sched_entity t_batch, t_interactive;
    /* Batch task: default 6ms slice */
    eevfd_entity_init(&t_batch, 0, 6000000ULL);
    /* Interactive task: small 1ms slice for low latency */
    eevfd_entity_init(&t_interactive, 0, 1000000ULL);

    eevfd_place_entity(&rq, &t_batch, true);
    eevfd_place_entity(&rq, &t_interactive, true);

    eevfd_enqueue_entity(&rq, &t_batch);
    eevfd_enqueue_entity(&rq, &t_interactive);

    /* Both have same vruntime (0), but interactive has earlier deadline (1ms vs 6ms) */
    P2_ASSERT(t_interactive.deadline < t_batch.deadline);

    struct sched_entity *first = eevfd_pick_next_entity(&rq);
    P2_ASSERT(first == &t_interactive);

    /* Preemption check */
    bool should_preempt = eevfd_check_preempt(&rq, &t_batch, &t_interactive);
    P2_ASSERT(should_preempt == true);
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Phase 2 Master Runner                                                     */
/* ────────────────────────────────────────────────────────────────────────── */

void eevfd_test_phase2(void) {
    klog_puts("[TEST] EEVFD Phase 2: Core Runqueue, Virtual Time & Selection...\n");
    g_phase2_fail = 0;

    test_eevfd_convergence();
    if (g_phase2_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 2: CPU share convergence failed!\n");
        return;
    }

    test_eevfd_lag_bounding();
    if (g_phase2_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 2: Lag bounding failed!\n");
        return;
    }

    test_eevfd_latency_preemption();
    if (g_phase2_fail) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EEVFD Phase 2: Latency preemption failed!\n");
        return;
    }

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
              " EEVFD Phase 2 Passed: 20,000-slice Convergence & Latency Tests.\n");
}
