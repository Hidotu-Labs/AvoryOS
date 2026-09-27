// rfkill.c — Linux-compatible RF switch subsystem (/dev/rfkill)
//
// BlueZQt (and BlueZ, util-linux' rfkill, systemd-rfkill, ...) open
// /dev/rfkill and exchange 8-byte struct rfkill_event records over it:
//
//   read()            dequeues one event (ADD / DEL / CHANGE / CHANGE_ALL)
//   write()           asks the kernel to change soft-block state
//   poll()/epoll()    watches for events
//   RFKILL_IOCTL_NOINPUT / RFKILL_IOCTL_MAX_SIZE
//
// Without a Bluetooth or WLAN driver there is nothing to register, so the
// device simply has no switches — but it still has to exist and speak the
// protocol, otherwise BlueZQt logs "Cannot open /dev/rfkill for reading!" and
// loses the ability to soft-block radios.
//
// Each open gets its own event queue and wait queue through open_instance(),
// matching Linux' per-file rfkill_data.  Only the global state (switch list,
// per-type defaults) is shared.

#include "drivers/rfkill/rfkill.h"
#include "../../console/klog.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../lib/list.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "../../sched/sched.h"
#include "../../sched/wait.h"
#include "../../socket/epoll.h"

#define RFKILL_MAX_SWITCHES 16
#define RFKILL_EVENT_QUEUE 32

/* Per-open state, one per file descriptor of /dev/rfkill. */
typedef struct rfkill_file {
  struct list_head list;                          /* global open-file list */
  wait_queue_t read_wait;                         /* read()/poll() sleepers */
  struct rfkill_event events[RFKILL_EVENT_QUEUE]; /* FIFO ring buffer */
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint8_t max_size; /* RFKILL_EVENT_SIZE_V1 unless raised by ioctl */
  vfs_node_t *node;
} rfkill_file_t;

static rfkill_switch_t *g_switches[RFKILL_MAX_SWITCHES];
static size_t g_switch_count;
static uint32_t g_next_idx;
static bool g_soft_state[NUM_RFKILL_TYPES]; /* per-type default set by
                                               RFKILL_OP_CHANGE_ALL */
static struct list_head g_files;
static spinlock_t g_lock = SPINLOCK_INIT;
static vfs_node_t g_rfkill_node; /* pinned metadata node in /dev */

/* ------------------------------------------------------------------------ */
/* Event queue                                                              */
/* ------------------------------------------------------------------------ */

/* Caller holds g_lock. */
static void rfkill_queue_event_locked(rfkill_file_t *f,
                                      const rfkill_switch_t *sw, uint8_t op) {
  if (f->count == RFKILL_EVENT_QUEUE) {
    /* Full: drop the oldest event, like an overflowing input queue. */
    f->tail = (f->tail + 1) % RFKILL_EVENT_QUEUE;
    f->count--;
  }

  struct rfkill_event *ev = &f->events[f->head];
  ev->idx = sw->idx;
  ev->type = sw->type;
  ev->op = op;
  ev->soft = sw->soft_blocked ? 1 : 0;
  ev->hard = sw->hard_blocked ? 1 : 0;

  f->head = (f->head + 1) % RFKILL_EVENT_QUEUE;
  f->count++;
}

/* Caller holds g_lock.  Queue the event for every open /dev/rfkill fd and wake
 * its readers.  Wait queues carry their own lock and nothing below takes
 * g_lock, so waking from here is safe. */
static void rfkill_broadcast_locked(const rfkill_switch_t *sw, uint8_t op) {
  rfkill_file_t *f;
  list_for_each_entry(f, &g_files, list) {
    rfkill_queue_event_locked(f, sw, op);
    wait_queue_wake_all(&f->read_wait);
    epoll_notify_event(f->node, POLLIN);
  }
}

/* ------------------------------------------------------------------------ */
/* VFS callbacks (per-open node)                                            */
/* ------------------------------------------------------------------------ */

