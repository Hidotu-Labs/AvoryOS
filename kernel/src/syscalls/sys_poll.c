// Poll/Select Syscalls: poll, ppoll, select, pselect6
#include "../apic/lapic_timer.h"
#include "../include/arch/uaccess.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

/* Safety-net rescan for a blocking poll(): wait queues drive normal wakeups,
 * this only bounds how long a producer that never signals one can go
 * unnoticed.  Was 10 ticks, i.e. a full re-scan of every fd roughly every
 * 10 ms (~100/s) for every blocked poll(). */
#define POLL_SAFETY_NET_MS 1000

struct pollfd {
  int fd;
  short events;
  short revents;
};

/* Check all fds for readiness, return count of ready fds. */
static int poll_check_fds(struct pollfd *fds, uint64_t nfds, struct thread *t) {
  int ready = 0;
  for (uint64_t i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    fds[i].revents = 0;
    if (fd < 0)
      continue;
    if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd])) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    int ret = vfs_poll(t->fds[fd], fds[i].events);
    /* poll handlers may describe every currently ready direction. Only
     * ERR/HUP/NVAL bypass the caller's interest mask. */
    if (ret > 0)
      ret &= (unsigned short)fds[i].events | POLLERR | POLLHUP | POLLNVAL;
    if (ret < 0) {
      fds[i].revents = POLLNVAL;
      ready++;
    } else if (ret > 0) {
      fds[i].revents = (short)ret;
      ready++;
    }
  }
  return ready;
}

