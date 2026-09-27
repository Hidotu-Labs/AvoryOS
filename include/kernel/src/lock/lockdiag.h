#ifndef LOCK_LOCKDIAG_H
#define LOCK_LOCKDIAG_H

#include <stdbool.h>
#include <stdint.h>

/* ===========================================================================
 * lockdiag — hang / deadlock diagnostics.
 *
 * Two independent layers, because they cost very different things:
 *
 *  1. ALWAYS ON (this is what makes a reproduced hang readable):
 *       - a per-CPU heartbeat stamped from the LAPIC timer tick;
 *       - a hung-CPU detector that probes every core with an IPI and reports
 *         the cores that cannot answer;
 *       - a full lock-free report of every CPU, its thread, its ktrack site,
 *         its interrupted RIP and a scan of its kernel stack, printed with
 *         serial_write_sync() so it still works when the lock that hung the
 *         box is serial_lock itself;
 *       - a spotlight on vmm_lock (owner, how long it has been held, how long
 *         the last acquisition took) — the one lock the page fault handler
 *         also needs, so the one lock whose holder can stop the machine;
 *       - a "stuck futex waiter" warning for tasks parked with no wake
 *         pending (a lost wakeup looks exactly like this);
 *       - counters for futex/wait-queue/shootdown traffic, printed with the
 *         report.
 *
 *  2. BEHIND A FLAG (`make LOCKDIAG_OWNER=1`):
 *       - per-CPU shadow stacks of every spinlock each core currently holds,
 *         which is what turns "CPU 2 is spinning" into "CPU 2 is spinning on
 *         serial_lock, which CPU 1 has held since it entered
 *         sched_process_reap_queue".
 *
 * Layer 2 is compiled to nothing by default: it adds a store plus a shadow
 * stack push to every spinlock in the kernel, including the slab allocator's,
 * and that is a real cost on the paths this project is tuned on.  It needs the
 * owning CPU id per acquisition, which means a GS-base read, so it cannot be
 * folded into the generic lock without charging every lock operation for it.
 * =========================================================================== */

#ifndef LOCKDIAG
#define LOCKDIAG 1
#endif

#ifndef LOCKDIAG_OWNER
#define LOCKDIAG_OWNER 0
#endif

/* A core that has not taken a timer tick for this long, while the rest of the
 * machine is ticking, is not merely busy: the tick is the one thing a healthy
 * kernel always gets back to.  Exceptions mask interrupts, so a core parked in
 * a lock spin inside an ISR is exactly this shape. */
#ifndef LOCKDIAG_HUNG_CPU_MS
#define LOCKDIAG_HUNG_CPU_MS 3000ULL
#endif

/* How long to wait for a probed core to acknowledge before calling it stuck.
 * A healthy IPI round trip is single digit microseconds. */
#ifndef LOCKDIAG_PROBE_TIMEOUT_MS
#define LOCKDIAG_PROBE_TIMEOUT_MS 250ULL
#endif

/* A thread parked on a futex longer than this gets named periodically.
 *
 * OFF by default, deliberately.  Measured on a healthy, fully responsive XFCE
 * session: tumblerd parks seven pool workers plus its spawner, xfdesktop parks
 * a pool-spawner, and they sit in FUTEX_WAIT with no timeout for as long as
 * nobody gives them work - which is what an idle worker pool *is*.  Nothing
 * about a long park distinguishes that from a lost wakeup from the kernel's
 * side, so the periodic warning was noise on a healthy machine.
 *
 * Long parks are still listed in the hang report proper (lockdiag_dump_all),
 * where they are read next to the stuck CPUs rather than on their own.  Set
 * this (make LOCKDIAG_STUCK_MS=30000) when you know the desktop is wedged and
 * want the waiters enumerated as they happen. */
#ifndef LOCKDIAG_STUCK_TASK_MS
#define LOCKDIAG_STUCK_TASK_MS 0
#endif

/* Ack waits beyond this are logged: a healthy shootdown is microseconds. */
#ifndef LOCKDIAG_SLOW_ACK_MS
#define LOCKDIAG_SLOW_ACK_MS 50ULL
#endif

/* Acquisition waits beyond this are logged for spotlighted locks. */
#ifndef LOCKDIAG_SLOW_ACQUIRE_MS
#define LOCKDIAG_SLOW_ACQUIRE_MS 10ULL
#endif

/* Locks tracked by name and owner.  Only locks that are worth a named line in
 * the report are here, and each one is instrumented at its own acquire/release
 * wrappers rather than inside spinlock.h. */