static uint32_t rfkill_vfs_read(struct vfs_node *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  (void)offset;
  rfkill_file_t *f = (rfkill_file_t *)node->device;
  struct thread *t = sched_get_current();
  if (!f || !t || !buffer || size == 0)
    return 0;

  /* Heap-allocated so the entry survives the context switches below. */
  wait_queue_entry_t *entry = kmalloc(sizeof(wait_queue_entry_t));
  if (!entry)
    return (uint32_t)-12; // -ENOMEM
  entry->thread = t;
  entry->next = NULL;
  entry->wq = NULL;
  entry->thread_next = NULL;
  entry->thread_prev = NULL;

  for (;;) {
    spinlock_acquire(&g_lock);

    if (f->count > 0) {
      uint32_t n = size;
      if (n > f->max_size)
        n = f->max_size;
      if (n > sizeof(struct rfkill_event))
        n = sizeof(struct rfkill_event);
      memcpy(buffer, &f->events[f->tail], n);
      f->tail = (f->tail + 1) % RFKILL_EVENT_QUEUE;
      f->count--;
      spinlock_release(&g_lock);
      kfree(entry);
      return n;
    }

    /* Linux returns EAGAIN for non-blocking readers; blocking readers wait
     * until a switch event arrives. */
    if (node->flags & FS_NONBLOCK) {
      spinlock_release(&g_lock);
      kfree(entry);
      return (uint32_t)-11; // -EAGAIN
    }

    wait_queue_add(&f->read_wait, entry);
    t->state = THREAD_BLOCKED;

    /* The broadcaster queues events under g_lock before waking, so either the
     * event is visible here or the wake lands after we are queued+blocked. */
    if (f->count > 0) {
      t->state = THREAD_RUNNING;
      spinlock_release(&g_lock);
    } else {
      spinlock_release(&g_lock);
      sched_yield();
    }

    wait_queue_remove(&f->read_wait, entry);
    t->state = THREAD_RUNNING;

    if (thread_has_pending_signal(t)) {
      kfree(entry);
      return (uint32_t)-4; // -EINTR
    }
  }
}

static uint32_t rfkill_vfs_write(struct vfs_node *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  (void)offset;
  rfkill_file_t *f = (rfkill_file_t *)node->device;
  if (!f || !buffer)
    return (uint32_t)-14; // -EFAULT

  /* Linux accepts one byte short of the full event ("we don't need the 'hard'
   * variable but accept it"). */
  if (size < RFKILL_EVENT_SIZE_V1 - 1)
    return (uint32_t)-22; // -EINVAL

  struct rfkill_event ev;
  memset(&ev, 0, sizeof(ev));
  uint32_t n = size;
  if (n > f->max_size)
    n = f->max_size;
  if (n > sizeof(ev))
    n = sizeof(ev);
  memcpy(&ev, buffer, n);

  if (ev.type >= NUM_RFKILL_TYPES)
    return (uint32_t)-22; // -EINVAL

  /* Collect changed switches so driver callbacks run with no lock held. */
  rfkill_switch_t *targets[RFKILL_MAX_SWITCHES];
  bool target_state[RFKILL_MAX_SWITCHES];
  size_t target_count = 0;
  int ret = 0;
  bool blocked = ev.soft != 0;

  spinlock_acquire(&g_lock);

  if (ev.op == RFKILL_OP_CHANGE_ALL) {
    /* Remember the requested default for radios hot-plugged later. */
    if (ev.type == RFKILL_TYPE_ALL) {
      for (int i = 0; i < NUM_RFKILL_TYPES; i++)
        g_soft_state[i] = blocked;
    } else {
      g_soft_state[ev.type] = blocked;
    }
  } else if (ev.op != RFKILL_OP_CHANGE) {
    ret = -22; // -EINVAL
  }

  for (size_t i = 0; ret == 0 && i < g_switch_count &&
                     target_count < RFKILL_MAX_SWITCHES;
       i++) {
    rfkill_switch_t *sw = g_switches[i];
    if (ev.op == RFKILL_OP_CHANGE && sw->idx != ev.idx)
      continue;
    if (ev.type != RFKILL_TYPE_ALL && sw->type != ev.type)
      continue;
    if (sw->soft_blocked == blocked)
      continue;

    sw->soft_blocked = blocked;
    rfkill_broadcast_locked(sw, RFKILL_OP_CHANGE);
    targets[target_count] = sw;
    target_state[target_count] = blocked;
    target_count++;
  }

  spinlock_release(&g_lock);

  for (size_t i = 0; i < target_count; i++) {
    if (targets[i]->set_block)
      targets[i]->set_block(targets[i], target_state[i]);
  }

  /* Success returns the number of bytes accepted, even when no switch matched
   * (same as Linux). */
  return ret ? (uint32_t)ret : n;
}

