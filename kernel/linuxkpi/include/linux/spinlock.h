#ifndef __AVORY_LINUXKPI_SPINLOCK_H
#define __AVORY_LINUXKPI_SPINLOCK_H

/* Native-backed Linux <linux/spinlock.h>.
 *
 * Why an overlay: the upstream header reaches asm/processor.h, per-CPU and
 * thread_info machinery AvoryOS does not have yet.  The API here is the one
 * imported code actually uses, backed by GCC atomics.
 *
 * Semantics note: plain spin_lock()/spin_unlock() disable preemption (and keep
 * interrupts enabled), matching Linux.  The native scheduler honors the
 * LinuxKPI preemption counter through the weak linuxkpi_preempt_allowed()
 * hook, so a holder cannot be switched away from.  The irqsave/irq variants
 * additionally mask interrupts and keep exact Linux semantics; they are what
 * code shared with hard IRQ context must use.
 */

#include <linux/spinlock_types.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/lockdep.h>

#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)

/* Native local invalidation service for IRQ-masked lock contention. */
void linuxkpi_tlb_poll(void);

static __always_inline void __kpi_spin_acquire(volatile unsigned int *locked) {
  while (__atomic_exchange_n(locked, 1u, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(locked, __ATOMIC_RELAXED)) {
      linuxkpi_tlb_poll();
      __asm__ volatile("pause" ::: "memory");
    }
  }
}

static __always_inline void __kpi_spin_release(volatile unsigned int *locked) {
  __atomic_store_n(locked, 0u, __ATOMIC_RELEASE);
}

static __always_inline bool
__kpi_spin_tryacquire(volatile unsigned int *locked) {
  unsigned int expected = 0;
  return __atomic_compare_exchange_n(locked, &expected, 1u, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* ── init ───────────────────────────────────────────────────────────────── */

static inline void spin_lock_init(spinlock_t *lock) {
  lock->locked = 0;
  lock->irq_flags = 0;
}

/* ── plain variants: disable preemption, keep interrupts enabled ────────── */

static __always_inline void spin_lock(spinlock_t *lock) {
  preempt_disable();
  __kpi_spin_acquire(&lock->locked);
}

static __always_inline void spin_unlock(spinlock_t *lock) {
  __kpi_spin_release(&lock->locked);
  preempt_enable_resched();
}

static __always_inline bool spin_trylock(spinlock_t *lock) {
  preempt_disable();
  if (!__kpi_spin_tryacquire(&lock->locked)) {
    preempt_enable();
    return false;
  }
  return true;
}

static __always_inline bool spin_is_locked(const spinlock_t *lock) {
  return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

/* ── irq variants ───────────────────────────────────────────────────────── */

static __always_inline void spin_lock_irq(spinlock_t *lock) {
  local_irq_disable();
  preempt_disable();
  __kpi_spin_acquire(&lock->locked);
}

static __always_inline void spin_unlock_irq(spinlock_t *lock) {
  __kpi_spin_release(&lock->locked);
  local_irq_enable();
  preempt_enable_resched();
}

#define spin_lock_irqsave(lock, flags)                                        \
  do {                                                                        \
    local_irq_save(flags);                                                    \
    preempt_disable();                                                        \
    __kpi_spin_acquire(&(lock)->locked);                                      \
  } while (0)

#define spin_unlock_irqrestore(lock, flags)                                   \
  do {                                                                        \
    __kpi_spin_release(&(lock)->locked);                                      \
    local_irq_restore(flags);                                                 \
    preempt_enable_resched();                                                 \
  } while (0)

/* ── bottom-half variants (no softirqs here; plain lock is already safe) ── */

#define spin_lock_bh(lock) spin_lock(lock)
#define spin_unlock_bh(lock) spin_unlock(lock)

/* ── lockdep-ish helpers ────────────────────────────────────────────────── */

#define spin_lock_nested(lock, subclass)                                      \
  do {                                                                        \
    (void)(subclass);                                                         \
    spin_lock(lock);                                                          \
  } while (0)
#define spin_lock_irqsave_nested(lock, flags, subclass)                       \
  do {                                                                        \
    (void)(subclass);                                                         \
    spin_lock_irqsave(lock, flags);                                           \
  } while (0)

/* ── raw spinlock: same implementation on this kernel ───────────────────── */

#define raw_spin_lock(lock) spin_lock((spinlock_t *)(lock))
#define raw_spin_unlock(lock) spin_unlock((spinlock_t *)(lock))
#define raw_spin_lock_init(lock) spin_lock_init((spinlock_t *)(lock))
#define raw_spin_lock_irq(lock) spin_lock_irq((spinlock_t *)(lock))
#define raw_spin_unlock_irq(lock) spin_unlock_irq((spinlock_t *)(lock))
#define raw_spin_lock_irqsave(lock, flags) spin_lock_irqsave((spinlock_t *)(lock), flags)
#define raw_spin_unlock_irqrestore(lock, flags) spin_unlock_irqrestore((spinlock_t *)(lock), flags)
#define raw_spin_trylock(lock) spin_trylock((spinlock_t *)(lock))
#define raw_spin_is_locked(lock) spin_is_locked((spinlock_t *)(lock))

#include <linux/rwlock.h>
#include <linux/seqlock.h>

#endif /* __AVORY_LINUXKPI_SPINLOCK_H */
