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
static uint64_t fd_table_description_id_locked(struct fd_table *files, int fd) {
  if (!files || fd < 0 || (uint32_t)fd >= files->capacity)
    return 0;
  uint64_t *id = &files->description_ids[fd];
  if (!*id) {
    do {
      *id = __atomic_add_fetch(&next_description_id, 1, __ATOMIC_RELAXED);
    } while (!*id);
  }
  return *id;
}

uint64_t fd_description_id_locked(struct thread *t, int fd) {
  if (!t)
    return 0;
  return fd_table_description_id_locked(t->files, fd);
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
    size_t len = 0;
    while (len < 255 && value[len])
      len++;
    path = kmalloc(sizeof(*path) + len + 1);
    if (!path)
      return false;
    path->offset = t->files->fd_offsets[fd];
    path->ref_count = 1;
    memcpy(path->value, value, len);
    path->value[len] = '\0';
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

static void fd_table_destroy(struct fd_table *files, bool debug_forked) {
  for (uint32_t i = 0; i < files->capacity; i++) {
    if (files->reserved_nodes[i]) {
      vfs_close(files->reserved_nodes[i]);
      files->reserved_nodes[i] = NULL;
    }
    if (files->fds[i] && files->fds[i] != (vfs_node_t *)-1) {
      vfs_node_t *node = files->fds[i];
      if (debug_forked) {
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
      if (debug_forked)
        klog_debug_puts("[FDDBG] close returned\n");
    }
  }
  if (files->array_buf)
    kfree(files->array_buf);
  kfree(files);
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
  bool debug_forked = t->is_forked_child;
  thread_set_files(t, NULL);
  spinlock_release(&tid_lock);

  if (!last)
    return;

  fd_table_destroy(files, debug_forked);
}

void sched_share_files(struct thread *child, struct thread *parent) {
  if (!child || !parent || !parent->files)
    return;

  sched_release_files(child);
  spinlock_acquire(&parent->files->lock);
  parent->files->ref_count++;
  /* Every thread caches this table's array pointers (t->fds, t->fd_offsets,
   * ...), but fd_table_expand_locked() can only refresh the caller's copy
   * before freeing the old buffer. Siblings then read and write freed memory
   * (stale descriptors, scribbles into recycled heap). Size a shared table to
   * its maximum once, here, so it never has to be reallocated while shared. */
  if (parent->files->capacity < MAX_FDS)
    fd_table_expand_locked(parent->files, MAX_FDS - 1);
  spinlock_release(&parent->files->lock);
  thread_set_files(parent, parent->files);
  thread_set_files(child, parent->files);
}

static void fd_table_copy_locked(struct fd_table *dst, struct fd_table *src,
                                 vfs_node_t **stack_nodes, int stack_cap,
                                 vfs_node_t ***dyn_nodes_out,
                                 int *pinned_count_out) {
  dst->next_fd = src->next_fd;
  uint32_t cap = src->capacity;

  int total_open = 0;
  for (uint32_t fd = 0; fd < cap; fd++) {
    if (src->fds[fd] && src->fds[fd] != (vfs_node_t *)-1)
      total_open++;
  }

  vfs_node_t **dyn_nodes = NULL;
  vfs_node_t **nodes_to_open = stack_nodes;
  if (total_open > stack_cap) {
    dyn_nodes = kmalloc((size_t)total_open * sizeof(vfs_node_t *));
    if (dyn_nodes)
      nodes_to_open = dyn_nodes;
  }

  int pinned_count = 0;
  int max_pins = dyn_nodes ? total_open : stack_cap;
  for (uint32_t fd = 0; fd < cap; fd++) {
    vfs_node_t *node = src->fds[fd];
    if (!node || node == (vfs_node_t *)-1)
      continue;

    vfs_node_ref(node);
    dst->fds[fd] = node;
    struct fd_path *desc = src->fd_paths[fd];
    dst->fd_offsets[fd] = desc
        ? __atomic_load_n(&desc->offset, __ATOMIC_RELAXED)
        : src->fd_offsets[fd];
    dst->fd_flags[fd] = src->fd_flags[fd];
    if (fd < dst->capacity)
      dst->description_ids[fd] = fd_table_description_id_locked(src, (int)fd);
    if (desc)
      __atomic_add_fetch(&desc->ref_count, 1, __ATOMIC_RELAXED);
    dst->fd_paths[fd] = desc;

    if (pinned_count < max_pins)
      nodes_to_open[pinned_count++] = node;
  }

  *dyn_nodes_out = dyn_nodes;
  *pinned_count_out = pinned_count;
}

static void fd_table_finish_copy(vfs_node_t **stack_nodes,
                                 vfs_node_t **dyn_nodes, int pinned_count) {
  vfs_node_t **nodes_to_open = dyn_nodes ? dyn_nodes : stack_nodes;
  for (int i = 0; i < pinned_count; i++) {
    vfs_node_t *node = nodes_to_open[i];
    vfs_open(node);
    vfs_close(node); /* release snapshot pin after new table owns the open */
  }
  if (dyn_nodes)
    kfree(dyn_nodes);
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

  vfs_node_t *pinned_nodes[64];
  vfs_node_t **dyn_nodes = NULL;
  int pinned_count = 0;

  spinlock_acquire(&parent->files->lock);
  fd_table_copy_locked(child->files, parent->files, pinned_nodes, 64,
                       &dyn_nodes, &pinned_count);
  spinlock_release(&parent->files->lock);

  fd_table_finish_copy(pinned_nodes, dyn_nodes, pinned_count);
}

bool sched_unshare_files(struct thread *t) {
  if (!t || !t->files)
    return false;

  spinlock_acquire(&t->files->lock);
  if (t->files->ref_count <= 1) {
    spinlock_release(&t->files->lock);
    return true;
  }
  uint32_t needed_cap = t->files->capacity;
  spinlock_release(&t->files->lock);

  struct fd_table *new_files = fd_table_create();
  if (!new_files)
    return false;

  vfs_node_t *pinned_nodes[64];
  vfs_node_t **dyn_nodes = NULL;
  int pinned_count = 0;
  struct fd_table *old_files = NULL;

  for (;;) {
    if (needed_cap > new_files->capacity &&
        !fd_table_expand_locked(new_files, needed_cap - 1)) {
      fd_table_destroy(new_files, false);
      return false;
    }

    old_files = t->files;
    spinlock_acquire(&old_files->lock);
    if (old_files->ref_count <= 1) {
      spinlock_release(&old_files->lock);
      fd_table_destroy(new_files, false);
      return true;
    }
    if (old_files->capacity > new_files->capacity) {
      needed_cap = old_files->capacity;
      spinlock_release(&old_files->lock);
      continue;
    }

    fd_table_copy_locked(new_files, old_files, pinned_nodes, 64,
                         &dyn_nodes, &pinned_count);
    spinlock_release(&old_files->lock);
    break;
  }

  fd_table_finish_copy(pinned_nodes, dyn_nodes, pinned_count);

  spinlock_acquire(&tid_lock);
  thread_set_files(t, new_files);
  spinlock_release(&tid_lock);

  bool last = false;
  spinlock_acquire(&old_files->lock);
  if (--old_files->ref_count == 0)
    last = true;
  spinlock_release(&old_files->lock);
  if (last)
    fd_table_destroy(old_files, false);
  return true;
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
      *out_pos = fd_offset_get_locked(t, fd);
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
vfs_node_t *sched_get_fd_node_snapshot(uint32_t tid, uint32_t fd,
                                      bool socket_metadata_only) {
  if (fd >= MAX_FDS) return NULL;
  struct thread *current = sched_get_current();
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !current || !pid_namespace_tid(current, t) || !t->files ||
      (!socket_metadata_only && t->tgid != current->tgid &&
       !capability_has(current, CAP_SYS_PTRACE))) {
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
  if (socket_metadata_only && node && (node->flags & FS_TYPE_MASK) != FS_SOCKET)
    node = NULL;
  if (node) vfs_node_ref(node);
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  if (node && node->open) node->open(node);
  return node;
}
