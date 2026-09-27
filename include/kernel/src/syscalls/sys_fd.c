// sys_fd.c — Core file-descriptor syscalls:
//   open, openat, close, dup, dup2, read, write, readv, writev,
//   sendfile, lseek, fcntl, flock, ftruncate, fallocate, fsync, fadvise64
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../drivers/gpu/drm/drm.h"
#include "../font/font.h"
#include "../fs/procfs.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "../fs/ext2.h"
#include "../fs/ext4.h"
#include "../drivers/storage/block.h"
#include "arch/uaccess.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// FD allocation helpers
// ---------------------------------------------------------------------------

int alloc_fd(struct thread *t) {
  if (!t || !t->files)
    return -1;
  spinlock_acquire(&t->files->lock);
  for (int i = (int)t->files->next_fd; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      t->fds[i] = FD_RESERVED;
      t->files->next_fd = (uint32_t)i + 1;
      spinlock_release(&t->files->lock);
      return i;
    }
  }
  spinlock_release(&t->files->lock);
  return -1;
}

int alloc_fd_from(struct thread *t, int from) {
  if (!t || !t->files || from < 0 || from >= MAX_FDS)
    return -1;
  spinlock_acquire(&t->files->lock);
  int start = from;
  if ((uint32_t)start < t->files->next_fd)
    start = (int)t->files->next_fd;
  for (int i = start; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      t->fds[i] = FD_RESERVED;
      t->files->next_fd = (uint32_t)i + 1;
      spinlock_release(&t->files->lock);
      return i;
    }
  }
  spinlock_release(&t->files->lock);
  return -1;
}

// ---------------------------------------------------------------------------
// open / openat
// ---------------------------------------------------------------------------

