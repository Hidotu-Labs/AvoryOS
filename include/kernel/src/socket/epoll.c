// Epoll Implementation

// Implements the epoll API for event multiplexing

#include "epoll.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "socket.h"
#include <stdint.h>

// Global Epoll Instance Table and Memory Pools
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
static eventpoll_t epoll_pool[EPOLL_MAX_INSTANCES];
static vfs_node_t epoll_vfs_pool[EPOLL_MAX_INSTANCES];
static uint64_t epoll_free_bitmap = ~0ULL; // 1 = free, 0 = in use
static spinlock_t epoll_table_lock = SPINLOCK_INIT;

#define EPOLL_RESCAN_INTERVAL_MS 100

static int epoll_table_alloc(void) {
  spinlock_acquire(&epoll_table_lock);
  if (epoll_free_bitmap == 0) {
    spinlock_release(&epoll_table_lock);
    return -1; // Table full
  }
  int idx = __builtin_ctzll(epoll_free_bitmap);
  epoll_free_bitmap &= ~(1ULL << idx);
  spinlock_release(&epoll_table_lock);
  return idx;
}

static void epoll_table_free(int idx) {
  if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
    return;

  spinlock_acquire(&epoll_table_lock);
  epoll_table[idx] = NULL;
  epoll_free_bitmap |= (1ULL << idx);
  spinlock_release(&epoll_table_lock);
}

// Epoll Item Management

static epitem_t *epitem_alloc(void) {
  epitem_t *epi = kmalloc(sizeof(epitem_t));
  if (!epi)
    return NULL;

  memset(epi, 0, sizeof(epitem_t));
  INIT_LIST_HEAD(&epi->rdllink);
  INIT_LIST_HEAD(&epi->fllink);
  INIT_LIST_HEAD(&epi->ep_node_link);
  spinlock_init(&epi->lock);
  return epi;
}

static void epitem_free(epitem_t *epi) {
  if (!epi)
    return;
  kfree(epi);
}

// Epoll Instance Creation/Destruction

eventpoll_t *epoll_create(void) {
  int idx = epoll_table_alloc();
  if (idx < 0) {
    klog_puts("[ERR] epoll: epoll table full\n");
    return NULL;
  }

  eventpoll_t *ep = &epoll_pool[idx];

  // Fast reset: item_count and watched items only reset if previously used
  if (ep->item_count > 0) {
    for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
      if (ep->items[i]) {
        epitem_free(ep->items[i]);
        ep->items[i] = NULL;
      }
    }
    ep->item_count = 0;
  } else {
    // Ensure array is null-initialized
    for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
      ep->items[i] = NULL;
    }
  }

  // Fast targeted field initialization
  ep->fd = -1;
  ep->vfs_node = NULL;
  ep->item_count = 0;
  ep->items_high = 0;
  INIT_LIST_HEAD(&ep->rdllist);
  ep->rdllist_count = 0;
  wait_queue_init(&ep->wq);
  spinlock_init(&ep->lock);
  ep->refcount = 1;

  epoll_table[idx] = ep;

  return ep;
}

void epoll_destroy(eventpoll_t *ep) {
  if (!ep) {
    klog_puts("[WARN] epoll_destroy: NULL instance\n");
    return;
  }

  /* A thread can be parked in epoll_wait_impl() on ep->wq.  Wake it and detach
   * its entry before the pool slot is reused, so the stale entry cannot be
   * attached to whatever instance reuses this slot next. */
  wait_queue_wake_all(&ep->wq);

  // Free all watched items and release the VFS references acquired by
  // EPOLL_CTL_ADD.
  if (ep->item_count > 0) {
    for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
      epitem_t *epi = ep->items[i];
      if (epi) {
        if (epi->on_ready_list) {
          list_del(&epi->rdllink);
          epi->on_ready_list = false;
        }
        if (epi->node) {
          spinlock_acquire(&epi->node->ep_lock);
          list_del(&epi->ep_node_link);
          spinlock_release(&epi->node->ep_lock);
          vfs_close(epi->node);
        }
        epitem_free(epi);
        ep->items[i] = NULL;
      }
    }
    ep->item_count = 0;
  }
  ep->items_high = 0;

  int idx = (int)(ep - epoll_pool);
  if (idx >= 0 && idx < EPOLL_MAX_INSTANCES) {
    epoll_table_free(idx);
  }
}

