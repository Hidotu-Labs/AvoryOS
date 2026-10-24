#ifndef FS_VFS_RCU_H
#define FS_VFS_RCU_H

#include <stdbool.h>
#include <stdint.h>

struct vfs_node;

/* IPI vector for the deferred-free barrier (49 = reschedule, 50 = TLB). */
#define IPI_VECTOR_VFS_RCU 51

/* One queued deferred-free record.  Embed it in any object whose final
 * release must wait for a grace period; the queue links through `next` and
 * `reclaim` is called with the memory still intact once the grace period has
 * passed.  Only the queue may touch these two words while the object sits on
 * it. */
struct vfs_rcu_head {
  struct vfs_rcu_head *next;
  void (*reclaim)(struct vfs_rcu_head *head);
};

/*
 * Deferred reclamation for the lockless dentry/path-cache readers.
 *
 * Why this exists
 * ---------------
 * The dentry and full-path caches used to serialize every lookup on a
 * per-bucket spinlock.  That is a locked RMW on a shared cacheline plus
 * interrupts masked per path component, paid on every open()/stat().  The
 * buckets already carry a seqlock-style sequence counter (writers increment
 * it before and after mutating an entry), so readers can scan locklessly,
 * but only if a node pointer observed in an entry stays dereferenceable
 * until the reader has taken its own reference.  The cache holds a reference
 * only while the entry is installed; a reader can read the pointer and lose
 * the race to the entry's vfs_close(), which used to free the node
 * immediately.  Hence: the final kfree of any node is deferred here.
 *
 * The same engine covers the page cache's lockless lookups.  Two object
 * classes there are reachable from a lockless reader and are released by
 * writers: the radix-tree nodes that index node->pages (freed on every
 * collapse) and the vfs_page_t objects themselves (freed by the final
 * vfs_cache_put()).  Both are queued through vfs_rcu_defer() before any
 * memory is recycled, so a reader that loses the race can still safely
 * validate a stale pointer and walk away from it.
 *
 * How a grace period is established
 * ---------------------------------
 * Lockless reads run inside an IRQ-off region (hal_irq_save/restore).  Such a
 * region cannot be preempted or migrated, so a CPU is either between regions
 * or running one to completion - there is no "suspended reader".  To prove
 * that every reader which could still hold a raw pointer has finished, the
 * drainer arms a per-CPU ack flag and sends an IPI to every other online CPU:
 * an ack at time t proves that CPU left all IRQ-off regions that were active
 * when the IPI was raised (interrupt delivery is blocked until IF=1).  The
 * initiator's own CPU needs no IPI: it is executing thread context, and an
 * IRQ-off region cannot be suspended, so any earlier region there is done.
 *
 * Ordinary IPIs count too.  Any delivery to a CPU after the request was
 * armed proves the same thing, which is why a delayed IPI from an earlier
 * round is harmless: the ack flag is only read by the single drainer, and
 * barriers are serialized by vfs_rcu_barrier_lock, so a late handler simply
 * acknowledges the current request - which it is entitled to do, because it
 * ran after the request was armed.
 *
 * Writer contract
 * ---------------
 * A node may only reach refcount zero after it is unreachable from both
 * caches.  Invalidation must clear the bucket entry (under the bucket lock,
 * with the seqlock pair) and only then drop the entry's reference; the final
 * vfs_close() defers the kfree instead of freeing.  Anything that frees a
 * node outside vfs_close()/vfs_node_retire() must be sure it was never
 * inserted into a cache.
 *
 * Cost
 * ----
 * The reader path pays two local IF flips and one tryref; the free path pays
 * one batched IPI round.  Frees are accumulated for VFS_RCU_ACCUMULATE_MS so
 * a delete-heavy workload amortizes one barrier over many nodes.
 */

/* Initialize the engine and start the drainer.  Call once, from kmain, after
 * the scheduler is up; deferrals before this point simply queue up. */
void vfs_rcu_init(void);

/* True once the drainer thread was created. */
bool vfs_rcu_ready(void);

/* Queue `node` (whose last reference just dropped) for kfree after a grace
 * period.  Safe from any thread context; the node's teardown callbacks must
 * already have run. */
void vfs_rcu_defer_free(struct vfs_node *node);

/* Queue an arbitrary object identified by its embedded head for `reclaim`
 * after a grace period.  Safe from any thread context.  With the engine down
 * (vfs_rcu_ready() false) the reclaim runs immediately: the lockless readers
 * are gated on the same predicate, so nothing can still be holding a raw
 * pointer. */
void vfs_rcu_defer(struct vfs_rcu_head *head,
                   void (*reclaim)(struct vfs_rcu_head *head));

/* Reclaim everything queued, running grace periods as needed.  Thread
 * context only (selftest/bench); the drainer uses the same path. */
void vfs_rcu_synchronize(void);

/* Service a pending deferred-free acknowledgement from an IRQ-masked wait. */
void vfs_rcu_poll(void);

/* Counters for the selftest and diagnostics. */
uint64_t vfs_rcu_freed_count(void);
uint64_t vfs_rcu_queued_count(void);

#endif
