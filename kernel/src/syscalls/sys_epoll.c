// Epoll Syscall Implementation

// Implements epoll_create1, epoll_ctl, epoll_wait, and epoll_pwait syscalls

#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../socket/epoll.h"
#include "syscall.h"
#include <stdint.h>
#include "arch/uaccess.h"

// Helper: Validate user pointer
static bool epoll_user_ptr(uint64_t ptr) {
  // User-space addresses are in lower half of address space
  return ptr < 0x0000800000000000ULL && ptr != 0;
}

// sys_epoll_create: Create epoll instance (deprecated interface)
static uint64_t sys_epoll_create(uint64_t size, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  // size is ignored (historical parameter)
  (void)size;

  // Create epoll instance
  eventpoll_t *ep = epoll_create();
  if (!ep) {
    return (uint64_t)-12; // ENOMEM
  }

  // Allocate FD
  int fd = epoll_alloc_fd(ep);
  if (fd < 0) {
    epoll_put(ep);
    return (uint64_t)-24; // EMFILE
  }

  return (uint64_t)fd;
}

/* FD_FLAGS_CLOEXEC_BIT must match the definition in sys_io.c */
#define EPOLL_FD_FLAGS_CLOEXEC_BIT (1u << 24)

// sys_epoll_create1: Create epoll instance with flags
static uint64_t sys_epoll_create1(uint64_t flags, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  // Validate flags — only EPOLL_CLOEXEC (0x80000) is valid
  if (flags & ~EPOLL_CLOEXEC) {
    return (uint64_t)-22; // EINVAL
  }

  // Create epoll instance
  eventpoll_t *ep = epoll_create();
  if (!ep) {
    return (uint64_t)-12; // ENOMEM
  }

  // Allocate FD
  int fd = epoll_alloc_fd(ep);
  if (fd < 0) {
    epoll_put(ep);
    return (uint64_t)-24; // EMFILE
  }

  // Apply EPOLL_CLOEXEC: set FD_CLOEXEC on the file descriptor.
  // We use the same bit (bit 24) that sys_io.c uses for FD_FLAGS_CLOEXEC_BIT.
  if (flags & EPOLL_CLOEXEC) {
    struct thread *t = sched_get_current();
    if (t && fd < MAX_FDS) {
      t->fd_flags[fd] |= EPOLL_FD_FLAGS_CLOEXEC_BIT;
    }
  }

  return (uint64_t)fd;
}

// sys_epoll_ctl: Control epoll instance
static uint64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                              uint64_t event_ptr, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  // Get epoll instance
  eventpoll_t *ep = epoll_from_fd((int)epfd);
  if (!ep) {
    return (uint64_t)-9; // EBADF
  }

  // Validate FD
  if ((int)fd < 0) {
    return (uint64_t)-9; // EBADF
  }

  // Handle operation
  switch (op) {
  case EPOLL_CTL_ADD: {
    if (!epoll_user_ptr(event_ptr)) {
      return (uint64_t)-14; // EFAULT
    }

    struct epoll_event *event = (struct epoll_event *)event_ptr;
    int ret = epoll_ctl_add(ep, (int)fd, event);
    return (uint64_t)ret;
  }

  case EPOLL_CTL_DEL: {
    int ret = epoll_ctl_del(ep, (int)fd);
    return (uint64_t)ret;
  }

  case EPOLL_CTL_MOD: {
    if (!epoll_user_ptr(event_ptr)) {
      return (uint64_t)-14; // EFAULT
    }

    struct epoll_event *event = (struct epoll_event *)event_ptr;
    int ret = epoll_ctl_mod(ep, (int)fd, event);
    return (uint64_t)ret;
  }

  default:
    return (uint64_t)-22; // EINVAL
  }
}

