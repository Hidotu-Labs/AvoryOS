#ifndef MM_TLB_SHOOTDOWN_H
#define MM_TLB_SHOOTDOWN_H

#include <stdint.h>

#define IPI_VECTOR_TLB_SHOOTDOWN 50

#define TLB_SHOOTDOWN_ALL UINT64_MAX

void tlb_shootdown_init(void);

void tlb_shootdown_page(uint64_t addr);

/*
 * Same as tlb_shootdown_page(), but for an address inside a specific address
 * space.  Without the address space there is no way to tell which PCID the
 * stale translations are tagged with, and the handler has to fall back to
 * flushing everything on every CPU.  Pass the pml4 you are modifying whenever
 * you have one.
 */
void tlb_shootdown_page_for(uint64_t addr, uint64_t pml4);

void tlb_shootdown_all(void);

void tlb_shootdown_handle_ipi(void);

/* Cost counters, exported for /proc/tlb_stats.
 *
 * These exist to answer one question with numbers instead of guesses: how much
 * time does the system spend paying for TLB invalidation, and how much of it is
 * the single-address path escalating into a full flush of every other CPU's
 * TLB? handler_full_flushes is the number to watch - every one of those is a
 * single page invalidation that destroyed all remote non-global translations. */
typedef struct {
  uint64_t shootdowns;         /* shootdowns that had to IPI somebody          */
  uint64_t ipis_sent;          /* individual IPIs sent                         */
  uint64_t broadcast_all;      /* tlb_shootdown_all() calls                    */
  uint64_t handler_full_flushes; /* remote full TLB flushes for a single page  */
  uint64_t ack_wait_ns;        /* total time spin-waiting for acknowledgements */
  uint64_t max_ack_wait_ns;    /* worst single wait, in ns                     */
} tlb_shootdown_stats_t;

void tlb_shootdown_get_stats(tlb_shootdown_stats_t *out);

/* Per-call-site shootdown accounting, read by the lockdiag report. Fills slot
 * `slot` and returns 0 once past the end of the recorded callers. `ip` is the
 * instruction that asked for the flush; `under_vmm_lock` counts the ones issued
 * while the initiator held vmm_lock, which is the shape that deadlocks. */
uint64_t tlb_shootdown_caller_info(int slot, uint64_t *ip, uint64_t *total,
                                   uint64_t *under_vmm_lock);

/* Request an invalidation without waiting for remote CPUs, and drain the
 * requests recorded so far on this CPU.
 *
 * The two are split because waiting for acknowledgements while holding a lock
 * that the targets also take is how the machine stops: a target inside its page
 * fault handler has interrupts masked (interrupt gates) and so cannot answer the
 * shootdown IPI, while the initiator holds the lock it is spinning on. Code that
 * modifies page tables therefore records its flushes under the lock and drains
 * them after dropping it - see vmm_lock_release() in mm/vmm_map.c.
 *
 * Draining more often than necessary is safe; never drain while holding such a
 * lock. Requests collapse into a single full invalidation once too many distinct
 * addresses accumulate, so a large range costs one broadcast rather than one per
 * page. */
void tlb_flush_deferred(uint64_t addr, uint64_t pml4);
void tlb_flush_deferred_all(void);
void tlb_flush_deferred_drain(void);
void tlb_shootdown_reset_stats(void);

#endif // MM_TLB_SHOOTDOWN_H
