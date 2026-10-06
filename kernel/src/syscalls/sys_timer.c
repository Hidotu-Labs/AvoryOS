// sys_timer.c — POSIX per-process timers: timer_create/settime/gettime/
//   getoverrun/delete (222-226) + clock_settime (227).
//
// vim uses timer_create(CLOCK_MONOTONIC, SIGEV_SIGNAL) for its event loop;
// without it every call failed ENOSYS and vim fell back to busy polling.
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../drivers/timer/rtc.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "arch/uaccess.h"
#include "syscall.h"
#include <stdint.h>

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;
void signal_notify_thread(struct thread *t, int sig);

#define POSIX_TIMER_MAX 64
#define TIMER_ABSTIME 1

#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4

/* Kernel view of struct sigevent (matches musl/glibc layout on x86-64:
 * value, signo, notify, function, attributes, tid). */
struct k_sigevent {
  uint64_t sival_ptr;
  int32_t signo;
  int32_t notify;
  uint64_t function;
  uint64_t attributes;
  int32_t tid;
  int32_t pad;
};

struct k_itimerspec {
  uint64_t interval_sec;
  uint64_t interval_nsec;
  uint64_t value_sec;
  uint64_t value_nsec;
};

typedef struct {
  bool in_use;
  uint32_t owner_tgid;
  uint32_t clockid;
  int32_t notify;
  int32_t signo;
  uint64_t sival_ptr;
  uint32_t notify_tid;
  uint64_t interval_ms;
  uint64_t expire_ms; // 0 = disarmed
  uint32_t overrun;
} posix_timer_t;

static posix_timer_t posix_timers[POSIX_TIMER_MAX];
static spinlock_t posix_timer_lock = SPINLOCK_INIT;
static volatile uint32_t posix_timer_armed = 0;
static volatile uint64_t posix_timer_earliest = 0;
static bool sigev_thread_warned = false;

static uint64_t clock_now_ns(uint32_t clockid) {
  uint64_t mono_ns = lapic_timer_get_ns();
  switch (clockid) {
  case 0: // CLOCK_REALTIME
  case 4:
  case 5:
    return rtc_get_boot_timestamp() * 1000000000ULL + mono_ns;
  case 1: // CLOCK_MONOTONIC
  case 6:
  case 7: // CLOCK_BOOTTIME
  case 11:
    return mono_ns;
  default:
    return UINT64_MAX;
  }
}

static bool clockid_valid(uint32_t c) {
  return c == 0 || c == 1 || c == 4 || c == 5 || c == 6 || c == 7 || c == 11;
}

static posix_timer_t *timer_lookup(uint64_t id) {
  if (id == 0 || id > POSIX_TIMER_MAX)
    return NULL;
  posix_timer_t *tm = &posix_timers[id - 1];
  return tm->in_use ? tm : NULL;
}

static void timer_update_earliest_locked(void) {
  uint64_t earliest = 0;
  uint32_t armed = 0;
  for (int i = 0; i < POSIX_TIMER_MAX; i++) {
    if (posix_timers[i].in_use && posix_timers[i].expire_ms) {
      armed++;
      if (!earliest || posix_timers[i].expire_ms < earliest)
        earliest = posix_timers[i].expire_ms;
    }
  }
  __atomic_store_n(&posix_timer_armed, armed, __ATOMIC_RELAXED);
  __atomic_store_n(&posix_timer_earliest, earliest, __ATOMIC_RELAXED);
  if (earliest)
    lapic_timer_rearm_if_earlier(earliest);
}