void epoll_get(eventpoll_t *ep) {
  if (!ep)
    return;
  __atomic_fetch_add(&ep->refcount, 1, __ATOMIC_ACQ_REL);
}

void epoll_put(eventpoll_t *ep) {
  if (!ep)
    return;

  if (__atomic_fetch_sub(&ep->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
    epoll_destroy(ep);
  }
}

// Helper: Check if FD has events

static uint32_t ep_check_events(epitem_t *epi) {
  if (!epi || !epi->node)
    return 0;

  // Get the registered events, excluding modifier flags
  uint32_t watch_mask =
      epi->registered_events &
      ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);

  // Call VFS poll to get current events
  int revents = vfs_poll(epi->node, watch_mask);

  // Mask to only requested events plus error/hangup
  return (uint32_t)revents & (watch_mask | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

// Helper: Add item to ready list

static void ep_add_to_ready_list(eventpoll_t *ep, epitem_t *epi) {
  spinlock_acquire(&ep->lock);

  bool was_empty = (ep->rdllist_count == 0);
  if (!epi->on_ready_list) {
    list_add_tail(&epi->rdllink, &ep->rdllist);
    epi->on_ready_list = true;
    ep->rdllist_count++;
  }

  // Wake up waiters while still holding ep->lock.
  // This closes the race: epoll_wait_impl sets THREAD_BLOCKED while holding
  // ep->lock, so if we wake here (also under ep->lock) we are guaranteed to
  // see the correct blocked state and the thread won't miss the wakeup.
  if (epi->exclusive) {
    wait_queue_wake_one(&ep->wq);
  } else {
    wait_queue_wake_all(&ep->wq);
  }

  spinlock_release(&ep->lock);

  // Propagate to any outer epoll instances watching this epoll fd.
  // This is required for nested epoll (epoll-in-epoll) to work: when this
  // epoll's ready list becomes non-empty, any outer epoll that has registered
  // this epoll's VFS node must be woken up.
  if (was_empty && ep->vfs_node) {
    epoll_notify_event(ep->vfs_node, EPOLLIN);
  }
}

// Helper: Remove item from ready list

static void ep_remove_from_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (!epi->on_ready_list)
    return;

  spinlock_acquire(&ep->lock);

  if (epi->on_ready_list) {
    list_del(&epi->rdllink);
    epi->on_ready_list = false;
    ep->rdllist_count--;
  }

  spinlock_release(&ep->lock);
}

// epoll_ctl Operations

int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  // Get current thread and VFS node for FD
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  if (fd >= MAX_FDS || !t->fds[fd]) {
    return -9; // EBADF
  }

  vfs_node_t *node = t->fds[fd];

  // Check if already registered for the same node
  if (ep->items[fd]) {
    epitem_t *old = ep->items[fd];
    if (old->node == node) {
      return -17; // EEXIST - same socket already registered
    }
    // Different node - fd was reused, remove stale item
    ep_remove_from_ready_list(ep, old);
    if (old->node) {
      spinlock_acquire(&old->node->ep_lock);
      list_del(&old->ep_node_link);
      spinlock_release(&old->node->ep_lock);
    }
    spinlock_acquire(&ep->lock);
    ep->items[fd] = NULL;
    ep->item_count--;
    spinlock_release(&ep->lock);
    vfs_close(old->node);
    epitem_free(old);
  }

  // Create epitem
  epitem_t *epi = epitem_alloc();
  if (!epi) {
    return -12; // ENOMEM
  }

  epi->fd = fd;
  epi->node = node;
  epi->event = *event;
  epi->ep = ep;
  epi->last_events = 0;
  epi->on_ready_list = false;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->oneshot_disabled = false;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events;

  // Keep the watched object alive until DEL or epoll destruction.
  vfs_open(node);

  // Add to epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = epi;
  ep->item_count++;
  if (fd >= ep->items_high)
    ep->items_high = fd + 1;
  spinlock_release(&ep->lock);

  // Fast notification link
  spinlock_acquire(&node->ep_lock);
  if (node->ep_watchers.next == NULL) {
    INIT_LIST_HEAD(&node->ep_watchers);
  }
  list_add_tail(&epi->ep_node_link, &node->ep_watchers);
  spinlock_release(&node->ep_lock);

  // Check for immediate events. For level-triggered mode this is the normal
  // path. For edge-triggered mode, Linux also fires an initial synthetic edge
  // on EPOLL_CTL_ADD if the fd already has data — without this, any data
  // queued before the fd was added to epoll would be silently lost.
  uint32_t cur_events = ep_check_events(epi);
  if (cur_events) {
    epi->last_events = cur_events;
    ep_add_to_ready_list(ep, epi);
  }

  return 0;
}