// sys_epoll_wait: Wait for events
static uint64_t sys_epoll_wait(uint64_t epfd, uint64_t events_ptr,
                               uint64_t maxevents, uint64_t timeout_ms,
                               uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  // Get epoll instance
  eventpoll_t *ep = epoll_from_fd((int)epfd);
  if (!ep) {
    return (uint64_t)-9; // EBADF
  }

  // Validate parameters
  if (!epoll_user_ptr(events_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if ((int)maxevents <= 0) {
    return (uint64_t)-22; // EINVAL
  }

  struct epoll_event *events = (struct epoll_event *)events_ptr;

  // Call implementation
  int ret = epoll_wait_impl(ep, events, (int)maxevents, (int)timeout_ms);
  return (uint64_t)ret;
}

/* Shared pwait path: Linux uses an eight-byte kernel signal set on x86_64. */
static uint64_t epoll_pwait_common(uint64_t epfd, uint64_t events_ptr,
                                    uint64_t maxevents, int64_t timeout_ms,
                                    uint64_t sigmask_ptr, uint64_t sigsetsize) {
  eventpoll_t *ep = epoll_from_fd((int)epfd);
  if (!ep)
    return (uint64_t)-9;
  if (!epoll_user_ptr(events_ptr))
    return (uint64_t)-14;
  if ((int)maxevents <= 0)
    return (uint64_t)-22;

  struct thread *current = sched_get_current();
  uint64_t old_mask = 0, new_mask = 0;
  if (sigmask_ptr) {
    if (sigsetsize != sizeof(new_mask))
      return (uint64_t)-22;
    if (!current || !epoll_user_ptr(sigmask_ptr) ||
        copy_from_user(&new_mask, (void *)sigmask_ptr, sizeof(new_mask)))
      return (uint64_t)-14;
    new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    old_mask = current->signal_mask;
    current->signal_mask = new_mask;
  }
  int ret = epoll_wait_impl(ep, (struct epoll_event *)events_ptr,
                            (int)maxevents, timeout_ms);
  if (sigmask_ptr)
    current->signal_mask = old_mask;
  return (uint64_t)ret;
}

static uint64_t sys_epoll_pwait(uint64_t epfd, uint64_t events_ptr,
                                uint64_t maxevents, uint64_t timeout_ms,
                                uint64_t sigmask_ptr, uint64_t sigsetsize) {
  int timeout = (int)timeout_ms;
  return epoll_pwait_common(epfd, events_ptr, maxevents,
                            timeout < 0 ? -1 : timeout,
                            sigmask_ptr, sigsetsize);
}

/* epoll_pwait2 supplies a relative signed timespec, NULL means infinite.
 * The scheduler clock has millisecond resolution; round up, never expire early. */
static uint64_t sys_epoll_pwait2(uint64_t epfd, uint64_t events_ptr,
                                 uint64_t maxevents, uint64_t timeout_ptr,
                                 uint64_t sigmask_ptr, uint64_t sigsetsize) {
  int64_t timeout_ms = -1;
  if (timeout_ptr) {
    struct { int64_t sec, nsec; } timeout;
    if (!epoll_user_ptr(timeout_ptr) ||
        copy_from_user(&timeout, (void *)timeout_ptr, sizeof(timeout)))
      return (uint64_t)-14;
    if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000)
      return (uint64_t)-22;
    int64_t fraction = (timeout.nsec + 999999) / 1000000;
    timeout_ms = timeout.sec > (INT64_MAX - fraction) / 1000
                     ? INT64_MAX : timeout.sec * 1000 + fraction;
  }
  return epoll_pwait_common(epfd, events_ptr, maxevents, timeout_ms,
                            sigmask_ptr, sigsetsize);
}

// Register epoll syscalls
void syscall_register_epoll(void) {
  syscall_register(SYS_EPOLL_CREATE, sys_epoll_create);
  syscall_register(SYS_EPOLL_CREATE1, sys_epoll_create1);
  syscall_register(SYS_EPOLL_CTL, sys_epoll_ctl);
  syscall_register(SYS_EPOLL_WAIT, sys_epoll_wait);
  syscall_register(SYS_EPOLL_PWAIT, sys_epoll_pwait);
  syscall_register(SYS_EPOLL_PWAIT2, sys_epoll_pwait2);

  klog_puts("[OK] Epoll syscalls registered\n");
}
