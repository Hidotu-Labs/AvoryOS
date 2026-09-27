#ifndef SCHED_EEVFD_MATH_H
#define SCHED_EEVFD_MATH_H

#include <stdint.h>
#include <stdbool.h>

#define EEVFD_NICE_0_WEIGHT    1024U
#define EEVFD_NICE_0_WMULT     4194304U /* 2^22 */

#define EEVFD_BASE_SLICE_NS          3000000ULL  /* 3.0 ms default scheduling slice */
#define EEVFD_MIN_SLICE_NS            500000ULL  /* 0.5 ms minimum slice */
#define EEVFD_MAX_SLICE_NS          20000000ULL  /* 20 ms maximum slice */
#define EEVFD_INTERACTIVE_SLICE_NS   1000000ULL  /* 1.0 ms for interactive/low-latency tasks */
#define EEVFD_LATENCY_SLICE_NS        500000ULL  /* 0.5 ms for ultra-low latency */
#define EEVFD_BATCH_SLICE_NS         6000000ULL  /* 6.0 ms for background/batch tasks */
#define EEVFD_MIN_GRANULARITY_NS      500000ULL  /* 0.5 ms standard minimum execution floor */
#define EEVFD_INTERACTIVE_MIN_GRANULARITY_NS 100000ULL /* 0.1 ms execution floor for interactive preemption */
#define EEVFD_WAKEUP_GRANULARITY_NS   500000ULL  /* 0.5 ms wakeup preemption threshold */

/**
 * Calculates dynamic physical slice duration based on nice value.
 * Negative nice (interactive) gets shorter slice (1.0ms - 2.8ms) -> earlier deadlines -> scheduled sooner.
 * Nice 0 (default) gets 3.0ms base slice -> good cache locality.
 * Positive nice (batch) gets longer slice (3.3ms - 6.0ms) -> higher throughput, fewer context switches.
 */
static inline uint64_t eevfd_calc_slice_for_nice(int nice) {
    if (nice <= -10)
        return EEVFD_INTERACTIVE_SLICE_NS;
    if (nice < 0) {
        return EEVFD_INTERACTIVE_SLICE_NS + (uint64_t)(nice + 10) * 200000ULL;
    }
    if (nice == 0)
        return EEVFD_BASE_SLICE_NS;
    if (nice >= 10)
        return EEVFD_BATCH_SLICE_NS;
    return EEVFD_BASE_SLICE_NS + (uint64_t)nice * 300000ULL;
}

/* Nice range is [-20, 19], mapped to index [0, 39] */
#define NICE_TO_INDEX(nice) ((int)((nice) + 20))

static const uint32_t prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

static const uint32_t prio_to_wmult[40] = {
 /* -20 */      48388,      59856,      76040,      92818,     118348,
 /* -15 */     147320,     184698,     229616,     287308,     360437,
 /* -10 */     449829,     563644,     704093,     875809,    1099582,
 /*  -5 */    1376151,    1717300,    2157191,    2708050,    3363326,
 /*   0 */    4194304,    5237765,    6557202,    8165337,   10153587,
 /*   5 */   12820798,   15790321,   19976592,   24970740,   31350126,
 /*  10 */   39045157,   49367440,   61356676,   76695845,   95443718,
 /*  15 */  119304647,  148102321,  186737709,  238609294,  286331153,
};

static inline uint32_t nice_to_weight(int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;
    return prio_to_weight[NICE_TO_INDEX(nice)];
}

static inline uint32_t nice_to_wmult(int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;
    return prio_to_wmult[NICE_TO_INDEX(nice)];
}

/**
 * Calculates virtual runtime advance for a task that ran for delta_exec_ns.
 * delta_vruntime = (delta_exec_ns * 1024) / weight = (delta_exec_ns * wmult) >> 22
 */
static inline uint64_t calc_delta_vruntime(uint64_t delta_exec_ns, uint32_t weight, uint32_t wmult) {
    if (weight == EEVFD_NICE_0_WEIGHT)
        return delta_exec_ns;

    /* Avoid 64-bit multiplication overflow for huge delta_exec_ns */
    if (__builtin_expect(delta_exec_ns <= (UINT64_MAX >> 29), 1)) {
        return (delta_exec_ns * (uint64_t)wmult) >> 22;
    } else {
        return ((delta_exec_ns >> 10) * (uint64_t)wmult) >> 12;
    }
}

/**
 * Calculates system virtual time advance:
 * delta_vtime = (delta_exec_ns * 1024) / total_weight
 */
static inline uint64_t calc_delta_sys_vtime(uint64_t delta_exec_ns, uint64_t total_weight) {
    if (total_weight == 0)
        return 0;
    if (total_weight == EEVFD_NICE_0_WEIGHT)
        return delta_exec_ns;

    /* Scaled multiplication with round-to-nearest */
    uint64_t scaled = delta_exec_ns * 1024ULL;
    return (scaled + (total_weight >> 1)) / total_weight;
}

/**
 * Calculates virtual duration of a given physical slice.
 */
static inline uint64_t calc_slice_vruntime(uint64_t slice_ns, uint32_t weight, uint32_t wmult) {
    return calc_delta_vruntime(slice_ns, weight, wmult);
}

/**
 * Calculates the virtual deadline for an entity:
 * deadline = vruntime + calc_slice_vruntime(slice_ns, weight, wmult)
 */
static inline uint64_t calc_deadline(uint64_t vruntime, uint64_t slice_ns, uint32_t weight, uint32_t wmult) {
    return vruntime + calc_slice_vruntime(slice_ns, weight, wmult);
}

#endif /* SCHED_EEVFD_MATH_H */
