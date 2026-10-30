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
#include "arch/uaccess.h"
#include <stdint.h>

// Global Epoll Instance Table
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
#define EPOLL_BITMAP_WORDS ((EPOLL_MAX_INSTANCES + 63) / 64)
static uint64_t epoll_free_bitmap[EPOLL_BITMAP_WORDS]; // 1 = free
static spinlock_t epoll_table_lock = SPINLOCK_INIT;

/* Fallback full rescan of the watched set.  Event discovery is driven by
 * epoll_notify_event(); the rescan is only a safety net for producers that
 * never notify (pidfd exit, pty slave input, console input) and for lost
 * wakeups.  It is rate-limited to at most one O(watched-fd) pass per interval
 * per epoll instance, instead of one pass every time epoll_wait() parks with
 * an empty ready list.  Raised from 100 ms, which cost a full wakeup + scan
 * ten times per second per idle epoll_wait. */
#define EPOLL_RESCAN_INTERVAL_MS 1000

static int epoll_table_alloc(void) {
  spinlock_acquire(&epoll_table_lock);
  for (unsigned word = 0; word < EPOLL_BITMAP_WORDS; word++) {
    uint64_t free = epoll_free_bitmap[word];
    if (!free)
      continue;
    int bit = __builtin_ctzll(free);
    int idx = (int)(word * 64) + bit;
    if (idx >= EPOLL_MAX_INSTANCES)
      break;
    epoll_free_bitmap[word] &= ~(1ULL << bit);
    spinlock_release(&epoll_table_lock);
    return idx;
  }
  spinlock_release(&epoll_table_lock);
  return -1; // Table full
}

static void epoll_table_free(int idx) {
  if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
    return;

  spinlock_acquire(&epoll_table_lock);
  epoll_table[idx] = NULL;
  epoll_free_bitmap[idx / 64] |= (1ULL << (idx % 64));
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
  epi->refcount = 1;
  return epi;
}

