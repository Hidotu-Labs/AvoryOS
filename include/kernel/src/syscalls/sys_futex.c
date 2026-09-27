// Futex Syscall (202)
// Implements FUTEX_WAIT and FUTEX_WAKE using a hash table. Shared futexes use
// physical addresses; private futexes use (mm, virtual address) identities.
//
// Linux futex(2) signature:
//   long futex(uint32_t *uaddr, int futex_op, uint32_t val,
//              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)

#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../lock/lockdiag.h"
#include "../lock/spinlock.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Futex operation constants (Linux ABI)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_PRIVATE 128 // FUTEX_WAIT | FUTEX_PRIVATE_FLAG
#define FUTEX_WAKE_PRIVATE 129 // FUTEX_WAKE | FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_WAKE_OP      5
#define FUTEX_LOCK_PI      6
#define FUTEX_UNLOCK_PI    7
#define FUTEX_TRYLOCK_PI   8
#define FUTEX_WAIT_BITSET  9
#define FUTEX_WAKE_BITSET  10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2     13
#define FUTEX_CMD_MASK     127
#define FUTEX_BITSET_MATCH_ANY 0xffffffff

// PI futex word layout (futex(2) policy).  Bits 0-29 hold the owner TID.
#define FUTEX_TID_MASK     0x3fffffff
#define FUTEX_OWNER_DIED   0x40000000
#define FUTEX_WAITERS      0x80000000

// FUTEX_WAKE_OP encoded field widths (val3 argument)
#define FUTEX_OP_OP_SHIFT    28
#define FUTEX_OP_CMP_SHIFT   24
#define FUTEX_OP_OPARG_SHIFT 12
#define FUTEX_OP_OP_MASK     0xf
#define FUTEX_OP_CMP_MASK    0xf
#define FUTEX_OP_OPARG_MASK  0xfff
#define FUTEX_OP_CMPARG_MASK 0xfff

// Encoded op codes (FUTEX_OP_*)
#define FUTEX_OP_SET        0  // *uaddr2 = oparg
#define FUTEX_OP_ADD        1  // *uaddr2 += oparg
#define FUTEX_OP_OR         2  // *uaddr2 |= oparg
#define FUTEX_OP_ANDN       3  // *uaddr2 &= ~oparg
#define FUTEX_OP_XOR        4  // *uaddr2 ^= oparg
#define FUTEX_OP_ARG_SHIFT  8  // oparg = 1 << oparg (bit in op field)

// Encoded cmp codes (FUTEX_OP_CMP_*)
#define FUTEX_OP_CMP_EQ     0
#define FUTEX_OP_CMP_NE     1
#define FUTEX_OP_CMP_LT     2
#define FUTEX_OP_CMP_LE     3
#define FUTEX_OP_CMP_GT     4
#define FUTEX_OP_CMP_GE     5

// Error codes
#define EPERM 1
#define EINTR 4
#define EAGAIN 11
#define EFAULT 14
#define EINVAL 22
#define EDEADLK 35
#define ETIMEDOUT 110

// Futex hash table
// Each bucket is an intrusive linked list of waiters, protected by its own
// spinlock.  We key on the physical address so that two processes mapping the
// same physical page see the same bucket.

#define FUTEX_HASH_BITS 8
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS) // 256 buckets

struct futex_key {
  // Zero selects a shared, physical-address key. Private futexes use the
  // process mm pointer and never need a guest page-table walk.
  uint64_t space;
  uint64_t address;
};

struct futex_waiter {
  struct futex_key key;
  struct thread *thread; // Blocked thread
  uint32_t bitset;
  bool pi;               // Waiting in FUTEX_LOCK_PI, not FUTEX_WAIT
  bool acquired;         // FUTEX_UNLOCK_PI handed the PI lock over
  struct futex_waiter *next;
};

static struct {
  spinlock_t lock;
  struct futex_waiter *head;
} futex_hash[FUTEX_HASH_SIZE];

/* Per-bucket wake traffic, so the hang report can tell an idle worker pool
 * apart from a lost wakeup: a parked thread whose bucket has never seen a wake
 * attempt is waiting for work; one whose bucket saw a wake that matched
 * nothing is the bug.  futex_bucket_stats() below reads these. */
static volatile uint64_t futex_bucket_wakes[FUTEX_HASH_SIZE];
static volatile uint64_t futex_bucket_nomatch[FUTEX_HASH_SIZE];

static void futex_bucket_note(uint32_t bucket, bool woke_anyone) {
  if (bucket >= FUTEX_HASH_SIZE)
    return;
  __atomic_add_fetch(&futex_bucket_wakes[bucket], 1, __ATOMIC_RELAXED);
  if (!woke_anyone)
    __atomic_add_fetch(&futex_bucket_nomatch[bucket], 1, __ATOMIC_RELAXED);
}