uint64_t sys_open_path(int dirfd, const char *path, uint64_t flags,
                            uint64_t mode) {
  (void)dirfd;
  if (!path)
    return (uint64_t)-14; // EFAULT

#if SYSCALL_LOG
  klog_puts("[SYSCALL] open path=\"");
  klog_puts(path);
  klog_puts("\"\n");
#endif

  (void)flags;
  (void)mode;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  /* O_TMPFILE asks for an anonymous inode that is only linked into the
   * namespace later (through linkat on /proc/self/fd).  AvoryOS has neither
   * unlinked inodes nor /proc fd materialisation, and quietly accepting the
   * request would hand back a descriptor for the *directory* itself: Qt's
   * QTemporaryFile then reports an empty file name, which is what makes KIO's
   * QLocalServer::listen("") fail and every KIO worker refuse to start.
   * Answering like a pre-3.11 kernel lets those callers fall back to a named
   * temporary file, which is the behaviour they all already implement.
   *
   * __O_TMPFILE shares its bit with O_PATH, so O_TMPFILE is only in play when
   * the caller also asked for write access.  Reading the same bits as
   * read-only plus O_DIRECTORY is an O_PATH handle, which Qt's QProcess uses
   * for the child's working directory (open(dir, O_RDONLY|O_DIRECTORY|O_PATH)
   * followed by fchdir) - rejecting that makes Konsole report
   * "Could not start program '/bin/bash'". */
  if ((flags & O_TMPFILE) == O_TMPFILE && (flags & O_ACCMODE) != O_RDONLY)
    return (uint64_t)-95; // EOPNOTSUPP

  int fd;

  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if (dirfd == AT_FDCWD) {
      base_dir = t->cwd_node ? t->cwd_node : fs_root;
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  vfs_node_t *node = NULL;
  bool node_owned = false;
  const char *dev_path = NULL;
  if (strncmp(path, "/dev/", 5) == 0)
    dev_path = path + 5;
  else if (strncmp(path, "dev/", 4) == 0)
    dev_path = path + 4;

  if (dev_path) {
    if (strcmp(dev_path, "dri/card0") == 0) {
      node = drm_create_client_node();
      if (!node)
        return (uint64_t)-12;
    }

    if (strcmp(dev_path, "ptmx") == 0) {
      int pty_index = pty_alloc_pair();
      if (pty_index < 0)
        return (uint64_t)-16; // EBUSY

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty)
        return (uint64_t)-16;

      node = kmalloc(sizeof(vfs_node_t));
      if (!node)
        return (uint64_t)-12; // ENOMEM
      vfs_node_init(node);
      strcpy(node->name, "ptmx");
      node->flags = FS_CHARDEV;
      node->mask = 0666;
      node->read = ptmx_read;
      node->write = ptmx_write;
      node->ioctl = ptmx_ioctl;
      node->poll = ptmx_poll;
      node->close = ptmx_close;
      node->mmap = ptmx_mmap;
      node->device = pty;
      node->wait_queue = pty->master_waitq;
      pty->master_node = node;
      klog_puts("[PTYMASTER] pair=");
      klog_uint64((uint64_t)pty->index);
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strncmp(dev_path, "pts/", 4) == 0) {
      const char *num_str = dev_path + 4;
      int pty_index = 0;
      while (*num_str >= '0' && *num_str <= '9') {
        pty_index = pty_index * 10 + (*num_str - '0');
        num_str++;
      }
      if (*num_str != '\0')
        return (uint64_t)-2;

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty || pty->locked)
        return (uint64_t)-2;

      node = kmalloc(sizeof(vfs_node_t));
      if (!node)
        return (uint64_t)-12;
      vfs_node_init(node);
      strcpy(node->name, dev_path);
      node->flags = FS_CHARDEV;
      node->mask = 0620;
      node->uid = t->euid;
      node->gid = t->egid;
      node->read = pty_slave_read;
      node->write = pty_slave_write;
      node->ioctl = pty_slave_ioctl;
      node->poll = pty_slave_poll;
      node->open = pty_slave_open;
      node->close = pty_slave_close;
      node->mmap = pty_slave_mmap;
      node->device = pty;
      node->wait_queue = pty->slave_waitq;
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strcmp(dev_path, "tty") == 0) {
      struct thread *ct = sched_get_current();
      if (ct && ct->ctty) {
        node = ct->ctty;
        goto open_done;
      }
    }

    if (!node)
      node = fb_lookup_device((char *)dev_path);
  }

  if (!node) {
    node = vfs_resolve_path_at(base_dir, path);
    node_owned = node != NULL;
  }

  if (!node) {
    if (flags & O_CREAT) {
      char parent_path[512];
      char file_name[256];
      size_t len = strlen(path);
      if (len == 0 || len >= 4096)
        return (uint64_t)-14;

      const char *slash = 0;
      for (const char *p = path; *p; p++)
        if (*p == '/')
          slash = p;

      vfs_node_t *parent = base_dir;
      bool parent_owned = false;
      if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
          parent = fs_root;
        } else {
          if (parent_len >= sizeof(parent_path))
            return (uint64_t)-14;
          for (size_t i = 0; i < parent_len; i++)
            parent_path[i] = path[i];
          parent_path[parent_len] = '\0';
          parent = vfs_resolve_path_at(base_dir, parent_path);
          parent_owned = parent != NULL;
        }
        size_t file_len = strlen(slash + 1);
        if (file_len >= sizeof(file_name)) {
          if (parent_owned) vfs_close(parent);
          return (uint64_t)-14;
        }
        strcpy(file_name, slash + 1);
      } else {
        size_t file_len = len;
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, path);
      }

      if (!parent)
        return (uint64_t)-2; // ENOENT
      if ((parent->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-20; // ENOTDIR
      }
      if (!vfs_access(parent, 3)) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-13;
      }

      mode &= ~t->umask;
      if (vfs_create(parent, file_name, (uint16_t)mode) != 0) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-17;
      }

      node = vfs_finddir(parent, file_name);
      if (!node) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-2;
      }
      node_owned = true;
      uint32_t new_gid = (parent->mask & 02000) ? parent->gid : t->fsgid;
      vfs_chown(node, t->fsuid, new_gid);
      if (parent_owned) vfs_close(parent);
    } else {
      return (uint64_t)-2; // ENOENT
    }
  } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-17; // EEXIST
  }

  uint32_t requested = 0;
  if ((flags & O_ACCMODE) == O_RDONLY) requested = 4;
  else if ((flags & O_ACCMODE) == O_WRONLY) requested = 2;
  else if ((flags & O_ACCMODE) == O_RDWR) requested = 6;
  if (requested && !vfs_access(node, requested)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-13;
  }

  /* Directories are read-only descriptors (readdir(2) is the only thing you
   * can do with one).  Returning a writable handle would let callers believe
   * they created something inside the directory. */
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY &&
      (flags & O_ACCMODE) != O_RDONLY) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-21; // EISDIR
  }

  if ((flags & O_TRUNC) && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    vfs_truncate(node, 0);
    node->length = 0;
  }

  /* Persistent metadata may provide a fresh per-open object.  DRM was the
   * first user of this pattern; driver capability handles use it as well. */
  if (node->open_instance) {
    vfs_node_t *prev = node;
    node = node->open_instance(prev);
    if (!node) {
      if (node_owned) vfs_close(prev);
      return (uint64_t)-12;
    }
    node_owned = false; // fresh per-open node owns no resolver reference
  } else if (drm_is_card_node(node)) {
    vfs_node_t *prev = node;
    node = drm_create_client_node();
    if (!node) {
      if (node_owned) vfs_close(prev);
      return (uint64_t)-12;
    }
    node_owned = false; // fresh client node owns no resolver reference
  }