static int rfkill_vfs_poll(struct vfs_node *node, int events) {
  rfkill_file_t *f = (rfkill_file_t *)node->device;
  if (!f)
    return POLLNVAL;

  int revents = 0;
  if (events & (POLLOUT | POLLWRNORM))
    revents |= POLLOUT | POLLWRNORM;

  spinlock_acquire(&g_lock);
  if ((events & (POLLIN | POLLRDNORM)) && f->count > 0)
    revents |= POLLIN | POLLRDNORM;
  spinlock_release(&g_lock);

  return revents;
}

static int rfkill_vfs_ioctl(struct vfs_node *node, uint32_t request,
                            uint64_t arg) {
  rfkill_file_t *f = (rfkill_file_t *)node->device;
  if (!f)
    return -25; // -ENOTTY

  if (((request >> 8) & 0xFF) != RFKILL_IOC_MAGIC)
    return -25;

  switch (request & 0xFF) {
  case 1: /* RFKILL_IOCTL_NOINPUT — no rfkill-input handler to disable. */
    return 0;
  case 2: { /* RFKILL_IOCTL_MAX_SIZE */
    uint32_t *size = (uint32_t *)arg;
    if (!size)
      return -14; // -EFAULT
    if (*size < RFKILL_EVENT_SIZE_V1 || *size > 255)
      return -22; // -EINVAL
    f->max_size = (uint8_t)*size;
    return 0;
  }
  default:
    return -25; // -ENOTTY
  }
}

static void rfkill_vfs_close(struct vfs_node *node) {
  if (!node)
    return;
  rfkill_file_t *f = (rfkill_file_t *)node->device;
  if (!f)
    return;

  node->device = NULL;
  spinlock_acquire(&g_lock);
  list_del(&f->list);
  f->node = NULL;
  spinlock_release(&g_lock);
  kfree(f);
}

/* Per-open node factory: gives every descriptor its own event queue. */
static vfs_node_t *rfkill_open_instance(vfs_node_t *metadata) {
  (void)metadata;

  rfkill_file_t *f = kmalloc(sizeof(rfkill_file_t));
  if (!f)
    return NULL;
  memset(f, 0, sizeof(*f));
  f->max_size = RFKILL_EVENT_SIZE_V1;
  wait_queue_init(&f->read_wait);
  INIT_LIST_HEAD(&f->list);

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    kfree(f);
    return NULL;
  }
  vfs_node_init(node);
  strcpy(node->name, "rfkill");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->device = f;
  node->read = rfkill_vfs_read;
  node->write = rfkill_vfs_write;
  node->poll = rfkill_vfs_poll;
  node->ioctl = rfkill_vfs_ioctl;
  node->close = rfkill_vfs_close;
  node->wait_queue = &f->read_wait;
  f->node = node;
  node->refcount = 0; /* the descriptor owns the initial reference */

  /* New readers learn about switches that already exist, exactly like
   * rfkill_fop_open() enqueueing RFKILL_OP_ADD for each registered radio. */
  spinlock_acquire(&g_lock);
  for (size_t i = 0; i < g_switch_count; i++)
    rfkill_queue_event_locked(f, g_switches[i], RFKILL_OP_ADD);
  list_add_tail(&f->list, &g_files);
  spinlock_release(&g_lock);

  return node;
}

/* ------------------------------------------------------------------------ */
/* Driver API                                                               */
/* ------------------------------------------------------------------------ */

