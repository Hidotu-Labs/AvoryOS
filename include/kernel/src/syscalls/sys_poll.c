// Poll/Select Syscalls: poll, ppoll, select, pselect6
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "syscall.h"
#include <stdint.h>

// User-space pointer validation: reject kernel/HHDM addresses
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_ptr(uint64_t addr) {
  return addr != 0 && addr <= USER_ADDR_MAX;
}

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
    if (fd >= MAX_FDS || !t->fds[fd]) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    int ret = vfs_poll(t->fds[fd], fds[i].events);
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
    for (uint64_t i = 0; i < nfds && wq_count < max_wq; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd] && t->fds[fd]->wait_queue) {
        wait_queue_t *wq = (wait_queue_t *)t->fds[fd]->wait_queue;
        wq_entries[wq_count].thread = t;
        wq_entries[wq_count].next = NULL;
        wq_ptrs[wq_count] = wq;
        wait_queue_add(wq, &wq_entries[wq_count]);
        wq_count++;
      }
    }

    t->state = THREAD_BLOCKED;
    if (deadline != (uint64_t)-1) {
      t->wakeup_ticks = deadline;
    } else {
      t->wakeup_ticks = lapic_timer_get_ticks() + 10;
    }

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

  struct thread *t = sched_get_current();
  if (nfds == 0) {
    if (timeout_ms > 0 && timeout_ms != (uint64_t)-1) {
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    return 0;
  }
  if (!is_user_ptr(fds_ptr) ||
      !vmm_is_user_addr_range_valid(fds_ptr, nfds * sizeof(struct pollfd)))
    return (uint64_t)-14;

  struct pollfd *p = (struct pollfd *)fds_ptr;
  return do_poll(p, nfds, timeout_ms);
}

static uint64_t sys_ppoll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ptr,
                          uint64_t sigmask, uint64_t sigsetsize, uint64_t a5) {
  (void)sigmask;
  (void)sigsetsize;
  (void)a5;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout_ptr && is_user_ptr(timeout_ptr)) {
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout_ptr;
    if (!vmm_is_user_addr_range_valid(timeout_ptr, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return sys_poll(fds_ptr, nfds, timeout_ms, 0, 0, 0);
}

static uint64_t do_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                            uint64_t exceptfds, uint64_t timeout_ms) {
  size_t set_size = (nfds + 7) / 8;
  struct pollfd pfds[MAX_FDS];
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
    return 0;
  }

  uint64_t ready = do_poll(pfds, p_count, timeout_ms);
  if ((int64_t)ready < 0)
    return ready;

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
  return res_count;
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                             uint64_t exceptfds, uint64_t timeout,
                             uint64_t sigmask) {
  (void)sigmask;
  if (nfds > 1024)
    return (uint64_t)-22;
  size_t set_size = (nfds + 7) / 8;
  if (readfds && (!is_user_ptr(readfds) ||
                  !vmm_is_user_addr_range_valid(readfds, set_size)))
    return (uint64_t)-14;
  if (writefds && (!is_user_ptr(writefds) ||
                   !vmm_is_user_addr_range_valid(writefds, set_size)))
    return (uint64_t)-14;
  if (exceptfds && (!is_user_ptr(exceptfds) ||
                    !vmm_is_user_addr_range_valid(exceptfds, set_size)))
    return (uint64_t)-14;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout && is_user_ptr(timeout)) {
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
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