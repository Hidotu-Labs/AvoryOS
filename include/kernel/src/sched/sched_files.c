#include "sched.h"
#include "sched_internal.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"

static struct fd_table *fd_table_create(void) {
  struct fd_table *files = kmalloc(sizeof(struct fd_table));
  if (!files)
    return NULL;
  memset(files, 0, sizeof(struct fd_table));
  files->ref_count = 1;
  spinlock_init(&files->lock);
  return files;
}

static void thread_set_files(struct thread *t, struct fd_table *files) {
  t->files = files;
  t->fds = files ? files->fds : NULL;
  t->fd_offsets = files ? files->fd_offsets : NULL;
  t->fd_flags = files ? files->fd_flags : NULL;
  t->fd_paths = files ? files->fd_paths : NULL;
}

static void fd_path_put(struct fd_path *path) {
  if (path && __atomic_sub_fetch(&path->ref_count, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(path);
}

bool fd_path_set(struct thread *t, int fd, const char *value) {
  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return false;
  struct fd_path *path = NULL;
  if (value && value[0]) {
    path = kmalloc(sizeof(*path));
    if (!path)
      return false;
    path->ref_count = 1;
    strncpy(path->value, value, sizeof(path->value) - 1);
    path->value[sizeof(path->value) - 1] = '\0';
  }
  struct fd_path *old = t->fd_paths[fd];
  t->fd_paths[fd] = path;
  fd_path_put(old);
  return true;
}

void fd_path_dup(struct thread *t, int dst, int src) {
  if (!t || dst < 0 || dst >= MAX_FDS || src < 0 || src >= MAX_FDS)
    return;
  struct fd_path *path = t->fd_paths[src];
  if (path)
    __atomic_add_fetch(&path->ref_count, 1, __ATOMIC_RELAXED);
  struct fd_path *old = t->fd_paths[dst];
  t->fd_paths[dst] = path;
  fd_path_put(old);
}

void fd_path_clear(struct thread *t, int fd) {
  if (!t || fd < 0 || fd >= MAX_FDS)
    return;
  struct fd_path *old = t->fd_paths[fd];
  t->fd_paths[fd] = NULL;
  fd_path_put(old);
}

const char *fd_path_value(struct thread *t, int fd) {
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fd_paths[fd])
    return NULL;
  return t->fd_paths[fd]->value;
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
  if (!t || !t->files)
    return;

  struct fd_table *files = t->files;
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

  if (!last)
    return;

  for (int i = 0; i < MAX_FDS; i++) {
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

bool sched_get_nth_open_fd(uint32_t tid, uint32_t index, uint32_t *fd) {
  if (!fd)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  bool found = false;
  for (uint32_t i = 0; i < MAX_FDS; i++) {
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
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
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
