#ifndef __AVORY_LINUXKPI_SEQLOCK_H
#define __AVORY_LINUXKPI_SEQLOCK_H

/* Sequence locks on top of the native-backed spinlock.  The sequence is even
 * outside a writer and odd inside; readers re-check it after copying data. */

#include <linux/spinlock_types.h>
#include <linux/spinlock.h>

#define DEFINE_SEQLOCK(x) seqlock_t x = __SEQLOCK_UNLOCKED(x)

static inline void seqlock_init(seqlock_t *sl) {
  spin_lock_init(&sl->lock);
  sl->sequence = 0;
}

static inline unsigned int read_seqbegin(const seqlock_t *sl) {
  unsigned int seq;
  do {
    seq = __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE);
    if (seq & 1)
      __asm__ volatile("pause" ::: "memory");
  } while (seq & 1);
  return seq;
}

static inline bool read_seqretry(const seqlock_t *sl, unsigned int start) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return __atomic_load_n(&sl->sequence, __ATOMIC_RELAXED) != start;
}

static inline void write_seqlock(seqlock_t *sl) {
  spin_lock(&sl->lock);
  unsigned int seq = __atomic_load_n(&sl->sequence, __ATOMIC_RELAXED);
  __atomic_store_n(&sl->sequence, seq + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void write_sequnlock(seqlock_t *sl) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
  unsigned int seq = __atomic_load_n(&sl->sequence, __ATOMIC_RELAXED);
  __atomic_store_n(&sl->sequence, seq + 1, __ATOMIC_RELAXED);
  spin_unlock(&sl->lock);
}

#define write_seqlock_irqsave(sl, flags)                                      \
  do {                                                                        \
    spin_lock_irqsave(&(sl)->lock, flags);                                    \
    unsigned int __seq = __atomic_load_n(&(sl)->sequence, __ATOMIC_RELAXED);  \
    __atomic_store_n(&(sl)->sequence, __seq + 1, __ATOMIC_RELAXED);           \
    __atomic_thread_fence(__ATOMIC_RELEASE);                                  \
  } while (0)
#define write_sequnlock_irqrestore(sl, flags)                                 \
  do {                                                                        \
    __atomic_thread_fence(__ATOMIC_RELEASE);                                  \
    unsigned int __seq = __atomic_load_n(&(sl)->sequence, __ATOMIC_RELAXED);  \
    __atomic_store_n(&(sl)->sequence, __seq + 1, __ATOMIC_RELAXED);           \
    spin_unlock_irqrestore(&(sl)->lock, flags);                               \
  } while (0)

/* Raw seqcount API used by a few structures. */
static inline void write_seqcount_begin(seqcount_t *s) {
  unsigned int seq = __atomic_load_n(&s->sequence, __ATOMIC_RELAXED);
  __atomic_store_n(&s->sequence, seq + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
}
static inline void write_seqcount_end(seqcount_t *s) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
  unsigned int seq = __atomic_load_n(&s->sequence, __ATOMIC_RELAXED);
  __atomic_store_n(&s->sequence, seq + 1, __ATOMIC_RELAXED);
}
static inline unsigned int read_seqcount_begin(const seqcount_t *s) {
  unsigned int seq;
  do {
    seq = __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE);
    if (seq & 1)
      __asm__ volatile("pause" ::: "memory");
  } while (seq & 1);
  return seq;
}
static inline bool read_seqcount_retry(const seqcount_t *s,
                                       unsigned int start) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return __atomic_load_n(&s->sequence, __ATOMIC_RELAXED) != start;
}

#endif /* __AVORY_LINUXKPI_SEQLOCK_H */