/* Serviced from sched_tick on the BSP, next to the ITIMER_REAL block. */
void posix_timer_tick(void) {
  if (__atomic_load_n(&posix_timer_armed, __ATOMIC_RELAXED) == 0)
    return;
  uint64_t earliest = __atomic_load_n(&posix_timer_earliest, __ATOMIC_RELAXED);
  uint64_t now = lapic_timer_get_ms();
  if (earliest && now < earliest)
    return;
  if (cpu_get_current() != cpu_get_bsp())
    return;

  // Collect delivery targets on the stack (no kmalloc in tick context),
  // then notify after releasing locks (same order as signal_send_pgid).
  struct thread *notify[64];
  int notify_sig[64];
  uint32_t notify_count = 0;

  spinlock_acquire(&posix_timer_lock);
  spinlock_acquire(&tid_lock);
  uint64_t next_earliest = 0;
  for (int i = 0; i < POSIX_TIMER_MAX; i++) {
    posix_timer_t *tm = &posix_timers[i];
    if (!tm->in_use || !tm->expire_ms)
      continue;
    if (now < tm->expire_ms) {
      if (!next_earliest || tm->expire_ms < next_earliest)
        next_earliest = tm->expire_ms;
      continue;
    }
    // Expired.
    uint64_t bit = 0;
    bool deliver = false;
    if (tm->notify == SIGEV_SIGNAL)
      deliver = tm->signo >= 1 && tm->signo <= 64;
    else if (tm->notify == SIGEV_THREAD_ID)
      deliver = tm->signo >= 1 && tm->signo <= 64;
    if (deliver)
      bit = 1ULL << (tm->signo - 1);

    if (tm->notify == SIGEV_THREAD_ID) {
      bool had = false;
      struct thread *target = NULL;
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tid == tm->notify_tid && t->tgid == tm->owner_tgid) {
          target = t;
          had = (t->pending_signals & bit) != 0;
          t->pending_signals |= bit;
          break;
        }
      }
      if (had)
        tm->overrun++;
      if (target && notify_count < 64) {
        notify[notify_count] = target;
        notify_sig[notify_count] = tm->signo;
        notify_count++;
      }
    } else if (deliver) {
      bool had = false;
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tgid != tm->owner_tgid)
          continue;
        if (t->pending_signals & bit)
          had = true;
        t->pending_signals |= bit;
        if (notify_count < 64) {
          // One wake per thread; duplicates across timers are harmless.
          bool dup = false;
          for (uint32_t k = 0; k < notify_count; k++)
            if (notify[k] == t && notify_sig[k] == tm->signo) {
              dup = true;
              break;
            }
          if (!dup) {
            notify[notify_count] = t;
            notify_sig[notify_count] = tm->signo;
            notify_count++;
          }
        }
      }
      if (had)
        tm->overrun++;
    }
    // SIGEV_NONE / SIGEV_THREAD: track expiry, no signal.

    if (tm->interval_ms) {
      tm->expire_ms = now + tm->interval_ms;
      lapic_timer_rearm_if_earlier(tm->expire_ms);
    } else {
      tm->expire_ms = 0;
    }
    if (tm->expire_ms && (!next_earliest || tm->expire_ms < next_earliest))
      next_earliest = tm->expire_ms;
  }
  __atomic_store_n(&posix_timer_earliest, next_earliest, __ATOMIC_RELAXED);
  // Recompute armed count cheaply while holding the lock.
  {
    uint32_t armed = 0;
    for (int i = 0; i < POSIX_TIMER_MAX; i++)
      if (posix_timers[i].in_use && posix_timers[i].expire_ms)
        armed++;
    __atomic_store_n(&posix_timer_armed, armed, __ATOMIC_RELAXED);
  }
  spinlock_release(&tid_lock);
  spinlock_release(&posix_timer_lock);

  for (uint32_t k = 0; k < notify_count; k++)
    signal_notify_thread(notify[k], notify_sig[k]);
}

/* Drop timers whose owner group is fully gone. Called from the reaper after
 * the thread leaves the global list, so "no live thread with this tgid"
 * means the process is dead. */