open_done:
  fd = alloc_fd(t);
  if (fd < 0) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-24; // EMFILE
  }

  vfs_open(node);
  if (node_owned) vfs_close(node); // descriptor holds its own reference now
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = flags & ~(uint64_t)O_CLOEXEC;
  if (flags & O_CLOEXEC)
    t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
  if (strcmp(node->name, "ptmx") == 0) {
    klog_puts(" fd=");
    klog_uint64((uint64_t)fd);
    klog_puts(" tid=");
    klog_uint64(t->tid);
    klog_puts("\n");
  }

  char full_path[256];
  if (path[0] == '/') {
    strncpy(full_path, path, sizeof(full_path) - 1);
    full_path[sizeof(full_path) - 1] = '\0';
  } else {
    full_path[0] = '\0';
    if (t->cwd_path[0] && strcmp(t->cwd_path, "/") != 0) {
      strncpy(full_path, t->cwd_path, sizeof(full_path) - 1);
      full_path[sizeof(full_path) - 1] = '\0';
      strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
      strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    } else {
      strcpy(full_path, "/");
      strncat(full_path, path, sizeof(full_path) - 2);
    }
    full_path[sizeof(full_path) - 1] = '\0';
  }
  fd_path_set(t, fd, full_path);

  return fd;
}

static uint64_t sys_openat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                           uint64_t mode, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  return (uint64_t)(int)sys_open_path((int)dirfd, (const char *)path_ptr, flags,
                                    mode);
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_openat(AT_FDCWD, path_ptr, flags, mode, 0, 0);
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  t->fds[fd] = NULL;
  fd_path_clear(t, (int)fd);
  if (fd < t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
  spinlock_release(&t->files->lock);
  vfs_close(node);
  return 0;
}

/* Linux close_range(2).  VTE uses this while preparing its shell child. */
#define CLOSE_RANGE_UNSHARE (1U << 1)
#define CLOSE_RANGE_CLOEXEC (1U << 2)
static uint64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (first > last || (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)))
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t || !t->files)
    return (uint64_t)-9;
  klog_debugf("[CLOSE_RANGE] tid=%llu first=%llu last=%llu flags=0x%llx\n",
              (unsigned long long)t->tid, (unsigned long long)first,
              (unsigned long long)last, (unsigned long long)flags);
  if (first >= MAX_FDS)
    return 0;
  if (last >= MAX_FDS)
    last = MAX_FDS - 1;

  /* File tables are already private for forked children.  CLONE_UNSHARE is
   * accepted here; callers using it only need the range operation itself. */
  if (flags & CLOSE_RANGE_CLOEXEC) {
    spinlock_acquire(&t->files->lock);
    for (uint64_t fd = first; fd <= last; fd++) {
      if (t->fds[fd] && t->fds[fd] != FD_RESERVED)
        t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
    }
    spinlock_release(&t->files->lock);
    return 0;
  }

  for (uint64_t fd = first; fd <= last; fd++) {
    spinlock_acquire(&t->files->lock);
    vfs_node_t *node = t->fds[fd];
    if (!node || node == FD_RESERVED) {
      spinlock_release(&t->files->lock);
      continue;
    }
    t->fds[fd] = NULL;
    t->fd_offsets[fd] = 0;
    t->fd_flags[fd] = 0;
    fd_path_clear(t, (int)fd);
    if (fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    vfs_close(node);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// dup / dup2
// ---------------------------------------------------------------------------

static uint64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9;

  int newfd = alloc_fd(t);
  if (newfd < 0)
    return (uint64_t)-24;

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  t->fd_flags[newfd] = t->fd_flags[oldfd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  fd_path_dup(t, newfd, (int)oldfd);
  vfs_open(t->fds[newfd]);
  return newfd;
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || newfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9;
  if (oldfd == newfd)
    return newfd;
  if (t->fds[newfd])
    vfs_close(t->fds[newfd]);

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  t->fd_flags[newfd] = t->fd_flags[oldfd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  fd_path_dup(t, (int)newfd, (int)oldfd);
  vfs_open(t->fds[newfd]);
  return newfd;
}

// ---------------------------------------------------------------------------
// read / write helpers
// ---------------------------------------------------------------------------

static void trace_userspace_debug_write(struct thread *t, int fd,
                                        const void *buf, size_t count) {
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd] || !buf || count == 0)
    return;

  const char *prefix = NULL;
  if (strcmp(t->fds[fd]->name, "weston-debug.log") == 0)
    prefix = "[WESTON-LOG] ";
  else if (strcmp(t->fds[fd]->name, "xfwm4.log") == 0)
    prefix = "[XFWM4-LOG] ";
  else
    return;

  klog_puts(prefix);
  const char *s = (const char *)buf;
  for (size_t i = 0; i < count; i++) {
    char c = s[i];
    if (c == '\0')
      break;
    klog_putchar(c);
  }
  if (((const char *)buf)[count - 1] != '\n')
    klog_putchar('\n');
}

static int64_t fd_write(int fd, const void *buf, size_t count) {
  struct thread *t = sched_get_current();
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd])
    return -9;

  vfs_node_t *node = t->fds[fd];
  trace_userspace_debug_write(t, fd, buf, count);
  int32_t bytes_written =
      (int32_t)vfs_write(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  if (bytes_written > 0)
    t->fd_offsets[fd] += (uint32_t)bytes_written;
  return (int64_t)bytes_written;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_read =
      (int32_t)vfs_read(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  if (bytes_read > 0)
    t->fd_offsets[fd] += (uint32_t)bytes_read;

  return (uint64_t)(int64_t)bytes_read;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  return (uint64_t)fd_write((int)fd, (const void *)buf, (size_t)count);
}

static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                            uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_read =
      (int32_t)vfs_read(node, (uint32_t)offset, count, (uint8_t *)buf);
  return (uint64_t)(int64_t)bytes_read;
}

