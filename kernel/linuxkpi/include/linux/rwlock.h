#ifndef __AVORY_LINUXKPI_RWLOCK_H
#define __AVORY_LINUXKPI_RWLOCK_H

/* Read/write locks on top of native atomics.  See spinlock.h for the general
 * overlay rationale.  Readers do not mask interrupts (Linux semantics); use
 * the irqsave variants when a lock can be taken from interrupt context.
 * Writers mask interrupts while spinning so they cannot be preempted out on a
 * uniprocessor. */

#include <linux/spinlock_types.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/compiler.h>

void linuxkpi_tlb_poll(void);

#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_UNLOCKED(x)

static inline void rwlock_init(rwlock_t *lock) {
  lock->cnt = 0;
  lock->irq_flags = 0;
}

static __always_inline void __kpi_read_acquire(rwlock_t *lock) {
  for (;;) {
    int cnt = __atomic_load_n(&lock->cnt, __ATOMIC_RELAXED);
    if (cnt < 0) {
      linuxkpi_tlb_poll();
      __asm__ volatile("pause" ::: "memory");
      continue;
    }
    if (__atomic_compare_exchange_n(&lock->cnt, &cnt, cnt + 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return;
  }
}

static __always_inline void __kpi_read_release(rwlock_t *lock) {
  __atomic_sub_fetch(&lock->cnt, 1, __ATOMIC_RELEASE);
}

static __always_inline void __kpi_write_acquire(rwlock_t *lock) {
  int expected = 0;
  while (!__atomic_compare_exchange_n(&lock->cnt, &expected, -1, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    expected = 0;
    while (__atomic_load_n(&lock->cnt, __ATOMIC_RELAXED) != 0) {
      linuxkpi_tlb_poll();
      __asm__ volatile("pause" ::: "memory");
    }
  }
}

static __always_inline void __kpi_write_release(rwlock_t *lock) {
  __atomic_store_n(&lock->cnt, 0, __ATOMIC_RELEASE);
}

static inline void read_lock(rwlock_t *lock) {
  preempt_disable();
  __kpi_read_acquire(lock);
}
static inline void read_unlock(rwlock_t *lock) {
  __kpi_read_release(lock);
  preempt_enable();
}
static inline bool read_trylock(rwlock_t *lock) {
  int cnt = __atomic_load_n(&lock->cnt, __ATOMIC_RELAXED);
  if (cnt < 0)
    return false;
  if (!__atomic_compare_exchange_n(&lock->cnt, &cnt, cnt + 1, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return false;
  preempt_disable();
  return true;
}

static inline void write_lock(rwlock_t *lock) {
  unsigned long flags;
  local_irq_save(flags);
  __kpi_write_acquire(lock);
  lock->irq_flags = flags;
}
static inline void write_unlock(rwlock_t *lock) {
  unsigned long flags = lock->irq_flags;
  __kpi_write_release(lock);
  local_irq_restore(flags);
}
static inline bool write_trylock(rwlock_t *lock) {
  unsigned long flags;
  int expected = 0;
  local_irq_save(flags);
  if (!__atomic_compare_exchange_n(&lock->cnt, &expected, -1, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    local_irq_restore(flags);
    return false;
  }
  lock->irq_flags = flags;
  return true;
}

#define read_lock_irqsave(lock, flags)                                        \
  do {                                                                        \
    local_irq_save(flags);                                                    \
    __kpi_read_acquire(lock);                                                 \
  } while (0)
#define read_unlock_irqrestore(lock, flags)                                   \
  do {                                                                        \
    __kpi_read_release(lock);                                                 \
    local_irq_restore(flags);                                                 \
  } while (0)
#define read_lock_irq(lock) read_lock_irqsave(lock, (lock)->irq_flags)
#define read_unlock_irq(lock) read_unlock_irqrestore(lock, (lock)->irq_flags)

#define write_lock_irqsave(lock, flags)                                       \
  do {                                                                        \
    local_irq_save(flags);                                                    \
    __kpi_write_acquire(lock);                                                \
    (lock)->irq_flags = (flags);                                              \
  } while (0)
#define write_unlock_irqrestore(lock, flags)                                  \
  do {                                                                        \
    __kpi_write_release(lock);                                                \
    local_irq_restore(flags);                                                 \
  } while (0)
#define write_lock_irq(lock) write_lock_irqsave(lock, (lock)->irq_flags)
#define write_unlock_irq(lock) write_unlock_irqrestore(lock, (lock)->irq_flags)

static inline void rwlock_destroy(rwlock_t *lock) { (void)lock; }

#endif /* __AVORY_LINUXKPI_RWLOCK_H */