static uint64_t do_poll(struct pollfd *fds, uint64_t nfds,
                        uint64_t timeout_ms) {
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  /* Snapshot the polled set for /proc/PID/syscall spin diagnosis. Runs in
   * the target's own context, so fd tables can be read directly. */
  {
    uint64_t n = nfds < 8 ? nfds : 8;
    for (uint64_t i = 0; i < 8; i++) {
      if (i < n && fds) {
        int fd = fds[i].fd;
        t->last_poll_fds[i] = fd;
        t->last_poll_events[i] = fds[i].events;
        const char *p = "?";
        if (FD_IN_RANGE(t, fd)) {
          if (t->fd_paths && t->fd_paths[fd] && t->fd_paths[fd]->value[0])
            p = t->fd_paths[fd]->value;
          else if (t->fds && fd_is_valid_node(t->fds[fd]))
            p = t->fds[fd]->name;
        }
        uint64_t j = 0;
        while (j < 47 && p[j]) {
          t->last_poll_paths[i][j] = p[j];
          j++;
        }
        t->last_poll_paths[i][j] = '\0';
      } else {
        t->last_poll_fds[i] = -1;
        t->last_poll_events[i] = 0;
        t->last_poll_paths[i][0] = '\0';
      }
    }
  }

  /* Fast path: check immediately. */
  int ready = poll_check_fds(fds, nfds, t);
  if (ready > 0 || timeout_ms == 0)
    return (uint64_t)ready;

  /* Slow path: block on wait queues while honoring the timeout. */
  uint64_t deadline = (timeout_ms != (uint64_t)-1)
                          ? lapic_timer_get_ticks() + timeout_ms
                          : (uint64_t)-1;

  wait_queue_entry_t wq_stack[64];
  wait_queue_t *wq_ptrs_stack[64];
  wait_queue_entry_t *wq_entries = wq_stack;
  wait_queue_t **wq_ptrs = wq_ptrs_stack;
  size_t max_wq = 64;

  if (nfds > 64) {
    size_t alloc_count = (nfds > MAX_FDS) ? MAX_FDS : (size_t)nfds;
    wait_queue_entry_t *dyn_entries = kmalloc(alloc_count * sizeof(wait_queue_entry_t));
    wait_queue_t **dyn_ptrs = kmalloc(alloc_count * sizeof(wait_queue_t *));
    if (dyn_entries && dyn_ptrs) {
      wq_entries = dyn_entries;
      wq_ptrs = dyn_ptrs;
      max_wq = alloc_count;
    } else {
      if (dyn_entries) kfree(dyn_entries);
      if (dyn_ptrs) kfree(dyn_ptrs);
    }
  }

  while (ready == 0) {
    if (thread_has_pending_signal(t)) {
      ready = -4; // -EINTR
      break;
    }

    if (deadline != (uint64_t)-1 && lapic_timer_get_ticks() >= deadline)
      break;

    size_t wq_count = 0;
    size_t valid_fds = 0;
    /* A timer must not park us before the wake entries and fallback deadline
     * exist. Producers may still race registration; the readiness recheck
     * below catches events that arrived before their entry was linked. */
    hal_irq_state_t irq_flags = hal_irq_save();
    uint64_t safety_at = lapic_timer_get_ticks() + POLL_SAFETY_NET_MS;
    t->wakeup_ticks = deadline < safety_at ? deadline : safety_at;
    t->state = THREAD_BLOCKED;
    for (uint64_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (FD_IN_RANGE(t, fd) && fd_is_valid_node(t->fds[fd])) {
        valid_fds++;
        if (wq_count < max_wq && t->fds[fd]->wait_queue) {
          wait_queue_t *wq = (wait_queue_t *)t->fds[fd]->wait_queue;
          wq_entries[wq_count].thread = t;
          wq_entries[wq_count].next = NULL;
          wq_entries[wq_count].wq = NULL;
          wq_entries[wq_count].thread_next = NULL;
          wq_entries[wq_count].thread_prev = NULL;
          wq_ptrs[wq_count] = wq;
          wait_queue_add(wq, &wq_entries[wq_count]);
          wq_count++;
        }
      }
    }

    hal_irq_restore(irq_flags);

    ready = poll_check_fds(fds, nfds, t);
    if (ready > 0) {
      t->state = THREAD_RUNNING;
      t->wakeup_ticks = 0;
      for (size_t i = 0; i < wq_count; i++) {
        wait_queue_remove(wq_ptrs[i], &wq_entries[i]);
      }
      break;
    }

    sched_yield();

    t->state = THREAD_RUNNING;
    t->wakeup_ticks = 0;

    for (size_t i = 0; i < wq_count; i++) {
      wait_queue_remove(wq_ptrs[i], &wq_entries[i]);
    }

    if (thread_has_pending_signal(t)) {
      ready = -4; // -EINTR
      break;
    }

    ready = poll_check_fds(fds, nfds, t);
  }

  if (wq_entries != wq_stack) {
    kfree(wq_entries);
    kfree(wq_ptrs);
  }

  return (uint64_t)ready;
}

static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  /* Snapshot userspace once. VMA bookkeeping is not a substitute for a
   * fault-safe access check, and user fds may change while poll sleeps. */
  if (nfds > MAX_FDS)
    return (uint64_t)-22;
  if (nfds == 0)
    return do_poll(NULL, 0, timeout_ms);
  size_t bytes = (size_t)nfds * sizeof(struct pollfd);
  if (!is_user_range((void *)fds_ptr, bytes))
    return (uint64_t)-14;
  struct pollfd *fds = kmalloc(bytes);
  if (!fds)
    return (uint64_t)-12;
  if (copy_from_user(fds, (void *)fds_ptr, bytes)) {
    kfree(fds);
    return (uint64_t)-14;
  }
  uint64_t result = do_poll(fds, nfds, timeout_ms);
  if ((int64_t)result >= 0) {
    for (size_t i = 0; i < nfds; i++) {
      void *out = (void *)(fds_ptr + i * sizeof(struct pollfd) +
                          __builtin_offsetof(struct pollfd, revents));
      if (copy_to_user(out, &fds[i].revents, sizeof(fds[i].revents))) {
        result = (uint64_t)-14;
        break;
      }
    }
  }
  kfree(fds);
  return result;
}