static uint64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
                             uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_written =
      (int32_t)vfs_write(node, (uint32_t)offset, count, (uint8_t *)buf);
  return (uint64_t)(int64_t)bytes_written;
}

static uint64_t sys_readv(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  if (!is_user_range((const void *)iov_u, iovcnt * sizeof(*iov)))
    return (uint64_t)-14;

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_range((const void *)base, len))
      return (uint64_t)-14;

    struct thread *ct = sched_get_current();
    if (!ct || fd >= MAX_FDS || !ct->fds[fd])
      return (uint64_t)-9;

    vfs_node_t *node = ct->fds[fd];
    int32_t bytes_read =
        (int32_t)vfs_read(node, ct->fd_offsets[fd], len, (uint8_t *)base);
    if (bytes_read < 0) {
      if (total > 0)
        break;
      return (uint64_t)(int64_t)bytes_read;
    }
    if (bytes_read > 0)
      ct->fd_offsets[fd] += (uint32_t)bytes_read;
    total += (size_t)bytes_read;
    if ((uint32_t)bytes_read < len)
      break;
  }
  return total;
}

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  if (!is_user_range((const void *)iov_u, iovcnt * sizeof(*iov)))
    return (uint64_t)-14;

  for (uint64_t i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len > 0 &&
        !is_user_range((const void *)iov[i].iov_base, iov[i].iov_len))
      return (uint64_t)-14;
  }

  /* A socket writev is one send operation.  Splitting it into write calls can
   * interleave vector fragments with another writer and corrupt protocols. */
  socket_t *sock = socket_from_fd((int)fd);
  if (sock && socket_try_get(sock)) {
    if (sock->closing) {
      socket_put(sock);
      return (uint64_t)-32;
    }
    struct msghdr msg = {0};
    msg.msg_iov = (struct iovec *)iov;
    msg.msg_iovlen = (size_t)iovcnt;
    ssize_t ret = sock->ops && sock->ops->sendmsg
                      ? sock->ops->sendmsg(sock, &msg, 0)
                      : -95;
    socket_put(sock);
    return (uint64_t)ret;
  }

  size_t total = 0;
  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;

    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0)
      return total > 0 ? total : (uint64_t)w;
    if (w == 0 && len != 0) {
      /* A non-empty writev must not report a zero-byte success: callers such
       * as GNU ld retry it forever. Leave a focused diagnostic while finding
       * the backing filesystem path responsible for the short write. */
      struct thread *t = sched_get_current();
      klog_puts("[WRITEV] zero write fd=");
      klog_uint64(fd);
      klog_puts(" len=");
      klog_uint64(len);
      klog_puts(" node=");
      if (t && fd < MAX_FDS && t->fds[fd])
        klog_puts(t->fds[fd]->name);
      else
        klog_puts("(invalid)");
      klog_puts(" flags=");
      if (t && fd < MAX_FDS && t->fds[fd])
        klog_uint64(t->fds[fd]->flags);
      else
        klog_puts("0");
      klog_puts("\n");
      return total > 0 ? total : (uint64_t)-5; /* EIO */
    }
    total += (size_t)w;
    if ((size_t)w != len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// sendfile
// ---------------------------------------------------------------------------

static uint64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd,
                             uint64_t offset_ptr, uint64_t count, uint64_t a4,
                             uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || out_fd >= MAX_FDS || in_fd >= MAX_FDS || !t->fds[out_fd] ||
      !t->fds[in_fd])
    return (uint64_t)-9;

  vfs_node_t *out_node = t->fds[out_fd];
  vfs_node_t *in_node = t->fds[in_fd];

  uint32_t offset;
  if (offset_ptr) {
    if (!vmm_is_user_addr_range_valid(offset_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    offset = (uint32_t)(*(uint64_t *)offset_ptr);
  } else {
    offset = t->fd_offsets[in_fd];
  }

  uint8_t *buffer = kmalloc(4096);
  if (!buffer)
    return (uint64_t)-12;

  uint32_t total_sent = 0;
  while (total_sent < count) {
    uint32_t to_read =
        (count - total_sent > 4096) ? 4096 : (uint32_t)(count - total_sent);
    int32_t bytes_read =
        (int32_t)vfs_read(in_node, offset + total_sent, to_read, buffer);
    if (bytes_read <= 0)
      break;
    int32_t bytes_written = (int32_t)vfs_write(out_node, t->fd_offsets[out_fd],
                                               (uint32_t)bytes_read, buffer);
    if (bytes_written <= 0)
      break;
    t->fd_offsets[out_fd] += (uint32_t)bytes_written;
    total_sent += (uint32_t)bytes_written;
    if (bytes_written < bytes_read)
      break;
  }

  kfree(buffer);
  if (offset_ptr)
    *(uint64_t *)offset_ptr = (uint64_t)(offset + total_sent);
  else
    t->fd_offsets[in_fd] = offset + total_sent;

  return (uint64_t)total_sent;
}

// ---------------------------------------------------------------------------
// copy_file_range / splice
// ---------------------------------------------------------------------------
//
// Both move bytes between two descriptors under Linux's offset rules: a
// non-NULL offset is used and updated in place, a NULL one means "use the
// descriptor's own position and advance it". Pipes have no position, so their
// offset argument is ignored.
//
// The bytes travel through a kernel bounce buffer. A real splice(2) hands
// page-cache frames to the pipe without copying, but neither the page cache nor
// the ramfs-backed pipe here has such a hand-off, so copying is what is
// actually correct. Callers depend on the bytes arriving and the offsets being
// updated, not on the transfer being zero-copy.

#define RANGE_CHUNK (64 * 1024)

#define SPLICE_F_MOVE     0x01
#define SPLICE_F_NONBLOCK 0x02
#define SPLICE_F_MORE     0x04
#define SPLICE_F_GIFT     0x08

static bool fd_is_pipe(vfs_node_t *node) {
  return node && (node->flags & FS_TYPE_MASK) == FS_PIPE;
}

static int64_t move_range(struct thread *t, uint64_t fd_in, uint64_t off_in_ptr,
                          uint64_t fd_out, uint64_t off_out_ptr, uint64_t len) {
  vfs_node_t *in_node = t->fds[fd_in];
  vfs_node_t *out_node = t->fds[fd_out];
  bool in_pipe = fd_is_pipe(in_node);
  bool out_pipe = fd_is_pipe(out_node);

  if (!in_node->read || !out_node->write)
    return -22; /* EINVAL: cannot read from, or write to, this descriptor */

  if (len == 0)
    return 0;

  bool in_tracked = !in_pipe && off_in_ptr != 0;
  bool out_tracked = !out_pipe && off_out_ptr != 0;

  uint64_t in_off = 0, out_off = 0;
  if (in_tracked) {
    if (!is_user_range((const void *)off_in_ptr, sizeof(uint64_t)) ||
        copy_from_user(&in_off, (const void *)off_in_ptr, sizeof(in_off)) != 0)
      return -14;
  } else if (!in_pipe) {
    in_off = t->fd_offsets[fd_in];
  }

  if (out_tracked) {
    if (!is_user_range((const void *)off_out_ptr, sizeof(uint64_t)) ||
        copy_from_user(&out_off, (const void *)off_out_ptr,
                       sizeof(out_off)) != 0)
      return -14;
  } else if (!out_pipe) {
    out_off = t->fd_offsets[fd_out];
  }

  if (in_off > 0xFFFFFFFFULL || out_off > 0xFFFFFFFFULL)
    return -27; /* EFBIG: the VFS works in 32-bit offsets */

  uint8_t *buf = kmalloc(RANGE_CHUNK);
  if (!buf)
    return -12;

  int64_t moved = 0;

  while ((uint64_t)moved < len) {
    uint64_t remaining = len - (uint64_t)moved;
    uint32_t want =
        (uint32_t)(remaining > RANGE_CHUNK ? RANGE_CHUNK : remaining);

    int32_t got = (int32_t)vfs_read(in_node, (uint32_t)in_off, want, buf);
    if (got <= 0) {
      /* Zero is end of file, or an empty pipe with no writers left. A negative
       * value is already an errno from the read implementation (EAGAIN for a
       * non-blocking pipe, for instance). */
      if (got < 0 && moved == 0)
        moved = got;
      break;
    }

    int32_t put =
        (int32_t)vfs_write(out_node, (uint32_t)out_off, (uint32_t)got, buf);
    if (put < 0) {
      if (moved == 0)
        moved = put; /* EPIPE for a pipe nobody is reading, etc. */
      break;
    }
    if (put == 0) {
      if (moved == 0)
        moved = -28; /* ENOSPC: the sink took nothing at all */
      break;
    }

    in_off += (uint64_t)got;
    out_off += (uint64_t)put;
    moved += put;

    if (put < got)
      break; /* short write: full pipe, non-blocking sink, or device limit */
    if (in_off > 0xFFFFFFFFULL || out_off > 0xFFFFFFFFULL)
      break;
  }

  kfree(buf);

  if (moved < 0)
    return moved;

  /* Publish positions for what was actually transferred. An explicit offset
   * goes back to the caller's variable; a NULL one means the descriptor's own
   * position moves. */
  if (!in_pipe) {
    if (in_tracked) {
      if (copy_to_user((void *)off_in_ptr, &in_off, sizeof(in_off)) != 0 &&
          moved == 0)
        return -14;
    } else {
      t->fd_offsets[fd_in] = in_off;
    }
  }
  if (!out_pipe) {
    if (out_tracked) {
      if (copy_to_user((void *)off_out_ptr, &out_off, sizeof(out_off)) != 0 &&
          moved == 0)
        return -14;
    } else {
      t->fd_offsets[fd_out] = out_off;
    }
  }

  return moved;
}

static uint64_t sys_copy_file_range(uint64_t fd_in, uint64_t off_in,
                                    uint64_t fd_out, uint64_t off_out,
                                    uint64_t len, uint64_t flags) {
  struct thread *t = sched_get_current();
  if (!t || fd_in >= MAX_FDS || fd_out >= MAX_FDS || !t->fds[fd_in] ||
      !t->fds[fd_out])
    return (uint64_t)-9;

  if (flags != 0)
    return (uint64_t)-22; /* copy_file_range(2) defines no flags yet */

  return (uint64_t)move_range(t, fd_in, off_in, fd_out, off_out, len);
}

static uint64_t sys_splice(uint64_t fd_in, uint64_t off_in, uint64_t fd_out,
                           uint64_t off_out, uint64_t len, uint64_t flags) {
  struct thread *t = sched_get_current();
  if (!t || fd_in >= MAX_FDS || fd_out >= MAX_FDS || !t->fds[fd_in] ||
      !t->fds[fd_out])
    return (uint64_t)-9;

  /* splice(2) moves data through a pipe. A plain file-to-file request is
   * copy_file_range(2), and Linux rejects it here with EINVAL. */
  if (!fd_is_pipe(t->fds[fd_in]) && !fd_is_pipe(t->fds[fd_out]))
    return (uint64_t)-22;

  if (flags & ~(uint64_t)(SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_MORE |
                          SPLICE_F_GIFT))
    return (uint64_t)-22;

  /* MOVE and GIFT describe how pages are handed to the pipe, which a copying
   * implementation may ignore. NONBLOCK is honoured through the descriptor's
   * own O_NONBLOCK flag, which is what the pipe read and write paths consult. */
  return (uint64_t)move_range(t, fd_in, off_in, fd_out, off_out, len);
}

// ---------------------------------------------------------------------------
// lseek
// ---------------------------------------------------------------------------

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int64_t new_offset = 0;
  if (whence == 0)
    new_offset = (int64_t)offset;
  else if (whence == 1)
    new_offset = (int64_t)t->fd_offsets[fd] + (int64_t)offset;
  else if (whence == 2)
    new_offset = (int64_t)node->length + (int64_t)offset;
  else
    return (uint64_t)-22;

  if (new_offset < 0)
    return (uint64_t)-22;
  t->fd_offsets[fd] = (uint32_t)new_offset;
  return (uint64_t)new_offset;
}