typedef enum {
  LOCKDIAG_SPOT_VMM = 0,      /* mm/vmm_map.c: vmm_lock */
  LOCKDIAG_SPOT_GPU_POLL,     /* drivers/gpu/virtio_gpu: gpu_poll_lock */
  LOCKDIAG_SPOT_GPU_PRESENT,  /* drivers/gpu/virtio_gpu: gpu_present_lock */
  LOCKDIAG_SPOT_GPU_CURSOR,   /* drivers/gpu/virtio_gpu: gpu_cursor_lock */
  LOCKDIAG_SPOT_GPU_SHAPE,    /* drivers/gpu/virtio_gpu: gpu_cursor_shape_lock */
  LOCKDIAG_SPOT_COUNT
} lockdiag_spot_t_id;

/* Wrap a lock whose holder's identity is worth naming in a hang report.  The
 * spot is taken after the lock is won (never while spinning) and dropped
 * before it is released. */
#define LOCKDIAG_SPOT_LOCK(spot, lock)                                         \
  do {                                                                         \
    spinlock_acquire(lock);                                                    \
    lockdiag_spot_take((spot), 0, (uint64_t)__builtin_return_address(0));      \
  } while (0)
#define LOCKDIAG_SPOT_UNLOCK(spot, lock)                                       \
  do {                                                                         \
    lockdiag_spot_drop((spot));                                                \
    spinlock_release(lock);                                                    \
  } while (0)

typedef struct {
  const char *name;
  volatile uint64_t owner_cpu; /* valid while depth != 0 */
  volatile uint64_t owner_tid;
  volatile uint64_t taken_tsc;
  volatile uint64_t taken_ip; /* caller that acquired it */
  volatile uint64_t depth;
  volatile uint64_t acquires;
  volatile uint64_t max_wait_cycles;
  volatile uint64_t max_hold_cycles;
  volatile uint64_t slow_waits;
} lockdiag_spot_t;

const char *lockdiag_spot_name(int id);
lockdiag_spot_t *lockdiag_spot(int id);

/* `wait_cycles` is how long the acquisition spin took (0 when the caller does
 * not measure), `ip` the acquiring call site.  These never log and never
 * allocate: they are called from inside the paging engine and from IRQ
 * context. */
void lockdiag_spot_take(int id, uint64_t wait_cycles, uint64_t ip);
void lockdiag_spot_drop(int id);

/* True when the calling CPU currently holds the spotlighted lock. */
bool lockdiag_spot_held_by_self(int id);

/* Convenience for the shootdown path: true when this core is inside vmm_lock
 * right now, i.e. it is about to wait for acknowledgements from cores that
 * cannot answer while they fault on that same lock. */
bool lockdiag_self_holds_vmm_lock(uint32_t self_id);

/* --- wait-queue holder tracking -----------------------------------------
 * Wait queues are dynamic objects, so the fixed spotlight table cannot name
 * them.  These record, per queue address, which thread is inside one of the
 * wait_queue_* critical sections and where it entered, so a core spinning on a
 * queue's lock can be told who it is waiting for (including "that thread is a
 * zombie that will never release it"). */
void lockdiag_wq_take(const void *wq, uint64_t ip);
void lockdiag_wq_drop(const void *wq);

#define LOCKDIAG_WQ_LOCK(wq)                                                   \
  do {                                                                         \
    spinlock_acquire(&(wq)->lock);                                             \
    lockdiag_wq_take((const void *)(wq), (uint64_t)__builtin_return_address(0));\
  } while (0)
#define LOCKDIAG_WQ_UNLOCK(wq)                                                 \
  do {                                                                         \
    lockdiag_wq_drop((const void *)(wq));                                      \
    spinlock_release(&(wq)->lock);                                             \
  } while (0)

/* --- contended-acquire tracking -----------------------------------------
 * A core found spinning with interrupts masked gives no clue about *which*
 * lock it is waiting for.  These record the lock address, the call site that
 * asked for it and when the wait started, in a per-CPU slot written once per
 * contended acquire (never per spin iteration).  A core that dies spinning
 * leaves its slot set, so the report can say exactly what it was waiting on. */
void lockdiag_spin_begin(const void *lock, uint64_t ip);
void lockdiag_spin_end(const void *lock);

#define LOCKDIAG_SPIN_BEGIN(lock)                                              \
  lockdiag_spin_begin((const void *)(lock),                                    \
                      (uint64_t)__builtin_return_address(0))
#define LOCKDIAG_SPIN_END(lock) lockdiag_spin_end((const void *)(lock))

/* --- detector ----------------------------------------------------------- */

void lockdiag_init(void);

