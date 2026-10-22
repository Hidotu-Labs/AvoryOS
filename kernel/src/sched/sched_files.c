#include "capability.h"
#include "pid_namespace.h"
#include "sched.h"
#include "sched_internal.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"

/* Keep fd_table/thread/VFS layouts unchanged: identity storage belongs to the
 * allocation containing the table, and is freed by its existing last put. */
struct fd_table_storage {
  struct fd_table table;
  uint64_t description_ids[MAX_FDS];
};
static uint64_t next_description_id;

/* Caller holds files->lock. Zero means a new, as-yet-unobserved description.
 * IDs survive dup/fork/SCM_RIGHTS without pointers or extra lifetime references. */
uint64_t fd_description_id_locked(struct thread *t, int fd) {
  if (!t || !t->files || fd < 0 || (uint32_t)fd >= t->files->capacity)
    return 0;
  uint64_t *id = &t->files->description_ids[fd];
  if (!*id) {
    do {
      *id = __atomic_add_fetch(&next_description_id, 1, __ATOMIC_RELAXED);
    } while (!*id);
  }
  return *id;
}

void fd_description_set_locked(struct thread *t, int fd, uint64_t id) {
  if (t && t->files && fd >= 0 && (uint32_t)fd < t->files->capacity)
    t->files->description_ids[fd] = id;
}

static struct fd_table *fd_table_create(void) {
  size_t initial_bytes = sizeof(struct fd_table) + (size_t)FD_INITIAL_CAPACITY * 48;
  struct fd_table *files = kmalloc(initial_bytes);
  if (!files)
    return NULL;
  memset(files, 0, initial_bytes);

  uint8_t *buf = (uint8_t *)(files + 1);
  files->array_buf = NULL; // inline
  files->reserved_nodes = (struct vfs_node **)buf;
  files->fds = (vfs_node_t **)(files->reserved_nodes + FD_INITIAL_CAPACITY);
  files->fd_offsets = (uint64_t *)(files->fds + FD_INITIAL_CAPACITY);
  files->fd_flags = (uint64_t *)(files->fd_offsets + FD_INITIAL_CAPACITY);
  files->fd_paths = (struct fd_path **)(files->fd_flags + FD_INITIAL_CAPACITY);
  files->description_ids = (uint64_t *)(files->fd_paths + FD_INITIAL_CAPACITY);
  files->capacity = FD_INITIAL_CAPACITY;
  files->ref_count = 1;
  spinlock_init(&files->lock);
  return files;
}

bool fd_table_expand_locked(struct fd_table *files, uint32_t needed_fd) {
  if (!files || needed_fd >= MAX_FDS)
    return false;

  uint32_t new_cap = files->capacity ? files->capacity * 2 : FD_INITIAL_CAPACITY;
  while (new_cap <= needed_fd && new_cap < MAX_FDS)
    new_cap *= 2;
  if (new_cap > MAX_FDS)
    new_cap = MAX_FDS;
  if (needed_fd >= new_cap || new_cap <= files->capacity)
    return false;

  size_t total_bytes = (size_t)new_cap * 48;
  void *new_buf = kmalloc(total_bytes);
  if (!new_buf)
    return false;
  memset(new_buf, 0, total_bytes);

  struct vfs_node **new_reserved = (struct vfs_node **)new_buf;
  vfs_node_t **new_fds = (vfs_node_t **)(new_reserved + new_cap);
  uint64_t *new_offsets = (uint64_t *)(new_fds + new_cap);
  uint64_t *new_flags = (uint64_t *)(new_offsets + new_cap);
  struct fd_path **new_paths = (struct fd_path **)(new_flags + new_cap);
  uint64_t *new_desc_ids = (uint64_t *)(new_paths + new_cap);

  uint32_t old_cap = files->capacity;
  if (old_cap > 0) {
    memcpy(new_reserved, files->reserved_nodes, old_cap * sizeof(struct vfs_node *));
    memcpy(new_fds, files->fds, old_cap * sizeof(vfs_node_t *));
    memcpy(new_offsets, files->fd_offsets, old_cap * sizeof(uint64_t));
    memcpy(new_flags, files->fd_flags, old_cap * sizeof(uint64_t));
    memcpy(new_paths, files->fd_paths, old_cap * sizeof(struct fd_path *));
    memcpy(new_desc_ids, files->description_ids, old_cap * sizeof(uint64_t));
  }

  void *old_buf = files->array_buf;

  files->reserved_nodes = new_reserved;
  files->fds = new_fds;
  files->fd_offsets = new_offsets;
  files->fd_flags = new_flags;
  files->fd_paths = new_paths;
  files->description_ids = new_desc_ids;
  files->array_buf = new_buf;
  files->capacity = new_cap;

  struct thread *curr = sched_get_current();
  if (curr && curr->files == files) {
    curr->fds = new_fds;
    curr->fd_offsets = new_offsets;
    curr->fd_flags = new_flags;
    curr->fd_paths = new_paths;
    curr->fd_capacity = new_cap;
  }

  if (old_buf)
    kfree(old_buf);

  return true;
}