static void epitem_free(epitem_t *epi) {
  if (!epi)
    return;
  if (__atomic_sub_fetch(&epi->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
    vfs_close(epi->node);
    kfree(epi);
  }
}

/* Snapshot pins survive dropping ep->lock for driver polling. The reaper
 * releases them too if a killed thread never returns from its syscall. */
static void epitem_pin(epitem_t *epi, unsigned slot) {
  struct thread *t = sched_get_current();
  __atomic_add_fetch(&epi->refcount, 1, __ATOMIC_RELAXED);
  t->active_epoll_items[slot] = epi;
}

static void epitem_unpin(unsigned slot) {
  struct thread *t = sched_get_current();
  epitem_t *epi = t->active_epoll_items[slot];
  t->active_epoll_items[slot] = NULL;
  epitem_free(epi);
}

void epoll_release_thread(struct thread *t) {
  if (!t) return;
  for (unsigned i = 0; i < 64; i++) {
    epitem_t *epi = t->active_epoll_items[i];
    t->active_epoll_items[i] = NULL;
    epitem_free(epi);
  }
  vfs_node_t *node = t->active_epoll_node;
  t->active_epoll_node = NULL;
  vfs_close(node);
}

// Epoll Instance Creation/Destruction

static bool epoll_expand_items_locked(eventpoll_t *ep, uint32_t needed_fd) {
  if (needed_fd >= EPOLL_MAX_WATCHED)
    return false;
  uint32_t new_cap = ep->items_capacity ? ep->items_capacity * 2 : EPOLL_INITIAL_CAPACITY;
  while (new_cap <= needed_fd && new_cap < EPOLL_MAX_WATCHED)
    new_cap *= 2;
  if (new_cap > EPOLL_MAX_WATCHED)
    new_cap = EPOLL_MAX_WATCHED;
  if (needed_fd >= new_cap || new_cap <= ep->items_capacity)
    return false;

  size_t old_bytes = ep->items_capacity * sizeof(epitem_t *);
  size_t new_bytes = new_cap * sizeof(epitem_t *);
  epitem_t **new_items = kmalloc(new_bytes);
  if (!new_items)
    return false;
  memset(new_items, 0, new_bytes);
  if (ep->items && ep->items_capacity > 0) {
    memcpy(new_items, ep->items, old_bytes);
    kfree(ep->items);
  }
  ep->items = new_items;
  ep->items_capacity = new_cap;
  return true;
}

eventpoll_t *epoll_create(void) {
  int idx = epoll_table_alloc();
  if (idx < 0) {
    klog_puts("[ERR] epoll: epoll table full\n");
    return NULL;
  }

  eventpoll_t *ep = kmalloc(sizeof(eventpoll_t));
  if (!ep) {
    epoll_table_free(idx);
    return NULL;
  }
  memset(ep, 0, sizeof(eventpoll_t));

  ep->items = kmalloc(EPOLL_INITIAL_CAPACITY * sizeof(epitem_t *));
  if (!ep->items) {
    epoll_table_free(idx);
    kfree(ep);
    return NULL;
  }
  memset(ep->items, 0, EPOLL_INITIAL_CAPACITY * sizeof(epitem_t *));
  ep->items_capacity = EPOLL_INITIAL_CAPACITY;

  // Fast targeted field initialization
  ep->fd = -1;
  ep->vfs_node = NULL;
  ep->item_count = 0;
  ep->items_high = 0;
  ep->next_scan_ticks = 0; // due immediately: first wait does a full scan
  INIT_LIST_HEAD(&ep->rdllist);
  ep->rdllist_count = 0;
  ep->generation = 1;
  wait_queue_init(&ep->wq);
  spinlock_init(&ep->lock);
  ep->refcount = 1;
  ep->table_index = idx;

  spinlock_acquire(&epoll_table_lock);
  epoll_table[idx] = ep;
  spinlock_release(&epoll_table_lock);

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
  if (ep->items) {
    int hi = ep->items_high;
    if (hi > (int)ep->items_capacity)
      hi = (int)ep->items_capacity;
    for (int i = 0; i < hi; i++) {
      epitem_t *epi = ep->items[i];
      if (epi) {
        spinlock_acquire(&epi->node->ep_lock);
        spinlock_acquire(&ep->lock);
        __atomic_store_n(&epi->removed, true, __ATOMIC_RELEASE);
        if (__atomic_load_n(&epi->on_ready_list, __ATOMIC_ACQUIRE)) {
          list_del(&epi->rdllink);
          if (!__atomic_load_n(&epi->in_scan, __ATOMIC_ACQUIRE) &&
              ep->rdllist_count > 0)
            ep->rdllist_count--;
          __atomic_store_n(&epi->on_ready_list, false, __ATOMIC_RELEASE);
          __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
        }
        list_del(&epi->ep_node_link);
        ep->items[i] = NULL;
        spinlock_release(&ep->lock);
        spinlock_release(&epi->node->ep_lock);
        epitem_free(epi);
      }
    }
    ep->item_count = 0;
    ep->items_high = 0;
    kfree(ep->items);
    ep->items = NULL;
    ep->items_capacity = 0;
  }

  int idx = ep->table_index;
  if (idx >= 0 && idx < EPOLL_MAX_INSTANCES) {
    epoll_table_free(idx);
  }
  kfree(ep);
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

static inline uint32_t ep_check_events_mask(epitem_t *epi, uint32_t watch_mask) {
  if (!epi || !epi->node ||
      __atomic_load_n(&epi->removed, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&epi->oneshot_disabled, __ATOMIC_ACQUIRE))
    return 0;

  uint32_t valid = watch_mask | EPOLLERR | EPOLLHUP;
  __atomic_exchange_n(&epi->ready_events, 0, __ATOMIC_ACQUIRE);
  int revents = vfs_poll(epi->node, watch_mask);
  return (uint32_t)revents & valid;
}

static uint32_t ep_check_events(epitem_t *epi) {
  if (!epi || !epi->node)
    return 0;

  if (__atomic_load_n(&epi->removed, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&epi->oneshot_disabled, __ATOMIC_ACQUIRE))
    return 0;

  // Get the registered events without acquiring parent ep->lock
  uint32_t reg = __atomic_load_n(&epi->registered_events, __ATOMIC_ACQUIRE);
  uint32_t watch_mask =
      reg & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);

  return ep_check_events_mask(epi, watch_mask);
}

/* Waiters on ep->wq are interchangeable consumers of ep->rdllist, so one
 * wake per queued event is enough.  Waking every parked thread made N-thread
 * workers on a shared epfd all run (and re-park) for each event.  Whoever
 * leaves ready items behind, or leaves without consuming its wake (signal,
 * timeout), passes the baton on with this helper.  Caller holds ep->lock. */
static inline void ep_wake_one_locked(eventpoll_t *ep) {
  if (__atomic_load_n(&ep->wq.head, __ATOMIC_RELAXED) != NULL)
    wait_queue_wake_one(&ep->wq);
}

/* Forward a wake to the next waiter when ready items remain. */
static void ep_chain_wake(eventpoll_t *ep) {
  if (__atomic_load_n(&ep->rdllist_count, __ATOMIC_ACQUIRE) == 0 ||
      __atomic_load_n(&ep->wq.head, __ATOMIC_RELAXED) == NULL)
    return;
  spinlock_acquire(&ep->lock);
  if (ep->rdllist_count > 0)
    ep_wake_one_locked(ep);
  spinlock_release(&ep->lock);
}

// Helper: Add item to ready list
static void ep_add_to_ready_list(eventpoll_t *ep, epitem_t *epi) {
  /* Advance before the fast path so a concurrent unlocked readiness poll can
   * detect this notification even if the item still appears queued. */
  __atomic_fetch_add(&epi->notify_seq, 1, __ATOMIC_ACQ_REL);

  // An in-flight item is not on the shared ready list and cannot be skipped.
  if (__atomic_load_n(&epi->on_ready_list, __ATOMIC_ACQUIRE) &&
      !__atomic_load_n(&epi->in_scan, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&ep->wq.head, __ATOMIC_RELAXED) == NULL) {
    return;
  }

  spinlock_acquire(&ep->lock);

  if (__atomic_load_n(&epi->removed, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&epi->oneshot_disabled, __ATOMIC_ACQUIRE)) {
    spinlock_release(&ep->lock);
    return;
  }
  bool was_empty = (ep->rdllist_count == 0);
  if (!__atomic_load_n(&epi->on_ready_list, __ATOMIC_ACQUIRE)) {
    list_add_tail(&epi->rdllink, &ep->rdllist);
    __atomic_store_n(&epi->on_ready_list, true, __ATOMIC_RELEASE);
    __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
    ep->rdllist_count++;
    __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
  }

  // Wake up waiters while still holding ep->lock.
  // This closes the race: epoll_wait_impl sets THREAD_BLOCKED while holding
  // ep->lock, so if we wake here (also under ep->lock) we are guaranteed to
  // see the correct blocked state and the thread won't miss the wakeup.
  ep_wake_one_locked(ep);

  spinlock_release(&ep->lock);

  // Propagate to any outer epoll instances watching this epoll fd.
  // This is required for nested epoll (epoll-in-epoll) to work: when this
  // epoll's ready list becomes non-empty, any outer epoll that has registered
  // this epoll's VFS node must be woken up.
  if (was_empty && ep->vfs_node) {
    epoll_notify_event(ep->vfs_node, EPOLLIN);
  }
}

// Membership changes use node->ep_lock -> ep->lock, matching notifiers.
int epoll_ctl_del(eventpoll_t *ep, int fd) {
  if (!ep) return -22;
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED) return -9;
  spinlock_acquire(&ep->lock);
  epitem_t *epi = ((uint32_t)fd < ep->items_capacity) ? ep->items[fd] : NULL;
  if (epi) epitem_pin(epi, 0);
  spinlock_release(&ep->lock);
  if (!epi) return -2;
  spinlock_acquire(&epi->node->ep_lock);
  spinlock_acquire(&ep->lock);
  bool owned = ((uint32_t)fd < ep->items_capacity && ep->items[fd] == epi) &&
      !__atomic_load_n(&epi->removed, __ATOMIC_ACQUIRE);
  if (owned) {
    __atomic_store_n(&epi->removed, true, __ATOMIC_RELEASE);
    ep->items[fd] = NULL;
    ep->item_count--;
    __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&epi->on_ready_list, __ATOMIC_ACQUIRE)) {
      list_del(&epi->rdllink);
      if (!__atomic_load_n(&epi->in_scan, __ATOMIC_ACQUIRE) &&
          ep->rdllist_count > 0)
        ep->rdllist_count--;
      __atomic_store_n(&epi->on_ready_list, false, __ATOMIC_RELEASE);
      __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
    }
    list_del(&epi->ep_node_link);
  }
  spinlock_release(&ep->lock);
  spinlock_release(&epi->node->ep_lock);
  if (owned) epitem_free(epi); // drop table ownership, then our snapshot
  epitem_unpin(0);
  return owned ? 0 : -2;
}