int epoll_ctl_del(eventpoll_t *ep, int fd) {
  if (!ep)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT

  // Remove from ready list if present
  ep_remove_from_ready_list(ep, epi);

  // Remove from epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = NULL;
  ep->item_count--;
  spinlock_release(&ep->lock);

  // Unlink from node
  if (epi->node) {
    spinlock_acquire(&epi->node->ep_lock);
    list_del(&epi->ep_node_link);
    spinlock_release(&epi->node->ep_lock);
  }

  // Release the watched object reference, then free the item.
  vfs_close(epi->node);
  epitem_free(epi);

  return 0;
}

int epoll_ctl_mod(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT

  // Update event mask
  spinlock_acquire(&epi->lock);
  epi->event = *event;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events; // Always update registered_events

  // Reset oneshot disabled state when re-arming via EPOLL_CTL_MOD
  if (epi->oneshot) {
    epi->oneshot_disabled = false;
  }
  spinlock_release(&epi->lock);

  // Remove from ready list and re-check
  ep_remove_from_ready_list(ep, epi);

  // Re-check events
  uint32_t events = ep_check_events(epi);
  if (events) {
    epi->last_events = events;
    ep_add_to_ready_list(ep, epi);
  }

  return 0;
}

// epoll_wait Implementation

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, int maxevents,
                    int timeout_ms) {
  if (!ep || !events || maxevents <= 0)
    return -22; // EINVAL

  int returned = 0;
  struct thread *current = sched_get_current();

  /* The wait entry is registered for each individual park, not for the whole
   * call: wakers unlink entries when they wake them (see sched/wait.c), so an
   * entry left linked across loop iterations would be gone after the first
   * wake and later events would never wake this thread again. */
  wait_queue_entry_t entry;
  entry.thread = current;
  entry.next = NULL;

  uint64_t deadline_ticks = 0;
  if (timeout_ms > 0 && timeout_ms != -1) {
    deadline_ticks = lapic_timer_get_ticks() + (uint64_t)timeout_ms;
  }

  while (returned == 0) {
    if (thread_has_pending_signal(current)) {
      returned = -4; // -EINTR
      break;
    }

    // 1. Collect pending items from ready list under ep->lock WITHOUT calling ep_check_events.
    epitem_t *candidates[64];
    int cand_count = 0;

    spinlock_acquire(&ep->lock);
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &ep->rdllist) {
      if (cand_count >= 64 || cand_count >= maxevents)
        break;
      epitem_t *epi = list_entry(pos, epitem_t, rdllink);
      list_del(&epi->rdllink);
      epi->on_ready_list = false;
      ep->rdllist_count--;
      candidates[cand_count++] = epi;
    }
    spinlock_release(&ep->lock);

    // 2. Poll candidates OUTSIDE of ep->lock to eliminate lock-inversion deadlocks.
    for (int i = 0; i < cand_count; i++) {
      epitem_t *epi = candidates[i];
      if (!epi)
        continue;

      uint32_t current_events = ep_check_events(epi);
      if (current_events && returned < maxevents) {
        events[returned].events = current_events;
        events[returned].data.u64 = epi->event.data.u64;
        returned++;

        if (epi->event.events & EPOLLET) {
          epi->last_events = current_events &
                             (EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM);
        }
        if (epi->oneshot) {
          epi->oneshot_disabled = true;
          epi->registered_events = 0;
        }
      }
    }

    if (returned > 0)
      break;

    // 3. Immediate return on non-blocking poll
    if (timeout_ms == 0)
      break;

    // 4. Check all watched items outside ep->lock for missed/level-triggered events
    int items_high = __atomic_load_n(&ep->items_high, __ATOMIC_RELAXED);
    for (int _i = 0; _i < items_high; _i++) {
      epitem_t *_epi = ep->items[_i];
      if (!_epi || _epi->on_ready_list)
        continue;
      if (_epi->oneshot && _epi->oneshot_disabled)
        continue;
      uint32_t _ev = ep_check_events(_epi);
      if (_ev) {
        spinlock_acquire(&ep->lock);
        if (!_epi->on_ready_list) {
          _epi->last_events = _ev;
          list_add_tail(&_epi->rdllink, &ep->rdllist);
          _epi->on_ready_list = true;
          ep->rdllist_count++;
        }
        spinlock_release(&ep->lock);
      }
    }

    // 5. Prepare to block under ep->lock
    spinlock_acquire(&ep->lock);

    // If ready list is non-empty, loop back immediately
    if (!list_empty(&ep->rdllist)) {
      spinlock_release(&ep->lock);
      continue;
    }

    current->state = THREAD_BLOCKED;
    if (timeout_ms > 0 && timeout_ms != -1) {
      current->wakeup_ticks = deadline_ticks;
    } else if (timeout_ms == -1) {
      current->wakeup_ticks = lapic_timer_get_ticks() + EPOLL_RESCAN_INTERVAL_MS;
    } else {
      current->wakeup_ticks = 0;
    }

    // Re-check ready list one last time before releasing lock
    if (!list_empty(&ep->rdllist)) {
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;
      spinlock_release(&ep->lock);
      continue;
    }

    /* Register under ep->lock, after the final check: a concurrent waker also
     * holds ep->lock, so it either adds to the ready list before this check
     * (we continue instead of blocking) or sees us in the queue and wakes us. */
    wait_queue_add(&ep->wq, &entry);

    spinlock_release(&ep->lock);

    // 6. Yield execution
    sched_yield();

    wait_queue_remove(&ep->wq, &entry);
    current->state = THREAD_RUNNING;
    current->wakeup_ticks = 0;

    if (thread_has_pending_signal(current)) {
      returned = -4; // -EINTR
      break;
    }

    // Check timeout
    if (timeout_ms > 0 && timeout_ms != -1) {
      if (deadline_ticks != 0 && lapic_timer_get_ticks() >= deadline_ticks) {
        break;
      }
    }
  }

  current->state = THREAD_RUNNING;
  current->wakeup_ticks = 0;

  return returned;
}

