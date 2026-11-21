#include "mm/devframe.h"

#include "console/klog.h"
#include "cpu/irq.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/wait.h"

#define DEVFRAME_MAX 512

struct devframe_range {
  uint64_t base, size;
  void *owner;
  const struct devframe_ops *ops;
};

struct devframe_job {
  void *owner;
  void (*fn)(void *owner);
};

static spinlock_t devframe_lock = SPINLOCK_INIT;
/* Sorted by base; ranges are disjoint. */
static struct devframe_range devframe_tab[DEVFRAME_MAX];
static uint32_t devframe_count;
/* Final releases waiting for the worker. */
static struct devframe_job devframe_jobs[DEVFRAME_MAX];
static uint32_t devframe_njobs;
static wait_queue_t devframe_wait;
static bool devframe_started;

/* Caller holds devframe_lock. */
static int devframe_find_phys(uint64_t phys) {
  uint32_t lo = 0, hi = devframe_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const struct devframe_range *r = &devframe_tab[mid];
    if (phys < r->base)
      hi = mid;
    else if (phys - r->base >= r->size)
      lo = mid + 1;
    else
      return (int)mid;
  }
  return -1;
}

/* Caller holds devframe_lock. Ranges are removed only by their owner's
 * release, so every owner here is still registered when it is found. */
static int devframe_find_owner(void *owner) {
  for (uint32_t i = 0; i < devframe_count; i++)
    if (devframe_tab[i].owner == owner)
      return (int)i;
  return -1;
}

static void devframe_worker(void) {
  for (;;) {
    struct devframe_job job = {0};
    bool have = false;

    spinlock_acquire(&devframe_lock);
    if (devframe_njobs) {
      job = devframe_jobs[0];
      memmove(&devframe_jobs[0], &devframe_jobs[1],
              (devframe_njobs - 1) * sizeof(devframe_jobs[0]));
      devframe_njobs--;
      have = true;
    }
    spinlock_release(&devframe_lock);

    if (have) {
      job.fn(job.owner);
      continue;
    }

    /* Park on the queue before the final check so a wake that lands between
     * the check and the yield is not lost. Same protocol as ioctl_gate. */
    struct thread *self = sched_get_current();
    wait_queue_entry_t entry = {.thread = self};
    hal_irq_state_t flags = hal_irq_save();
    self->wakeup_ticks = 0;
    self->state = THREAD_BLOCKED;
    wait_queue_add_tail(&devframe_wait, &entry);
    if (!__atomic_load_n(&devframe_njobs, __ATOMIC_ACQUIRE)) {
      hal_irq_restore(flags);
      sched_yield();
      flags = hal_irq_save();
    }
    wait_queue_remove(&devframe_wait, &entry);
    self->wakeup_ticks = 0;
    self->state = THREAD_RUNNING;
    hal_irq_restore(flags);
  }
}

/* Process context only: creates the worker on first use. */
static bool devframe_start(void) {
  if (devframe_started)
    return true;
  wait_queue_init(&devframe_wait);
  struct thread *w = sched_create_kernel_thread(devframe_worker, NULL, true);
  if (!w)
    return false;
  strcpy(w->comm, "devframe");
  devframe_started = true;
  return true;
}

int devframe_register(uint64_t base, uint64_t size, void *owner,
                      const struct devframe_ops *ops) {
  if (!base || !size || !owner || !ops || !ops->get || !ops->put ||
      !ops->release)
    return -22;
  if (!devframe_start())
    return -12;

  spinlock_acquire(&devframe_lock);
  if (devframe_count == DEVFRAME_MAX) {
    spinlock_release(&devframe_lock);
    return -12;
  }
  uint32_t pos = 0;
  while (pos < devframe_count && devframe_tab[pos].base < base)
    pos++;
  bool overlap =
      (pos > 0 && devframe_tab[pos - 1].base + devframe_tab[pos - 1].size > base) ||
      (pos < devframe_count && base + size > devframe_tab[pos].base);
  if (overlap) {
    spinlock_release(&devframe_lock);
    return -17;
  }
  memmove(&devframe_tab[pos + 1], &devframe_tab[pos],
          (devframe_count - pos) * sizeof(devframe_tab[0]));
  devframe_tab[pos] = (struct devframe_range){
      .base = base, .size = size, .owner = owner, .ops = ops};
  devframe_count++;
  spinlock_release(&devframe_lock);
  return 0;
}

void devframe_unregister(void *owner) {
  spinlock_acquire(&devframe_lock);
  int i = devframe_find_owner(owner);
  if (i >= 0) {
    memmove(&devframe_tab[i], &devframe_tab[i + 1],
            (devframe_count - (uint32_t)i - 1) * sizeof(devframe_tab[0]));
    devframe_count--;
  }
  spinlock_release(&devframe_lock);
}

bool devframe_defer(void *owner, void (*fn)(void *owner)) {
  if (!owner || !fn || !devframe_start())
    return false;
  spinlock_acquire(&devframe_lock);
  bool queued = devframe_njobs < DEVFRAME_MAX;
  if (queued)
    devframe_jobs[devframe_njobs++] = (struct devframe_job){owner, fn};
  spinlock_release(&devframe_lock);
  if (queued)
    wait_queue_wake_one(&devframe_wait);
  return queued;
}

void devframe_pte_install(uint64_t va, uint64_t phys) {
  if (va > USER_SPACE_LIMIT || !phys || pmm_is_managed(phys))
    return;
  spinlock_acquire(&devframe_lock);
  int i = devframe_find_phys(phys);
  if (i >= 0)
    devframe_tab[i].ops->get(devframe_tab[i].owner);
  spinlock_release(&devframe_lock);
}

/* Called under vmm_lock: never sleeps; the final release is queued. */
void devframe_pte_remove(uint64_t va, uint64_t phys) {
  if (va > USER_SPACE_LIMIT || !phys || pmm_is_managed(phys))
    return;
  bool queued = false;
  spinlock_acquire(&devframe_lock);
  int i = devframe_find_phys(phys);
  if (i >= 0 && devframe_tab[i].ops->put(devframe_tab[i].owner) &&
      devframe_njobs < DEVFRAME_MAX) {
    devframe_jobs[devframe_njobs++] =
        (struct devframe_job){devframe_tab[i].owner, devframe_tab[i].ops->release};
    queued = true;
  }
  spinlock_release(&devframe_lock);
  if (queued)
    wait_queue_wake_one(&devframe_wait);
}
