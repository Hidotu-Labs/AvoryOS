#include <linux/dma-fence.h>
#include <linux/errno.h>
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
    result = dma_fence_wait_timeout(fence, false, MAX_SCHEDULE_TIMEOUT);
  }
  return result;
}