// Epoll FD Management

int epoll_alloc_fd(eventpoll_t *ep) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE

  int idx = (int)(ep - epoll_pool);
  vfs_node_t *node = &epoll_vfs_pool[idx];

  // Fast initialize pre-allocated VFS node
  node->name[0] = '\0';
  node->flags = FS_EPOLL | FS_PERSISTENT;
  node->inode = (uint32_t)(uint64_t)ep;
  node->device = ep;
  node->wait_queue = &ep->wq;
  node->refcount = 1;
  INIT_LIST_HEAD(&node->ep_watchers);
  spinlock_init(&node->ep_lock);

  // Set epoll VFS operations
  node->read = epoll_vfs_read;
  node->write = epoll_vfs_write;
  node->open = epoll_vfs_open;
  node->close = epoll_vfs_close;
  node->poll = epoll_vfs_poll;

  ep->fd = fd;
  ep->vfs_node = node; // Store for nested epoll propagation
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  return fd;
}

eventpoll_t *epoll_from_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return NULL;

  if (fd < 0 || fd >= MAX_FDS)
    return NULL;

  vfs_node_t *node = t->fds[fd];
  if (!node)
    return NULL;

  if ((node->flags & FS_TYPE_MASK) != FS_EPOLL)
    return NULL;

  return (eventpoll_t *)node->device;
}