rfkill_switch_t *rfkill_register(uint8_t type, const char *name,
                                 void (*set_block)(rfkill_switch_t *, bool),
                                 void *data) {
  if (type == RFKILL_TYPE_ALL || type >= NUM_RFKILL_TYPES || !name)
    return NULL;

  rfkill_switch_t *sw = kmalloc(sizeof(rfkill_switch_t));
  if (!sw)
    return NULL;
  memset(sw, 0, sizeof(*sw));
  sw->type = type;
  sw->set_block = set_block;
  sw->data = data;
  strncpy(sw->name, name, sizeof(sw->name) - 1);

  spinlock_acquire(&g_lock);
  if (g_switch_count >= RFKILL_MAX_SWITCHES) {
    spinlock_release(&g_lock);
    kfree(sw);
    return NULL;
  }
  sw->idx = g_next_idx++;
  sw->soft_blocked = g_soft_state[type];
  g_switches[g_switch_count++] = sw;
  rfkill_broadcast_locked(sw, RFKILL_OP_ADD);
  spinlock_release(&g_lock);

  /* Tell the driver the default userspace chose for this type. */
  if (sw->soft_blocked && sw->set_block)
    sw->set_block(sw, true);

  return sw;
}

void rfkill_unregister(rfkill_switch_t *sw) {
  if (!sw)
    return;

  bool found = false;
  spinlock_acquire(&g_lock);
  for (size_t i = 0; i < g_switch_count; i++) {
    if (g_switches[i] != sw)
      continue;
    g_switches[i] = g_switches[--g_switch_count];
    g_switches[g_switch_count] = NULL;
    rfkill_broadcast_locked(sw, RFKILL_OP_DEL);
    found = true;
    break;
  }
  spinlock_release(&g_lock);

  if (found)
    kfree(sw);
}

void rfkill_set_sw_state(rfkill_switch_t *sw, bool blocked) {
  if (!sw)
    return;
  spinlock_acquire(&g_lock);
  if (sw->soft_blocked != blocked) {
    sw->soft_blocked = blocked;
    rfkill_broadcast_locked(sw, RFKILL_OP_CHANGE);
  }
  spinlock_release(&g_lock);
}

void rfkill_set_hw_state(rfkill_switch_t *sw, bool blocked) {
  if (!sw)
    return;
  spinlock_acquire(&g_lock);
  if (sw->hard_blocked != blocked) {
    sw->hard_blocked = blocked;
    rfkill_broadcast_locked(sw, RFKILL_OP_CHANGE);
  }
  spinlock_release(&g_lock);
}

bool rfkill_soft_blocked(const rfkill_switch_t *sw) {
  if (!sw)
    return false;
  spinlock_acquire(&g_lock);
  bool blocked = sw->soft_blocked;
  spinlock_release(&g_lock);
  return blocked;
}

bool rfkill_hard_blocked(const rfkill_switch_t *sw) {
  if (!sw)
    return false;
  spinlock_acquire(&g_lock);
  bool blocked = sw->hard_blocked;
  spinlock_release(&g_lock);
  return blocked;
}

/* ------------------------------------------------------------------------ */
/* Initialization                                                           */
/* ------------------------------------------------------------------------ */

void rfkill_init(void) {
  INIT_LIST_HEAD(&g_files);
  g_switch_count = 0;
  g_next_idx = 0;
  for (int i = 0; i < NUM_RFKILL_TYPES; i++)
    g_soft_state[i] = false;
  spinlock_init(&g_lock);

  vfs_node_init(&g_rfkill_node);
  strcpy(g_rfkill_node.name, "rfkill");
  g_rfkill_node.flags = FS_CHARDEV | FS_PERSISTENT;
  g_rfkill_node.mask = 0666;
  /* Linux exposes rfkill as misc device (major 10, minor 121). */
  g_rfkill_node.inode = (10u << 8) | 121u;
  g_rfkill_node.open_instance = rfkill_open_instance;

  /* This also registers the name in devfs and mounts it under /dev, which is
   * how open("/dev/rfkill") reaches the per-open factory. */
  fb_register_device_node("rfkill", &g_rfkill_node);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
            " rfkill core initialized (/dev/rfkill)\n");
}