/* Called from the LAPIC timer tick on every CPU. */
void lockdiag_tick(void);

/* Full all-CPU report.  Safe from IRQ and panic context: takes no locks and
 * writes only through serial_write_sync().  `reason` names the trigger. */
void lockdiag_dump_all(const char *reason);

/* --- on-demand report trigger -------------------------------------------
 * Right-Ctrl three times within LOCKDIAG_TRIGGER_WINDOW_MS renders a full
 * report.  Needed for the case the detector cannot see: a userspace deadlock,
 * where every core is healthy and only the desktop is frozen.
 *
 * Deliberately not Ctrl+Alt+F-key: the host's console eats that as a VT switch.
 * KEY_RIGHTCTRL is the Linux input code 97; a bare modifier bound by nothing. */
#define LOCKDIAG_TRIGGER_KEY 97
#define LOCKDIAG_TRIGGER_TAPS 3
#define LOCKDIAG_TRIGGER_WINDOW_MS 1500
#define LOCKDIAG_TRIGGER_DEBOUNCE_MS 60
#define LOCKDIAG_TRIGGER_COOLDOWN_MS 5000

/* Feed keyboard events here (drivers/input/evdev.c). */
void lockdiag_keyboard_event(uint16_t code, bool pressed);

/* Same trigger from the scancode path (drivers/input/keyboard.c), which runs
 * even when no evdev device exists yet. */
void lockdiag_keyboard_scancode(uint8_t scancode, bool extended, bool release);

/* Ask every other core to acknowledge; returns a bitmask of cores that did.
 * Used by the shootdown timeout path so it can name the core at fault. */
uint64_t lockdiag_probe_cpus(uint32_t timeout_ms);

/* --- counters ----------------------------------------------------------- */
typedef struct {
  volatile uint64_t futex_waits;
  volatile uint64_t futex_wakes;
  volatile uint64_t futex_wake_nomatch; /* wake that found an empty bucket */
  volatile uint64_t futex_timeouts;
  volatile uint64_t futex_eagain;
  volatile uint64_t futex_zero_timeout; /* FUTEX_WAIT with a {0,0} timeout */
  volatile uint64_t wq_adds;
  volatile uint64_t wq_wakes;
  volatile uint64_t shootdowns;
  volatile uint64_t shootdown_slow_acks;
  volatile uint64_t shootdown_max_ack_ns;
  volatile uint64_t shootdown_under_vmm_lock;
  volatile uint64_t hung_cpu_reports;
  volatile uint64_t stuck_task_reports;
} lockdiag_stats_t;

lockdiag_stats_t *lockdiag_stats(void);

/* Use these, not lockdiag_stats() directly: with LOCKDIAG compiled out there is
 * no stats object, and lockdiag_stat_add(&lockdiag_stats()->field, 1) would
 * happily write through a null pointer. */
#if LOCKDIAG
#define LOCKDIAG_STAT(field, n) lockdiag_stat_add(&lockdiag_stats()->field, (n))
#define LOCKDIAG_STAT_MAX(field, v)                                            \
  lockdiag_stat_max(&lockdiag_stats()->field, (v))
#else
#define LOCKDIAG_STAT(field, n) ((void)0)
#define LOCKDIAG_STAT_MAX(field, v) ((void)0)
#endif

static inline void lockdiag_stat_add(volatile uint64_t *field, uint64_t n) {
  __atomic_add_fetch(field, n, __ATOMIC_RELAXED);
}

static inline void lockdiag_stat_max(volatile uint64_t *field, uint64_t v) {
  uint64_t cur = __atomic_load_n(field, __ATOMIC_RELAXED);
  while (v > cur && !__atomic_compare_exchange_n(field, &cur, v, false,
                                                 __ATOMIC_RELAXED,
                                                 __ATOMIC_RELAXED)) {
    /* cur reloaded by the CAS */
  }
}

/* --- owner shadow stacks (LOCKDIAG_OWNER only) -------------------------- */

#if LOCKDIAG && LOCKDIAG_OWNER
void lockdiag_hold_note(const void *lock, uint64_t ip);
void lockdiag_hold_drop(const void *lock);
#define LOCKDIAG_HOLD(lock)                                                    \
  lockdiag_hold_note((const void *)(lock),                                     \
                     (uint64_t)__builtin_return_address(0))
#define LOCKDIAG_UNHOLD(lock) lockdiag_hold_drop((const void *)(lock))
#else
#define LOCKDIAG_HOLD(lock) ((void)0)
#define LOCKDIAG_UNHOLD(lock) ((void)0)
#endif

#endif /* LOCK_LOCKDIAG_H */
