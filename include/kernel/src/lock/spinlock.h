#ifndef LOCK_SPINLOCK_H
#define LOCK_SPINLOCK_H

#include "hal/hal.h"
#include "lockdiag.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Test-and-test-and-set spinlock with PAUSE backoff.
//
// Why this is NOT a ticket lock
// -----------------------------
// A waiter must never own a position in a queue, because a waiter can be
// interrupted by something that needs the same lock:
//
//   * an IRQ handler on the same CPU (the LAPIC tick takes serial_lock through
//     serial_flush(), and timerfd_tick() takes wait-queue locks), and
//   * an exception handler on the same CPU (a #PF reaches vmm_lock through the
//     paging engine, and exceptions are not maskable by IF at all).
//
// With FIFO tickets the interrupting context claims the *next* ticket and then
// spins - behind the very task it just interrupted.  That task cannot run to
// release its ticket until the interrupting context returns, so both spin
// forever.  With test-and-set the interrupting context takes the lock the
// instant it is free, finishes, and returns, and the interrupted waiter then
// proceeds normally.
//
// Waiting with interrupts ENABLED (we only mask once the lock is ours) is
// deliberate as well: a CPU that is blocked on a lock must still be able to
// answer a TLB shootdown IPI, otherwise a shootdown initiator waits for an
// acknowledgement that can never be sent.  See the note in
// mm/tlb_shootdown.c.  A caller that already masked interrupts keeps them
// masked (the flags are saved, not forced).
//
// Trade-off: there is no FIFO fairness, so a heavily contended lock can keep a
// waiter waiting.  That is the same trade Linux's spin_lock() makes, and it
// beats a guaranteed deadlock.  A holder always runs with interrupts disabled,
// so a holder cannot be preempted or interrupted while it owns the lock.
// ---------------------------------------------------------------------------
typedef struct {
  volatile uint32_t locked;
  hal_irq_state_t saved_flags;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

static inline void spinlock_init(spinlock_t *lock) {
  __atomic_store_n(&lock->locked, 0, __ATOMIC_RELAXED);
  lock->saved_flags = 0;
}

/* The RFLAGS interrupt-enable bit, for deciding whether a waiter may open an
 * interrupt window while it spins.  (hal.h exposes no predicate for a saved
 * state, and re-reading RFLAGS would race the save itself.) */
#define SPINLOCK_RFLAGS_IF (1ULL << 9)

/* Acquire with interrupts masked from before the atomic that wins the lock.
 *
 * This ordering is the whole point: winning the lock with IF=1 and only then
 * calling hal_irq_save() leaves a window of a few instructions in which an
 * interrupt can arrive.  The handler enters through an interrupt gate with
 * IF=0, so if it takes the same lock - and locks shared with IRQ handlers are
 * exactly the ones that matter, e.g. serial_lock or a wait queue woken from a
 * device IRQ - it spins forever: the interrupted context owns the lock and
 * cannot run again to release it.  That is a self-deadlock that no lock owner
 * tracking can see coming, and it is timing-dependent, so it looks like a
 * random freeze.  Masking first closes the window entirely.
 *
 * Interrupts are still *opened while waiting* (when the caller had them
 * enabled), which is what keeps a spinning core able to answer a TLB shootdown
 * IPI - the property the original design wanted.  They are never enabled once
 * the lock is held. */
static inline void spinlock_acquire(spinlock_t *lock) {
  hal_irq_state_t flags = hal_irq_save();
  bool may_open = (flags & SPINLOCK_RFLAGS_IF) != 0;

  if (__builtin_expect(__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE),
                       0)) {
    LOCKDIAG_SPIN_BEGIN(lock);
    // Test-and-test-and-set: poll this CPU's copy of the line instead of
    // re-issuing a locked RMW, which would bounce the cacheline between cores.
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
      if (may_open) {
        hal_irq_enable();
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
          __asm__ volatile("pause" ::: "memory");
        hal_irq_disable();
      } else {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
          __asm__ volatile("pause" ::: "memory");
      }
    }
    LOCKDIAG_SPIN_END(lock);
  }

  lock->saved_flags = flags;
  LOCKDIAG_HOLD(lock);
}

static inline void spinlock_release(spinlock_t *lock) {
  hal_irq_state_t flags = lock->saved_flags;
  LOCKDIAG_UNHOLD(lock);
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
  hal_irq_restore(flags);
}

// Same lock, but the caller keeps the previous interrupt state in its own
// variable instead of in the lock.  Do not mix these with the plain
// spinlock_release(): restoring flags that a different acquisition saved would
// re-enable interrupts inside a live critical section.
static inline void spinlock_acquire_save(spinlock_t *lock, uint64_t *flags) {
  /* Same mask-first ordering as spinlock_acquire; see the comment there. */
  hal_irq_state_t saved = hal_irq_save();
  bool may_open = (saved & SPINLOCK_RFLAGS_IF) != 0;

  if (__builtin_expect(__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE),
                       0)) {
    LOCKDIAG_SPIN_BEGIN(lock);
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
      if (may_open) {
        hal_irq_enable();
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
          __asm__ volatile("pause" ::: "memory");
        hal_irq_disable();
      } else {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
          __asm__ volatile("pause" ::: "memory");
      }
    }
    LOCKDIAG_SPIN_END(lock);
  }
  *flags = saved;
  LOCKDIAG_HOLD(lock);
}

static inline void spinlock_release_restore(spinlock_t *lock, uint64_t flags) {
  LOCKDIAG_UNHOLD(lock);
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
  hal_irq_restore((hal_irq_state_t)flags);
}

// Unlike a ticket lock, where this fails whenever anybody at all is queued,
// this only fails when the lock is genuinely held.
static inline bool spinlock_try_acquire(spinlock_t *lock) {
  hal_irq_state_t flags = hal_irq_save();

  if (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
    hal_irq_restore(flags);
    return false;
  }

  lock->saved_flags = flags;
  LOCKDIAG_HOLD(lock);
  return true;
}

static inline bool spinlock_is_locked(spinlock_t *lock) {
  return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

// ---------------------------------------------------------------------------
// rawspinlock_t — the same lock WITHOUT masking interrupts, held or waited on.
//
// Use this for locks that:
//   1. Can be held while another CPU sends TLB shootdown IPIs (e.g. vmm_lock)
//   2. Are never acquired from an IRQ handler
//   3. Can be held for longer durations (page table walks, etc.)
//
// Waiters stay interruptible so they can ack an IPI, and a holder never runs
// with IF=0, so it can ack one too.
// ---------------------------------------------------------------------------
typedef struct {
  volatile uint32_t locked;
} rawspinlock_t;

#define RAWSPINLOCK_INIT { .locked = 0 }

static inline void rawspinlock_init(rawspinlock_t *lock) {
  __atomic_store_n(&lock->locked, 0, __ATOMIC_RELAXED);
}

static inline void rawspinlock_acquire(rawspinlock_t *lock) {
  if (__builtin_expect(__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE),
                       0)) {
    LOCKDIAG_SPIN_BEGIN(lock);
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
        __asm__ volatile("pause" ::: "memory");
    }
    LOCKDIAG_SPIN_END(lock);
  }
  LOCKDIAG_HOLD(lock);
}

static inline void rawspinlock_release(rawspinlock_t *lock) {
  LOCKDIAG_UNHOLD(lock);
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

static inline bool rawspinlock_try_acquire(rawspinlock_t *lock) {
  if (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE))
    return false;
  LOCKDIAG_HOLD(lock);
  return true;
}

static inline bool rawspinlock_is_locked(rawspinlock_t *lock) {
  return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

#endif