static int futex_initialized = 0;

static void futex_init_once(void) {
  if (futex_initialized)
    return;
  for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
    spinlock_init(&futex_hash[i].lock);
    futex_hash[i].head = NULL;
  }
  futex_initialized = 1;
}

static inline uint32_t futex_hash_key(struct futex_key key) {
  uint64_t h = key.address >> 2;
  h ^= key.space + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return (uint32_t)(h & (FUTEX_HASH_SIZE - 1));
}

static inline bool futex_key_equal(struct futex_key a, struct futex_key b) {
  return a.space == b.space && a.address == b.address;
}

// Remove every waiter owned by a thread that is about to be reaped. Futex
// waiters live in futex_wait() stack frames; forced exit_group teardown does
// not return through that function, so entries must be detached before the
// kernel stack is released.
void futex_remove_thread_waiters(struct thread *thread) {
  if (!thread || !futex_initialized)
    return;

  for (uint32_t bucket = 0; bucket < FUTEX_HASH_SIZE; bucket++) {
    spinlock_acquire(&futex_hash[bucket].lock);
    struct futex_waiter **pp = &futex_hash[bucket].head;
    while (*pp) {
      struct futex_waiter *waiter = *pp;
      if (waiter->thread == thread) {
        *pp = waiter->next;
      } else {
        pp = &waiter->next;
      }
    }
    spinlock_release(&futex_hash[bucket].lock);
  }
}

/* Build a futex identity. Process-private futexes are only meaningful within
 * one mm, so (mm, uaddr) is sufficient and avoids vmm_virt_to_phys() on every
 * Mesa/LLVM worker wait and wake. Shared futexes retain physical identities. */
static inline int futex_get_key(uint32_t *uaddr, bool private,
                                struct futex_key *key) {
  struct thread *t = sched_get_current();
  if (__builtin_expect(!t || !key, 0))
    return -EFAULT;

  uint64_t vaddr = (uint64_t)uaddr;
  if (__builtin_expect((vaddr & (sizeof(uint32_t) - 1)) != 0, 0))
    return -EINVAL;
  if (__builtin_expect(!vmm_is_user_addr_range_valid(vaddr, sizeof(uint32_t)), 0))
    return -EFAULT;

  if (__builtin_expect(private, 1)) {
    if (__builtin_expect(!t->mm, 0))
      return -EFAULT;
    key->space = (uint64_t)t->mm;
    key->address = vaddr;
    return 0;
  }

  if (!t->cr3)
    return -EFAULT;
  uint64_t phys = vmm_virt_to_phys((uint64_t *)t->cr3, vaddr);
  if (!phys)
    return -EFAULT;
  key->space = 0;
  key->address = phys;
  return 0;
}