// ---------------------------------------------------------------------------
// fcntl
// ---------------------------------------------------------------------------

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  switch (cmd) {
  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
    int newfd = alloc_fd_from(t, (int)arg);
    if (newfd < 0)
      return (uint64_t)-24;
    t->fds[newfd] = t->fds[fd];
    t->fd_offsets[newfd] = t->fd_offsets[fd];
    t->fd_flags[newfd] = t->fd_flags[fd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
    if (cmd == F_DUPFD_CLOEXEC)
      t->fd_flags[newfd] |= FD_FLAGS_CLOEXEC_BIT;
    fd_path_dup(t, newfd, (int)fd);
    vfs_open(t->fds[newfd]);
    return (uint64_t)newfd;
  }
  case F_GETFD: {
    uint64_t dflags = 0;
    if (t->fd_flags[fd] & FD_FLAGS_CLOEXEC_BIT)
      dflags |= 1;
    return dflags;
  }
  case F_SETFD: {
    if (arg & 1)
      t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
    else
      t->fd_flags[fd] &= ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
    return 0;
  }
  case F_GETFL: {
    uint64_t fl = t->fd_flags[fd] & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    vfs_node_t *node = t->fds[fd];
    if (node && (node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      fl = (fl & ~O_ACCMODE) | O_RDWR;
      socket_t *sock = (socket_t *)node->device;
      if (sock && (sock->flags & SOCK_NONBLOCK))
        fl |= O_NONBLOCK;
    }
    if (node && (node->flags & FS_NONBLOCK))
      fl |= O_NONBLOCK;
    return fl;
  }
  case F_SETFL: {
    uint64_t status_flags = arg & (O_APPEND | O_NONBLOCK);
    t->fd_flags[fd] =
        (t->fd_flags[fd] & ~(O_APPEND | O_NONBLOCK)) | status_flags;
    vfs_node_t *node = t->fds[fd];
    if (node && (node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      socket_t *sock = (socket_t *)node->device;
      if (sock) {
        if (arg & O_NONBLOCK)
          sock->flags |= SOCK_NONBLOCK;
        else
          sock->flags &= ~SOCK_NONBLOCK;
      }
    }
    if (node) {
      if (arg & O_NONBLOCK)
        node->flags |= FS_NONBLOCK;
      else
        node->flags &= ~FS_NONBLOCK;
    }
    return 0;
  }
  case F_SETOWN:
    (void)arg;
    return 0;
  case F_GETLK:
  case 12: // F_GETLK64
  case 36: { // F_OFD_GETLK
    /* This kernel does not track byte-range locks yet, but a probe must still
     * answer honestly: callers pass their desired lock type in and treat the
     * value left in l_type as the answer, so returning 0 without touching it
     * makes every query look like a conflicting write lock is held (SQLite's
     * WAL setup performs exactly this check).  Report F_UNLCK. */
    if (arg && is_user_range((const void *)arg, 2))
      *(volatile int16_t *)arg = 2; // F_UNLCK
    return 0;
  }
  case F_SETLK:
  case F_SETLKW:
  case 13: // F_SETLK64
  case 14: // F_SETLKW64
  case 37: // F_OFD_SETLK
  case 38: // F_OFD_SETLKW
    return 0;
  case 1031: // F_SETPIPE_SZ
    return (uint64_t)(arg > 0 ? arg : 65536);
  case 1032: // F_GETPIPE_SZ
    return 65536;
  case 1033: { // F_ADD_SEALS
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) != FS_FILE)
      return (uint64_t)-22;
    node->impl |= (uint32_t)arg;
    return 0;
  }
  case 1034: { // F_GET_SEALS
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) != FS_FILE)
      return (uint64_t)-22;
    return (uint64_t)node->impl;
  }
  default:
    klog_puts("[SYSCALL] sys_fcntl: unhandled cmd=");
    klog_uint64(cmd);
    klog_puts("\n");
    return (uint64_t)-22;
  }
}