int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event) return -22;
  struct thread *t = sched_get_current();
  if (!t || fd < 0 || fd >= MAX_FDS || fd >= EPOLL_MAX_WATCHED) return -9;
  spinlock_acquire(&t->files->lock);
  if ((uint32_t)fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  vfs_node_t *node = t->fds[fd];
  if (node && node != (vfs_node_t *)-1) vfs_node_ref(node);
  else node = NULL;
  spinlock_release(&t->files->lock);
  if (!node) return -9;
  if (node == ep->vfs_node) { vfs_close(node); return -22; }
  epitem_t *epi = epitem_alloc();
  if (!epi) { vfs_close(node); return -12; }
  epi->node = node;
  epi->fd = fd;
  epi->ep = ep;
  epi->event = *event;
  __atomic_store_n(&epi->oneshot, (event->events & EPOLLONESHOT) != 0, __ATOMIC_RELEASE);
  __atomic_store_n(&epi->exclusive, (event->events & EPOLLEXCLUSIVE) != 0, __ATOMIC_RELEASE);
  __atomic_store_n(&epi->registered_events, event->events, __ATOMIC_RELEASE);

  // Preserve the existing array's fd-reuse replacement behavior.
  spinlock_acquire(&ep->lock);
  bool stale = ((uint32_t)fd < ep->items_capacity && ep->items[fd]) && ep->items[fd]->node != node;
  spinlock_release(&ep->lock);
  if (stale) epoll_ctl_del(ep, fd);

  spinlock_acquire(&node->ep_lock);
  spinlock_acquire(&ep->lock);
  if ((uint32_t)fd >= ep->items_capacity) {
    if (!epoll_expand_items_locked(ep, (uint32_t)fd)) {
      spinlock_release(&ep->lock);
      spinlock_release(&node->ep_lock);
      epitem_free(epi);
      return -12; // ENOMEM
    }
  }
  if (ep->items[fd]) {
    spinlock_release(&ep->lock);
    spinlock_release(&node->ep_lock);
    epitem_free(epi);
    return -17;
  }
  ep->items[fd] = epi;
  ep->item_count++;
  if (fd >= ep->items_high) ep->items_high = fd + 1;
  __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
  if (node->ep_watchers.next == NULL || node->ep_watchers.prev == NULL) {
    INIT_LIST_HEAD(&node->ep_watchers);
  }
  list_add_tail(&epi->ep_node_link, &node->ep_watchers);
  epitem_pin(epi, 0);
  spinlock_release(&ep->lock);
  spinlock_release(&node->ep_lock);
  if (ep_check_events(epi)) ep_add_to_ready_list(ep, epi);
  epitem_unpin(0);
  return 0;
}