// FUTEX_WAIT
// Atomically check that *uaddr == val, then block the calling thread.
// If a timeout is specified, the thread will be woken after the timeout.
// Returns 0 on success (woken by FUTEX_WAKE).
// Returns -EAGAIN if *uaddr != val at time of check.
// Returns -ETIMEDOUT if timeout expired.
static uint64_t futex_wait(uint32_t *uaddr, uint32_t val,
                           const uint64_t *timeout_ts, bool private,
                           bool is_abs, uint32_t bitset) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  uint32_t bucket = futex_hash_key(key);

  // Allocate waiter on the kernel stack — it's safe because we block in this
  // function and only return after being woken (the stack frame stays valid).
  struct futex_waiter waiter;
  waiter.key = key;
  waiter.thread = sched_get_current();
  waiter.bitset = bitset;
  waiter.pi = false;
  waiter.acquired = false;
  waiter.next = NULL;

  if (!waiter.thread)
    return (uint64_t)(-(int64_t)EFAULT);

  // Critical section: check value + enqueue + block
  spinlock_acquire(&futex_hash[bucket].lock);

  // Re-read the user value while holding the lock to prevent races with
  // FUTEX_WAKE.  If *uaddr changed since the caller read it we must not
  // block (Linux returns -EAGAIN).
  uint32_t current_val = __atomic_load_n(uaddr, __ATOMIC_RELAXED);
  if (current_val != val) {
    spinlock_release(&futex_hash[bucket].lock);
    LOCKDIAG_STAT(futex_eagain, 1);
    return (uint64_t)(-(int64_t)EAGAIN);
  }

  /* FUTEX_WAIT with a {0,0} relative timeout is a non-blocking value probe on
   * Linux: the value still matches, so report the timeout instead of parking.
   * Before this, {0,0} fell through every branch below (timeout_ms stays 0)
   * and the thread parked forever with no wakeup to blame. */
  if (timeout_ts && !is_abs && timeout_ts[0] == 0 && timeout_ts[1] == 0) {
    spinlock_release(&futex_hash[bucket].lock);
    LOCKDIAG_STAT(futex_zero_timeout, 1);
    LOCKDIAG_STAT(futex_timeouts, 1);
    return (uint64_t)(-(int64_t)ETIMEDOUT);
  }

  // Enqueue the waiter
  waiter.next = futex_hash[bucket].head;
  futex_hash[bucket].head = &waiter;

  // Set up the thread for blocking
  waiter.thread->state = THREAD_BLOCKED;

  // If a timeout was specified, compute deadline in LAPIC ticks
  if (timeout_ts) {
    uint64_t sec = timeout_ts[0];
    uint64_t nsec = timeout_ts[1];
    uint64_t target_ms = sec * 1000 + nsec / 1000000;
    uint64_t now_ms = lapic_timer_get_ticks();
    uint64_t timeout_ms = 0;
    if (is_abs) {
      if (target_ms > now_ms)
        timeout_ms = target_ms - now_ms;
      else
        timeout_ms = 0;
    } else {
      timeout_ms = target_ms;
    }

    if (timeout_ms == 0 && nsec > 0 && !is_abs)
      timeout_ms = 1; // Minimum 1ms granularity

    if (timeout_ms > 0) {
      waiter.thread->wakeup_ticks = now_ms + timeout_ms;
    } else if (is_abs && target_ms <= now_ms) {
      // Immediate timeout!
      waiter.thread->state = THREAD_RUNNING;
      futex_hash[bucket].head = waiter.next;
      spinlock_release(&futex_hash[bucket].lock);
      LOCKDIAG_STAT(futex_timeouts, 1);
      return (uint64_t)(-(int64_t)ETIMEDOUT);
    }
  }

  /* Publish the park for the hang report before dropping the bucket lock: a
   * lost wakeup is only distinguishable from an idle worker pool if the report
   * can see which bucket this thread parked on and the wake traffic against
   * it.  Cleared again the moment the thread runs (below). */
  waiter.thread->in_futex_wait = true;
  waiter.thread->blocked_since_ms = lapic_timer_get_ticks();
  waiter.thread->futex_bucket = bucket;
  waiter.thread->blocked_reported = false;
  LOCKDIAG_STAT(futex_waits, 1);

  spinlock_release(&futex_hash[bucket].lock);

  // Yield the CPU — we'll be rescheduled when woken by FUTEX_WAKE or timeout
  if (waiter.thread->state == THREAD_BLOCKED)
    sched_yield();

  /* Running again: this park is over either way. */
  waiter.thread->in_futex_wait = false;
  waiter.thread->blocked_since_ms = 0;

  // We're back!  Remove ourselves from the hash bucket
  // The waiter might have been requeued to a different bucket.
  uint32_t final_bucket = futex_hash_key(waiter.key);
  spinlock_acquire(&futex_hash[final_bucket].lock);

  bool was_woken = true;
  // Remove waiter from the list (may already have been removed by wake)
  struct futex_waiter **pp = &futex_hash[final_bucket].head;
  while (*pp) {
    if (*pp == &waiter) {
      *pp = waiter.next;
      was_woken = false; // We were still in the list, so we were NOT woken by FUTEX_WAKE!
      break;
    }
    pp = &(*pp)->next;
  }

  spinlock_release(&futex_hash[final_bucket].lock);

  if (thread_has_pending_signal(waiter.thread) && !was_woken) {
    return (uint64_t)(-(int64_t)EINTR);
  }

  // Check if we timed out: if a timeout was set, and we removed ourselves from the
  // list (meaning futex_wake didn't wake us up), return -ETIMEDOUT.
  if (timeout_ts && !was_woken) {
    LOCKDIAG_STAT(futex_timeouts, 1);
    return (uint64_t)(-(int64_t)ETIMEDOUT);
  }

  return 0;
}

static inline uint64_t futex_wake_key(struct futex_key key, uint32_t val, uint32_t bitset) {
  uint32_t bucket = futex_hash_key(key);

  // Fast path: if no waiters exist in this hash bucket, skip spinlock acquisition
  if (__builtin_expect(!futex_hash[bucket].head, 1)) {
    futex_bucket_note(bucket, false);
    return 0;
  }

  uint32_t woken = 0;
  spinlock_acquire(&futex_hash[bucket].lock);
  struct futex_waiter **pp = &futex_hash[bucket].head;
  while (*pp && woken < val) {
    struct futex_waiter *w = *pp;
    if (futex_key_equal(w->key, key)) {
      if ((w->bitset & bitset) == 0) {
        pp = &w->next;
        continue;
      }
      if (w->thread && w->thread->state == THREAD_BLOCKED) {
        sched_wakeup(w->thread);
        woken++;
        LOCKDIAG_STAT(futex_wakes, 1);
      }
      *pp = w->next;
    } else {
      pp = &w->next;
    }
  }
  spinlock_release(&futex_hash[bucket].lock);
  futex_bucket_note(bucket, woken != 0);
  if (!woken)
    LOCKDIAG_STAT(futex_wake_nomatch, 1);
  return (uint64_t)woken;
}

