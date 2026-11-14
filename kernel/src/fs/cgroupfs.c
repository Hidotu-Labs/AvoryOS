/* Process membership for the cgroup2 hierarchy.
 *
 * cgroup2 is a tmpfs with stub control files (tmpfs_cgroup_populate) and no
 * resource control. This file only tracks which cgroup each process is in, so
 * cgroup.procs, cgroup.threads and cgroup.events read back live state:
 * systemd waits for a unit's cgroup to empty before a stop completes, and
 * kills whatever cgroup.procs still lists.
 *
 * Membership is per process (tgid), as on Linux: writing a PID to
 * cgroup.procs moves its process, a fork inherits the parent's cgroup, and a
 * process absent from the table is in the root cgroup. Exited processes are
 * dropped lazily on the next read. A new process always passes through
 * cgroupfs_fork, which overwrites any stale entry left by a reused PID.
 *
 * Lock order: members_lock, then tid_lock (via sched_get_thread_snapshot). */

#include "cgroupfs.h"
#include "inotify.h"
#include "tmpfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../sched/pid_namespace.h"
#include "../sched/sched.h"

struct cgroupfs_group {
  struct cgroupfs_group *parent;
  vfs_node_t *events; /* cgroup.events, for IN_MODIFY when it empties */
  bool removed;
};

struct cgroupfs_member {
  uint32_t tgid;
  struct cgroupfs_group *group;
  struct cgroupfs_member *next;
};

#define CGROUPFS_BUCKETS 256
static struct cgroupfs_member *members[CGROUPFS_BUCKETS];
static spinlock_t members_lock = SPINLOCK_INIT;

static struct cgroupfs_member **bucket_of(uint32_t tgid) {
  return &members[tgid % CGROUPFS_BUCKETS];
}

static bool tgid_alive(uint32_t tgid) {
  struct sched_thread_snapshot s;
  return sched_get_thread_snapshot(tgid, &s) && s.tgid == tgid &&
         s.state != THREAD_ZOMBIE && s.state != THREAD_DEAD;
}

static bool group_within(const struct cgroupfs_group *g,
                         const struct cgroupfs_group *ancestor) {
  for (; g; g = g->parent)
    if (g == ancestor)
      return true;
  return false;
}

/* Calls fn for every live member of group (or of its subtree when
 * subtree is set) and frees entries of exited processes. Caller holds
 * members_lock. Returns the number of live members visited. */
static uint32_t walk_members_locked(struct cgroupfs_group *group, bool subtree,
                                    void (*fn)(uint32_t tgid, void *ctx),
                                    void *ctx) {
  uint32_t count = 0;
  for (int b = 0; b < CGROUPFS_BUCKETS; b++) {
    struct cgroupfs_member **link = &members[b];
    while (*link) {
      struct cgroupfs_member *m = *link;
      if (!tgid_alive(m->tgid)) {
        *link = m->next;
        kfree(m);
        continue;
      }
      if (subtree ? group_within(m->group, group) : m->group == group) {
        count++;
        if (fn)
          fn(m->tgid, ctx);
      }
      link = &m->next;
    }
  }
  return count;
}

/* Sets tgid's cgroup (NULL = root). Returns the group it left. */
static struct cgroupfs_group *attach_locked(uint32_t tgid,
                                            struct cgroupfs_group *group,
                                            struct cgroupfs_member *spare) {
  struct cgroupfs_member **link = bucket_of(tgid);
  for (; *link; link = &(*link)->next) {
    if ((*link)->tgid != tgid)
      continue;
    struct cgroupfs_member *m = *link;
    struct cgroupfs_group *old = m->group;
    if (group) {
      m->group = group;
    } else {
      *link = m->next;
      kfree(m);
    }
    if (spare)
      kfree(spare);
    return old;
  }
  if (group && spare) {
    spare->tgid = tgid;
    spare->group = group;
    spare->next = *bucket_of(tgid);
    *bucket_of(tgid) = spare;
  } else if (spare) {
    kfree(spare);
  }
  return NULL;
}

/* After a move, tell watchers of every cgroup the process left that has now
 * emptied (systemd watches cgroup.events with inotify). */
static void notify_emptied(struct cgroupfs_group *left,
                           struct cgroupfs_group *joined) {
  vfs_node_t *events[16];
  int n = 0;
  spinlock_acquire(&members_lock);
  for (struct cgroupfs_group *g = left; g && n < 16; g = g->parent) {
    if (group_within(joined, g))
      break; /* still populated through the cgroup it joined */
    if (g->events && !g->removed &&
        walk_members_locked(g, true, NULL, NULL) == 0) {
      vfs_node_ref(g->events);
      events[n++] = g->events;
    }
  }
  spinlock_release(&members_lock);
  for (int i = 0; i < n; i++) {
    inotify_notify(events[i], IN_MODIFY, NULL, 0);
    vfs_close(events[i]);
  }
}

static void move_process(uint32_t tgid, struct cgroupfs_group *group) {
  struct cgroupfs_member *spare =
      group ? kmalloc(sizeof(struct cgroupfs_member)) : NULL;
  spinlock_acquire(&members_lock);
  struct cgroupfs_group *old = attach_locked(tgid, group, spare);
  spinlock_release(&members_lock);
  if (old && old != group)
    notify_emptied(old, group);
}