int epoll_ctl_mod(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event) return -22;
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED) return -9;
  spinlock_acquire(&ep->lock);
  epitem_t *epi = ((uint32_t)fd < ep->items_capacity) ? ep->items[fd] : NULL;
  if (!epi) { spinlock_release(&ep->lock); return -2; }
  epitem_pin(epi, 0);
  epi->event = *event;
  __atomic_store_n(&epi->ready_events, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&epi->oneshot, (event->events & EPOLLONESHOT) != 0, __ATOMIC_RELEASE);
  __atomic_store_n(&epi->exclusive, (event->events & EPOLLEXCLUSIVE) != 0, __ATOMIC_RELEASE);
  __atomic_store_n(&epi->registered_events, event->events, __ATOMIC_RELEASE);
  __atomic_store_n(&epi->oneshot_disabled, false, __ATOMIC_RELEASE);
  __atomic_fetch_add(&epi->notify_seq, 1, __ATOMIC_ACQ_REL);
  __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
  spinlock_release(&ep->lock);
  if (ep_check_events(epi)) ep_add_to_ready_list(ep, epi);
  epitem_unpin(0);
  return 0;
}

// epoll_wait Implementation

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, int maxevents,
                    int64_t timeout_ms) {
  if (!ep || !events || maxevents <= 0)
    return -22; // EINVAL

  // Lockless fast path: if non-blocking poll and ready list is empty, return immediately
  if (timeout_ms == 0 && __atomic_load_n(&ep->rdllist_count, __ATOMIC_ACQUIRE) == 0) {
    return 0;
  }

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
    uint64_t now = lapic_timer_get_ticks();
    deadline_ticks = (uint64_t)timeout_ms > UINT64_MAX - now
                         ? UINT64_MAX : now + (uint64_t)timeout_ms;
  }

  while (returned == 0) {
    if (thread_has_pending_signal(current)) {
      returned = -4; // -EINTR
      break;
    }

    // Detach a bounded batch under ep->lock, then poll it without the lock.
    // Items remain marked on_ready_list while in_scan is true, so producers
    // know they are pending but cannot mistake them for entries on rdllist.
    int cand_count = 0;
    epitem_t *candidates[64];
    uint32_t watch_masks[64];
    uint64_t scan_sequences[64];
    int scan_limit = maxevents < 64 ? maxevents : 64;

    spinlock_acquire(&ep->lock);
    while (cand_count < scan_limit && !list_empty(&ep->rdllist)) {
      epitem_t *epi = list_first_entry(&ep->rdllist, epitem_t, rdllink);
      list_del(&epi->rdllink);
      INIT_LIST_HEAD(&epi->rdllink);
      if (ep->rdllist_count > 0)
        ep->rdllist_count--;
      __atomic_store_n(&epi->in_scan, true, __ATOMIC_RELEASE);
      __atomic_store_n(&epi->on_ready_list, true, __ATOMIC_RELEASE);
      epitem_pin(epi, (unsigned)cand_count);
      watch_masks[cand_count] =
          __atomic_load_n(&epi->registered_events, __ATOMIC_ACQUIRE) &
          ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);
      scan_sequences[cand_count] =
          __atomic_load_n(&epi->notify_seq, __ATOMIC_ACQUIRE);
      candidates[cand_count++] = epi;
    }
    if (cand_count > 0) {
      __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
    }
    spinlock_release(&ep->lock);

    // Poll candidates outside ep->lock to avoid lock contention and deadlocks.
    uint32_t polled_events[64];
    for (int i = 0; i < cand_count; i++) {
      polled_events[i] = ep_check_events_mask(candidates[i], watch_masks[i]);
    }

    // Reconcile each poll with notifications that arrived while it was unlocked.
    struct epoll_event event_batch[64];
    int batch_count = 0;

    for (int i = 0; i < cand_count; i++) {
      epitem_t *epi = candidates[i];
      spinlock_acquire(&ep->lock);
      if (!__atomic_load_n(&epi->in_scan, __ATOMIC_ACQUIRE) ||
          !__atomic_load_n(&epi->on_ready_list, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&epi->removed, __ATOMIC_ACQUIRE)) {
        // EPOLL_CTL_DEL may have detached and retired this pinned item.
        spinlock_release(&ep->lock);
        continue;
      }

      bool was_empty = ep->rdllist_count == 0;
      bool requeue =
          scan_sequences[i] != __atomic_load_n(&epi->notify_seq, __ATOMIC_ACQUIRE);
      uint32_t current_events = polled_events[i] &
          (watch_masks[i] | EPOLLERR | EPOLLHUP);
      bool deliver = !requeue &&
          !__atomic_load_n(&epi->oneshot_disabled, __ATOMIC_ACQUIRE) &&
          current_events != 0;
      bool level_requeue = deliver &&
          !(epi->event.events & (EPOLLET | EPOLLONESHOT));
      bool keep_ready = requeue || level_requeue;

      if (requeue) {
        // A producer raced with the poll. Keep the candidate queued for a
        // fresh poll instead of delivering a stale/partial readiness result.
        __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
        list_add_tail(&epi->rdllink, &ep->rdllist);
        ep->rdllist_count++;
        __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
      } else if (deliver && batch_count < maxevents) {
        event_batch[batch_count] = epi->event;
        event_batch[batch_count].events = current_events;
        batch_count++;

        if (epi->event.events & EPOLLET) {
          epi->last_events = current_events &
                             (EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM);
          __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
          __atomic_store_n(&epi->on_ready_list, false, __ATOMIC_RELEASE);
          INIT_LIST_HEAD(&epi->rdllink);
        } else if (__atomic_load_n(&epi->oneshot, __ATOMIC_ACQUIRE)) {
          __atomic_store_n(&epi->oneshot_disabled, true, __ATOMIC_RELEASE);
          __atomic_store_n(&epi->registered_events, 0, __ATOMIC_RELEASE);
          __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
          __atomic_store_n(&epi->on_ready_list, false, __ATOMIC_RELEASE);
          INIT_LIST_HEAD(&epi->rdllink);
        } else {
          // Level-triggered readiness stays queued for the next wait.
          __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
          list_add_tail(&epi->rdllink, &ep->rdllist);
          ep->rdllist_count++;
        }
      } else {
        __atomic_store_n(&epi->in_scan, false, __ATOMIC_RELEASE);
        __atomic_store_n(&epi->on_ready_list, false, __ATOMIC_RELEASE);
        INIT_LIST_HEAD(&epi->rdllink);
      }

      if (keep_ready)
        ep_wake_one_locked(ep);
      if (keep_ready)
        __atomic_fetch_add(&ep->generation, 1, __ATOMIC_RELEASE);
      spinlock_release(&ep->lock);

      if (keep_ready && was_empty && ep->vfs_node) {
        epoll_notify_event(ep->vfs_node, EPOLLIN);
      }
    }

    // Copy the collected events to user space in a single operation.
    if (batch_count > 0) {
      if (copy_to_user(&events[returned], event_batch,
                       batch_count * sizeof(struct epoll_event))) {
        returned = -14; // -EFAULT
        for (int j = 0; j < cand_count; j++)
          epitem_unpin(j);
        break;
      }
      returned += batch_count;
    }

    for (int i = 0; i < cand_count; i++) {
      epitem_unpin(i);
    }

    if (returned != 0)
      break;

    // 6. Immediate return on non-blocking poll
    if (timeout_ms == 0)
      break;

    // 7. Rate-limited fallback rescan of every watched item, outside ep->lock.
    uint64_t scan_now = lapic_timer_get_ticks();
    if (scan_now >= __atomic_load_n(&ep->next_scan_ticks, __ATOMIC_RELAXED)) {
      int items_high = __atomic_load_n(&ep->items_high, __ATOMIC_RELAXED);
      for (int _i = 0; _i < items_high; _i++) {
        spinlock_acquire(&ep->lock);
        epitem_t *_epi = (_i < (int)ep->items_capacity) ? ep->items[_i] : NULL;
        if (!_epi || __atomic_load_n(&_epi->on_ready_list, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&_epi->oneshot_disabled, __ATOMIC_ACQUIRE)) {
          spinlock_release(&ep->lock);
          continue;
        }
        epitem_pin(_epi, 0);
        spinlock_release(&ep->lock);
        if (ep_check_events(_epi)) ep_add_to_ready_list(ep, _epi);
        epitem_unpin(0);
      }
      __atomic_store_n(&ep->next_scan_ticks, scan_now + EPOLL_RESCAN_INTERVAL_MS,
                       __ATOMIC_RELAXED);
    }

    // 8. Prepare to block under ep->lock
    spinlock_acquire(&ep->lock);

    // If ready list is non-empty, loop back immediately
    if (ep->rdllist_count > 0 || !list_empty(&ep->rdllist)) {
      spinlock_release(&ep->lock);
      continue;
    }

    current->state = THREAD_BLOCKED;
    uint64_t scan_at = __atomic_load_n(&ep->next_scan_ticks, __ATOMIC_RELAXED);
    if (timeout_ms > 0 && timeout_ms != -1) {
      current->wakeup_ticks =
          (deadline_ticks < scan_at) ? deadline_ticks : scan_at;
    } else if (timeout_ms == -1) {
      current->wakeup_ticks = scan_at;
    } else {
      current->wakeup_ticks = 0;
    }

    // Re-check ready list one last time before releasing lock
    if (ep->rdllist_count > 0 || !list_empty(&ep->rdllist)) {
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;
      spinlock_release(&ep->lock);
      continue;
    }

    wait_queue_add(&ep->wq, &entry);
    spinlock_release(&ep->lock);

    // 9. Yield execution
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

  /* Items still queued (maxevents cap, level-triggered requeue) or a wake we
   * did not consume (EINTR/timeout) must reach another waiter. */
  ep_chain_wake(ep);

  return returned;
}