// FUTEX_WAKE
// Wake at most `val` threads waiting on the futex at *uaddr.
// Returns the number of threads woken.
static uint64_t futex_wake(uint32_t *uaddr, uint32_t val, bool private) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  return futex_wake_key(key, val, FUTEX_BITSET_MATCH_ANY);
}

static uint64_t futex_wake_bitset(uint32_t *uaddr, uint32_t val, uint32_t bitset, bool private) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  return futex_wake_key(key, val, bitset);
}

// FUTEX_REQUEUE
// Wake at most `val` threads waiting on uaddr1, and move at most `val2`
// remaining threads to wait on uaddr2 instead.
static uint64_t futex_requeue(uint32_t *uaddr1, uint32_t val, uint32_t val2,
                              uint32_t *uaddr2, bool private) {
  struct futex_key key1, key2;
  int error = futex_get_key(uaddr1, private, &key1);
  if (error)
    return (uint64_t)(int64_t)error;
  error = futex_get_key(uaddr2, private, &key2);
  if (error)
    return (uint64_t)(int64_t)error;

  if (futex_key_equal(key1, key2))
    return futex_wake_key(key1, val, FUTEX_BITSET_MATCH_ANY);

  uint32_t bucket1 = futex_hash_key(key1);
  uint32_t bucket2 = futex_hash_key(key2);

  uint32_t total_woken = 0;
  uint32_t total_requeued = 0;

  // Always acquire locks in bucket order to avoid deadlocks
  if (bucket1 == bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
  } else if (bucket1 < bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
    spinlock_acquire(&futex_hash[bucket2].lock);
  } else {
    spinlock_acquire(&futex_hash[bucket2].lock);
    spinlock_acquire(&futex_hash[bucket1].lock);
  }

  struct futex_waiter **pp = &futex_hash[bucket1].head;
  while (*pp) {
    struct futex_waiter *w = *pp;
    if (futex_key_equal(w->key, key1)) {
      if (total_woken < val) {
        // Wake this thread
        if (w->thread && w->thread->state == THREAD_BLOCKED) {
          sched_wakeup(w->thread);
          total_woken++;
        }
        // Remove from bucket1
        *pp = w->next;
      } else if (total_requeued < val2) {
        // Requeue: move to bucket2
        *pp = w->next; // Remove from bucket1
        w->key = key2;
        w->next = futex_hash[bucket2].head;
        futex_hash[bucket2].head = w;
        if (w->thread)
          w->thread->futex_bucket = bucket2; // keep the hang report's key in sync
        total_requeued++;
      } else {
        // Limit reached for both waking and requeueing
        pp = &w->next;
      }
    } else {
      pp = &w->next;
    }
  }

  spinlock_release(&futex_hash[bucket1].lock);
  if (bucket2 != bucket1)
    spinlock_release(&futex_hash[bucket2].lock);

  return (uint64_t)(total_woken + total_requeued);
}

// FUTEX_CMP_REQUEUE (4)
static uint64_t futex_cmp_requeue(uint32_t *uaddr1, uint32_t val, uint32_t val2,
                                  uint32_t *uaddr2, uint32_t val3, bool private) {
  struct futex_key key1, key2;
  int error = futex_get_key(uaddr1, private, &key1);
  if (error)
    return (uint64_t)(int64_t)error;
  error = futex_get_key(uaddr2, private, &key2);
  if (error)
    return (uint64_t)(int64_t)error;

  uint32_t bucket1 = futex_hash_key(key1);
  uint32_t bucket2 = futex_hash_key(key2);

  // Always acquire locks in bucket order to avoid deadlocks
  if (bucket1 == bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
  } else if (bucket1 < bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
    spinlock_acquire(&futex_hash[bucket2].lock);
  } else {
    spinlock_acquire(&futex_hash[bucket2].lock);
    spinlock_acquire(&futex_hash[bucket1].lock);
  }

  uint32_t cur = __atomic_load_n(uaddr1, __ATOMIC_RELAXED);
  if (cur != val3) {
    spinlock_release(&futex_hash[bucket1].lock);
    if (bucket2 != bucket1)
      spinlock_release(&futex_hash[bucket2].lock);
    return (uint64_t)(-(int64_t)EAGAIN);
  }

  if (futex_key_equal(key1, key2)) {
    spinlock_release(&futex_hash[bucket1].lock);
    if (bucket2 != bucket1)
      spinlock_release(&futex_hash[bucket2].lock);
    return futex_wake_key(key1, val, FUTEX_BITSET_MATCH_ANY);
  }

  uint32_t total_woken = 0;
  uint32_t total_requeued = 0;

  struct futex_waiter **pp = &futex_hash[bucket1].head;
  while (*pp) {
    struct futex_waiter *w = *pp;
    if (futex_key_equal(w->key, key1)) {
      if (total_woken < val) {
        if (w->thread && w->thread->state == THREAD_BLOCKED) {
          sched_wakeup(w->thread);
          total_woken++;
        }
        *pp = w->next;
      } else if (total_requeued < val2) {
        *pp = w->next;
        w->key = key2;
        w->next = futex_hash[bucket2].head;
        futex_hash[bucket2].head = w;
        total_requeued++;
      } else {
        pp = &w->next;
      }
    } else {
      pp = &w->next;
    }
  }

  spinlock_release(&futex_hash[bucket1].lock);
  if (bucket2 != bucket1)
    spinlock_release(&futex_hash[bucket2].lock);

  return (uint64_t)(total_woken + total_requeued);
}