static uint64_t sys_ppoll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ptr,
                          uint64_t sigmask, uint64_t sigsetsize, uint64_t a5) {
  (void)a5;

  uint64_t timeout_ms = (uint64_t)-1;
  int64_t seconds = -1, nanoseconds = 0;
  if (timeout_ptr) {
    struct { int64_t tv_sec, tv_nsec; } ts;
    struct { int32_t tv_sec, tv_nsec; } ts32;
    struct thread *caller = sched_get_current();
    bool compat32 = caller && caller->seccomp_compat_dispatch &&
                    caller->mm && caller->mm->compat32;
    if (compat32) {
      if (!is_user_range((void *)timeout_ptr, sizeof(ts32)) ||
          copy_from_user(&ts32, (void *)timeout_ptr, sizeof(ts32)))
        return (uint64_t)-14;
      ts.tv_sec = ts32.tv_sec;
      ts.tv_nsec = ts32.tv_nsec;
    } else if (!is_user_range((void *)timeout_ptr, sizeof(ts)) ||
               copy_from_user(&ts, (void *)timeout_ptr, sizeof(ts))) {
      return (uint64_t)-14;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
      return (uint64_t)-22;
    seconds = ts.tv_sec;
    nanoseconds = ts.tv_nsec;
    /* Round up sub-millisecond waits and saturate long valid timeouts. */
    uint64_t fraction = ((uint64_t)nanoseconds + 999999) / 1000000;
    const uint64_t max_ms = (uint64_t)INT64_MAX;
    timeout_ms = (uint64_t)seconds > (max_ms - fraction) / 1000
                   ? max_ms : (uint64_t)seconds * 1000 + fraction;
  }

  struct thread *t = sched_get_current();
  if (t) {
    t->last_poll_sec = seconds;
    t->last_poll_nsec = nanoseconds;
    t->last_poll_nfds = nfds;
  }
  uint64_t old_mask = t ? t->signal_mask : 0;
  bool swapped = false;
  if (sigmask) {
    if (sigsetsize != 8)
      return (uint64_t)-22;
    uint64_t new_mask;
    if (!is_user_range((void *)sigmask, sizeof(new_mask)) ||
        copy_from_user(&new_mask, (void *)sigmask, sizeof(new_mask)))
      return (uint64_t)-14;
    new_mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
    if (t) {
      t->signal_mask = new_mask;
      swapped = true;
    }
  }
  uint64_t ret = sys_poll(fds_ptr, nfds, timeout_ms, 0, 0, 0);
  if (swapped)
    t->signal_mask = old_mask;
  return ret;
}

static uint64_t do_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                            uint64_t exceptfds, uint64_t timeout_ms) {
  size_t set_size = (nfds + 7) / 8;
  if (nfds == 0)
    return 0;
  if (nfds > MAX_FDS)
    nfds = MAX_FDS;
  struct pollfd *pfds = kmalloc(nfds * sizeof(*pfds));
  if (!pfds)
    return (uint64_t)-12; // ENOMEM
  uint64_t p_count = 0;
  for (int fd = 0; fd < (int)nfds && p_count < MAX_FDS; fd++) {
    short events = 0;
    if (readfds && (((uint64_t *)readfds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0001;
    if (writefds && (((uint64_t *)writefds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0004;
    if (events) {
      pfds[p_count].fd = fd;
      pfds[p_count].events = events;
      pfds[p_count].revents = 0;
      p_count++;
    }
  }

  if (p_count == 0) {
    if (timeout_ms != (uint64_t)-1 && timeout_ms > 0) {
      struct thread *t = sched_get_current();
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    if (readfds)
      memset((void *)readfds, 0, set_size);
    if (writefds)
      memset((void *)writefds, 0, set_size);
    if (exceptfds)
      memset((void *)exceptfds, 0, set_size);
    kfree(pfds);
    return 0;
  }

  uint64_t ready = do_poll(pfds, p_count, timeout_ms);
  if ((int64_t)ready < 0) {
    kfree(pfds);
    return ready;
  }

  if (readfds)
    memset((void *)readfds, 0, set_size);
  if (writefds)
    memset((void *)writefds, 0, set_size);
  if (exceptfds)
    memset((void *)exceptfds, 0, set_size);

  uint64_t res_count = 0;
  for (uint64_t i = 0; i < p_count; i++) {
    if (pfds[i].revents == 0)
      continue;
    int fd = pfds[i].fd;
    if (pfds[i].revents & (0x0001 | 0x0010 | 0x0008)) {
      if (readfds)
        ((uint64_t *)readfds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
    if (pfds[i].revents & 0x0004) {
      if (writefds)
        ((uint64_t *)writefds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
  }
  kfree(pfds);
  return res_count;
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                             uint64_t exceptfds, uint64_t timeout,
                             uint64_t sigmask) {
  if (nfds > 1024) return (uint64_t)-22;
  struct thread *t = sched_get_current();
  bool compat = t && t->seccomp_compat_dispatch;
  size_t word = compat ? 4 : 8;
  size_t bytes = ((nfds + word * 8 - 1) / (word * 8)) * word;
  uint64_t sets[3][16] = {{0}};
  uint64_t ptrs[3] = {readfds, writefds, exceptfds};
  for (int i=0; i<3; i++)
    if (ptrs[i] && copy_from_user(sets[i], (void *)ptrs[i], bytes)) return (uint64_t)-14;
  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout) {
    int64_t sec, ns;
    if (compat) {
      int32_t ts[2];
      if (copy_from_user(ts, (void *)timeout, sizeof(ts))) return (uint64_t)-14;
      sec=ts[0]; ns=ts[1];
    } else {
      int64_t ts[2];
      if (copy_from_user(ts, (void *)timeout, sizeof(ts))) return (uint64_t)-14;
      sec=ts[0]; ns=ts[1];
    }
    if (sec<0 || ns<0 || ns>=1000000000) return (uint64_t)-22;
    timeout_ms = sec > INT64_MAX/1000 ? INT64_MAX : (uint64_t)sec*1000+(ns+999999)/1000000;
  }
  uint64_t old_mask=t ? t->signal_mask : 0;
  bool swapped=false;
  if (sigmask) {
    uint64_t maskptr, len;
    if (compat) {
      uint32_t pair[2];
      if (copy_from_user(pair, (void *)sigmask, sizeof(pair))) return (uint64_t)-14;
      maskptr=pair[0]; len=pair[1];
    } else {
      uint64_t pair[2];
      if (copy_from_user(pair, (void *)sigmask, sizeof(pair))) return (uint64_t)-14;
      maskptr=pair[0]; len=pair[1];
    }
    if (maskptr) {
      uint64_t mask;
      if (len!=8) return (uint64_t)-22;
      if (copy_from_user(&mask, (void *)maskptr, 8)) return (uint64_t)-14;
      if (t) { t->signal_mask=mask & ~((1ULL<<8)|(1ULL<<18)); swapped=true; }
    }
  }
  uint64_t ret=do_pselect6(nfds, readfds ? (uint64_t)sets[0] : 0,
      writefds ? (uint64_t)sets[1] : 0, exceptfds ? (uint64_t)sets[2] : 0, timeout_ms);
  if (swapped) t->signal_mask=old_mask;
  if ((int64_t)ret>=0)
    for (int i=0; i<3; i++)
      if (ptrs[i] && copy_to_user((void *)ptrs[i], sets[i], bytes)) return (uint64_t)-14;
  return ret;
}

static uint64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                           uint64_t exceptfds, uint64_t timeout, uint64_t a5) {
  (void)a5;
  uint64_t timeout_ms = (uint64_t)-1;

  if (timeout && is_user_ptr(timeout)) {
    struct {
      int64_t tv_sec;
      int64_t tv_usec;
    } *tv = (void *)timeout;
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
}

// Register poll/select syscalls
void syscall_register_poll(void) {
  syscall_register(SYS_POLL, sys_poll);
  syscall_register(SYS_PPOLL, sys_ppoll);
  syscall_register(SYS_PSELECT6, sys_pselect6);
  syscall_register(SYS_SELECT, sys_select);

  klog_puts("[OK] Poll/Select syscalls registered\n");
}