// Epoll FD Management

int epoll_alloc_fd(eventpoll_t *ep, uint64_t flags) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE

  /* vfs_close still accesses the node after its close callback returns the
   * epoll slot to the pool. Give each incarnation its own node. */
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node) {
    fd_release_reserved(t, fd);
    return -12;
  }
  vfs_node_init(node);
  node->flags = FS_EPOLL;
  node->inode = (uint32_t)(uint64_t)ep;
  node->device = ep;
  node->wait_queue = &ep->wq;

  // Set epoll VFS operations
  node->read = epoll_vfs_read;
  node->write = epoll_vfs_write;
  /* One epoll reference belongs to the node. dup/fork share VFS refs;
   * adding an epoll ref on each open leaks it at the last VFS close. */
  node->close = epoll_vfs_close;
  node->poll = epoll_vfs_poll;

  ep->fd = fd;
  ep->vfs_node = node; // Store for nested epoll propagation
  spinlock_acquire(&t->files->lock);
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = (flags & EPOLL_CLOEXEC) ? (1ULL << 24) : 0;
  t->fds[fd] = node;
  fd_path_set(t, fd, "anon_inode:[eventpoll]");
  spinlock_release(&t->files->lock);

  return fd;
}

eventpoll_t *epoll_from_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return NULL;

  if (fd < 0 || fd >= MAX_FDS)
    return NULL;

  spinlock_acquire(&t->files->lock);
  if ((uint32_t)fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return NULL;
  }
  vfs_node_t *node = t->fds[fd];
  if (!node || node == (vfs_node_t *)-1 ||
      (node->flags & FS_TYPE_MASK) != FS_EPOLL) {
    spinlock_release(&t->files->lock);
    return NULL;
  }
  vfs_node_ref(node);
  t->active_epoll_node = node;
  spinlock_release(&t->files->lock);
  return (eventpoll_t *)node->device;
}