int epoll_close_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return -9; // EBADF

  if (fd < 0 || fd >= MAX_FDS)
    return -9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (!node)
    return -9; // EBADF

  if ((node->flags & FS_TYPE_MASK) != FS_EPOLL)
    return -22; // EINVAL

  eventpoll_t *ep = (eventpoll_t *)node->device;

  // Clear FD
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;

  // Release epoll reference through VFS close
  if (node) {
    vfs_close(node);
  }

  // Release the initial reference from epoll_create()
  if (ep) {
    epoll_put(ep);
  }

  return 0;
}

// Epoll VFS Operations

uint32_t epoll_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not readable via read()
  return 0;
}

uint32_t epoll_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not writable via write()
  return 0;
}

void epoll_vfs_open(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_get(ep);
}

void epoll_vfs_close(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_put(ep);
}

int epoll_vfs_poll(struct vfs_node *node, int events) {
  if (!node)
    return POLLNVAL;

  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (!ep)
    return POLLNVAL;

  int revents = 0;

  // Check if ready list has items
  spinlock_acquire(&ep->lock);
  if (!list_empty(&ep->rdllist)) {
    if (events & POLLIN)
      revents |= POLLIN;
  }
  // Always writable (for poll purposes)
  if (events & POLLOUT)
    revents |= POLLOUT;
  spinlock_release(&ep->lock);

  return revents;
}

// Event Notification

void epoll_notify_event(struct vfs_node *node, uint32_t events) {
  if (!node || (uint64_t)node < 0xFFFF800000000000ULL)
    return;

  // Lazy-init check for node notification list
  if (node->ep_watchers.next == NULL || (uint64_t)node->ep_watchers.next < 0xFFFF800000000000ULL)
    return;

  spinlock_acquire(&node->ep_lock);
  struct list_head *pos, *n;
  list_for_each_safe(pos, n, &node->ep_watchers) {
    if ((uint64_t)pos < 0xFFFF800000000000ULL)
      break;
    epitem_t *epi = list_entry(pos, epitem_t, ep_node_link);
    if ((uint64_t)epi < 0xFFFF800000000000ULL)
      continue;
    eventpoll_t *ep = epi->ep;
    if (!ep || (uint64_t)ep < 0xFFFF800000000000ULL)
      continue;

    // Skip disabled oneshot items
    if (epi->oneshot && epi->oneshot_disabled)
      continue;

    uint32_t mask =
        events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    if (!mask)
      continue;

    // ep_add_to_ready_list now handles both adding to the ready list AND
    // waking up waiters atomically under ep->lock, eliminating the
    // missed-wakeup race.
    ep_add_to_ready_list(ep, epi);

    // EPOLLEXCLUSIVE: stop after waking the first matching watcher so only
    // one epoll instance is woken per event (prevents thundering herd).
    if (epi->exclusive)
      break;
  }
  spinlock_release(&node->ep_lock);
}

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events) {
  if (fd < 0)
    return;

  // Find all epoll instances watching this FD
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    eventpoll_t *ep = epoll_table[i];
    if (!ep)
      continue;

    // Find epitem for this FD
    epitem_t *epi = ep->items[fd];
    if (!epi)
      continue;

    // Skip disabled oneshot items
    if (epi->oneshot && epi->oneshot_disabled)
      continue;

    uint32_t mask =
        events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);

    if (mask) {
      // ep_add_to_ready_list handles both adding to the ready list AND
      // waking up waiters atomically under ep->lock.
      ep_add_to_ready_list(ep, epi);
    }
  }
}

// Epoll Subsystem Initialization

void epoll_init(void) {
  memset(epoll_table, 0, sizeof(epoll_table));
  memset(epoll_pool, 0, sizeof(epoll_pool));
  memset(epoll_vfs_pool, 0, sizeof(epoll_vfs_pool));
  epoll_free_bitmap = ~0ULL;
  spinlock_init(&epoll_table_lock);

  klog_puts("[OK] Epoll subsystem initialized (max instances: ");
  klog_uint64(EPOLL_MAX_INSTANCES);
  klog_puts(", max watched FDs: ");
  klog_uint64(EPOLL_MAX_WATCHED);
  klog_puts(")\n");
}