// ---------------------------------------------------------------------------
// ftruncate / fallocate / flock / fsync / fadvise64
// ---------------------------------------------------------------------------

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-1;
  return vfs_truncate(node, (uint32_t)length) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                              uint64_t len, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (!node)
    return (uint64_t)-9;
  if ((node->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-22;

  klog_puts("[SYSCALL] fallocate: fd=");
  klog_uint64(fd);
  klog_puts(" mode=");
  klog_uint64(mode);
  klog_puts(" offset=");
  klog_uint64(offset);
  klog_puts(" len=");
  klog_uint64(len);
  klog_puts("\n");

  int ret = vfs_fallocate(node, (int)mode, (uint32_t)offset, (uint32_t)len);
  klog_puts("[SYSCALL] fallocate: result=");
  klog_uint64((uint64_t)(int64_t)ret);
  klog_puts("\n");
  if (ret != 0)
    return ret == -1 ? (uint64_t)-95 : (uint64_t)ret; // -95 = -EOPNOTSUPP
  return 0;
}

static uint64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)fd;
  (void)operation;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}

static uint64_t sys_fsync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_cache_sync(t->fds[fd]);
  return 0;
}

static uint64_t sys_fdatasync(uint64_t fd, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  return sys_fsync(fd, a1, a2, a3, a4, a5);
}