int epoll_close_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t || !t->files)
    return -9; // EBADF

  if (fd < 0 || fd >= MAX_FDS)
    return -9; // EBADF

  spinlock_acquire(&t->files->lock);
  if ((uint32_t)fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return -9; // EBADF
  }
  vfs_node_t *node = t->fds[fd];
  if (!node || node == (vfs_node_t *)-1) { /* an allocation reservation */
    spinlock_release(&t->files->lock);
    return -9; // EBADF
  }

  if ((node->flags & FS_TYPE_MASK) != FS_EPOLL) {
    spinlock_release(&t->files->lock);
    return -22; // EINVAL
  }

  // Clear FD
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = 0;
  fd_path_clear(t, fd);
  if ((uint32_t)fd < t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
  spinlock_release(&t->files->lock);

  // Release epoll reference through VFS close
  if (node) {
    vfs_close(node);
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

void epoll_vfs_close(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  node->device = NULL;
  node->wait_queue = NULL;
  if (ep) {
    ep->vfs_node = NULL;
    epoll_put(ep);
  }
}

int epoll_vfs_poll(struct vfs_node *node, int events) {
  if (!node)
    return POLLNVAL;

  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (!ep)
    return POLLNVAL;

  int revents = 0;

  // Lockless check: ready list count and head pointer
  if (__atomic_load_n(&ep->rdllist_count, __ATOMIC_ACQUIRE) > 0) {
    if (events & POLLIN)
      revents |= POLLIN;
  }
  // Always writable (for poll purposes)
  if (events & POLLOUT)
    revents |= POLLOUT;

  return revents;
}

// Event Notification

void epoll_notify_event(struct vfs_node *node, uint32_t events) {
  if (!node || (uint64_t)node < 0xFFFF800000000000ULL)
    return;

  // Lazy-init check for node notification list
  if (node->ep_watchers.next == NULL || (uint64_t)node->ep_watchers.next < 0xFFFF800000000000ULL ||
      node->ep_watchers.next == &node->ep_watchers)
    return;

  spinlock_acquire(&node->ep_lock);
  if (node->ep_watchers.next == NULL || (uint64_t)node->ep_watchers.next < 0xFFFF800000000000ULL ||
      node->ep_watchers.next == &node->ep_watchers) {
    spinlock_release(&node->ep_lock);
    return;
  }
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
    if (__atomic_load_n(&epi->oneshot, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&epi->oneshot_disabled, __ATOMIC_ACQUIRE))
      continue;

    uint32_t mask =
        events & (__atomic_load_n(&epi->registered_events, __ATOMIC_ACQUIRE) |
                  EPOLLERR | EPOLLHUP);
    if (!mask)
      continue;

    /* Record the notification and queue a candidate.  The waiter polls the
     * fd again before delivery because readiness may change meanwhile. */
    __atomic_fetch_or(&epi->ready_events, mask, __ATOMIC_RELAXED);

    // ep_add_to_ready_list now handles both adding to the ready list AND
    // waking up waiters atomically under ep->lock, eliminating the
    // missed-wakeup race.
    ep_add_to_ready_list(ep, epi);

    // EPOLLEXCLUSIVE: stop after waking the first matching watcher so only
    // one epoll instance is woken per event (prevents thundering herd).
    if (__atomic_load_n(&epi->exclusive, __ATOMIC_ACQUIRE))
      break;
  }
  spinlock_release(&node->ep_lock);
}

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events) {
  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return;

  /* Walk only live instances: the free-bitmap marks in-use slots (1 = free),
   * so a system with a handful of epoll instances open does a handful of
   * iterations instead of probing every slot.  The bitmap and
   * epoll_table[] are read racily, exactly as the old loop did - a slot
   * missed in this instant is covered by epoll_wait's rate-limited rescan. */
  for (unsigned word = 0; word < EPOLL_BITMAP_WORDS; word++) {
    uint64_t live = ~__atomic_load_n(&epoll_free_bitmap[word], __ATOMIC_RELAXED);
    while (live) {
      int i = (int)(word * 64) + __builtin_ctzll(live);
      live &= live - 1;
      if (i >= EPOLL_MAX_INSTANCES)
        continue;
      spinlock_acquire(&epoll_table_lock);
      eventpoll_t *ep = epoll_table[i];
      uint64_t refs = ep ? __atomic_load_n(&ep->refcount, __ATOMIC_RELAXED) : 0;
      while (refs && !__atomic_compare_exchange_n(&ep->refcount, &refs, refs + 1,
                 false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {}
      spinlock_release(&epoll_table_lock);
      if (!refs) continue; // destruction already owns this slot
      spinlock_acquire(&ep->lock);
      epitem_t *epi = ((uint32_t)fd < ep->items_capacity) ? ep->items[fd] : NULL;
      if (epi) __atomic_add_fetch(&epi->refcount, 1, __ATOMIC_RELAXED);
      spinlock_release(&ep->lock);
      if (epi) {
        __atomic_fetch_or(&epi->ready_events, events, __ATOMIC_RELAXED);
        ep_add_to_ready_list(ep, epi);
        epitem_free(epi);
      }
      epoll_put(ep);
    }
  }
}

// Epoll Subsystem Initialization

void epoll_init(void) {
  memset(epoll_table, 0, sizeof(epoll_table));
  memset(epoll_free_bitmap, 0xff, sizeof(epoll_free_bitmap));
  spinlock_init(&epoll_table_lock);

  klog_puts("[OK] Epoll subsystem initialized (max instances: ");
  klog_uint64(EPOLL_MAX_INSTANCES);
  klog_puts(", max watched FDs: ");
  klog_uint64(EPOLL_MAX_WATCHED);
  klog_puts(")\n");
}