// FUTEX_WAKE_OP
// Atomically applies an encoded operation to *uaddr2, wakes up to val threads
// on uaddr, then conditionally wakes up to val2 threads on uaddr2 depending on
// whether the old value of *uaddr2 satisfies a comparison.
// val3 encodes: op[31:28] | cmp[27:24] | oparg[23:12] | cmparg[11:0]
static uint64_t futex_wake_op(uint32_t *uaddr, uint32_t val,
                              uint32_t val2, uint32_t *uaddr2,
                              uint32_t val3, bool private) {
  // Decode val3
  uint32_t op_code  = (val3 >> FUTEX_OP_OP_SHIFT)   & FUTEX_OP_OP_MASK;
  uint32_t cmp_code = (val3 >> FUTEX_OP_CMP_SHIFT)  & FUTEX_OP_CMP_MASK;
  uint32_t oparg    = (val3 >> FUTEX_OP_OPARG_SHIFT) & FUTEX_OP_OPARG_MASK;
  uint32_t cmparg   =  val3                          & FUTEX_OP_CMPARG_MASK;

  // FUTEX_OP_ARG_SHIFT: oparg is a shift count rather than a literal value
  if (op_code & FUTEX_OP_ARG_SHIFT) {
    op_code &= ~FUTEX_OP_ARG_SHIFT;
    if (oparg >= 32)
      return (uint64_t)(-(int64_t)EINVAL);
    oparg = 1u << oparg;
  }

  // Validate uaddr2
  uint64_t vaddr2 = (uint64_t)uaddr2;
  if ((vaddr2 & (sizeof(uint32_t) - 1)) != 0)
    return (uint64_t)(-(int64_t)EINVAL);
  if (!vmm_is_user_addr_range_valid(vaddr2, sizeof(uint32_t)))
    return (uint64_t)(-(int64_t)EFAULT);

  // Atomically apply the operation to *uaddr2 and capture the old value
  uint32_t old_val;
  switch (op_code) {
  case FUTEX_OP_SET:
    old_val = __atomic_exchange_n(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_ADD:
    old_val = __atomic_fetch_add(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_OR:
    old_val = __atomic_fetch_or(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_ANDN:
    old_val = __atomic_fetch_and(uaddr2, ~oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_XOR:
    old_val = __atomic_fetch_xor(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  default:
    return (uint64_t)(-(int64_t)EINVAL);
  }

  // Wake up to val threads waiting on uaddr (always)
  uint64_t woken = futex_wake(uaddr, val, private);

  // Evaluate the comparison against old_val
  bool cmp_result;
  switch (cmp_code) {
  case FUTEX_OP_CMP_EQ: cmp_result = (old_val == cmparg); break;
  case FUTEX_OP_CMP_NE: cmp_result = (old_val != cmparg); break;
  case FUTEX_OP_CMP_LT: cmp_result = (old_val <  cmparg); break;
  case FUTEX_OP_CMP_LE: cmp_result = (old_val <= cmparg); break;
  case FUTEX_OP_CMP_GT: cmp_result = (old_val >  cmparg); break;
  case FUTEX_OP_CMP_GE: cmp_result = (old_val >= cmparg); break;
  default:
    return (uint64_t)(-(int64_t)EINVAL);
  }

  // Conditionally wake up to val2 threads waiting on uaddr2
  if (cmp_result)
    woken += futex_wake(uaddr2, val2, private);

  return woken;
}

/* ------------------------------------------------------------------------ *
 * Priority-inheritance futexes
 *
 * musl probes kernel PI support once in pthread_mutexattr_setprotocol() by
 * issuing FUTEX_LOCK_PI on a scratch word; libpulse and friends treat the
 * probe error as fatal, so these ops have to exist.  The kernel does not
 * schedule with priorities, so no actual priority donation happens here, but
 * the lock/hand-over protocol is the Linux one:
 *
 *   - owner TID lives in bits 0-29 of the futex word
 *   - FUTEX_WAITERS says a kernel waiter exists and unlock must hand over
 *   - FUTEX_OWNER_DIED marks a lock recovered from a dead owner
 *
 * FUTEX_LOCK_PI timeouts are absolute and measured against CLOCK_REALTIME;
 * FUTEX_LOCK_PI2 (Linux 5.14) selects CLOCK_MONOTONIC unless
 * FUTEX_CLOCK_REALTIME is set, which is what glibc's PI mutexes use.
 * ------------------------------------------------------------------------ */

extern uint64_t rtc_get_boot_timestamp(void);

/* A PI owner is gone once it has left the task list or published DEAD/ZOMBIE:
 * either way it cannot run the matching FUTEX_UNLOCK_PI anymore. */
static bool futex_pi_owner_gone(uint32_t owner) {
  if (owner == 0)
    return true;

  struct sched_thread_snapshot snap;
  if (!sched_get_thread_snapshot(owner, &snap))
    return true;

  return snap.state == THREAD_DEAD || snap.state == THREAD_ZOMBIE;
}

/* Validate an absolute PI timeout and convert it into remaining LAPIC ticks
 * (ms).  Returns 0 and stores the remaining time, or a negative -errno. */
static int futex_pi_timeout_to_ms(const uint64_t *ts, bool realtime,
                                  uint64_t *remaining_ms) {
  if (!vmm_is_user_addr_range_valid((uint64_t)ts, sizeof(uint64_t) * 2))
    return -EFAULT;

  uint64_t sec = ts[0];
  uint64_t nsec = ts[1];
  if ((int64_t)sec < 0 || nsec >= 1000000000ULL)
    return -EINVAL;

  uint64_t target_ms = sec * 1000ULL + nsec / 1000000ULL;
  uint64_t now_ms = realtime
      ? rtc_get_boot_timestamp() * 1000ULL + lapic_timer_get_ticks()
      : lapic_timer_get_ticks();

  *remaining_ms = target_ms > now_ms ? target_ms - now_ms : 0;
  return 0;
}

/* FUTEX_LOCK_PI / FUTEX_LOCK_PI2 / FUTEX_TRYLOCK_PI. */
static uint64_t futex_pi_lock(uint32_t *uaddr, bool private,
                              const uint64_t *timeout_ts, bool trylock,
                              bool realtime) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  struct thread *self = sched_get_current();
  if (!self)
    return (uint64_t)(-(int64_t)EFAULT);
  const uint32_t self_tid = self->tid;

  /* Validate the timespec up front even if the lock is free: Linux reports
   * EFAULT/EINVAL for a bad timeout regardless of contention. */
  if (timeout_ts) {
    uint64_t remaining;
    error = futex_pi_timeout_to_ms(timeout_ts, realtime, &remaining);
    if (error)
      return (uint64_t)(int64_t)error;
  }

retry:
  /* Uncontended / recover-from-dead-owner fast path. */
  for (;;) {
    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    uint32_t owner = cur & FUTEX_TID_MASK;

    if (owner == self_tid)
      return (uint64_t)(-(int64_t)EDEADLK);

    if (owner == 0) {
      /* Keep FUTEX_WAITERS so a racing unlock still knows to look. */
      uint32_t newv = (cur & FUTEX_WAITERS) | self_tid;
      if (__atomic_compare_exchange_n(uaddr, &cur, newv, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
      continue;
    }

    if (futex_pi_owner_gone(owner)) {
      /* The owner died without unlocking.  Take the lock and leave
       * FUTEX_OWNER_DIED set so robust userspace can clean up. */
      uint32_t newv = (cur & FUTEX_WAITERS) | self_tid | FUTEX_OWNER_DIED;
      if (__atomic_compare_exchange_n(uaddr, &cur, newv, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
      continue;
    }

    break; /* held by a live thread */
  }

  if (trylock)
    return (uint64_t)(-(int64_t)EAGAIN);

  /* An expired absolute timeout fails immediately rather than enqueueing. */
  if (timeout_ts) {
    uint64_t remaining;
    error = futex_pi_timeout_to_ms(timeout_ts, realtime, &remaining);
    if (error)
      return (uint64_t)(int64_t)error;
    if (remaining == 0)
      return (uint64_t)(-(int64_t)ETIMEDOUT);
  }

  /* Mark that unlockers must go through the kernel for this word. */
  for (;;) {
    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    if (cur & FUTEX_WAITERS)
      break;
    if (__atomic_compare_exchange_n(uaddr, &cur, cur | FUTEX_WAITERS, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      break;
  }

  struct futex_waiter waiter;
  waiter.key = key;
  waiter.thread = self;
  waiter.bitset = FUTEX_BITSET_MATCH_ANY;
  waiter.pi = true;
  waiter.acquired = false;
  waiter.next = NULL;

  uint32_t bucket = futex_hash_key(key);
  spinlock_acquire(&futex_hash[bucket].lock);

  /* Re-check under the bucket lock: the owner may have released (or died)
   * while the waiters bit was being set. */
  uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_RELAXED);
  uint32_t owner = cur & FUTEX_TID_MASK;
  if (owner == 0 || owner == self_tid || futex_pi_owner_gone(owner)) {
    spinlock_release(&futex_hash[bucket].lock);
    goto retry;
  }

  /* FIFO hand-over order. */
  struct futex_waiter **tail = &futex_hash[bucket].head;
  while (*tail)
    tail = &(*tail)->next;
  *tail = &waiter;

  self->state = THREAD_BLOCKED;
  self->wakeup_ticks = 0;

  if (timeout_ts) {
    uint64_t remaining;
    error = futex_pi_timeout_to_ms(timeout_ts, realtime, &remaining);
    if (error || remaining == 0) {
      *tail = NULL;
      self->state = THREAD_RUNNING;
      spinlock_release(&futex_hash[bucket].lock);
      LOCKDIAG_STAT(futex_timeouts, 1);
      return error ? (uint64_t)(int64_t)error
                   : (uint64_t)(-(int64_t)ETIMEDOUT);
    }
    self->wakeup_ticks = lapic_timer_get_ticks() + remaining;
  }

  /* Same park diagnostics as FUTEX_WAIT; a PI waiter stuck without a
   * hand-over is exactly the shape a lost-wakeup report must be able to name. */
  self->in_futex_wait = true;
  self->blocked_since_ms = lapic_timer_get_ticks();
  self->futex_bucket = bucket;
  self->blocked_reported = false;
  LOCKDIAG_STAT(futex_waits, 1);

  spinlock_release(&futex_hash[bucket].lock);
  if (self->state == THREAD_BLOCKED)
    sched_yield();

  self->in_futex_wait = false;
  self->blocked_since_ms = 0;

  /* Remove ourselves if unlock did not already do so, then check whether the
   * lock was handed over. */
  uint32_t final_bucket = futex_hash_key(waiter.key);
  spinlock_acquire(&futex_hash[final_bucket].lock);
  struct futex_waiter **pp = &futex_hash[final_bucket].head;
  while (*pp) {
    if (*pp == &waiter) {
      *pp = waiter.next;
      break;
    }
    pp = &(*pp)->next;
  }
  bool acquired = waiter.acquired;
  spinlock_release(&futex_hash[final_bucket].lock);

  if (acquired)
    return 0;
  if (thread_has_pending_signal(self))
    return (uint64_t)(-(int64_t)EINTR);
  if (timeout_ts)
    return (uint64_t)(-(int64_t)ETIMEDOUT);

  /* Spurious wake without the lock: try again. */
  goto retry;
}

/* FUTEX_UNLOCK_PI: hand the lock to the first queued waiter, or clear it. */
static uint64_t futex_pi_unlock(uint32_t *uaddr, bool private) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  struct thread *self = sched_get_current();
  if (!self)
    return (uint64_t)(-(int64_t)EFAULT);

  uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
  if ((cur & FUTEX_TID_MASK) != self->tid)
    return (uint64_t)(-(int64_t)EPERM);

  uint32_t bucket = futex_hash_key(key);
  spinlock_acquire(&futex_hash[bucket].lock);

  struct futex_waiter *next = NULL;
  struct futex_waiter **next_link = NULL;
  for (struct futex_waiter **pp = &futex_hash[bucket].head; *pp;
       pp = &(*pp)->next) {
    struct futex_waiter *w = *pp;
    if (w->pi && w->thread && w->thread->state == THREAD_BLOCKED &&
        futex_key_equal(w->key, key)) {
      next = w;
      next_link = pp;
      break;
    }
  }

  if (!next) {
    /* No blocked waiter (or a stale FUTEX_WAITERS bit): release. */
    __atomic_store_n(uaddr, 0, __ATOMIC_RELEASE);
    spinlock_release(&futex_hash[bucket].lock);
    return 0;
  }

  *next_link = next->next;

  /* Keep FUTEX_WAITERS set while more blocked waiters remain. */
  uint32_t newv = next->thread->tid;
  for (struct futex_waiter *w = futex_hash[bucket].head; w; w = w->next) {
    if (w->pi && w->thread && w->thread->state == THREAD_BLOCKED &&
        futex_key_equal(w->key, key)) {
      newv |= FUTEX_WAITERS;
      break;
    }
  }

  next->acquired = true;
  __atomic_store_n(uaddr, newv, __ATOMIC_RELEASE);
  sched_wakeup(next->thread);
  spinlock_release(&futex_hash[bucket].lock);
  return 0;
}

// sys_futex dispatcher
static uint64_t sys_futex(uint64_t uaddr_val, uint64_t op_val, uint64_t val_arg,
                          uint64_t timeout_ptr, uint64_t uaddr2_val,
                          uint64_t val3) {
  (void)val3;

  uint32_t *uaddr = (uint32_t *)uaddr_val;
  bool private = (op_val & FUTEX_PRIVATE_FLAG) != 0;
  int op = (int)(op_val & FUTEX_CMD_MASK);
  uint32_t val = (uint32_t)val_arg;

  switch (op) {
  case FUTEX_WAIT: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    bool is_abs = (op_val & FUTEX_CLOCK_REALTIME) != 0;
    return futex_wait(uaddr, val, timeout, private, is_abs, FUTEX_BITSET_MATCH_ANY);
  }

  case FUTEX_WAIT_BITSET: {
    if (val3 == 0)
      return (uint64_t)(-(int64_t)EINVAL);
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    bool is_abs = true; // FUTEX_WAIT_BITSET timeout is absolute
    return futex_wait(uaddr, val, timeout, private, is_abs, (uint32_t)val3);
  }

  case FUTEX_WAKE: {
    uint64_t woken = futex_wake(uaddr, val, private);
    if (woken == 0) {
      woken = futex_wake(uaddr, val, !private);
    }
    return woken;
  }

  case FUTEX_WAKE_BITSET: {
    if (val3 == 0)
      return (uint64_t)(-(int64_t)EINVAL);
    uint64_t woken = futex_wake_bitset(uaddr, val, (uint32_t)val3, private);
    if (woken == 0) {
      woken = futex_wake_bitset(uaddr, val, (uint32_t)val3, !private);
    }
    return woken;
  }

  case FUTEX_REQUEUE:
    return futex_requeue(uaddr, val, (uint32_t)timeout_ptr,
                         (uint32_t *)uaddr2_val, private);

  case FUTEX_CMP_REQUEUE:
  case FUTEX_CMP_REQUEUE_PI:
    return futex_cmp_requeue(uaddr, val, (uint32_t)timeout_ptr,
                             (uint32_t *)uaddr2_val, (uint32_t)val3, private);

  case FUTEX_WAIT_REQUEUE_PI: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    bool is_abs = (op_val & FUTEX_CLOCK_REALTIME) != 0;
    return futex_wait(uaddr, val, timeout, private, is_abs, FUTEX_BITSET_MATCH_ANY);
  }

  case FUTEX_WAKE_OP:
    return futex_wake_op(uaddr, val, (uint32_t)timeout_ptr,
                         (uint32_t *)uaddr2_val, (uint32_t)val3, private);

  case FUTEX_LOCK_PI: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    /* FUTEX_LOCK_PI timeouts are always absolute CLOCK_REALTIME values. */
    return futex_pi_lock(uaddr, private, timeout, false, true);
  }

  case FUTEX_LOCK_PI2: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    bool realtime = (op_val & FUTEX_CLOCK_REALTIME) != 0;
    return futex_pi_lock(uaddr, private, timeout, false, realtime);
  }

  case FUTEX_TRYLOCK_PI:
    /* The timeout is meaningless for a non-blocking lock. */
    return futex_pi_lock(uaddr, private, NULL, true, false);

  case FUTEX_UNLOCK_PI:
    return futex_pi_unlock(uaddr, private);

  default:
    klog_puts("[FUTEX] Unsupported op: ");
    klog_uint64(op_val);
    klog_puts("\n");
    return (uint64_t)(-(int64_t)EINVAL);
  }
}

// Registration
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count) {
  // CLONE_CHILD_CLEARTID is paired with pthread-private futex waits in musl,
  // but glibc pthread_join/lll_wait_tid uses shared futex waits (LLL_SHARED).
  // Wake private waiters first, and if there are remaining slots, wake shared waiters.
  uint64_t woken = futex_wake(uaddr, count, true);
  if (woken < count) {
    woken += futex_wake(uaddr, count - (uint32_t)woken, false);
  }
  return woken;
}
/* Per-bucket wake traffic is defined next to the hash table above; this is the
 * read side used by the hang report. */
uint64_t futex_bucket_stats(uint32_t bucket, uint64_t *wakes,
                            uint64_t *no_match) {
  if (bucket >= FUTEX_HASH_SIZE)
    return 0;
  if (wakes)
    *wakes = __atomic_load_n(&futex_bucket_wakes[bucket], __ATOMIC_RELAXED);
  if (no_match)
    *no_match = __atomic_load_n(&futex_bucket_nomatch[bucket], __ATOMIC_RELAXED);
  return 1;
}
void syscall_register_futex(void) {
  // Registration runs before userspace and makes lazy initialization and its
  // race/branch unnecessary in every futex operation.
  futex_init_once();
  syscall_register(SYS_FUTEX, sys_futex);
}