static void thread_set_files(struct thread *t, struct fd_table *files) {
  t->files = files;
  t->fds = files ? files->fds : NULL;
  t->fd_offsets = files ? files->fd_offsets : NULL;
  t->fd_flags = files ? files->fd_flags : NULL;
  t->fd_paths = files ? files->fd_paths : NULL;
  t->fd_capacity = files ? files->capacity : 0;
}

static void fd_path_put(struct fd_path *path) {
  if (path && __atomic_sub_fetch(&path->ref_count, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(path);
}

bool fd_path_set(struct thread *t, int fd, const char *value) {
  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return false;
  if ((uint32_t)fd >= t->files->capacity) {
    if (!fd_table_expand_locked(t->files, (uint32_t)fd))
      return false;
  }
  struct fd_path *path = NULL;
  if (value && value[0]) {
    path = kmalloc(sizeof(*path));
    if (!path)
      return false;
    path->ref_count = 1;
    strncpy(path->value, value, sizeof(path->value) - 1);
    path->value[sizeof(path->value) - 1] = '\0';
  }
  struct fd_path *old = t->files->fd_paths[fd];
  t->files->fd_paths[fd] = path;
  fd_path_put(old);
  return true;
}

void fd_path_dup(struct thread *t, int dst, int src) {
  if (!t || !t->files || dst < 0 || src < 0 || dst >= MAX_FDS || src >= MAX_FDS)
    return;
  if ((uint32_t)src >= t->files->capacity)
    return;
  if ((uint32_t)dst >= t->files->capacity) {
    if (!fd_table_expand_locked(t->files, (uint32_t)dst))
      return;
  }
  /* Authoritative table pointers: a concurrent CLONE_FILES sibling may have
   * swapped the arrays via fd_table_expand_locked; the per-thread cache
   * (t->fd_paths) then aliases the freed, recycled old buffer. */
  struct fd_path *path = t->files->fd_paths[src];
  if (path)
    __atomic_add_fetch(&path->ref_count, 1, __ATOMIC_RELAXED);
  struct fd_path *old = t->files->fd_paths[dst];
  t->files->fd_paths[dst] = path;
  fd_path_put(old);
}

void fd_path_clear(struct thread *t, int fd) {
  if (!t || !t->files || fd < 0 || (uint32_t)fd >= t->files->capacity)
    return;
  struct fd_path *old = t->files->fd_paths[fd];
  t->files->fd_paths[fd] = NULL;
  fd_path_put(old);
}

const char *fd_path_value(struct thread *t, int fd) {
  if (!t || !t->files || fd < 0 ||
      (uint32_t)fd >= t->files->capacity || !t->files->fd_paths[fd])
    return NULL;
  return t->files->fd_paths[fd]->value;
}

bool sched_ensure_files(struct thread *t) {
  if (!t)
    return false;
  if (t->files) {
    thread_set_files(t, t->files);
    return true;
  }

  struct fd_table *files = fd_table_create();
  if (!files)
    return false;
  thread_set_files(t, files);
  return true;
}

void sched_release_files(struct thread *t) {
  if (!t)
    return;

  /* Serialize detaching the per-thread table with users that take a stable
   * snapshot under tid_lock (signal notification, proc fd inspection). */
  spinlock_acquire(&tid_lock);
  struct fd_table *files = t->files;
  if (!files) {
    spinlock_release(&tid_lock);
    return;
  }
  bool last = false;
  if (t->is_forked_child) {
    klog_debug_puts("[FDDBG] lock table ");
    klog_debug_hex64((uint64_t)files);
    klog_debug_puts(" refs=");
    klog_debug_uint64(files->ref_count);
    klog_debug_puts(" locked=");
    klog_debug_uint64(files->lock.locked);
    klog_debug_puts("\n");
  }
  spinlock_acquire(&files->lock);
  if (t->is_forked_child)
    klog_debug_puts("[FDDBG] table locked\n");
  if (--files->ref_count == 0)
    last = true;
  spinlock_release(&files->lock);
  thread_set_files(t, NULL);
  spinlock_release(&tid_lock);

  if (!last)
    return;

  for (uint32_t i = 0; i < files->capacity; i++) {
    if (files->reserved_nodes[i]) {
      vfs_close(files->reserved_nodes[i]);
      files->reserved_nodes[i] = NULL;
    }
    if (files->fds[i] && files->fds[i] != (vfs_node_t *)-1) {
      vfs_node_t *node = files->fds[i];
      if (t->is_forked_child) {
        klog_debug_puts("[FDDBG] closing fd ");
        klog_debug_uint64(i);
        klog_debug_puts(" node=");
        klog_debug_hex64((uint64_t)node);
        klog_debug_puts(" refs=");
        klog_debug_uint64(node->refcount);
        klog_debug_puts("\n");
      }
      files->fds[i] = NULL;
      fd_path_put(files->fd_paths[i]);
      files->fd_paths[i] = NULL;
      vfs_close(node);
      if (t->is_forked_child)
        klog_debug_puts("[FDDBG] close returned\n");
    }
  }
  if (files->array_buf)
    kfree(files->array_buf);
  kfree(files);
}

void sched_share_files(struct thread *child, struct thread *parent) {
  if (!child || !parent || !parent->files)
    return;

  sched_release_files(child);
  spinlock_acquire(&parent->files->lock);
  parent->files->ref_count++;

  spinlock_release(&parent->files->lock);
  thread_set_files(child, parent->files);
}

/* The child has a private, empty table and is not runnable yet. Snapshot each
 * descriptor, including its identity, while close/reuse is excluded. */
void sched_copy_files(struct thread *child, struct thread *parent) {
  if (!child || !parent || !parent->files || !child->files)
    return;

  if (child->files->capacity < parent->files->capacity) {
    spinlock_acquire(&child->files->lock);
    fd_table_expand_locked(child->files, parent->files->capacity - 1);
    spinlock_release(&child->files->lock);
    thread_set_files(child, child->files);
  }
  child->files->next_fd = parent->files->next_fd;

  uint32_t cap = parent->files->capacity;
  for (uint32_t fd = 0; fd < cap; fd++) {
    spinlock_acquire(&parent->files->lock);
    vfs_node_t *node = parent->fds[fd];
    if (!node || node == (vfs_node_t *)-1) {
      spinlock_release(&parent->files->lock);
      continue;
    }
    vfs_node_ref(node);
    child->files->fds[fd] = node;
    child->files->fd_offsets[fd] = parent->fd_offsets[fd];
    child->files->fd_flags[fd] = parent->fd_flags[fd];
    fd_description_set_locked(child, fd, fd_description_id_locked(parent, fd));
    struct fd_path *path = parent->fd_paths[fd];
    if (path)
      __atomic_add_fetch(&path->ref_count, 1, __ATOMIC_RELAXED);
    child->files->fd_paths[fd] = path;
    spinlock_release(&parent->files->lock);
    vfs_open(node);
    vfs_close(node); /* release snapshot pin after child owns the open */
  }
}

bool sched_get_nth_open_fd(uint32_t tid, uint32_t index, uint32_t *fd) {
  if (!fd)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  /* /proc/<pid> uses the TGID. For multithreaded processes (wireplumber,
   * pipewire) the TGID may differ from the thread holding the fd table
   * lookup, or the leader may share files with workers. Fall back to any
   * thread in the same group that has an fd table. */
  if ((!t || !t->files) && global_thread_list) {
    struct thread *curr = global_thread_list;
    while (curr) {
      if (curr->tgid == tid && curr->files) {
        t = curr;
        break;
      }
      curr = curr->global_next;
    }
  }
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  bool found = false;
  for (uint32_t i = 0; i < t->files->capacity; i++) {
    if (!t->fds[i] || t->fds[i] == (vfs_node_t *)-1)
      continue;
    if (index-- == 0) {
      *fd = i;
      found = true;
      break;
    }
  }
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  return found;
}

bool sched_get_fd_path_snapshot(uint32_t tid, uint32_t fd, char *path,
                                size_t path_size) {
  if (!path || !path_size || fd >= MAX_FDS)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if ((!t || !t->files) && global_thread_list) {
    struct thread *curr = global_thread_list;
    while (curr) {
      if (curr->tgid == tid && curr->files) {
        t = curr;
        break;
      }
      curr = curr->global_next;
    }
  }
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  if (fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    spinlock_release(&tid_lock);
    return false;
  }
  vfs_node_t *node = t->fds[fd];
  bool found = node && node != (vfs_node_t *)-1;
  if (found) {
    const char *value =
        t->fd_paths[fd] && t->fd_paths[fd]->value[0]
            ? t->fd_paths[fd]->value
            : node->name;
    strncpy(path, value, path_size - 1);
    path[path_size - 1] = 0;
  }
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  return found;
}

bool sched_get_fd_info_snapshot(uint32_t tid, uint32_t fd, uint64_t *out_pos,
                                uint32_t *out_flags, uint32_t *out_ino,
                                bool *out_is_pidfd, uint32_t *out_target_pid) {
  if (fd >= MAX_FDS)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if ((!t || !t->files) && global_thread_list) {
    struct thread *curr = global_thread_list;
    while (curr) {
      if (curr->tgid == tid && curr->files) {
        t = curr;
        break;
      }
      curr = curr->global_next;
    }
  }
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  if (fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    spinlock_release(&tid_lock);
    return false;
  }
  vfs_node_t *node = t->fds[fd];
  bool found = node && node != (vfs_node_t *)-1;
  if (found) {
    if (out_pos)
      *out_pos = t->fd_offsets[fd];
    if (out_flags)
      *out_flags = t->fd_flags[fd];
    if (out_ino)
      *out_ino = node->inode ? node->inode : fd;
    bool is_pidfd = (strcmp(node->name, "pidfd") == 0 && node->device != NULL);
    if (out_is_pidfd)
      *out_is_pidfd = is_pidfd;
    if (out_target_pid)
      *out_target_pid = is_pidfd ? *(uint32_t *)node->device : 0;
  }
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  return found;
}


/* Proc magic links hold a vnode reference, independent of descriptor lifetime.
 * Resolve only visible tasks, and require ptrace authority for another process. */
vfs_node_t *sched_get_fd_node_snapshot(uint32_t tid, uint32_t fd) {
  if (fd >= MAX_FDS) return NULL;
  struct thread *current = sched_get_current();
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !current || !pid_namespace_tid(current, t) || !t->files ||
      (t->tgid != current->tgid && !capability_has(current, CAP_SYS_PTRACE))) {
    spinlock_release(&tid_lock); return NULL;
  }
  spinlock_acquire(&t->files->lock);
  if (fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    spinlock_release(&tid_lock);
    return NULL;
  }
  vfs_node_t *node = t->fds[fd];
  if (node == (vfs_node_t *)-1) node = NULL;
  if (node) vfs_node_ref(node);
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  if (node && node->open) node->open(node);
  return node;
}