void posix_timer_delete_process(uint32_t tgid) {
  if (!tgid)
    return;
  spinlock_acquire(&posix_timer_lock);
  spinlock_acquire(&tid_lock);
  bool alive = false;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid == tgid) {
      alive = true;
      break;
    }
  }
  if (alive) {
    spinlock_release(&tid_lock);
    spinlock_release(&posix_timer_lock);
    return;
  }
  bool changed = false;
  for (int i = 0; i < POSIX_TIMER_MAX; i++) {
    if (posix_timers[i].in_use && posix_timers[i].owner_tgid == tgid) {
      posix_timers[i].in_use = false;
      posix_timers[i].expire_ms = 0;
      changed = true;
    }
  }
  spinlock_release(&tid_lock);
  if (changed)
    timer_update_earliest_locked();
  spinlock_release(&posix_timer_lock);
}

static uint64_t sys_timer_create(uint64_t clockid, uint64_t sevp,
                                 uint64_t timerid_ptr, uint64_t a3, uint64_t a4,
                                 uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!timerid_ptr ||
      !vmm_is_user_addr_range_writable(timerid_ptr, sizeof(uint64_t)))
    return (uint64_t)-14;
  if (!clockid_valid((uint32_t)clockid))
    return (uint64_t)-22;

  int32_t notify = SIGEV_SIGNAL;
  int32_t signo = 14; // SIGALRM default when sevp == NULL
  uint64_t sival = 0;
  uint32_t tid = 0;
  if (sevp) {
    if (!vmm_is_user_addr_range_valid(sevp, sizeof(struct k_sigevent)))
      return (uint64_t)-14;
    struct k_sigevent ev;
    if (copy_from_user(&ev, (const void *)sevp, sizeof(ev)) != 0)
      return (uint64_t)-14;
    notify = ev.notify;
    signo = ev.signo;
    sival = ev.sival_ptr;
    tid = (uint32_t)ev.tid;
    if (notify != SIGEV_SIGNAL && notify != SIGEV_NONE &&
        notify != SIGEV_THREAD && notify != SIGEV_THREAD_ID)
      return (uint64_t)-22;
    if ((notify == SIGEV_SIGNAL || notify == SIGEV_THREAD_ID) &&
        (signo < 1 || signo > 64))
      return (uint64_t)-22;
    if (notify == SIGEV_THREAD && !sigev_thread_warned) {
      sigev_thread_warned = true;
      klog_puts("[TIMER] SIGEV_THREAD accepted without thread "
                "delivery; timer arms, no callback runs\n");
    }
  }

  struct thread *cur = sched_get_current();
  if (!cur)
    return (uint64_t)-22;

  spinlock_acquire(&posix_timer_lock);
  int slot = -1;
  for (int i = 0; i < POSIX_TIMER_MAX; i++) {
    if (!posix_timers[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    spinlock_release(&posix_timer_lock);
    return (uint64_t)-11; // EAGAIN: too many timers
  }
  posix_timer_t *tm = &posix_timers[slot];
  tm->in_use = true;
  tm->owner_tgid = cur->tgid ? cur->tgid : cur->tid;
  tm->clockid = (uint32_t)clockid;
  tm->notify = notify;
  tm->signo = signo;
  tm->sival_ptr = sival;
  tm->notify_tid = tid;
  tm->interval_ms = 0;
  tm->expire_ms = 0;
  tm->overrun = 0;
  spinlock_release(&posix_timer_lock);

  uint64_t id = (uint64_t)(slot + 1);
  if (copy_to_user((void *)timerid_ptr, &id, sizeof(id)) != 0) {
    spinlock_acquire(&posix_timer_lock);
    tm->in_use = false;
    spinlock_release(&posix_timer_lock);
    return (uint64_t)-14;
  }
  return 0;
}

static bool itimerspec_to_ms(const struct k_itimerspec *in, uint64_t *i_ms,
                             uint64_t *v_ms) {
  if (in->interval_nsec >= 1000000000ULL || in->value_nsec >= 1000000000ULL)
    return false;
  if (in->interval_sec > (UINT64_MAX - 999) / 1000 ||
      in->value_sec > (UINT64_MAX - 999) / 1000)
    return false;
  uint64_t ims = in->interval_sec * 1000 + in->interval_nsec / 1000000ULL;
  uint64_t vms = in->value_sec * 1000 + in->value_nsec / 1000000ULL;
  // Sub-millisecond but nonzero values must still arm the timer.
  if (ims == 0 && (in->interval_sec || in->interval_nsec))
    ims = 1;
  if (vms == 0 && (in->value_sec || in->value_nsec))
    vms = 1;
  *i_ms = ims;
  *v_ms = vms;
  return true;
}

static uint64_t sys_timer_settime(uint64_t timerid, uint64_t flags,
                                  uint64_t new_ptr, uint64_t old_ptr,
                                  uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (flags & ~(uint64_t)TIMER_ABSTIME)
    return (uint64_t)-22;
  posix_timer_t *tm = timer_lookup(timerid);
  if (!tm)
    return (uint64_t)-22;
  struct thread *cur = sched_get_current();
  uint32_t me = cur ? (cur->tgid ? cur->tgid : cur->tid) : 0;
  if (!cur || tm->owner_tgid != me)
    return (uint64_t)-22;
  if (!new_ptr || !vmm_is_user_addr_range_valid(new_ptr, sizeof(struct k_itimerspec)))
    return (uint64_t)-14;
  if (old_ptr && !vmm_is_user_addr_range_writable(old_ptr, sizeof(struct k_itimerspec)))
    return (uint64_t)-14;

  // Snapshot old value first (Linux allows old == new aliasing).
  uint64_t now_ms = lapic_timer_get_ms();
  struct k_itimerspec old = {0};
  spinlock_acquire(&posix_timer_lock);
  uint64_t rem = tm->expire_ms > now_ms ? tm->expire_ms - now_ms : 0;
  if (tm->expire_ms && rem == 0)
    rem = 1;
  old.interval_sec = tm->interval_ms / 1000;
  old.interval_nsec = (tm->interval_ms % 1000) * 1000000ULL;
  if (!tm->expire_ms) {
    old.value_sec = 0;
    old.value_nsec = 0;
  } else {
    old.value_sec = rem / 1000;
    old.value_nsec = (rem % 1000) * 1000000ULL;
  }
  spinlock_release(&posix_timer_lock);
  if (old_ptr && copy_to_user((void *)old_ptr, &old, sizeof(old)) != 0)
    return (uint64_t)-14;

  struct k_itimerspec in;
  if (copy_from_user(&in, (const void *)new_ptr, sizeof(in)) != 0)
    return (uint64_t)-14;
  uint64_t i_ms = 0, v_ms = 0;
  if (!itimerspec_to_ms(&in, &i_ms, &v_ms))
    return (uint64_t)-22;

  spinlock_acquire(&posix_timer_lock);
  if (!tm->in_use || tm->owner_tgid != me) {
    spinlock_release(&posix_timer_lock);
    return (uint64_t)-22;
  }
  tm->interval_ms = i_ms;
  tm->overrun = 0;
  if (in.value_sec == 0 && in.value_nsec == 0) {
    tm->expire_ms = 0;
  } else if (flags & TIMER_ABSTIME) {
    uint64_t now_ns = clock_now_ns(tm->clockid);
    if (now_ns == UINT64_MAX) {
      spinlock_release(&posix_timer_lock);
      return (uint64_t)-22;
    }
    __uint128_t target =
        (__uint128_t)in.value_sec * 1000000000ULL + in.value_nsec;
    __uint128_t now128 = now_ns;
    if (target <= now128) {
      tm->expire_ms = now_ms; // fire on next tick
    } else {
      uint64_t delay_ms =
          (uint64_t)((target - now128 + 999999ULL) / 1000000ULL);
      if (!delay_ms)
        delay_ms = 1;
      tm->expire_ms = now_ms + delay_ms;
    }
  } else {
    tm->expire_ms = now_ms + (v_ms ? v_ms : 1);
  }
  timer_update_earliest_locked();
  spinlock_release(&posix_timer_lock);
  return 0;
}

static uint64_t sys_timer_gettime(uint64_t timerid, uint64_t curr_ptr,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  posix_timer_t *tm = timer_lookup(timerid);
  if (!tm)
    return (uint64_t)-22;
  struct thread *cur = sched_get_current();
  uint32_t me = cur ? (cur->tgid ? cur->tgid : cur->tid) : 0;
  if (!cur || tm->owner_tgid != me)
    return (uint64_t)-22;
  if (!curr_ptr || !vmm_is_user_addr_range_writable(curr_ptr, sizeof(struct k_itimerspec)))
    return (uint64_t)-14;
  uint64_t now_ms = lapic_timer_get_ms();
  struct k_itimerspec out;
  spinlock_acquire(&posix_timer_lock);
  out.interval_sec = tm->interval_ms / 1000;
  out.interval_nsec = (tm->interval_ms % 1000) * 1000000ULL;
  if (!tm->expire_ms) {
    out.value_sec = 0;
    out.value_nsec = 0;
  } else if (tm->expire_ms <= now_ms) {
    out.value_sec = 0;
    out.value_nsec = 1; // expired, delivery pending
  } else {
    uint64_t rem = tm->expire_ms - now_ms;
    out.value_sec = rem / 1000;
    out.value_nsec = (rem % 1000) * 1000000ULL;
  }
  spinlock_release(&posix_timer_lock);
  if (copy_to_user((void *)curr_ptr, &out, sizeof(out)) != 0)
    return (uint64_t)-14;
  return 0;
}

static uint64_t sys_timer_getoverrun(uint64_t timerid, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  posix_timer_t *tm = timer_lookup(timerid);
  if (!tm)
    return (uint64_t)-22;
  struct thread *cur = sched_get_current();
  uint32_t me = cur ? (cur->tgid ? cur->tgid : cur->tid) : 0;
  if (!cur || tm->owner_tgid != me)
    return (uint64_t)-22;
  spinlock_acquire(&posix_timer_lock);
  uint32_t ov = tm->overrun;
  spinlock_release(&posix_timer_lock);
  return ov;
}

static uint64_t sys_timer_delete(uint64_t timerid, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  posix_timer_t *tm = timer_lookup(timerid);
  if (!tm)
    return (uint64_t)-22;
  struct thread *cur = sched_get_current();
  uint32_t me = cur ? (cur->tgid ? cur->tgid : cur->tid) : 0;
  if (!cur || tm->owner_tgid != me)
    return (uint64_t)-22;
  spinlock_acquire(&posix_timer_lock);
  tm->in_use = false;
  tm->expire_ms = 0;
  timer_update_earliest_locked();
  spinlock_release(&posix_timer_lock);
  return 0;
}

static uint64_t sys_clock_settime(uint64_t clockid, uint64_t tp_ptr,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!clockid_valid((uint32_t)clockid))
    return (uint64_t)-22;
  if (!tp_ptr || !vmm_is_user_addr_range_valid(tp_ptr, 16))
    return (uint64_t)-14;
  return (uint64_t)-1; // EPERM: no RTC-write path (mirrors settimeofday)
}

void syscall_register_timer(void) {
  syscall_register(SYS_TIMER_CREATE, sys_timer_create);
  syscall_register(SYS_TIMER_SETTIME, sys_timer_settime);
  syscall_register(SYS_TIMER_GETTIME, sys_timer_gettime);
  syscall_register(SYS_TIMER_GETOVERRUN, sys_timer_getoverrun);
  syscall_register(SYS_TIMER_DELETE, sys_timer_delete);
  syscall_register(SYS_CLOCK_SETTIME, sys_clock_settime);
}
