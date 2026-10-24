#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

static unsigned int interrupted_waits;

signed long linuxkpi_nouveau_bind_wait_timeout(struct dma_fence *fence,
                                               bool interruptible,
                                               signed long timeout) {
  signed long result = dma_fence_wait_timeout(fence, interruptible, timeout);
  if (result == -ERESTARTSYS && interruptible && timeout == MAX_SCHEDULE_TIMEOUT) {
    if (__atomic_add_fetch(&interrupted_waits, 1, __ATOMIC_RELAXED) <= 4)
      klogf("[INFO] LinuxKPI: Nouveau synchronous VM_BIND interrupted; "
            "waiting for completion tgid=%lu\n", linuxkpi_current_tgid());
    /* Do not turn a submitted synchronous mapping into premature success.
     * Recheck periodically instead of hiding all progress behind an infinite
     * sleep: a completed fence with a missed wake can then be observed, and
     * a genuinely stalled GPU job is identified by context/sequence in logs. */
    unsigned long long start = linuxkpi_monotonic_ms();
    unsigned long long report_at = start + 5000;
    bool reported = false;
    do {
      result = dma_fence_wait_timeout(fence, false, msecs_to_jiffies(1000));
      unsigned long long now = linuxkpi_monotonic_ms();
      if (result == 0 && now >= report_at) {
        klogf("[WARN] LinuxKPI: Nouveau VM_BIND still pending tgid=%lu "
              "context=%llu seqno=%llu flags=%lx wait_ms=%llu\n",
              linuxkpi_current_tgid(), (unsigned long long)fence->context,
              (unsigned long long)fence->seqno, READ_ONCE(fence->flags),
              now - start);
        report_at = now + 5000;
        if (!reported)
          linuxkpi_report_stalled_gpu_wait();
        reported = true;
      }
    } while (result == 0);
    if (result > 0) {
      if (reported)
        klogf("[INFO] LinuxKPI: Nouveau VM_BIND completed tgid=%lu "
              "context=%llu seqno=%llu wait_ms=%llu\n",
              linuxkpi_current_tgid(), (unsigned long long)fence->context,
              (unsigned long long)fence->seqno,
              linuxkpi_monotonic_ms() - start);
      /* Preserve the infinite-wait return contract, not the slice remainder. */
      result = MAX_SCHEDULE_TIMEOUT;
    }
  }
  return result;
}