struct cgroupfs_group *cgroupfs_group_create(struct cgroupfs_group *parent) {
  struct cgroupfs_group *g = kmalloc(sizeof(*g));
  if (!g)
    return NULL;
  g->parent = parent;
  g->events = NULL;
  g->removed = false;
  return g;
}

/* Groups are never freed: control-file nodes may outlive the directory
 * through open descriptors and still point here. Each is a few words. */
bool cgroupfs_group_remove(struct cgroupfs_group *group) {
  if (!group)
    return true;
  spinlock_acquire(&members_lock);
  bool empty = walk_members_locked(group, true, NULL, NULL) == 0;
  if (empty) {
    group->removed = true;
    group->events = NULL;
  }
  spinlock_release(&members_lock);
  return empty;
}

struct procs_buf {
  char *out;
  uint32_t len, cap;
};

static void append_pid(uint32_t tgid, void *ctx) {
  struct procs_buf *b = ctx;
  uint32_t visible = pid_namespace_visible(sched_get_current(), tgid);
  if (!visible || b->len + 12 > b->cap)
    return;
  b->len += (uint32_t)snprintf(b->out + b->len, b->cap - b->len, "%u\n",
                               visible);
}

static uint32_t copy_window(const char *text, uint32_t len, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  if (offset >= len)
    return 0;
  if (size > len - offset)
    size = len - offset;
  memcpy(buffer, text + offset, size);
  return size;
}

/* Bound control files keep their group in node->ptr, the fs-private pointer
 * sysfs and procfs use too (impl is only 32 bits). */
static struct cgroupfs_group *group_of(vfs_node_t *node) {
  return (struct cgroupfs_group *)node->ptr;
}

static uint32_t cgroupfs_procs_read(vfs_node_t *node, uint32_t offset,
                                    uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  spinlock_acquire(&members_lock);
  uint32_t count = walk_members_locked(g, false, NULL, NULL);
  spinlock_release(&members_lock);

  struct procs_buf b = {.len = 0, .cap = (count + 8) * 12};
  b.out = kmalloc(b.cap);
  if (!b.out)
    return 0;
  spinlock_acquire(&members_lock);
  walk_members_locked(g, false, append_pid, &b);
  spinlock_release(&members_lock);
  uint32_t n = copy_window(b.out, b.len, offset, size, buffer);
  kfree(b.out);
  return n;
}

static uint32_t cgroupfs_events_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  struct cgroupfs_group *g = group_of(node);
  if (!g)
    return 0;
  spinlock_acquire(&members_lock);
  bool populated = walk_members_locked(g, true, NULL, NULL) != 0;
  spinlock_release(&members_lock);
  char text[32];
  int len = snprintf(text, sizeof(text), "populated %d\nfrozen 0\n",
                     populated ? 1 : 0);
  return copy_window(text, (uint32_t)len, offset, size, buffer);
}

/* Writing a PID (0 = the writer) moves that process into this cgroup.
 * A thread ID moves its whole process, as cgroup.procs does on Linux. */
static uint32_t cgroupfs_procs_write(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct cgroupfs_group *g = group_of(node);
  if (!g || g->removed)
    return size;
  uint32_t pid = 0;
  for (uint32_t i = 0; i < size && buffer[i] >= '0' && buffer[i] <= '9'; i++)
    pid = pid * 10 + (uint32_t)(buffer[i] - '0');

  struct thread *current = sched_get_current();
  uint32_t tgid = 0;
  if (pid == 0) {
    tgid = current ? (current->tgid ? current->tgid : current->tid) : 0;
  } else {
    struct sched_thread_snapshot s;
    if (sched_get_thread_snapshot(pid, &s))
      tgid = s.tgid ? s.tgid : s.tid;
  }
  if (tgid)
    move_process(tgid, g);
  return size;
}

void cgroupfs_bind_file(vfs_node_t *file, struct cgroupfs_group *group,
                        enum cgroupfs_file kind) {
  file->ptr = (vfs_node_t *)group;
  switch (kind) {
  case CGROUPFS_PROCS:
  case CGROUPFS_THREADS:
    file->read = cgroupfs_procs_read;
    file->write = cgroupfs_procs_write;
    break;
  case CGROUPFS_EVENTS:
    file->read = cgroupfs_events_read;
    file->write = NULL;
    group->events = file;
    break;
  }
}

void cgroupfs_fork(struct thread *parent, struct thread *child) {
  if (!parent || !child)
    return;
  uint32_t ptgid = parent->tgid ? parent->tgid : parent->tid;
  uint32_t ctgid = child->tgid ? child->tgid : child->tid;
  struct cgroupfs_member *spare = kmalloc(sizeof(struct cgroupfs_member));
  spinlock_acquire(&members_lock);
  struct cgroupfs_group *group = NULL;
  for (struct cgroupfs_member *m = *bucket_of(ptgid); m; m = m->next) {
    if (m->tgid == ptgid) {
      group = m->group;
      break;
    }
  }
  attach_locked(ctgid, group, spare);
  spinlock_release(&members_lock);
}

int cgroupfs_clone_into(struct thread *child, vfs_node_t *dir) {
  struct cgroupfs_group *group = tmpfs_cgroup_of(dir);
  if (!group || group->removed)
    return -9; /* EBADF: not a cgroup2 directory */
  move_process(child->tgid ? child->tgid : child->tid, group);
  return 0;
}
