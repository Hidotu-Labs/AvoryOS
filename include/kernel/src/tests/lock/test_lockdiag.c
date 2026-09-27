/* Lockdiag self-test: prove the hang report renders.
 *
 * The report is the one piece of code that only ever runs when the machine is
 * already broken, which is the worst possible time to discover it does not
 * compile, deadlocks on serial_lock, or prints garbage.  This exercises every
 * part of it on a known-good machine at boot:
 *
 *   - the liveness probe IPI and the per-CPU acknowledge bookkeeping;
 *   - the per-CPU dump, including the kernel stack scans;
 *   - the vmm_lock spotlight and the lockdep rule check that the shootdown path
 *     relies on to name a shootdown-while-holding-vmm_lock deadlock;
 *   - the futex/wait-queue counter block and the tid_lock census.
 *
 * Built only with `make LOCKDIAG_SELFTEST=1`.
 */

#include "../../lock/lockdiag.h"
#include "../../console/klog.h"
#include "../../mm/vmm.h"
#include "../../sched/sched.h"

#if LOCKDIAG

/* What a real shootdown would see on this core. */
static void check_rule(void) {
  klog_puts("[SELFTEST] lockdep rule, vmm_lock free -> self-holds=");
  klog_puts(lockdiag_self_holds_vmm_lock(0) ? "TRUE (WRONG)\n" : "false (ok)\n");

  /* Pretend this core is inside vmm_lock, the way fork() is when it issues the
   * batched CoW shootdown from vmm_clone_user_mappings_vma(). */
  lockdiag_spot_take(LOCKDIAG_SPOT_VMM, 0, (uint64_t)&check_rule);
  klog_puts("[SELFTEST] lockdep rule, vmm_lock held -> self-holds=");
  klog_puts(lockdiag_self_holds_vmm_lock(0) ? "true (ok), would WARN\n"
                                            : "FALSE (WRONG)\n");
  lockdiag_spot_drop(LOCKDIAG_SPOT_VMM);

  if (lockdiag_self_holds_vmm_lock(0))
    klog_puts("[SELFTEST] FAIL: spotlight stuck after release\n");
}

void test_lockdiag_selftest(void) {
  klog_puts("\n[SELFTEST] ===== lockdiag report self-test =====\n");

  check_rule();

  lockdiag_stats_t *s = lockdiag_stats();
  if (!s)
    klog_puts("[SELFTEST] FAIL: no stats object\n");
  klog_puts("[SELFTEST] (a full HANG REPORT follows; it is deliberate and the "
            "machine is healthy)\n");

  lockdiag_dump_all("self-test: deliberate report on a healthy machine");

  klog_puts("[SELFTEST] expect above: one line per CPU, PROBE-ACK with a RIP "
            "per answering CPU, the vmm_lock spotlight line, and a futex "
            "census.\n");
  klog_puts("[SELFTEST] ===== self-test done =====\n\n");
}

#else
void test_lockdiag_selftest(void) {
  klog_puts("[SELFTEST] lockdiag compiled out (LOCKDIAG=0)\n");
}
#endif