static uint64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len,
                              uint64_t advice, uint64_t a4, uint64_t a5) {
  (void)fd;
  (void)offset;
  (void)len;
  (void)advice;
  (void)a4;
  (void)a5;
  return 0;
}

static uint64_t sys_mount(uint64_t source_ptr, uint64_t target_ptr,
                          uint64_t fstype_ptr, uint64_t flags,
                          uint64_t data_ptr, uint64_t a5) {
    (void)fstype_ptr;
    (void)flags;
    (void)data_ptr;
    (void)a5;

    const char *source = (const char *)source_ptr;
    const char *target = (const char *)target_ptr;

    if (!source || !target)
        return (uint64_t)-14;

    vfs_node_t *mountpoint = vfs_resolve_path(target);
    if (!mountpoint)
        return (uint64_t)-2;
    if ((mountpoint->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        vfs_close(mountpoint);
        return (uint64_t)-20;
    }

    const char *dev_name = source;
    if (strncmp(source, "/dev/", 5) == 0)
        dev_name = source + 5;

    struct block_device *dev = NULL;
    int n = block_count();
    for (int i = 0; i < n; i++) {
        struct block_device *d = block_get(i);
        if (d && strcmp(d->name, dev_name) == 0) {
            dev = d;
            break;
        }
    }

    if (!dev) {
        vfs_close(mountpoint);
        return (uint64_t)-6;
    }

    if (ext4_mount(dev, mountpoint) == 0)
        return 0;
    if (ext2_mount(dev, mountpoint) == 0)
        return 0;

    /* Both probes failed before touching mountpoint: drop our resolver ref.
     * On success the filesystem adopts the node in place (clearing
     * FS_PERSISTENT), so that reference is what keeps the mounted root alive
     * and must not be dropped. */
    vfs_close(mountpoint);
    return (uint64_t)-22;
}

void syscall_register_fd(void) {
  syscall_register(SYS_READ, sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_PREAD64, sys_pread64);
  syscall_register(SYS_PWRITE64, sys_pwrite64);
  syscall_register(SYS_READV, sys_readv);
  syscall_register(SYS_WRITEV, sys_writev);
  syscall_register(SYS_OPEN, sys_open);
  syscall_register(SYS_OPENAT, sys_openat);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_CLOSE_RANGE, sys_close_range);
  syscall_register(SYS_DUP, sys_dup);
  syscall_register(SYS_DUP2, sys_dup2);
  syscall_register(SYS_LSEEK, sys_lseek);
  syscall_register(SYS_FCNTL, sys_fcntl);
  syscall_register(SYS_FTRUNCATE, sys_ftruncate);
  syscall_register(SYS_FALLOCATE, sys_fallocate);
  syscall_register(SYS_FLOCK, sys_flock);
  syscall_register(SYS_FSYNC, sys_fsync);
  syscall_register(SYS_FDATASYNC, sys_fdatasync);
  syscall_register(SYS_MOUNT, sys_mount);
  syscall_register(SYS_SENDFILE, sys_sendfile);
  syscall_register(SYS_COPY_FILE_RANGE, sys_copy_file_range);
  syscall_register(SYS_SPLICE, sys_splice);
  syscall_register(SYS_FADVISE64, sys_fadvise64);
}
