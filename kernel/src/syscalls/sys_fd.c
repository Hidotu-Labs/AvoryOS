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
#include "../fs/fifo.h"
#include "../fs/fuse.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/sched_internal.h"
#include "../socket/socket.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "sysdiag.h"
#include "../fs/ext2.h"
#include "../fs/ext4.h"
#include "../fs/exfat.h"
#include "../fs/fat32.h"
#include "../fs/cgroupfs.h"
#include "../fs/tmpfs.h"
#include "../drivers/storage/block.h"
#include "arch/uaccess.h"
#include <stdint.h>
#include <linuxkpi/native_vfs.h>

// ---------------------------------------------------------------------------
// FD allocation helpers
// ---------------------------------------------------------------------------

/* lock held. The hint accelerates allocation, but is not proof of exhaustion:
 * a legacy release path may have left a reusable descriptor below it. */
static int fd_find_free_locked(struct thread *t, int from) {
  if (from < 0)
    from = 0;
  if (from >= MAX_FDS)
    return -1;

  if ((uint32_t)from >= t->files->capacity) {
    if (!fd_table_expand_locked(t->files, (uint32_t)from))
      return -1;
  }

  uint32_t hint = t->files->next_fd;
  if (hint > t->files->capacity)
    hint = t->files->capacity;
  uint32_t start = from > (int)hint ? (uint32_t)from : hint;
  uint32_t cap = t->files->capacity;

  /* Fast-path: hint points directly to a free slot in the 99% common case! */
  if (start < cap && !t->files->fds[start]) {
    return (int)start;
  }

  for (uint32_t i = start + 1; i < cap; i++) {
    if (!t->files->fds[i])
      return (int)i;
  }
  for (uint32_t i = (uint32_t)from; i < start; i++) {
    if (!t->files->fds[i]) {
      t->files->next_fd = 0;
      return (int)i;
    }
  }

  /* Expand table on demand if capacity < MAX_FDS */
  if (t->files->capacity < MAX_FDS) {
    uint32_t old_cap = t->files->capacity;
    if (fd_table_expand_locked(t->files, old_cap)) {
      return (int)old_cap;
    }
  }

  return -1;
}



int alloc_fd(struct thread *t) {
  if (!t || !t->files)
    return -1;
  spinlock_acquire(&t->files->lock);
  int fd = fd_find_free_locked(t, 0);
  if (fd >= 0) {
    fd_description_set_locked(t, fd, 0);
    t->files->fds[fd] = FD_RESERVED;
    t->files->next_fd = (uint32_t)fd + 1;
  }
  spinlock_release(&t->files->lock);
  return fd;
}

int alloc_fd_from(struct thread *t, int from) {
  if (!t || !t->files || from < 0 || from >= MAX_FDS)
    return -1;
  spinlock_acquire(&t->files->lock);
  int fd = fd_find_free_locked(t, from);
  if (fd >= 0) {
    fd_description_set_locked(t, fd, 0);
    t->files->fds[fd] = FD_RESERVED;
    /* A minimum-fd allocation must not hide free slots below it. */
    if ((uint32_t)fd == t->files->next_fd)
      t->files->next_fd = (uint32_t)fd + 1;
  }
  spinlock_release(&t->files->lock);
  return fd;
}

/* Undo an open/socket/pipe reservation without leaving the free-slot hint
 * past the released descriptor. Failure paths must not manufacture EMFILE. */
void fd_release_reserved(struct thread *t, int fd) {
  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return;
  vfs_node_t *reserved = NULL;
  spinlock_acquire(&t->files->lock);
  if ((uint32_t)fd < t->files->capacity && t->files->fds[fd] == FD_RESERVED) {
    reserved = t->files->reserved_nodes[fd];
    t->files->reserved_nodes[fd] = NULL;
    t->files->fds[fd] = NULL;
    t->files->fd_offsets[fd] = 0;
    t->files->fd_flags[fd] = 0;
    fd_path_clear(t, fd);
    if ((uint32_t)fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
  }
  spinlock_release(&t->files->lock);
  if (reserved)
    vfs_close(reserved);
}

/*
 * Borrowed vs referenced descriptor token.
 * In single-threaded execution (t->files->ref_count == 1, no CLONE_FILES),
 * no peer thread can close or reassign the descriptor during a synchronous
 * syscall. We borrow the pointer directly with ZERO locks and ZERO atomic
 * refcount churn (modeled on Linux's fdget/fdput optimization).
 */
struct fd_ref {
  vfs_node_t *node;
  bool borrowed;
};

static inline struct fd_ref fd_get_node(struct thread *t, uint64_t fd,
                                        uint64_t *offset_out,
                                        uint64_t *fd_flags_out) {
  struct fd_ref ref = { .node = NULL, .borrowed = false };
  if (!t || !t->files || fd >= MAX_FDS)
    return ref;

  /* Fast-path: single-threaded file table (ref_count == 1).
   * No concurrent thread shares this table, so no thread can close or replace
   * this descriptor under our feet during synchronous syscall handling.
   * Skip files->lock and skip vfs_node_ref() atomic operations entirely. */
  if (t->files->ref_count == 1) {
    if (fd < t->files->capacity) {
      vfs_node_t *node = t->files->fds[fd];
      if (node && node != FD_RESERVED) {
        ref.node = node;
        ref.borrowed = true;
        if (offset_out)
          *offset_out = fd_offset_get_locked(t, fd);
        if (fd_flags_out)
          *fd_flags_out = t->files->fd_flags[fd];
        return ref;
      }
    }
    return ref;
  }

  /* Slow-path: multi-threaded shared file table (CLONE_FILES).
   * A sibling thread could concurrently close/reuse this slot while I/O is in flight.
   * Acquire files->lock and bump node->refcount. */
  spinlock_acquire(&t->files->lock);
  if (fd < t->files->capacity) {
    vfs_node_t *node = t->files->fds[fd];
    if (node && node != FD_RESERVED) {
      vfs_node_ref(node);
      ref.node = node;
      ref.borrowed = false;
      if (offset_out)
        *offset_out = fd_offset_get_locked(t, fd);
      if (fd_flags_out)
        *fd_flags_out = t->files->fd_flags[fd];
    }
  }
  spinlock_release(&t->files->lock);
  return ref;
}

static inline void fd_put_node(struct fd_ref ref) {
  if (ref.node && !ref.borrowed) {
    vfs_close(ref.node);
  }
}

/* Pin the node while an fd operation runs. CLONE_FILES threads can close and
 * reuse an fd concurrently; holding only the pointer lets the last close tear
 * down a tmpfs node while its read callback or page-cache fill is in flight. */
static vfs_node_t *fd_pin_node(struct thread *t, uint64_t fd,
                               uint64_t *offset_out,
                               uint64_t *fd_flags_out) {
  if (!t || !t->files || fd >= MAX_FDS)
    return NULL;

  spinlock_acquire(&t->files->lock);
  if (fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return NULL;
  }
  vfs_node_t *node = t->files->fds[fd];
  if (!node || node == FD_RESERVED) {
    node = NULL;
  } else {
    vfs_node_ref(node);
    if (offset_out)
      *offset_out = fd_offset_get_locked(t, fd);
    if (fd_flags_out)
      *fd_flags_out = t->files->fd_flags[fd];
  }
  spinlock_release(&t->files->lock);
  return node;
}

static void fd_advance_offset(struct thread *t, uint64_t fd,
                              vfs_node_t *node, uint64_t amount) {
  if (!t || !t->files || !node || fd >= MAX_FDS || !amount)
    return;
  /* Fast-path for single-threaded tables: skip files->lock */
  if (t->files->ref_count == 1) {
    if (fd < t->files->capacity && t->files->fds[fd] == node)
      fd_offset_add_locked(t, fd, amount);
    return;
  }
  spinlock_acquire(&t->files->lock);
  /* A concurrent close may have reused this slot for a different file. */
  if (fd < t->files->capacity && t->files->fds[fd] == node)
    fd_offset_add_locked(t, fd, amount);
  spinlock_release(&t->files->lock);
}

static void fd_set_offset(struct thread *t, uint64_t fd,
                           vfs_node_t *node, uint64_t offset) {
  if (!t || !t->files || !node || fd >= MAX_FDS)
    return;
  /* Fast-path for single-threaded tables: skip files->lock */
  if (t->files->ref_count == 1) {
    if (fd < t->files->capacity && t->files->fds[fd] == node)
      fd_offset_set_locked(t, fd, offset);
    return;
  }
  spinlock_acquire(&t->files->lock);
  if (fd < t->files->capacity && t->files->fds[fd] == node)
    fd_offset_set_locked(t, fd, offset);
  spinlock_release(&t->files->lock);
}

/* Vectored I/O descriptor snapshot. Small counts (the common 1-4 iovec
 * Wayland/file pattern) stay on the caller's stack; larger ones use the
 * heap. Pair with fd_release_iov(). Returns 0 on success, -12 on allocation
 * failure, -14 on a bad user pointer. */
#include "../fs/mount_namespace.h"
#include "../sched/capability.h"
#include "../fs/ext3/ext3.h"
#define fs_root vfs_task_root()

#define FD_IOV_STACK_MAX 16u

static int fd_snap_iov(uint64_t iov_u, uint64_t iovcnt,
                       struct user_iovec *stack, struct user_iovec **out) {
  struct user_iovec *iov =
      iovcnt <= FD_IOV_STACK_MAX ? stack : kmalloc(iovcnt * sizeof(*iov));
  if (!iov)
    return -12;
  struct thread *t = sched_get_current();
  if (t && t->seccomp_compat_dispatch) {
    struct { uint32_t base, length; } narrow;
    for (uint64_t i = 0; i < iovcnt; i++) {
      if (copy_from_user(&narrow, (void *)(iov_u + i * sizeof(narrow)), sizeof(narrow))) {
        if (iov != stack) kfree(iov);
        return -14;
      }
      iov[i].iov_base = narrow.base;
      iov[i].iov_len = narrow.length;
    }
  } else if (copy_from_user(iov, (const void *)iov_u,
                            iovcnt * sizeof(*iov))) {
    if (iov != stack) kfree(iov);
    return -14;
  }
  *out = iov;
  return 0;
}

static void fd_release_iov(struct user_iovec *iov, struct user_iovec *stack) {
  if (iov && iov != stack)
    kfree(iov);
}

static void fd_release_kvec(struct iovec *vec, struct iovec *stack) {
  if (vec && vec != stack)
    kfree(vec);
}

/* /proc/self/fd/N is a magic link on Linux: opening it reopens the referenced
 * object, rather than resolving the printable target returned by readlink.
 * Firefox uses this to create a read-only descriptor for a memfd. */
static bool parse_proc_self_fd_path(const char *path, uint64_t *fd_out) {
  static const char prefix[] = "/proc/self/fd/";
  static const char dev_prefix[] = "/dev/fd/";
  if (!path || !fd_out)
    return false;
  const char *p;
  if (strncmp(path, prefix, sizeof(prefix) - 1) == 0)
    p = path + sizeof(prefix) - 1;
  else if (strncmp(path, dev_prefix, sizeof(dev_prefix) - 1) == 0)
    p = path + sizeof(dev_prefix) - 1;
  else
    return false;
  if (*p < '0' || *p > '9')
    return false;
  uint64_t fd = 0;
  do {
    uint32_t digit = (uint32_t)(*p - '0');
    if (fd > (UINT64_MAX - digit) / 10)
      return false;
    fd = fd * 10 + digit;
    p++;
  } while (*p >= '0' && *p <= '9');
  if (*p != '\0')
    return false;

  *fd_out = fd;
  return true;
}

// ---------------------------------------------------------------------------
// open / openat
// ---------------------------------------------------------------------------

/* Copy a path argument to a kernel buffer, whether the caller passed a user
 * pointer (open/openat) or a kernel pointer (TIOCGPTPEER-style peer_path).
 * Returns 0 on success with dst NUL-terminated, -14 on fault. */
static int open_copy_path(const char *path, char *dst, size_t dst_size)
{
  if (!path || !dst || dst_size == 0)
    return -14;
  if (is_user_range(path, 1)) {
    long n = strncpy_from_user(dst, path, (long)dst_size);
    if (n < 0)
      return -14;
    if ((size_t)n >= dst_size)
      dst[dst_size - 1] = '\0';
    else
      dst[n] = '\0';
    return 0;
  }
  strncpy(dst, path, dst_size - 1);
  dst[dst_size - 1] = '\0';
  return 0;
}

#if SYSCALL_LOG
static void open_log_entry(int dirfd, const char *kpath, uint64_t flags,
                           uint64_t mode)
{
  struct thread *log_t = sched_get_current();
  if (!log_t || !syscall_log_allowed(log_t))
    return;
  const char *accmode = "RDONLY";
  if ((flags & O_ACCMODE) == O_WRONLY)
    accmode = "WRONLY";
  else if ((flags & O_ACCMODE) == O_RDWR)
    accmode = "RDWR";
  klogf("[SYSCALL] open dirfd=%d path=\"%s\" flags=0x%llx(%s%s%s%s%s%s%s) mode=0%llo\n",
        dirfd, kpath, (unsigned long long)flags, accmode,
        (flags & O_CREAT) ? "|CREAT" : "",
        (flags & O_EXCL) ? "|EXCL" : "",
        (flags & O_TRUNC) ? "|TRUNC" : "",
        (flags & O_APPEND) ? "|APPEND" : "",
        (flags & O_NONBLOCK) ? "|NONBLOCK" : "",
        (flags & O_CLOEXEC) ? "|CLOEXEC" : "",
        (unsigned long long)mode);
}

static void open_log_result(const char *kpath, int64_t ret)
{
  struct thread *log_t = sched_get_current();
  if (!log_t || !syscall_log_allowed(log_t))
    return;
  if (ret >= 0)
    klogf("[SYSCALL] open path=\"%s\" -> fd=%lld\n", kpath,
          (long long)ret);
  else
    klogf("[SYSCALL] open path=\"%s\" -> errno=%lld\n", kpath,
          (long long)-ret);
}
#endif

/* Collapse empty and dot components in the saved pathname. Wine repeatedly
 * opens "." and returns through fchdir; retaining every /./ eventually
 * overflowed the pathname even though it still named the same directory.
 * Keep "..": eliminating it lexically would change symlink semantics. */
static bool fd_make_path(char *out, size_t size, const char *base,
                         const char *path) {
  if (size < 2)
    return false;
  size_t used = 1;
  out[0] = '/';
  for (int part = 0; part < 2; part++) {
    const char *p = part == 0 ? (path[0] == '/' ? NULL : base) : path;
    if (!p)
      continue;
    while (*p) {
      while (*p == '/') p++;
      const char *start = p;
      while (*p && *p != '/') p++;
      size_t len = (size_t)(p - start);
      if (!len || (len == 1 && start[0] == '.'))
        continue;
      size_t separator = used > 1 ? 1 : 0;
      if (len >= size - used - separator)
        return false;
      if (separator) out[used++] = '/';
      memcpy(out + used, start, len);
      used += len;
    }
  }
  out[used] = '\0';
  return true;
}

uint64_t sys_open_path(int dirfd, const char *path, uint64_t flags,
                            uint64_t mode) {
  if (!path)
    return (uint64_t)-14; // EFAULT

  /* Snapshot the user string once: the direct klog_puts(path)/strlen/strncmp
   * pattern used to dereference the user pointer unbounded, faulting in the
   * kernel without an exception-table fixup when the string ran into an
   * unmapped page. All VFS work below uses the kernel copy. */
  char kpath[512];
  if (open_copy_path(path, kpath, sizeof(kpath)) < 0) {
#if SYSCALL_LOG
    {
      struct thread *log_t = sched_get_current();
      if (log_t && syscall_log_allowed(log_t))
        klogf("[SYSCALL] open dirfd=%d path=\"<fault>\" flags=0x%llx\n",
              dirfd, (unsigned long long)flags);
    }
#endif
    return (uint64_t)-14;
  }
  path = kpath;

#if SYSCALL_LOG
  open_log_entry(dirfd, kpath, flags, mode);
#endif

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
   * __O_TMPFILE (0x400000) and O_PATH (0x200000) are different bits, so
   * O_TMPFILE is only in play when the caller also asked for write access.
   * Reading the same bits as read-only plus O_DIRECTORY is an O_PATH handle,
   * which Qt's QProcess uses for the child's working directory (open(dir,
   * O_RDONLY|O_DIRECTORY|O_PATH) followed by fchdir) - rejecting that makes
   * Konsole report "Could not start program '/bin/bash'". */
  if ((flags & O_TMPFILE) == O_TMPFILE && (flags & O_ACCMODE) != O_RDONLY)
    return (uint64_t)-95; // EOPNOTSUPP

  int fd;

  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if (dirfd == AT_FDCWD) {
      base_dir = t->cwd_node ? t->cwd_node : fs_root;
    } else {
      if (!FD_IN_RANGE(t, dirfd) || !t->fds[dirfd])
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

  if (dev_path && !vfs_namespace_active() && fs_root == vfs_backing_node(fs_root)) {
    if (strcmp(dev_path, "ptmx") == 0) {
      /* A mounted devpts owns its PTYs. Bypassing it creates legacy slave
       * descriptors with no matching directory entry, so openpty callers
       * such as sudo cannot stat/chown their newly allocated terminal. */
      vfs_node_t *mux = vfs_resolve_path("/dev/pts/ptmx");
      bool mounted_devpts = mux && mux->open_instance;
      if (mux)
        vfs_close(mux);
      if (mounted_devpts)
        return sys_open_path(dirfd, "/dev/pts/ptmx", flags, mode);
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
      node->inode = (128U << 8) | (uint32_t)pty_index;
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
      if (flags & O_DIRECTORY) {
        kfree(node);
        return (uint64_t)-20; // ENOTDIR: ptmx is not a directory
      }
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strncmp(dev_path, "pts/", 4) == 0) {
      if (strcmp(dev_path, "pts/ptmx") == 0)
        goto device_vfs_lookup;
      const char *num_str = dev_path + 4;
      int pty_index = 0;
      while (*num_str >= '0' && *num_str <= '9') {
        pty_index = pty_index * 10 + (*num_str - '0');
        num_str++;
      }
      if (*num_str != '\0')
        return (uint64_t)-2;

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty || pty->devpts_instance)
        goto device_vfs_lookup;
      if (pty->locked)
        return (uint64_t)-2;

      node = kmalloc(sizeof(vfs_node_t));
      if (!node)
        return (uint64_t)-12;
      vfs_node_init(node);
      strcpy(node->name, dev_path);
      node->flags = FS_CHARDEV;
      node->inode = (136U << 8) | (uint32_t)pty_index;
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
      if (flags & O_DIRECTORY) {
        kfree(node);
        return (uint64_t)-20; // ENOTDIR: a pty slave is not a directory
      }
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strcmp(dev_path, "tty") == 0) {
      struct thread *ct = sched_get_current();
      if (ct && ct->ctty) {
        if (!vfs_node_is_alive(ct->ctty) || !vfs_node_tryref(ct->ctty)) {
          ct->ctty = NULL;
        } else {
          if (flags & O_DIRECTORY) {
            vfs_close(ct->ctty);
            return (uint64_t)-20; // ENOTDIR: controlling tty is not a directory
          }
          node = ct->ctty;
          node_owned = true;
          goto open_done;
        }
      }
    }

    if (!node)
      node = fb_lookup_device((char *)dev_path);
  }

device_vfs_lookup:
  if (!node) {
    uint64_t proc_fd;
    if (!t->namei_resolve_flags && !(flags & O_NOFOLLOW) && parse_proc_self_fd_path(path, &proc_fd)) {
      uint64_t source_flags;
      node = fd_pin_node(t, proc_fd, NULL, &source_flags);
      if (!node)
        return (uint64_t)-2; // ENOENT: no such open descriptor
      /* Treat the pin as the resolver's owned reference. open_done() takes
       * the descriptor reference and drops this temporary reference. */
      node_owned = true;
      if ((node->flags & FS_TYPE_MASK) == FS_PIPE && !(flags & O_PATH)) {
        /* A pipe descriptor already refers to an open endpoint, not a FIFO
         * inode: fifo_open() cannot reopen its private pipe state. Retain the
         * endpoint for same-direction opens such as bash process substitution. */
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
          vfs_close(node);
          return (uint64_t)-17;
        }
        if (flags & O_DIRECTORY) {
          vfs_close(node);
          return (uint64_t)-20;
        }
        if ((flags & O_ACCMODE) != (source_flags & O_ACCMODE)) {
          vfs_close(node);
          return (uint64_t)-95; // Cross-direction pipe reopening unsupported.
        }
        uint32_t access = (flags & O_ACCMODE) == O_RDONLY ? 4 :
                          (flags & O_ACCMODE) == O_WRONLY ? 2 : 6;
        if (!vfs_access(node, access)) {
          vfs_close(node);
          return (uint64_t)-13;
        }
        goto open_done;
      }
    }
  }

  const char *lookup_path = path;
  vfs_node_t *lookup_base = base_dir;
  bool created_node = false;

  if (!node) {
    node = (flags & O_NOFOLLOW)
               ? vfs_resolve_symlink_node(lookup_base, lookup_path)
               : vfs_resolve_path_at(lookup_base, lookup_path);
    node_owned = node != NULL;
  }

  /* The device-name branch above is skipped inside a mount namespace, where
   * /dev/ttyN resolves to the ramfs entry registered under the console node's
   * name ("console"), so the lookup misses. Fall back to the registry only
   * when the namespace view has no entry, so a sandbox's own /dev still wins. */
  if (!node && dev_path && !(flags & O_CREAT) &&
      fs_root == vfs_backing_node(fs_root))
    node = fb_lookup_device((char *)dev_path);

  if (node && (node->flags & FS_TYPE_MASK) == FS_SYMLINK &&
      (flags & O_NOFOLLOW) && !(flags & O_PATH) &&
      (flags & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-40; // ELOOP: only O_PATH can open the link itself
  }

  if (!node) {
    if (flags & O_CREAT) {
      char parent_path[512];
      char file_name[256];
      size_t len = strlen(lookup_path);
      if (len == 0 || len >= 4096)
        return (uint64_t)-14;

      const char *slash = 0;
      for (const char *p = lookup_path; *p; p++)
        if (*p == '/')
          slash = p;

      vfs_node_t *parent = lookup_base;
      bool parent_owned = false;
      if (slash) {
        size_t parent_len = (size_t)(slash - lookup_path);
        if (parent_len == 0) {
          parent = fs_root;
        } else {
          if (parent_len >= sizeof(parent_path))
            return (uint64_t)-14;
          for (size_t i = 0; i < parent_len; i++)
            parent_path[i] = lookup_path[i];
          parent_path[parent_len] = '\0';
          parent = vfs_resolve_path_at(lookup_base, parent_path);
          parent_owned = parent != NULL;
        }
        size_t file_len = strlen(slash + 1);
        if (file_len == 0) {
          if (parent_owned) vfs_close(parent);
          return (uint64_t)-21; // EISDIR
        }
        if (file_len >= sizeof(file_name)) {
          if (parent_owned) vfs_close(parent);
          return (uint64_t)-14;
        }
        strcpy(file_name, slash + 1);
      } else {
        size_t file_len = len;
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, lookup_path);
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
      int create_result = vfs_create(parent, file_name, (uint16_t)mode);
      if (create_result != 0) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)(create_result < 0 ? create_result : -17);
      }

      node = vfs_finddir(parent, file_name);
      if (!node) {
        if (parent_owned) vfs_close(parent);
        return (uint64_t)-2;
      }
      node_owned = true;
      created_node = true;
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

  if (!(flags & O_PATH) && (vfs_mount_flags(node) & 4) &&
      ((node->flags & FS_TYPE_MASK) == FS_CHARDEV || (node->flags & FS_TYPE_MASK) == FS_BLOCKDEV)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-13;
  }
  uint32_t requested = 0;
  /* O_PATH handles carry no I/O rights: Linux skips the file's own permission
   * check at open (path-component search checks still applied during the
   * resolve above). */
  if (!(flags & O_PATH)) {
    if ((flags & O_ACCMODE) == O_RDONLY) requested = 4;
    else if ((flags & O_ACCMODE) == O_WRONLY) requested = 2;
    else if ((flags & O_ACCMODE) == O_RDWR) requested = 6;
  }
  if ((requested & 2) && vfs_mount_readonly(node)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-30;
  }
  /* open(O_CREAT, mode) returns an already-open descriptor even when the
   * creation mode removes the caller's requested access bits (for example,
   * Git creates writable temporary pack files with mode 0444).  The parent
   * directory check above authorizes creation; the resulting mode applies to
   * later opens.  Existing files still require the normal access check. */
  if (requested && !created_node && !vfs_access(node, requested)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-13;
  }

  /* Directories are read-only descriptors (readdir(2) is the only thing you
   * can do with one).  Returning a writable handle would let callers believe
   * they created something inside the directory. O_PATH handles are exempt:
   * they cannot be used for I/O at all. */
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY &&
      (flags & O_ACCMODE) != O_RDONLY && !(flags & O_PATH)) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-21; // EISDIR
  }

  if ((flags & O_TRUNC) && !(flags & O_PATH) &&
      (node->flags & FS_TYPE_MASK) == FS_FILE) {
    if (!(requested & 2)) {
      if (node_owned) vfs_close(node);
      return (uint64_t)-13;
    }
    int truncate_result = vfs_truncate(node, 0);
    if (truncate_result != 0) {
      if (node_owned) vfs_close(node);
      return (uint64_t)truncate_result;
    }
  }

  /* Persistent metadata may provide a fresh per-open object.  DRM was the
   * first user of this pattern; driver capability handles use it as well. */
  if ((node->flags & FS_TYPE_MASK) == FS_PIPE) {
    vfs_node_t *prev = node;
    vfs_node_t *inst = NULL;
    int err = fifo_open(prev, flags, &inst);
    if (node_owned) vfs_close(prev);
    if (err != 0) {
      return (uint64_t)err;
    }
    node = inst;
    node_owned = true;
  } else if (node->open_instance) {
    vfs_node_t *prev = node;
    node = node->open_instance(prev);
    if (!node) {
      if (node_owned) vfs_close(prev);
      return (uint64_t)-12;
    }
    if (node_owned) vfs_close(prev);
    /* Keep a temporary reference until fd installation. Failure paths
     * (EMFILE, O_DIRECTORY, bad pathname) must release per-open state too. */
    vfs_node_ref(node);
    node_owned = true;
    if (flags & O_NONBLOCK)
      node->flags |= FS_NONBLOCK;
  } else if (ascentdrm_is_card_node(node)) {
    vfs_node_t *prev = node;
    node = ascentdrm_create_client_node();
    if (!node) {
      if (node_owned) vfs_close(prev);
      return (uint64_t)-12;
    }
    node_owned = false; // fresh client node owns no resolver reference
    if (flags & O_NONBLOCK)
      node->flags |= FS_NONBLOCK;
  }

  /* Linux fails open(file, O_DIRECTORY) with ENOTDIR here, so musl opendir()
   * rejects files at open time instead of surfacing the error later from
   * getdents. Applies to O_PATH handles as well. */
  if ((flags & O_DIRECTORY) && (node->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-20; // ENOTDIR
  }

open_done:
  char full_path[256];
  spinlock_acquire(&t->files->lock);
  const char *base_path = t->cwd_path;
  if (path[0] != '/' && dirfd != AT_FDCWD && FD_IN_RANGE(t, dirfd))
    base_path = fd_path_value(t, dirfd);
  bool path_ok = vfs_namespace_active() && node->view_backing
      ? vfs_namespace_node_path(node, full_path, sizeof(full_path))
      : fd_make_path(full_path, sizeof(full_path), base_path, path);
  spinlock_release(&t->files->lock);
  if (!path_ok) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-36; // ENAMETOOLONG: never retain a truncated pathname
  }

  fd = alloc_fd(t);
  if (fd < 0) {
    if (node_owned) vfs_close(node);
    return (uint64_t)-24; // EMFILE
  }

  vfs_open(node);
  if (node_owned) vfs_close(node); // descriptor holds its own reference now
  spinlock_acquire(&t->files->lock);
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = flags & ~(uint64_t)O_CLOEXEC;
  if (flags & O_CLOEXEC)
    t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
  if (!fd_path_set(t, fd, full_path)) {
    spinlock_release(&t->files->lock);
    fd_release_reserved(t, fd);
    vfs_close(node);
    return (uint64_t)-12;
  }
  t->fds[fd] = node; /* publish only after offset, flags and path are installed */
  spinlock_release(&t->files->lock);
  if (strcmp(node->name, "ptmx") == 0) {
    klog_puts(" fd=");
    klog_uint64((uint64_t)fd);
    klog_puts(" tid=");
    klog_uint64(t->tid);
    klog_puts("\n");
  }


  return fd;
}

/* Linux open_how v0. Restrict the complete walk, including absolute
 * symlink targets and fd-relative '..', instead of checking path spelling. */
static uint64_t sys_mount_setattr(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                                  uint64_t attr_ptr, uint64_t size, uint64_t a5) {
  (void)a5;
  struct thread *task=sched_get_current();
  bool ns_ok = vfs_namespace_active() && capability_mount_ns_admin(task);
  if(!ns_ok && !capability_has_init_ns(task,CAP_SYS_ADMIN))return (uint64_t)-1;
  if(flags&~(0x100ULL|0x1000ULL|0x8000ULL))return (uint64_t)-22;
  struct {uint64_t set,clear,propagation,userns_fd;} attr;
  if(size<sizeof(attr))return (uint64_t)-22;
  if(size>4096)return (uint64_t)-7;
  if(!is_user_range((void*)attr_ptr,size)||copy_from_user(&attr,(void*)attr_ptr,sizeof(attr)))return (uint64_t)-14;
  for(uint64_t i=sizeof(attr);i<size;i++){unsigned char extra;if(copy_from_user(&extra,(void*)(attr_ptr+i),1))return (uint64_t)-14;if(extra)return (uint64_t)-7;}
  if((attr.set|attr.clear)&~15ULL)return (uint64_t)-95;
  if(attr.propagation&&attr.propagation!=(1<<18)&&attr.propagation!=(1<<19))return (uint64_t)-95;
  char path[512];if(!is_user_range((void*)path_ptr,1))return (uint64_t)-14;
  long len=strncpy_from_user(path,(void*)path_ptr,sizeof(path));if(len<0)return (uint64_t)-14;if((uint64_t)len>=sizeof(path))return (uint64_t)-36;
  if(!len&&!(flags&0x1000))return (uint64_t)-2;
  vfs_node_t *base=(int)dirfd==AT_FDCWD?(task->cwd_node?task->cwd_node:fs_root):fd_pin_node(task,dirfd,NULL,NULL);
  if(!base)return (uint64_t)-9;
  vfs_node_t *point;
  if(!len){point=base;vfs_open(point);}
  else point=(flags&0x100)?vfs_resolve_symlink_node(base,path):vfs_resolve_path_at(base,path);
  if((int)dirfd!=AT_FDCWD)vfs_close(base);
  if(!point)return (uint64_t)-2;
  /* Initial namespace: mount attributes live on the mount root's backing
   * node (systemd marks its credential tmpfs read-only this way). */
  if(!vfs_namespace_active()){
    if(!capability_has_init_ns(task,CAP_SYS_ADMIN)){vfs_close(point);return (uint64_t)-1;}
    int r=-22;
    vfs_node_t *backing=vfs_backing_node(point);
    if(backing&&backing!=vfs_backing_node(fs_root)&&vfs_is_mount_root(point)){
      backing->filesystem_mount_flags=(backing->filesystem_mount_flags&~attr.clear)|attr.set;
      r=0;
    }
    vfs_close(point);return (uint64_t)r;
  }
  int result=vfs_namespace_setattr(point,attr.set,attr.clear,(flags&0x8000)!=0);
  vfs_close(point);return (uint64_t)result;
}

static uint64_t sys_openat2(uint64_t dirfd, uint64_t path_ptr, uint64_t how_ptr,
                            uint64_t size, uint64_t a4, uint64_t a5) {
  (void)a4; (void)a5;
  struct { uint64_t flags, mode, resolve; } how;
  if (size < sizeof(how)) return (uint64_t)-22;
  if (size > 4096) return (uint64_t)-7;
  if (!is_user_range((void *)how_ptr, size) || copy_from_user(&how, (void *)how_ptr, sizeof(how))) return (uint64_t)-14;
  for (uint64_t i = sizeof(how); i < size; i++) {
    unsigned char extra;
    if (copy_from_user(&extra, (void *)(how_ptr+i), 1)) return (uint64_t)-14;
    if (extra) return (uint64_t)-7;
  }
  if ((how.resolve & ~0x3fULL) || ((how.resolve & 0x18) == 0x18)) return (uint64_t)-22;
  if (how.resolve & 0x20) return (uint64_t)-11; /* RESOLVE_CACHED: caller can retry. */
  if (how.flags & ~0x7fffffULL) return (uint64_t)-22;
  if ((how.flags & O_ACCMODE) == O_ACCMODE || (how.mode & ~07777ULL)) return (uint64_t)-22;
  if (how.mode && !(how.flags & (O_CREAT | __O_TMPFILE))) return (uint64_t)-22;
  if ((how.flags & O_PATH) && (how.flags & ~(uint64_t)(O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW))) return (uint64_t)-22;
  char path[512];
  if (!is_user_range((void *)path_ptr, 1)) return (uint64_t)-14;
  long n = strncpy_from_user(path, (void *)path_ptr, sizeof(path));
  if (n < 0) return (uint64_t)-14;
  if ((uint64_t)n >= sizeof(path)) return (uint64_t)-36;
  struct thread *task = sched_get_current();
  if (!task) return (uint64_t)-1;
  if (!how.resolve) return sys_open_path((int)dirfd, path, how.flags, how.mode);
  vfs_node_t *anchor = NULL;
  if (how.resolve & 0x18) {
    vfs_node_t *base = (int)dirfd == AT_FDCWD ? (task->cwd_node ? task->cwd_node : fs_root) : fd_pin_node(task, dirfd, NULL, NULL);
    if (!base) return (uint64_t)-9;
    if ((base->flags & FS_TYPE_MASK) != FS_DIRECTORY) { if ((int)dirfd != AT_FDCWD) vfs_close(base); return (uint64_t)-20; }
    anchor = vfs_namespace_root_view(base);
    if ((int)dirfd != AT_FDCWD) vfs_close(base);
    if (!anchor) return (uint64_t)-12;
  }
  struct mount_namespace *temporary = vfs_namespace_lookup_copy(task);
  if (!temporary) { if (anchor) vfs_close(anchor); return (uint64_t)-12; }
  spinlock_acquire(&task->namespace_lock);
  struct mount_namespace *saved_ns = task->mount_ns;
  vfs_node_t *saved_root = task->fs_root_node;
  uint64_t saved_policy = task->namei_resolve_flags;
  int saved_error = task->namei_error;
  vfs_node_t *saved_path_root = task->namei_path_root;
  task->namei_path_root = vfs_task_root();
  task->mount_ns = temporary;
  task->fs_root_node = anchor ? anchor : vfs_task_root();
  task->namei_resolve_flags = how.resolve;
  task->namei_error = 0;
  spinlock_release(&task->namespace_lock);
  int64_t result = (int64_t)sys_open_path((int)dirfd, path, how.flags, how.mode);
  if (result < 0 && task->namei_error) result = task->namei_error;
  spinlock_acquire(&task->namespace_lock);
  task->mount_ns = saved_ns;
  task->fs_root_node = saved_root;
  task->namei_resolve_flags = saved_policy;
  task->namei_error = saved_error;
  task->namei_path_root = saved_path_root;
  spinlock_release(&task->namespace_lock);
  vfs_namespace_put(temporary);
  if (anchor) vfs_close(anchor);
  return (uint64_t)result;
}

static uint64_t sys_openat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                           uint64_t mode, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  int ret = (int)sys_open_path((int)dirfd, (const char *)path_ptr, flags, mode);
#if SYSCALL_LOG
  {
    char kpath[512];
    if (open_copy_path((const char *)path_ptr, kpath, sizeof(kpath)) == 0)
      open_log_result(kpath, (int64_t)ret);
    else
      open_log_result("<fault>", (int64_t)ret);
  }
#endif
  if (ret < 0) {
    struct thread *t = sched_get_current();
    if (t) {
      if (path_ptr && is_user_range((const void *)path_ptr, 1)) {
        long n = strncpy_from_user(t->last_error_path,
                                   (const char *)path_ptr,
                                   (long)sizeof(t->last_error_path));
        if (n < 0) {
          t->last_error_path[0] = '\0';
        } else if ((size_t)n >= sizeof(t->last_error_path)) {
          t->last_error_path[sizeof(t->last_error_path) - 1] = '\0';
        } else {
          t->last_error_path[n] = '\0';
        }
      }
    }
  }
  return (uint64_t)(int64_t)ret;
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_openat(AT_FDCWD, path_ptr, flags, mode, 0, 0);
}

/* Legacy x86-64 creat(path, mode) is open with these exact flags. */
static uint64_t sys_creat(uint64_t path, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  return sys_openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, mode, 0, 0);
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
  if (!t || !t->files || fd >= MAX_FDS)
    return (uint64_t)-9;

  spinlock_acquire(&t->files->lock);
  if (fd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return (uint64_t)-9;
  }
  vfs_node_t *node = t->files->fds[fd];
  if (!fd_is_valid_node(node)) {
    spinlock_release(&t->files->lock);
    return (uint64_t)-9;
  }
  t->files->fds[fd] = NULL;
  t->files->fd_offsets[fd] = 0;
  t->files->fd_flags[fd] = 0;
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
  if (flags & CLOSE_RANGE_UNSHARE) {
    if (!sched_unshare_files(t))
      return (uint64_t)-12;
  }

  if (first >= MAX_FDS)
    return 0;
  if (last >= MAX_FDS)
    last = MAX_FDS - 1;

  if (flags & CLOSE_RANGE_CLOEXEC) {
    spinlock_acquire(&t->files->lock);
    uint64_t cap = t->files->capacity;
    uint64_t end = last < cap ? last : (cap ? cap - 1 : 0);
    if (first < cap) {
      for (uint64_t fd = first; fd <= end; fd++) {
        if (t->files->fds[fd] && t->files->fds[fd] != FD_RESERVED)
          t->files->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
      }
    }
    spinlock_release(&t->files->lock);
    return 0;
  }

  for (uint64_t fd = first; fd <= last; fd++) {
    spinlock_acquire(&t->files->lock);
    if (fd >= t->files->capacity) {
      spinlock_release(&t->files->lock);
      break;
    }
    vfs_node_t *node = t->files->fds[fd];
    if (!node || node == FD_RESERVED) {
      spinlock_release(&t->files->lock);
      continue;
    }
    t->files->fds[fd] = NULL;
    t->files->fd_offsets[fd] = 0;
    t->files->fd_flags[fd] = 0;
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

/* Reserve the destination and pin the source in the same critical section.
 * A CLONE_FILES sibling may otherwise close the source between validation and
 * copying it, leaving a successful dup with a NULL destination (Firefox's
 * FileDescriptor constructor can assert when F_DUPFD_CLOEXEC sees EBADF).
 * VFS callbacks and final closes run outside the file-table lock. */
static int64_t fd_duplicate(struct thread *t, uint64_t oldfd,
                            uint64_t target, bool fixed, bool cloexec) {
  if (!t || !t->files || oldfd >= MAX_FDS)
    return -9;
  spinlock_acquire(&t->files->lock);
  if (oldfd >= t->files->capacity) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  vfs_node_t *node = t->fds[oldfd];
  if (!fd_is_valid_node(node)) {
    spinlock_release(&t->files->lock);
    return -9;
  }
  if (target >= MAX_FDS) {
    spinlock_release(&t->files->lock);
    return fixed ? -9 : -22;
  }
  if (fixed && oldfd == target) {
    spinlock_release(&t->files->lock);
    return (int64_t)target;
  }
  if (fixed && target >= t->files->capacity) {
    if (!fd_table_expand_locked(t->files, (uint32_t)target)) {
      spinlock_release(&t->files->lock);
      return -24; // EMFILE
    }
  }
  if (fixed && t->fds[target] == FD_RESERVED) {
    spinlock_release(&t->files->lock);
    return -16; // EBUSY: another open/dup is installing this descriptor
  }
  if (!fixed) {
    int free_fd = fd_find_free_locked(t, (int)target);
    if (free_fd < 0) {
      spinlock_release(&t->files->lock);
      return -24;
    }
    target = (uint32_t)free_fd;
  }

  vfs_node_t *previous = t->fds[target];
  vfs_node_ref(node); // temporary pin until the destination owns its reference
  fd_description_set_locked(t, (int)target,
                            fd_description_id_locked(t, (int)oldfd));
  t->fds[target] = FD_RESERVED;
  t->fd_offsets[target] = fd_offset_get_locked(t, oldfd);
  t->fd_flags[target] = t->fd_flags[oldfd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  if (cloexec)
    t->fd_flags[target] |= FD_FLAGS_CLOEXEC_BIT;
  fd_path_dup(t, (int)target, (int)oldfd);
  if (target == t->files->next_fd)
    t->files->next_fd++;
  spinlock_release(&t->files->lock);

  vfs_open(node);
  spinlock_acquire(&t->files->lock);
  t->fds[target] = node;
  spinlock_release(&t->files->lock);
  vfs_close(node); // release temporary pin
  if (previous)
    vfs_close(previous);
  return (int64_t)target;
}

static uint64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return (uint64_t)fd_duplicate(sched_get_current(), oldfd, 0, false, false);
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return (uint64_t)fd_duplicate(sched_get_current(), oldfd, newfd, true, false);
}

static uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if ((flags & ~(uint64_t)O_CLOEXEC) || oldfd == newfd)
    return (uint64_t)-22;
  return (uint64_t)fd_duplicate(sched_get_current(), oldfd, newfd, true,
                                 (flags & O_CLOEXEC) != 0);
}

// ---------------------------------------------------------------------------
// read / write helpers
// ---------------------------------------------------------------------------



static void trace_userspace_debug_write(struct thread *t, int fd,
                                        const void *buf, size_t count) {
  if (!FD_IN_RANGE(t, fd) || !t->fds[fd] || !buf || count == 0)
    return;

  /* Only two node names ever match the strcmp()s below.  Running the pair on
   * every write() showed up as the ~20-50ns gap between write(16) and
   * read(16) in test_syscall_speed (identical paths otherwise); reject on
   * the first byte so the common case is two loads and two compares. */
  const char *name = t->fds[fd]->name;
  if (name[0] != 'w' && name[0] != 'x')
    return;

  const char *prefix = NULL;
  if (strcmp(name, "weston-debug.log") == 0)
    prefix = "[WESTON-LOG] ";
  else if (strcmp(name, "xfwm4.log") == 0)
    prefix = "[XFWM4-LOG] ";
  else
    return;

  klog_puts(prefix);
  if (is_user_range(buf, count)) {
    char kbuf[256];
    size_t pos = 0;
    char last = '\0';
    while (pos < count) {
      size_t chunk = count - pos;
      if (chunk > sizeof(kbuf))
        chunk = sizeof(kbuf);
      if (copy_from_user(kbuf, (const uint8_t *)buf + pos, chunk) != 0)
        break;
      bool hit_nul = false;
      for (size_t i = 0; i < chunk; i++) {
        if (kbuf[i] == '\0') {
          hit_nul = true;
          break;
        }
        klog_putchar(kbuf[i]);
        last = kbuf[i];
      }
      pos += chunk;
      if (hit_nul)
        break;
    }
    if (last != '\n')
      klog_putchar('\n');
  } else {
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
}

/* Raw block-device nodes have a separate transfer interface. */
static uint32_t fd_vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size,
                            uint8_t *buf) {
  uint32_t done;
  if (block_node_rw64(node, offset, size, buf, false, &done))
    return done;
  return vfs_read(node, offset, size, buf);
}

static uint32_t fd_vfs_write(vfs_node_t *node, uint64_t offset, uint32_t size,
                             uint8_t *buf) {
  uint32_t done;
  if (block_node_rw64(node, offset, size, buf, true, &done))
    return done;
  return vfs_write(node, offset, size, buf);
}

#define FD_BOUNCE_STACK_MAX 4096
/* Keep per-syscall scratch memory bounded. Larger read/write requests are
 * already transferred in a loop, so 64 KiB limits peak kernel heap use without
 * changing the userspace-visible request size. */
#define FD_BOUNCE_HEAP_MAX  (64U * 1024U)

static int64_t fd_read_to_user(vfs_node_t *node, uint64_t offset, size_t count,
                               uint8_t *user_buf) {
  if (count == 0)
    return 0;
  if (asc_vfs_node_is_linux_file(node)) {
    uint32_t chunk = count > 0x7ffff000U ? 0x7ffff000U : (uint32_t)count;
    return (int32_t)fd_vfs_read(node, offset, chunk, user_buf);
  }
  bool packet_pipe = pipe_is_packet(node);
  if (packet_pipe && count > PIPE_PACKET_SIZE)
    count = PIPE_PACKET_SIZE;
  uint8_t stack_buf[FD_BOUNCE_STACK_MAX];
  size_t cap = count <= FD_BOUNCE_STACK_MAX ? count
             : (count <= FD_BOUNCE_HEAP_MAX ? count : FD_BOUNCE_HEAP_MAX);
  uint8_t *kbuf = cap <= FD_BOUNCE_STACK_MAX ? stack_buf : (uint8_t *)kmalloc(cap);
  if (!kbuf) {
    if (packet_pipe)
      return -12; // A smaller bounce buffer would discard part of the packet.
    kbuf = stack_buf;
    cap = FD_BOUNCE_STACK_MAX;
  }

  size_t total = 0;
  int64_t err = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > cap)
      chunk = cap;
    int32_t got = (int32_t)fd_vfs_read(node, offset + total, (uint32_t)chunk, kbuf);
    if (got < 0) {
      err = got;
      break;
    }
    if (got == 0)
      break;
    if (copy_to_user(user_buf + total, kbuf, (size_t)got) != 0) {
      err = -14;
      break;
    }
    total += (size_t)got;
    if (packet_pipe || (size_t)got < chunk)
      break;
  }

  if (kbuf != stack_buf)
    kfree(kbuf);
  return total > 0 ? (int64_t)total : err;
}

static int64_t fd_write_from_user_node(vfs_node_t *node, uint64_t offset,
                                       size_t count, const uint8_t *user_buf,
                                       bool append, uint64_t *end_offset_out) {
  if (count == 0)
    return 0;
  if (asc_vfs_node_is_linux_file(node)) {
    uint32_t chunk = count > 0x7ffff000U ? 0x7ffff000U : (uint32_t)count;
    int32_t wrote = (int32_t)fd_vfs_write(node, offset, chunk, (uint8_t *)user_buf);
    if (end_offset_out)
      *end_offset_out = 0;
    return wrote;
  }
  uint8_t stack_buf[FD_BOUNCE_STACK_MAX];
  size_t cap = count <= FD_BOUNCE_STACK_MAX ? count
             : (count <= FD_BOUNCE_HEAP_MAX ? count : FD_BOUNCE_HEAP_MAX);
  bool packet_pipe = pipe_is_packet(node);
  if (packet_pipe && cap > PIPE_PACKET_SIZE)
    cap = PIPE_PACKET_SIZE;
  uint8_t *kbuf = cap <= FD_BOUNCE_STACK_MAX ? stack_buf : (uint8_t *)kmalloc(cap);
  if (!kbuf) {
    if (packet_pipe)
      return -12; // Keep the original packet boundaries under memory pressure.
    kbuf = stack_buf;
    cap = FD_BOUNCE_STACK_MAX;
  }

  size_t total = 0;
  int64_t err = 0;
  uint64_t end_off = 0;
  while (total < count) {
    size_t chunk = count - total;
    if (chunk > cap)
      chunk = cap;
    if (copy_from_user(kbuf, user_buf + total, chunk) != 0) {
      err = -14;
      break;
    }
    int32_t wrote = append
        ? (int32_t)vfs_append(node, (uint32_t)chunk, kbuf, &end_off)
        : (int32_t)fd_vfs_write(node, offset + total, (uint32_t)chunk, kbuf);
    if (wrote < 0) {
      err = wrote;
      break;
    }
    if (wrote == 0)
      break;
    total += (size_t)wrote;
    if ((size_t)wrote < chunk)
      break;
  }

  if (end_offset_out)
    *end_offset_out = end_off;
  if (kbuf != stack_buf)
    kfree(kbuf);
  return total > 0 ? (int64_t)total : err;
}

static int64_t fd_write(int fd, const void *buf, size_t count) {
  if (count == 0)
    return 0;

  struct thread *t = sched_get_current();
  if (!t || fd < 0 || fd >= MAX_FDS)
    return -9;

  uint64_t offset = 0;
  uint64_t flags = 0;
  struct fd_ref ref = fd_get_node(t, (uint64_t)fd, &offset, &flags);
  vfs_node_t *node = ref.node;
  if (!node) {
    static uint32_t missing_write_fd_logs;
    if (__atomic_add_fetch(&missing_write_fd_logs, 1, __ATOMIC_RELAXED) <= 8)
      klogf("[FD-WRITE] missing fd=%d tid=%u tgid=%u\n", fd, t->tid, t->tgid);
    return -9;
  }

  trace_userspace_debug_write(t, fd, buf, count);
  bool append = (flags & O_APPEND) &&
                (node->flags & FS_TYPE_MASK) == FS_FILE;
  bool positioned = (node->flags & FS_TYPE_MASK) == FS_FILE;
  if (positioned) {
    while (__atomic_exchange_n(&node->write_pos_lock, 1, __ATOMIC_ACQUIRE))
      sched_yield();
    if (!append)
      offset = fd_offset_get(t, (uint64_t)fd);
  }
  uint64_t end_offset = 0;
  t->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;
  bool from_user = is_user_range(buf, count);
  int32_t bytes_written = from_user
      ? (int32_t)fd_write_from_user_node(node, offset, count, (const uint8_t *)buf,
                                         append, &end_offset)
      : (append
          ? (int32_t)vfs_append(node, count, (uint8_t *)buf, &end_offset)
          : (int32_t)fd_vfs_write(node, offset, count, (uint8_t *)buf));
  t->current_io_flags = 0;
  if (bytes_written == -9) {
    static uint32_t bad_write_node_logs;
    if (__atomic_add_fetch(&bad_write_node_logs, 1, __ATOMIC_RELAXED) <= 8)
      klogf("[FD-WRITE] backend EBADF fd=%d tid=%u tgid=%u node=%p "
            "magic=%x inode=%u type=%x refs=%u write=%p name='%s'\n",
            fd, t->tid, t->tgid, node, node->magic, node->inode,
            node->flags & FS_TYPE_MASK,
            __atomic_load_n(&node->refcount, __ATOMIC_RELAXED),
            node->write, node->name);
  }
  if (bytes_written > 0) {
    if (append)
      fd_set_offset(t, (uint64_t)fd, node, end_offset);
    else
      fd_advance_offset(t, (uint64_t)fd, node, (uint32_t)bytes_written);
  }
  if (positioned)
    __atomic_store_n(&node->write_pos_lock, 0, __ATOMIC_RELEASE);
  fd_put_node(ref);

  /* A non-empty write MUST NEVER return 0.
   * Standard C runtimes (glibc, musl, Qt, etc.) treat a 0-byte return as a
   * partial write of 0 bytes and immediately retry in an infinite spin loop,
   * driving 100% CPU. If the underlying device or pipe cannot accept bytes
   * and didn't block, return -EAGAIN if non-blocking or -ENOSPC / -EIO. */
  if (bytes_written == 0 && count > 0) {
    if (flags & (O_NONBLOCK | 0x800))
      return -11; // EAGAIN
    return -28;   // ENOSPC
  }

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
  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9;

  uint64_t offset = 0;
  uint64_t flags = 0;
  struct fd_ref ref = fd_get_node(t, fd, &offset, &flags);
  vfs_node_t *node = ref.node;
  if (!node)
    return (uint64_t)-9;
  t->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;
  int64_t bytes_read = fd_read_to_user(node, offset, count, (uint8_t *)buf);
  t->current_io_flags = 0;
  if (bytes_read > 0)
    fd_advance_offset(t, fd, node, (uint32_t)bytes_read);
  fd_put_node(ref);

  return (uint64_t)bytes_read;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;

  if (fd == 1 || fd == 2) {
    struct thread *t = sched_get_current();
    if (t) {
      if (fd == 2) {
        size_t copy_len = count < sizeof(t->last_stderr) - 1 ? count : sizeof(t->last_stderr) - 1;
        if (copy_len > 0) {
          if (copy_from_user(t->last_stderr, (const void *)buf, copy_len) != 0)
            t->last_stderr[0] = '\0';
          else
            t->last_stderr[copy_len] = '\0';
        } else {
          t->last_stderr[0] = '\0';
        }
      }
      if (count > 0 && t->uname_seen_ms && !t->uname_force && !t->uname_diag_match) {
        if (fd == 2 && count < sizeof(t->last_stderr)) {
          uname_policy_on_write(t, 2, t->last_stderr, (size_t)count);
        } else {
          char diag_buf[512];
          size_t dlen = count < sizeof(diag_buf) ? (size_t)count : sizeof(diag_buf);
          if (copy_from_user(diag_buf, (const void *)buf, dlen) == 0)
            uname_policy_on_write(t, (int)fd, diag_buf, dlen);
        }
      }
    }
  }

  return (uint64_t)fd_write((int)fd, (const void *)buf, (size_t)count);
}

static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                            uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9;

  uint64_t flags = 0;
  struct fd_ref ref = fd_get_node(t, fd, NULL, &flags);
  vfs_node_t *node = ref.node;
  if (!node)
    return (uint64_t)-9;
  t->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;
  int64_t bytes_read = fd_read_to_user(node, offset, count, (uint8_t *)buf);
  t->current_io_flags = 0;
  fd_put_node(ref);
  return (uint64_t)bytes_read;
}

static uint64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
                             uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9;

  uint64_t flags = 0;
  struct fd_ref ref = fd_get_node(t, fd, NULL, &flags);
  vfs_node_t *node = ref.node;
  if (!node)
    return (uint64_t)-9;
  /* Linux also honors O_APPEND for pwrite, without changing f_pos. */
  uint64_t end_offset;
  bool append = (flags & O_APPEND) && (node->flags & FS_TYPE_MASK) == FS_FILE;
  t->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;
  int64_t bytes_written = fd_write_from_user_node(
      node, offset, count, (const uint8_t *)buf, append, &end_offset);
  t->current_io_flags = 0;
  fd_put_node(ref);
  return (uint64_t)bytes_written;
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

  if (!is_user_range((const void *)iov_u, iovcnt * ((sched_get_current() && sched_get_current()->seccomp_compat_dispatch)
                               ? 8 : sizeof(struct user_iovec))))
    return (uint64_t)-14;
  struct user_iovec stack_iov[FD_IOV_STACK_MAX];
  struct user_iovec *iov = NULL;
  int snap_rc = fd_snap_iov(iov_u, iovcnt, stack_iov, &iov);
  if (snap_rc != 0)
    return (uint64_t)snap_rc;

  struct thread *ct = sched_get_current();
  if (!ct || fd >= MAX_FDS) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  uint64_t offset = 0;
  uint64_t flags = 0;
  struct fd_ref ref = fd_get_node(ct, fd, &offset, &flags);
  vfs_node_t *node = ref.node;
  if (!node) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  ct->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;

  bool fuse_device = fuse_is_device(node);
  if (fuse_device || pipe_is_packet(node)) {
    uint64_t capacity = 0;
    int64_t result = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
      if (iov[i].iov_len && !is_user_range((void *)iov[i].iov_base, iov[i].iov_len)) {
        result = -14; break;
      }
      if (iov[i].iov_len > UINT64_MAX - capacity) { result = -22; break; }
      capacity += iov[i].iov_len;
    }
    /* Read exactly one FUSE request or pipe packet across all vectors. */
    uint64_t limit = fuse_device ? 65576 : PIPE_PACKET_SIZE;
    if (capacity > limit) capacity = limit;
    uint8_t stack_packet[FD_BOUNCE_STACK_MAX];
    uint8_t *packet = NULL;
    if (!result && capacity) {
      packet = capacity <= sizeof(stack_packet) ? stack_packet
                                                : kmalloc(capacity);
      if (!packet) result = -12;
    }
    if (!result && capacity) {
      result = (int32_t)vfs_read(node, 0, capacity, packet);
      if (result > 0) {
        uint64_t copied = 0;
        for (uint64_t i = 0; i < iovcnt && copied < (uint64_t)result; i++) {
          uint64_t count = iov[i].iov_len;
          if (count > (uint64_t)result - copied) count = result - copied;
          if (copy_to_user((void *)iov[i].iov_base, packet + copied, count)) {
            /* The message was consumed: abort rather than strand its caller
             * forever waiting for a daemon that received only a fragment. */
            if (fuse_device)
              fuse_abort_device(node);
            result = -14;
            break;
          }
          copied += count;
        }
      }
    }
    ct->current_io_flags = 0;
    if (packet && packet != stack_packet) kfree(packet);
    fd_put_node(ref); fd_release_iov(iov, stack_iov);
    return (uint64_t)result;
  }

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_range((const void *)base, len)) {
      ct->current_io_flags = 0;
      fd_put_node(ref);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-14;
    }
    int32_t bytes_read =
        (int32_t)fd_read_to_user(node, offset + total, len,
                                 (uint8_t *)base);
    if (bytes_read < 0) {
      if (total > 0)
        break;
      ct->current_io_flags = 0;
      fd_put_node(ref);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)(int64_t)bytes_read;
    }
    total += (size_t)bytes_read;
    if ((uint32_t)bytes_read < len)
      break;
  }
  if (total > 0)
    fd_advance_offset(ct, fd, node, total);
  ct->current_io_flags = 0;
  fd_put_node(ref);
  fd_release_iov(iov, stack_iov);
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

  if (!is_user_range((const void *)iov_u, iovcnt * ((sched_get_current() && sched_get_current()->seccomp_compat_dispatch)
                               ? 8 : sizeof(struct user_iovec))))
    return (uint64_t)-14;

  /* The syscall runs with SMAP enabled. Keep both the descriptors and their
   * payloads in kernel memory before any socket/filesystem path inspects them;
   * a range check alone does not make direct user loads safe. Small descriptor
   * counts stay on the stack; the payload itself always needs the heap. */
  struct user_iovec stack_iov[FD_IOV_STACK_MAX];
  struct user_iovec *iov = NULL;
  int snap_rc = fd_snap_iov(iov_u, iovcnt, stack_iov, &iov);
  if (snap_rc != 0)
    return (uint64_t)snap_rc;
  struct iovec stack_kvec[FD_IOV_STACK_MAX];
  struct iovec *kernel_iov = iovcnt <= FD_IOV_STACK_MAX
                                 ? stack_kvec
                                 : kmalloc(iovcnt * sizeof(*kernel_iov));
  if (!kernel_iov) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-12;
  }
  size_t payload_size = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t len = iov[i].iov_len;
    if ((len && !is_user_range((const void *)iov[i].iov_base, len)) ||
        len > SIZE_MAX - payload_size) {
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-14;
    }
    kernel_iov[i].iov_len = (size_t)len;
    payload_size += (size_t)len;
  }
  uint8_t stack_payload[1024];
  uint8_t *payload = payload_size == 0 ? NULL
                   : (payload_size <= sizeof(stack_payload) ? stack_payload
                                                            : kmalloc(payload_size));
  if (payload_size && !payload) {
    extern size_t mm_file_cache_reclaim(size_t target_pages);
    extern size_t vfs_dentry_reclaim_pages(size_t target_pages);
    mm_file_cache_reclaim(payload_size / 4096 + 16);
    vfs_dentry_reclaim_pages(64);
    payload = kmalloc(payload_size);
  }
  if (payload_size && !payload) {
    socket_t *chk_sock = socket_from_fd((int)fd);
    bool is_stream_sock = chk_sock && chk_sock->type == 1 &&
                          chk_sock->ops && chk_sock->ops->sendmsg;
    if (chk_sock && !is_stream_sock) {
      socket_fd_put(chk_sock);
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-12;
    }
    if (chk_sock)
      socket_fd_put(chk_sock);

    struct thread *writer = sched_get_current();
    vfs_node_t *chk_node = !is_stream_sock ? fd_pin_node(writer, fd, NULL, NULL) : NULL;
    bool is_fuse = chk_node && fuse_is_device(chk_node);
    if (chk_node) vfs_close(chk_node);
    if (is_fuse) {
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-12;
    }

    /* Fall back to 4 KiB stack-buffered writes when contiguous payload
     * allocation fails under memory fragmentation. */
    uint8_t chunk[4096];
    size_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
      uint64_t ubase = iov[i].iov_base;
      size_t rem = (size_t)iov[i].iov_len;
      while (rem > 0) {
        size_t step = rem < sizeof(chunk) ? rem : sizeof(chunk);
        if (copy_from_user(chunk, (const void *)ubase, step) != 0) {
          fd_release_kvec(kernel_iov, stack_kvec);
          fd_release_iov(iov, stack_iov);
          return total > 0 ? total : (uint64_t)-14;
        }
        int64_t w;
        if (is_stream_sock) {
          socket_t *s = socket_from_fd((int)fd);
          if (!s || s->closing || !s->ops || !s->ops->sendmsg) {
            if (s) socket_fd_put(s);
            fd_release_kvec(kernel_iov, stack_kvec);
            fd_release_iov(iov, stack_iov);
            return total > 0 ? total : (uint64_t)-32;
          }
          struct iovec ciov = {.iov_base = chunk, .iov_len = step};
          struct msghdr cmsg = {0};
          cmsg.msg_iov = &ciov;
          cmsg.msg_iovlen = 1;
          w = (int64_t)s->ops->sendmsg(s, &cmsg, 0);
          socket_fd_put(s);
        } else {
          w = fd_write((int)fd, chunk, step);
        }
        if (w < 0) {
          fd_release_kvec(kernel_iov, stack_kvec);
          fd_release_iov(iov, stack_iov);
          return total > 0 ? total : (uint64_t)w;
        }
        total += (size_t)w;
        if ((size_t)w < step) {
          fd_release_kvec(kernel_iov, stack_kvec);
          fd_release_iov(iov, stack_iov);
          return total;
        }
        ubase += step;
        rem -= step;
      }
    }
    fd_release_kvec(kernel_iov, stack_kvec);
    fd_release_iov(iov, stack_iov);
    return total;
  }
  size_t payload_offset = 0;
  for (uint64_t i = 0; i < iovcnt; i++) {
    size_t len = (size_t)iov[i].iov_len;
    kernel_iov[i].iov_base = payload ? payload + payload_offset : NULL;
    if (len && copy_from_user(kernel_iov[i].iov_base,
                              (const void *)iov[i].iov_base, len) != 0) {
      if (payload && payload != stack_payload) kfree(payload);
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-14;
    }
    payload_offset += len;
  }

  /* Remember the last thing written to stderr, as sys_write() does: GLib and
   * most C++ runtimes report a fatal error with writev() just before abort(),
   * and that text is the only explanation a crash report can show. */
  if ((fd == 1 || fd == 2) && payload && payload_size) {
    struct thread *st = sched_get_current();
    if (st) {
      if (fd == 2) {
        size_t keep = payload_size < sizeof(st->last_stderr) - 1
                          ? payload_size : sizeof(st->last_stderr) - 1;
        memcpy(st->last_stderr, payload, keep);
        st->last_stderr[keep] = '\0';
      }
      if (st->uname_seen_ms && !st->uname_force && !st->uname_diag_match)
        uname_policy_on_write(st, (int)fd, (const char *)payload, payload_size);
    }
  }

  /* A socket writev is one send operation.  Splitting it into write calls can
   * interleave vector fragments with another writer and corrupt protocols. */
  socket_t *sock = socket_from_fd((int)fd);
  if (sock) {
    if (sock->closing) {
      socket_fd_put(sock);
      if (payload && payload != stack_payload) kfree(payload);
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-32;
    }
    struct msghdr msg = {0};
    msg.msg_iov = kernel_iov;
    msg.msg_iovlen = (size_t)iovcnt;
    ssize_t ret = sock->ops && sock->ops->sendmsg
                      ? sock->ops->sendmsg(sock, &msg, 0)
                      : -95;
    socket_fd_put(sock);
    if (payload && payload != stack_payload) kfree(payload);
    fd_release_kvec(kernel_iov, stack_kvec);
    fd_release_iov(iov, stack_iov);
    return (uint64_t)ret;
  }

  /* /dev/fuse replies are one framed message. libfuse supplies its header
   * and body in separate vectors; neither fragment is a valid write alone. */
  struct thread *writer = sched_get_current();
  vfs_node_t *packet_node = fd_pin_node(writer, fd, NULL, NULL);
  if (fuse_is_device(packet_node)) {
    int32_t written = payload_size > UINT32_MAX ? -22 :
        (int32_t)vfs_write(packet_node, 0, (uint32_t)payload_size, payload);
    vfs_close(packet_node);
    if (payload && payload != stack_payload) kfree(payload);
    fd_release_kvec(kernel_iov, stack_kvec);
    fd_release_iov(iov, stack_iov);
    return (uint64_t)(int64_t)written;
  }
  if (packet_node) vfs_close(packet_node);

  /* The payload already sits contiguously in kernel memory, so issue one
   * write for the whole vector instead of one fd lookup/lock/offset update
   * per element.  This also keeps small vectors (e.g. header + body) atomic
   * on pipes. */
  size_t total = 0;
  for (uint64_t i = 0; i < 1 && payload_size; i++) {
    uint64_t base = (uint64_t)payload;
    uint64_t len = payload_size;

    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0) {
      if (payload && payload != stack_payload) kfree(payload);
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return total > 0 ? total : (uint64_t)w;
    }
    if (w == 0 && len != 0) {
      /* A non-empty write must never report zero-byte success: callers such
       * as curl retry/fail the download (apm saw this as stalled .apk
       * writes once /tmp filled). Filesystems now return -ENOSPC instead
       * of 0, so reaching here means a backend bug — report ENOSPC so
       * userspace sees "No space left" rather than generic EIO. */
      struct thread *t = sched_get_current();
      vfs_node_t *wnode = (t && FD_IN_RANGE(t, fd)) ? t->fds[fd] : NULL;
      uint64_t woff = (t && FD_IN_RANGE(t, fd)) ? fd_offset_get(t, fd) : 0;
      klog_puts("[WRITEV] zero write fd=");
      klog_uint64(fd);
      klog_puts(" len=");
      klog_uint64(len);
      klog_puts(" off=");
      klog_uint64(woff);
      klog_puts(" node=");
      if (wnode)
        klog_puts(wnode->name);
      else
        klog_puts("(invalid)");
      klog_puts(" flags=");
      if (wnode)
        klog_uint64(wnode->flags);
      else
        klog_puts("0");
      klog_puts(" file_len=");
      if (wnode)
        klog_uint64(wnode->length);
      else
        klog_puts("0");
      klog_puts("\n");
      if (payload && payload != stack_payload) kfree(payload);
      fd_release_kvec(kernel_iov, stack_kvec);
      fd_release_iov(iov, stack_iov);
      return total > 0 ? total : (uint64_t)-28; /* ENOSPC */
    }
    total += (size_t)w;
    if ((size_t)w != len)
      break;
  }
  if (payload && payload != stack_payload) kfree(payload);
  fd_release_kvec(kernel_iov, stack_kvec);
  fd_release_iov(iov, stack_iov);
  return total;
}

// ---------------------------------------------------------------------------
// preadv / pwritev: vectored I/O at an explicit offset, fd position untouched.
// Mirrors readv/writev validation; like pread64/pwrite64 the offset is passed
// straight to the VFS layer (pipes and sockets ignore it there).
// ---------------------------------------------------------------------------

static uint64_t sys_preadv(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  if (!is_user_range((const void *)iov_u, iovcnt * ((sched_get_current() && sched_get_current()->seccomp_compat_dispatch)
                               ? 8 : sizeof(struct user_iovec))))
    return (uint64_t)-14;
  struct user_iovec stack_iov[FD_IOV_STACK_MAX];
  struct user_iovec *iov = NULL;
  int snap_rc = fd_snap_iov(iov_u, iovcnt, stack_iov, &iov);
  if (snap_rc != 0)
    return (uint64_t)snap_rc;

  struct thread *ct = sched_get_current();
  if (!ct || fd >= MAX_FDS) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  uint64_t flags = 0;
  vfs_node_t *node = fd_pin_node(ct, fd, NULL, &flags);
  if (!node) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  ct->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_range((const void *)base, len)) {
      ct->current_io_flags = 0;
      vfs_close(node);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-14;
    }
    int32_t bytes_read =
        (int32_t)fd_read_to_user(node, offset + total, len,
                                 (uint8_t *)base);
    if (bytes_read < 0) {
      if (total > 0)
        break;
      ct->current_io_flags = 0;
      vfs_close(node);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)(int64_t)bytes_read;
    }
    total += (size_t)bytes_read;
    if ((uint32_t)bytes_read < len)
      break;
  }
  ct->current_io_flags = 0;
  vfs_close(node);
  fd_release_iov(iov, stack_iov);
  return total;
}

static uint64_t sys_pwritev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                            uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;

  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  if (!is_user_range((const void *)iov_u, iovcnt * ((sched_get_current() && sched_get_current()->seccomp_compat_dispatch)
                               ? 8 : sizeof(struct user_iovec))))
    return (uint64_t)-14;
  struct user_iovec stack_iov[FD_IOV_STACK_MAX];
  struct user_iovec *iov = NULL;
  int snap_rc = fd_snap_iov(iov_u, iovcnt, stack_iov, &iov);
  if (snap_rc != 0)
    return (uint64_t)snap_rc;

  struct thread *ct = sched_get_current();
  if (!ct || fd >= MAX_FDS) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  uint64_t flags = 0;
  vfs_node_t *node = fd_pin_node(ct, fd, NULL, &flags);
  if (!node) {
    fd_release_iov(iov, stack_iov);
    return (uint64_t)-9;
  }
  ct->current_io_flags = (uint32_t)flags | IO_FLAGS_VALID;

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_range((const void *)base, len)) {
      ct->current_io_flags = 0;
      vfs_close(node);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)-14;
    }
    uint64_t end_offset;
    bool append = (flags & O_APPEND) && (node->flags & FS_TYPE_MASK) == FS_FILE;
    int32_t bytes_written = (int32_t)fd_write_from_user_node(
        node, offset + total, len, (const uint8_t *)base, append, &end_offset);
    if (bytes_written < 0) {
      if (total > 0)
        break;
      ct->current_io_flags = 0;
      vfs_close(node);
      fd_release_iov(iov, stack_iov);
      return (uint64_t)(int64_t)bytes_written;
    }
    total += (size_t)bytes_written;
    if ((uint32_t)bytes_written < len)
      break;
  }
  ct->current_io_flags = 0;
  vfs_close(node);
  fd_release_iov(iov, stack_iov);
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
  if (!t || !FD_IN_RANGE(t, out_fd) || !FD_IN_RANGE(t, in_fd) || !fd_is_valid_node(t->fds[out_fd]) ||
      !fd_is_valid_node(t->fds[in_fd]))
    return (uint64_t)-9;

  vfs_node_t *out_node = t->fds[out_fd];
  vfs_node_t *in_node = t->fds[in_fd];

  uint64_t offset;
  if (offset_ptr) {
    uint64_t user_offset;
    if (!vmm_is_user_addr_range_valid(offset_ptr, sizeof(user_offset)) ||
        copy_from_user(&user_offset, (const void *)offset_ptr,
                       sizeof(user_offset)))
      return (uint64_t)-14;
    offset = user_offset;
  } else {
    offset = fd_offset_get(t, in_fd);
  }

  uint8_t stack_buf[FD_BOUNCE_STACK_MAX];
  size_t buf_cap = count > 4096 ? 4096 : (size_t)count;
  uint8_t *buffer = buf_cap <= sizeof(stack_buf) ? stack_buf : kmalloc(buf_cap);
  if (!buffer)
    return (uint64_t)-12;

  uint64_t total_sent = 0;
  while (total_sent < count) {
    uint32_t to_read =
        (count - total_sent > buf_cap) ? (uint32_t)buf_cap : (uint32_t)(count - total_sent);
    int32_t bytes_read =
        (int32_t)vfs_read(in_node, offset + total_sent, to_read, buffer);
    if (bytes_read <= 0)
      break;
    int32_t bytes_written = (int32_t)vfs_write(out_node, fd_offset_get(t, out_fd),
                                               (uint32_t)bytes_read, buffer);
    if (bytes_written <= 0)
      break;
    fd_set_offset(t, out_fd, out_node, fd_offset_get(t, out_fd) + (uint32_t)bytes_written);
    total_sent += (uint32_t)bytes_written;
    if (bytes_written < bytes_read)
      break;
  }

  if (buffer != stack_buf)
    kfree(buffer);
  if (offset_ptr) {
    uint64_t user_offset = (uint64_t)(offset + total_sent);
    if (copy_to_user((void *)offset_ptr, &user_offset, sizeof(user_offset)))
      return (uint64_t)-14;
  } else {
    fd_set_offset(t, in_fd, in_node, offset + total_sent);
  }

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
    in_off = fd_offset_get(t, fd_in);
  }

  if (out_tracked) {
    if (!is_user_range((const void *)off_out_ptr, sizeof(uint64_t)) ||
        copy_from_user(&out_off, (const void *)off_out_ptr,
                       sizeof(out_off)) != 0)
      return -14;
  } else if (!out_pipe) {
    out_off = fd_offset_get(t, fd_out);
  }

  if ((in_off > UINT32_MAX && !vfs_backing_node(in_node)->io64) ||
      (out_off > UINT32_MAX && !vfs_backing_node(out_node)->io64))
    return -27;

  uint8_t stack_buf[FD_BOUNCE_STACK_MAX];
  size_t buf_cap = len > RANGE_CHUNK ? RANGE_CHUNK : (size_t)len;
  uint8_t *buf = buf_cap <= sizeof(stack_buf) ? stack_buf : kmalloc(buf_cap);
  if (!buf)
    return -12;

  int64_t moved = 0;

  while ((uint64_t)moved < len) {
    uint64_t remaining = len - (uint64_t)moved;
    uint32_t want =
        (uint32_t)(remaining > buf_cap ? buf_cap : remaining);

    int32_t got = (int32_t)vfs_read(in_node, in_off, want, buf);
    if (got <= 0) {
      /* Zero is end of file, or an empty pipe with no writers left. A negative
       * value is already an errno from the read implementation (EAGAIN for a
       * non-blocking pipe, for instance). */
      if (got < 0 && moved == 0)
        moved = got;
      break;
    }

    int32_t put =
        (int32_t)vfs_write(out_node, out_off, (uint32_t)got, buf);
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

    /* Linux splice returns as soon as data moved through a pipe end rather
     * than blocking to fill len. Looping here deadlocks coreutils cat, which
     * splices FIFO -> its own pipe -> stdout with len = 512 KiB: the second
     * read of its own pipe waits for a writer that is cat itself (Steam's
     * srt-logger handshake hung forever on this). */
    if (in_pipe || out_pipe)
      break;

    if (put < got)
      break; /* short write: full pipe, non-blocking sink, or device limit */
    if (in_off > 0xFFFFFFFFULL || out_off > 0xFFFFFFFFULL)
      break;
  }

  if (buf != stack_buf)
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
      fd_set_offset(t, fd_in, in_node, in_off);
    }
  }
  if (!out_pipe) {
    if (out_tracked) {
      if (copy_to_user((void *)off_out_ptr, &out_off, sizeof(out_off)) != 0 &&
          moved == 0)
        return -14;
    } else {
      fd_set_offset(t, fd_out, out_node, out_off);
    }
  }

  return moved;
}

static uint64_t sys_copy_file_range(uint64_t fd_in, uint64_t off_in,
                                    uint64_t fd_out, uint64_t off_out,
                                    uint64_t len, uint64_t flags) {
  struct thread *t = sched_get_current();
  if (!t || !FD_IN_RANGE(t, fd_in) || !FD_IN_RANGE(t, fd_out) || !fd_is_valid_node(t->fds[fd_in]) ||
      !fd_is_valid_node(t->fds[fd_out]))
    return (uint64_t)-9;

  if (flags != 0)
    return (uint64_t)-22; /* copy_file_range(2) defines no flags yet */

  return (uint64_t)move_range(t, fd_in, off_in, fd_out, off_out, len);
}

static uint64_t sys_splice(uint64_t fd_in, uint64_t off_in, uint64_t fd_out,
                           uint64_t off_out, uint64_t len, uint64_t flags) {
  struct thread *t = sched_get_current();
  if (!t || !FD_IN_RANGE(t, fd_in) || !FD_IN_RANGE(t, fd_out) || !fd_is_valid_node(t->fds[fd_in]) ||
      !fd_is_valid_node(t->fds[fd_out]))
    return (uint64_t)-9;

  /* splice(2) moves data through a pipe. A plain file-to-file request is
   * copy_file_range(2), and Linux rejects it here with EINVAL. */
  if (!fd_is_pipe(t->fds[fd_in]) && !fd_is_pipe(t->fds[fd_out]))
    return (uint64_t)-22;
  if (pipe_is_packet(t->fds[fd_in]) || pipe_is_packet(t->fds[fd_out]))
    return (uint64_t)-95; // The stream copy path would discard partial packets.

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
  if (!t || !t->files)
    return (uint64_t)-9;

  /* Fast-path: single-threaded file table (ref_count == 1). */
  if (t->files->ref_count == 1) {
    if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->files->fds[fd]))
      return (uint64_t)-9;
    vfs_node_t *node = t->files->fds[fd];
    /* dd must read/discard skip bytes when its input is a pipe. */
    uint32_t type = node->flags & FS_TYPE_MASK;
    if (type == FS_PIPE || type == FS_SOCKET)
      return (uint64_t)-29; /* ESPIPE */
    uint64_t base = 0;
    if (whence == 1)
      base = fd_offset_get_locked(t, fd);
    else if (whence == 2)
      base = vfs_size(node);
    else if (whence != 0)
      return (uint64_t)-22;
    /* Unsigned addition avoids signed overflow for hostile offsets. */
    uint64_t position = base + offset;
    if ((int64_t)position < 0 ||
        ((int64_t)offset >= 0 && position < base))
      return (uint64_t)-22;
    fd_offset_set_locked(t, fd, position);
    return (uint64_t)(int64_t)position;
  }

  spinlock_acquire(&t->files->lock);
  int64_t result = -9;
  if (!FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->files->fds[fd]))
    goto out;
  vfs_node_t *node = t->files->fds[fd];
  /* dd must read/discard skip bytes when its input is a pipe. */
  uint32_t type = node->flags & FS_TYPE_MASK;
  if (type == FS_PIPE || type == FS_SOCKET) {
    result = -29; /* ESPIPE */
    goto out;
  }
  uint64_t base = 0;
  if (whence == 1)
    base = fd_offset_get_locked(t, fd);
  else if (whence == 2)
    base = vfs_size(node);
  else if (whence != 0) {
    result = -22;
    goto out;
  }
  /* Unsigned addition avoids signed overflow for hostile offsets. */
  uint64_t position = base + offset;
  if ((int64_t)position < 0 ||
      ((int64_t)offset >= 0 && position < base)) {
    result = -22;
    goto out;
  }
  fd_offset_set_locked(t, fd, position);
  result = (int64_t)position;
out:
  spinlock_release(&t->files->lock);
  return (uint64_t)result;
}

// ---------------------------------------------------------------------------
// fcntl
// ---------------------------------------------------------------------------

/* Resolve both descriptors under one lock: CLONE_FILES close/reuse cannot
 * change either slot between validation and the identity comparison. */
static int64_t fd_query_description(struct thread *t, uint64_t fd, int other) {
  if (!t || !t->files || !FD_IN_RANGE(t, fd) || other < 0 || !FD_IN_RANGE(t, (uint64_t)other))
    return -9; // EBADF
  spinlock_acquire(&t->files->lock);
  int64_t result = -9;
  vfs_node_t *left = t->fds[fd], *right = t->fds[other];
  if (fd_is_valid_node(left) && fd_is_valid_node(right)) {
    /* fd_install() may export one Linux struct file through distinct native
     * wrappers. Its device pointer is the real description in that case. */
    bool left_linux = asc_vfs_node_is_linux_file(left);
    bool right_linux = asc_vfs_node_is_linux_file(right);
    if (left_linux || right_linux)
      result = left_linux && right_linux && left->device &&
               left->device == right->device;
    else
      result = fd_description_id_locked(t, (int)fd) ==
               fd_description_id_locked(t, other);
  }
  spinlock_release(&t->files->lock);
  return result;
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  /* The Linux ABI declares fd and cmd as unsigned int. Callers need only
   * provide their low 32 bits. The third argument can hold a full pointer. */
  fd = (uint32_t)fd;
  cmd = (uint32_t)cmd;
  struct thread *t = sched_get_current();
  if (cmd == F_DUPFD_QUERY)
    return (uint64_t)fd_query_description(t, fd, (int)arg);
  if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
    return (uint64_t)fd_duplicate(t, fd, (uint64_t)(int64_t)(int)arg,
                                   false, cmd == F_DUPFD_CLOEXEC);
  if (!t || !FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd]))
    return (uint64_t)-9; // EBADF

  switch (cmd) {
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
    if (pipe_is_packet(node))
      fl |= t->fd_flags[fd] & O_DIRECT;
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
    int16_t unlocked = 2; // F_UNLCK
    return copy_to_user((void *)arg, &unlocked, sizeof(unlocked))
               ? (uint64_t)-14 : 0;
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
  default: {
    char cmd_buf[32];
    snprintf(cmd_buf, sizeof(cmd_buf), "cmd=%lu", (unsigned long)cmd);
    sysdiag_record_unimplemented_feature("fcntl", cmd_buf, t, -22);
    klog_puts("[SYSCALL] sys_fcntl: unhandled cmd=");
    klog_uint64(cmd);
    klog_puts("\n");
    return (uint64_t)-22;
  }
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
  vfs_node_t *node = fd_pin_node(t, fd, NULL, NULL);
  if (!node) {
    uintptr_t slot = 0;
    if (t && t->files && FD_IN_RANGE(t, fd)) {
      spinlock_acquire(&t->files->lock);
      slot = (uintptr_t)t->files->fds[fd];
      spinlock_release(&t->files->lock);
    }
    klogf("[FTRUNCATE] EBADF fd=%llu slot=%p comm='%s' tid=%u\n",
          (unsigned long long)fd, (void *)slot,
          (t && t->comm[0]) ? t->comm : "?", t ? t->tid : 0);
    return (uint64_t)-9;
  }
  if ((node->flags & FS_TYPE_MASK) != FS_FILE) {
    vfs_close(node);
    return (uint64_t)-1;
  }
  if ((int64_t)length < 0) {
    vfs_close(node);
    return (uint64_t)-22;
  }
  int result = vfs_truncate(node, length);
  vfs_close(node);
  if (result == 0)
    return 0;
  /* Backends report negative errno values (e.g. -ENOMEM when the resize
   * cannot allocate under memory pressure); collapsing every failure to
   * -EPERM hid the real cause from applications. */
  return result < 0 ? (uint64_t)result : (uint64_t)-1;
}

static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                              uint64_t len, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  vfs_node_t *node = fd_pin_node(t, fd, NULL, NULL);
  if (!node) {
    uintptr_t slot = 0;
    if (t && t->files && FD_IN_RANGE(t, fd)) {
      spinlock_acquire(&t->files->lock);
      slot = (uintptr_t)t->files->fds[fd];
      spinlock_release(&t->files->lock);
    }
    klogf("[FALLOCATE] EBADF fd=%llu slot=%p comm='%s' tid=%u\n",
          (unsigned long long)fd, (void *)slot,
          (t && t->comm[0]) ? t->comm : "?", t ? t->tid : 0);
    return (uint64_t)-9;
  }
  if ((node->flags & FS_TYPE_MASK) != FS_FILE) {
    vfs_close(node);
    return (uint64_t)-22;
  }

  /* Validate before narrowing: large downloads must not wrap their reservation
   * or silently resize to the low 32 bits of the requested length. */
  if (!len || (int64_t)offset < 0 || (int64_t)len < 0) {
    vfs_close(node);
    return (uint64_t)-22;
  }
  if (mode & ~(uint64_t)0x01) {
    vfs_close(node);
    return (uint64_t)-95;
  }
  if (offset > UINT32_MAX || len > UINT32_MAX - offset) {
    vfs_close(node);
    return (uint64_t)-27;
  }
  int ret = vfs_fallocate(node, (int)mode, (uint32_t)offset, (uint32_t)len);
  vfs_close(node);
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
  if (!t || !FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd]))
    return (uint64_t)-9;
  vfs_cache_sync(t->fds[fd]);
  /* Metadata may sit in a deferred journal group commit. */
  ext3_journal_commit_all();
  return 0;
}

static uint64_t sys_fdatasync(uint64_t fd, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  return sys_fsync(fd, a1, a2, a3, a4, a5);
}

/* There is no global dirty-inode list. Snapshot references from every live
 * task's shared fd table under the scheduler locks, then do writeback unlocked. */
void sys_sync_all_open_files(void) {
  uint32_t max_tid = 0;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (t->tid > max_tid)
      max_tid = t->tid;
  spinlock_release(&tid_lock);

  uint32_t cursor = 0;
  vfs_node_t *stack_nodes[64];
  vfs_node_t **nodes = (vfs_node_t **)kmalloc(MAX_FDS * sizeof(vfs_node_t *));
  size_t nodes_cap = nodes ? MAX_FDS : 64;
  if (!nodes)
    nodes = stack_nodes;
  for (;;) {
    struct thread *next = NULL;
    bool found = false;
    uint32_t next_tid = 0;
    size_t node_count = 0;

    spinlock_acquire(&tid_lock);
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (!t->files || t->tid <= cursor || t->tid > max_tid ||
          (found && t->tid >= next_tid))
        continue;
      next = t;
      next_tid = t->tid;
      found = true;
    }
    if (!found) {
      spinlock_release(&tid_lock);
      break;
    }

    struct fd_table *files = next->files;
    spinlock_acquire(&files->lock);
    for (uint32_t fd = 0; fd < files->capacity && fd < MAX_FDS && node_count < nodes_cap; fd++) {
      vfs_node_t *node = files->fds[fd];
      if (!fd_is_valid_node(node))
        continue;
      vfs_node_ref(node);
      nodes[node_count++] = node;
    }
    spinlock_release(&files->lock);
    spinlock_release(&tid_lock);

    cursor = next_tid;
    for (size_t i = 0; i < node_count; i++) {
      vfs_cache_sync(nodes[i]);
      vfs_close(nodes[i]);
    }
  }
  if (nodes != stack_nodes)
    kfree(nodes);
  /* Open descriptors are only part of the dirty set: files closed but still
   * held by the dentry cache keep their dirty pages until they are synced. */
  vfs_dentry_sync_all();
}

static uint64_t sys_sync(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  sys_sync_all_open_files();
  ext3_journal_commit_all();
  return 0;
}

static uint64_t sys_syncfs(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  struct thread *t = sched_get_current();
  if (!t || !FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd]))
    return (uint64_t)-9;
  return sys_sync(0, a1, a2, a3, a4, a5);
}

/* Linux sync_file_range(2).  WAIT_BEFORE/WRITE/WAIT_AFTER stage writeback of
 * a byte range; this kernel has no dirty-page tracking to flush selectively,
 * so any valid request degrades to the same cache sync fsync performs. */
#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#define SYNC_FILE_RANGE_WRITE 2
#define SYNC_FILE_RANGE_WAIT_AFTER 4
static uint64_t sys_sync_file_range(uint64_t fd, uint64_t offset,
                                    uint64_t nbytes, uint64_t flags,
                                    uint64_t a4, uint64_t a5) {
  (void)offset;
  (void)nbytes;
  (void)a4;
  (void)a5;
  if (flags & ~(uint64_t)(SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                          SYNC_FILE_RANGE_WAIT_AFTER))
    return (uint64_t)-22; // EINVAL: unknown flag bits
  struct thread *t = sched_get_current();
  if (!t || !FD_IN_RANGE(t, fd) || !fd_is_valid_node(t->fds[fd]))
    return (uint64_t)-9; // EBADF
  vfs_cache_sync(t->fds[fd]);
  return 0;
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

static uint64_t sys_readahead(uint64_t fd, uint64_t offset, uint64_t count,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  uint64_t fd_flags = 0;
  struct fd_ref ref = fd_get_node(t, fd, NULL, &fd_flags);
  vfs_node_t *node = ref.node;
  if (!node)
    return (uint64_t)-9; // EBADF

  if ((fd_flags & O_PATH) || (fd_flags & O_ACCMODE) == O_WRONLY) {
    fd_put_node(ref);
    return (uint64_t)-9; // EBADF: descriptor is not open for reading
  }
  if ((int64_t)offset < 0) {
    fd_put_node(ref);
    return (uint64_t)-22; // EINVAL
  }
  if ((node->flags & FS_TYPE_MASK) != FS_FILE) {
    fd_put_node(ref);
    return (uint64_t)-29; // ESPIPE: no seekable file to prefetch
  }
  if (!node->read) {
    fd_put_node(ref);
    return (uint64_t)-22; // EINVAL: file does not support reads
  }

  /* Page cache keys are 32-bit offsets; for files exceeding 4 GiB, keep
   * readahead strictly below page 0xFFFFF000 so page_cache does not treat the
   * clamped length as EOF within that page. */
  uint64_t pc_limit = (node->io64 && node->io64->size(node) > UINT32_MAX)
                          ? 0xFFFFF000ULL
                          : (uint64_t)node->length;
  if (count && offset < pc_limit) {
    uint32_t byte_offset = (uint32_t)offset;
    uint64_t available = pc_limit - byte_offset;
    uint32_t bytes = count < available ? (uint32_t)count : (uint32_t)available;
    (void)vfs_cache_readahead(node, byte_offset, bytes);
  }

  fd_put_node(ref);
  return 0;
}

// Minimal cgroup2 hierarchy: tmpfs-backed mount with empty controller files
// so elogind can determine root cgroup ("0::/\n" + mountinfo entry) and
// allocate its manager. Without this, mount is a no-op stub and elogind
// fails with "Cannot determine cgroup ...: No data available".
#define CGROUP2_SUPER_MAGIC 0x63677270
#define CGROUP_SUPER_MAGIC 0x27e0eb

static int cgroup2_statfs(struct vfs_node *node, struct statfs_buf *buf) {
  (void)node;
  if (!buf)
    return -1;
  buf->f_type = CGROUP2_SUPER_MAGIC;
  buf->f_bsize = 4096;
  buf->f_blocks = (int64_t)(64ULL * 1024 * 1024 / 4096);
  buf->f_bfree = buf->f_blocks;
  buf->f_bavail = buf->f_blocks;
  buf->f_files = 65536;
  buf->f_ffree = 65500;
  buf->f_fsid[0] = 0;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = 4096;
  buf->f_flags = 0;
  return 0;
}

static int cgroup1_statfs(struct vfs_node *node, struct statfs_buf *buf) {
  int r = cgroup2_statfs(node, buf);
  if (r == 0)
    buf->f_type = CGROUP_SUPER_MAGIC;
  return r;
}

// Resolve /proc/self/fd/N and /proc/<pid>/fd/N to the fd's canonical path
// (libmount/OpenRC mount via fd to avoid TOCTOU). Returns buf when resolved,
// else the original target. Used so mountinfo shows real paths and tmpfs
// mounts can be guarded against shadowing /sys/fs/cgroup.
static const char *mount_canonicalize(const char *target, char *buf,
                                      uint32_t bufsz) {
  if (!target || strncmp(target, "/proc/", 6) != 0 ||
      strstr(target, "/fd/") == NULL)
    return target;
  const char *fdpart = strstr(target, "/fd/");
  uint32_t fd = 0;
  for (const char *p = fdpart + 4; *p >= '0' && *p <= '9'; p++)
    fd = fd * 10 + (uint32_t)(*p - '0');
  uint32_t pid = 0;
  struct thread *cur = sched_get_current();
  uint32_t self = cur ? (cur->tgid ? cur->tgid : cur->tid) : 0;
  if (strncmp(target, "/proc/self/fd/", 14) == 0) {
    pid = self;
  } else {
    for (const char *p = target + 6; *p >= '0' && *p <= '9'; p++)
      pid = pid * 10 + (uint32_t)(*p - '0');
  }
  /* The caller's own descriptors come straight from its file table: PID 1's
   * tid is rewritten after creation, so a tid lookup can miss it (systemd
   * mounts everything through /proc/self/fd/N). */
  if (cur && cur->files && pid == self) {
    bool ok = false;
    spinlock_acquire(&cur->files->lock);
    if (FD_IN_RANGE(cur, fd) && fd_is_valid_node(cur->fds[fd])) {
      const char *value = fd_path_value(cur, (int)fd);
      if (value && value[0] == '/') {
        strncpy(buf, value, bufsz - 1);
        buf[bufsz - 1] = '\0';
        ok = true;
      }
    }
    spinlock_release(&cur->files->lock);
    return ok ? buf : target;
  }
  if (pid && sched_get_fd_path_snapshot(pid, fd, buf, bufsz) && buf[0] == '/')
    return buf;
  return target;
}

static void cgroup2_populate(vfs_node_t *dir, const char *controllers) {
  (void)controllers;
  struct cgroupfs_group *root_group = cgroupfs_group_create(NULL);
  if (root_group)
    tmpfs_cgroup_populate_group(dir, root_group);
  /* Legacy v1 names for hybrid submounts (elogind/OpenRC). */
  vfs_create(dir, "tasks", 0644);
  vfs_create(dir, "notify_on_release", 0644);
}

uint64_t cgroup2_mount_at(const char *target, const char *fstype) {
  // OpenRC mounts via /proc/self/fd/N to avoid TOCTOU: canonicalize to the
  // real path so mountinfo shows /sys/fs/cgroup, not /proc/self/fd/N.
  // Otherwise elogind never finds the hierarchy (ENOMEDIUM).
  char canon[256];
  const char *ctarget = mount_canonicalize(target, canon, sizeof(canon));

  vfs_node_t *mountpoint = vfs_resolve_path(ctarget);
  if (!mountpoint) {
    // Auto-create leaf subdir for cgroup subhierarchies (e.g. .../unified)
    // when parent tmpfs root exists but leaf doesn't yet. Mirrors
    // OpenRC mkdir-before-mount without failing the mount.
    if (strncmp(ctarget, "/sys/fs/cgroup/", 15) == 0) {
      const char *leaf = ctarget + 15;
      // Single-level names only (unified/elogind/openrc); deeper paths fail.
      if (leaf[0] && strchr(leaf, '/') == NULL) {
        vfs_node_t *parent = vfs_resolve_path("/sys/fs/cgroup");
        if (parent) {
          if (parent->mkdir)
            parent->mkdir(parent, (char *)leaf, 0755);
          vfs_close(parent);
          mountpoint = vfs_resolve_path(ctarget);
        }
      }
    }
    if (!mountpoint)
      return (uint64_t)-2;
  }
  if ((mountpoint->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    vfs_close(mountpoint);
    return (uint64_t)-20; // ENOTDIR: never mount over /proc fd files/sockets
  }
  vfs_close(mountpoint);

  // Hybrid layout: /sys/fs/cgroup itself is tmpfs (mounted by OpenRC/elogind),
  // subdirs are cgroup2/v1 hierarchies. Do NOT auto-mount parent as cgroup2:
  // that would shadow the tmpfs container. Submounts only need the parent dir
  // to exist (ensured by sysfs_init + tmpfs root mount).
  mountpoint = vfs_resolve_path(ctarget);
  if (!mountpoint)
    return (uint64_t)-2;

  // Already has a cgroup mount? vfs_get_mounts is the mountinfo source, so
  // check for an existing entry to make mount idempotent.
  {
    vfs_mount_info_t mounts[16];
    int n = vfs_get_mounts(mounts, 16);
    for (int i = 0; i < n; i++) {
      if (strcmp(mounts[i].mountpoint, ctarget) == 0 &&
          (strcmp(mounts[i].fs_type, "cgroup") == 0 ||
           strcmp(mounts[i].fs_type, "cgroup2") == 0)) {
        vfs_close(mountpoint);
        return 0;
      }
    }
  }

  tmpfs_sb_t *sb = kmalloc(sizeof(tmpfs_sb_t));
  if (!sb) {
    vfs_close(mountpoint);
    return (uint64_t)-12;
  }
  sb->max_bytes = 64ULL * 1024 * 1024;
  sb->max_inodes = 65536;
  sb->used_bytes = 0;
  sb->used_inodes = 0;
  sb->cgroupfs = true;
  spinlock_init(&sb->lock);

  vfs_node_t *root = tmpfs_create_root(sb);
  if (!root) {
    kfree(sb);
    vfs_close(mountpoint);
    return (uint64_t)-12;
  }
  const char *name = ctarget;
  for (const char *p = ctarget; *p; p++)
    if (*p == '/')
      name = p + 1;
  strncpy(root->name, name, 127);
  root->name[127] = '\0';
  // statfs magic decides unified vs legacy in cg_all_unified():
  // tmpfs magic => not unified => ENODATA. Report real cgroup magics.
  if (fstype && strcmp(fstype, "cgroup") == 0)
    root->statfs = cgroup1_statfs;
  else
    root->statfs = cgroup2_statfs;

  if (vfs_mount_ex(mountpoint, root, "cgroup2", fstype ? fstype : "cgroup2",
                   ctarget) != 0) {
    vfs_close(mountpoint);
    return (uint64_t)-12;
  }
  vfs_close(mountpoint);

  // Stub control files: elogind 252 expects unified hierarchy with usable
  // controllers. Empty controllers => root deemed unusable => hybrid fallback
  // => ENODATA. Pre-populate with fake controller list; writes (e.g. "+cpu"
  // to subtree_control, PIDs to procs) succeed via tmpfs and are ignored.
  // Include v1 names (tasks) so hybrid submounts don't fail on missing files.
  vfs_node_t *cgroup_root = vfs_resolve_path(ctarget);
  if (cgroup_root) {
    cgroup2_populate(cgroup_root, "cpu memory pids\n");
    vfs_close(cgroup_root);
  }
  klog_puts("[  OK  ] cgroup2 mounted at ");
  klog_puts(ctarget);
  klog_puts(" fstype=");
  klog_puts(fstype ? fstype : "?");
  klog_puts("\n");
  return 0;
}


/* Mount one block filesystem type on `dev` in place.  False leaves
 * mountpoint untouched.  The mount entry and fat32 hold their own device
 * references.  ext2/ext4 never free their mount state, which keeps a raw
 * device pointer, so it pins the device for good. */
static bool sys_mount_block_try(struct block_device *dev, vfs_node_t *mountpoint,
                                vfs_node_t *saved, const char *target,
                                const char *type, uint64_t flags,
                                bool exfat_overwrite) {
    if (strcmp(type, "vfat") == 0) {
        if (fat32_mount(dev, mountpoint) != 0)
            return false;
        vfs_mount_inplace(mountpoint, saved, dev, dev->name, "vfat", target, NULL, fat32_unmount, fat32_live_nodes);
        return true;
    }
    if (strcmp(type, "exfat") == 0) {
        if (exfat_mount(dev, mountpoint, (flags & 1) != 0, exfat_overwrite) != 0)
            return false;
        vfs_mount_inplace(mountpoint, saved, dev, dev->name, "exfat", target, NULL, exfat_unmount, exfat_live_nodes);
        return true;
    }
    bool ext4 = strcmp(type, "ext4") == 0;
    if ((ext4 ? ext4_mount(dev, mountpoint) : ext2_mount(dev, mountpoint)) != 0)
        return false;
    block_ref(dev);
    vfs_mount_inplace(mountpoint, saved, dev, dev->name, type, target, ext4 ? ext4_unmount : ext2_unmount, NULL, NULL);
    return true;
}

/* The runtime-dir service supplies uid, gid and mode in the legacy mount
 * data. Set them before publishing the filesystem so the user manager can
 * open its bus socket as soon as the mount syscall succeeds. */
/* Parse tmpfs mode=/uid=/gid= options. Defaults follow Linux: a sticky,
 * world-writable root owned by the mounting task's fs credentials. bwrap
 * (Steam, Flatpak) mounts an option-less tmpfs as an unprivileged user and
 * then mkdir()s its new root inside it. */
static int tmpfs_parse_root_options(uint64_t data_ptr, uint32_t *mode_out,
                                    uint32_t *uid_out, uint32_t *gid_out) {
    struct thread *cur = sched_get_current();
    uint32_t mode = 01777;
    uint32_t uid = cur ? cur->fsuid : 0, gid = cur ? cur->fsgid : 0;
    if (data_ptr) {
        char options[512];
        long n = strncpy_from_user(options, (const void *)data_ptr, sizeof(options));
        if (n < 0) return -14;
        if ((size_t)n >= sizeof(options)) return -36;
        const char *option = options;
        while (*option) {
            const char *end = option;
            while (*end && *end != ',') end++;
            size_t length = end - option;
            uint32_t *out = NULL;
            unsigned base = 10, prefix = 4;
            uint32_t max = UINT32_MAX - 1;
            if (length >= 5 && !strncmp(option, "mode=", 5)) {
                out = &mode;
                base = 8;
                prefix = 5;
                max = 07777;
            } else if (length >= 4 && !strncmp(option, "uid=", 4)) {
                out = &uid;
            } else if (length >= 4 && !strncmp(option, "gid=", 4)) {
                out = &gid;
            }
            if (out) {
                const char *digit = option + prefix;
                if (digit == end) return -22;
                uint32_t value = 0;
                while (digit < end) {
                    unsigned d = (unsigned)(*digit++ - '0');
                    if (d >= base || value > (max - d) / base) return -22;
                    value = value * base + d;
                }
                *out = value;
            }
            option = *end ? end + 1 : end;
        }
    }
    *mode_out = mode; *uid_out = uid; *gid_out = gid;
    return 0;
}

static int mount_tmpfs_at(vfs_node_t *point, const char *target,
                          uint64_t data_ptr) {
    uint32_t mode, uid, gid;
    int parsed = tmpfs_parse_root_options(data_ptr, &mode, &uid, &gid);
    if (parsed) return parsed;
    if ((point->flags & FS_TYPE_MASK) != FS_DIRECTORY) return -20;
    vfs_node_t *root = tmpfs_create_private_root();
    if (!root) return -12;
    const char *name = strrchr(target, '/');
    strncpy(root->name, name ? name + 1 : target, sizeof(root->name) - 1);
    root->name[sizeof(root->name) - 1] = '\0';
    root->mask = mode;
    root->uid = uid;
    root->gid = gid;
    int result = vfs_mount_ex(point, root, "tmpfs", "tmpfs", target);
    if (result) {
        vfs_close(root);
        return -12;
    }
    return 0;
}

static uint64_t sys_mount(uint64_t source_ptr, uint64_t target_ptr,
                          uint64_t fstype_ptr, uint64_t flags,
                          uint64_t data_ptr, uint64_t a5) {
    (void)a5;

    /* Legacy libc mount magic occupies the propagation-bit range. Linux
     * strips MS_MGC_VAL before interpreting modern mount flags. */
    if ((flags & 0xffff0000ULL) == 0xc0ed0000ULL) flags &= 0xffffULL;

    char source_buf[512], target_buf[512], type_buf[64];
    const char *source = NULL, *target = target_buf, *fstype = NULL;
    if (!target_ptr || !is_user_range((void *)target_ptr, 1)) return (uint64_t)-14;
    long n = strncpy_from_user(target_buf, (void *)target_ptr, sizeof(target_buf));
    if (n < 0) return (uint64_t)-14;
    if ((uint64_t)n >= sizeof(target_buf)) return (uint64_t)-36;
    if (source_ptr) {
        if (!is_user_range((void *)source_ptr, 1)) return (uint64_t)-14;
        n = strncpy_from_user(source_buf, (void *)source_ptr, sizeof(source_buf));
        if (n < 0) return (uint64_t)-14;
        if ((uint64_t)n >= sizeof(source_buf)) return (uint64_t)-36;
        source = source_buf;
    }
    if (fstype_ptr) {
        if (!is_user_range((void *)fstype_ptr, 1)) return (uint64_t)-14;
        n = strncpy_from_user(type_buf, (void *)fstype_ptr, sizeof(type_buf));
        if (n < 0) return (uint64_t)-14;
        if ((uint64_t)n >= sizeof(type_buf)) return (uint64_t)-36;
        fstype = type_buf;
    }

    struct thread *task = sched_get_current();
    /* Inside an owned mount namespace, changes stay in that namespace's
     * table; everything else needs full CAP_SYS_ADMIN. */
    bool ns_ok = vfs_namespace_active() && capability_mount_ns_admin(task);
    if (!ns_ok && !capability_has_init_ns(task, CAP_SYS_ADMIN)) return (uint64_t)-1;
    vfs_node_t *base = task && task->cwd_node ? task->cwd_node : fs_root;
    if (fstype && (!strcmp(fstype, "fuse") || !strncmp(fstype, "fuse.", 5))) {
        if (!capability_has_init_ns(task, CAP_SYS_ADMIN)) return (uint64_t)-1;
        char options[1024];
        if (!data_ptr) return (uint64_t)-22;
        long length = strncpy_from_user(options, (void *)data_ptr, sizeof(options));
        if (length < 0) return (uint64_t)-14;
        if ((uint64_t)length >= sizeof(options)) return (uint64_t)-36;
        vfs_node_t *point = vfs_resolve_path_at(base, target);
        if (!point) return (uint64_t)-2;
        int result = fuse_mount_at(point, source, fstype, target, flags, options);
        vfs_close(point);
        return (uint64_t)result;
    }
    /* Namespace mount operations resolve relative to the actual directory
     * handle: after pivot_root the saved textual cwd is deliberately stale. */
    if (vfs_namespace_active()) {
        vfs_node_t *point = vfs_resolve_path_at(base, target);
        if (!point) return (uint64_t)-2;
        int result = -95;
        if (flags & ((1 << 17) | (1 << 18) | (1 << 19) | (1 << 20))) {
            /* Tables are private by construction. Shared propagation needs
             * peer groups, which do not exist: MS_SHARED is accepted as a
             * no-op because systemd-executor re-shares "/" after building
             * every sandbox and fails the unit otherwise. Mounts made later
             * elsewhere will not propagate in. MS_UNBINDABLE stays refused. */
            result = (flags & (1 << 17)) ? -95 : 0;
        } else if (flags & 0x20) {
            result = vfs_namespace_remount(point, flags);
        } else if (flags & 0x2000) {
            vfs_node_t *src = source ? vfs_resolve_path_at(base, source) : NULL;
            result = src ? vfs_namespace_move(src, point) : -2;
            if (src) vfs_close(src);
        } else if (flags & 0x1000) {
            vfs_node_t *src = source ? vfs_resolve_path_at(base, source) : NULL;
            if (!src) result = source ? -2 : -14;
            else {
                result = vfs_namespace_mount(point, src, source, "bind", target, flags);
                vfs_close(src);
            }
        } else if (fstype && (!strcmp(fstype, "tmpfs") || !strcmp(fstype, "ramfs"))) {
            uint32_t mode, uid, gid;
            if ((point->flags & FS_TYPE_MASK) != FS_DIRECTORY) result = -20;
            else if ((result = tmpfs_parse_root_options(data_ptr, &mode, &uid, &gid)) == 0) {
                vfs_node_t *root = tmpfs_create_private_root();
                if (!root) result = -12;
                else {
                    root->mask = mode & 07777;
                    root->uid = uid;
                    root->gid = gid;
                    result = vfs_namespace_mount(point, root, source, fstype, target, flags);
                    vfs_close(root);
                }
            }
        } else if (fstype && !strcmp(fstype, "devpts")) {
            vfs_node_t *root = devpts_create_root();
            result = root ? vfs_namespace_mount(point, root, source, "devpts", target, flags) : -12;
            if (root) vfs_close(root);
        } else if (fstype && !strcmp(fstype, "proc")) {
            vfs_node_t *root = vfs_initial_filesystem("proc");
            result = root ? vfs_namespace_mount(point, root, source, "proc", target, flags) : -19;
            if (root) vfs_close(root);
        } else if (fstype && !strcmp(fstype, "sysfs")) {
            /* sysfs has a single instance, like proc here; systemd sandboxes
             * (ProtectKernelTunables=, PrivateNetwork=) mount a fresh one. */
            vfs_node_t *root = vfs_initial_filesystem("sysfs");
            result = root ? vfs_namespace_mount(point, root, source, "sysfs", target, flags) : -19;
            if (root) vfs_close(root);
        }
        vfs_close(point);
        return (uint64_t)result;
    }
    /* systemd mounts on /proc/self/fd/N (an O_PATH handle on the target).
     * The legacy paths below resolve and record targets by name. */
    char target_canon[256];
    target = mount_canonicalize(target, target_canon, sizeof(target_canon));

    /* Initial-namespace propagation remains private; unsupported mutations
     * are rejected rather than silently claiming isolation or read-only. */
    /* MS_SHARED: same no-op as in a private namespace (see above). */
    if (flags & ((1 << 18) | (1 << 19) | (1 << 20))) return 0;
    if (flags & (0x20 | (1 << 17))) return (uint64_t)-95;
    if (flags & 0x1000) {
        vfs_node_t *src = source ? vfs_resolve_path_at(base, source) : NULL;
        vfs_node_t *point = vfs_resolve_path_at(base, target);
        if (!src || !point) { if (src) vfs_close(src); if (point) vfs_close(point); return (uint64_t)-2; }
        int result = vfs_mount_ex(point, src, source, "bind", target);
        vfs_close(point);
        if (result)
            vfs_close(src);
        return (uint64_t)result;
    }

    /* Pseudo-filesystems handling */
    if (fstype) {
        if (strcmp(fstype, "proc") == 0 ||
            strcmp(fstype, "sysfs") == 0 ||
            strcmp(fstype, "devtmpfs") == 0 ||
            strcmp(fstype, "devfs") == 0) {
            vfs_node_t *mountpoint = vfs_resolve_path(target);
            if (mountpoint) {
                vfs_close(mountpoint);
                return 0;
            }
            return (uint64_t)-2;
        }

        if (strcmp(fstype, "devpts") == 0) {
            vfs_node_t *point = vfs_resolve_path(target);
            if (!point) return (uint64_t)-2;
            if ((point->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
                vfs_close(point);
                return (uint64_t)-20;
            }
            vfs_node_t *root = devpts_create_root();
            if (!root) { vfs_close(point); return (uint64_t)-12; }
            int result = vfs_mount_ex(point, root, source, "devpts", target);
            vfs_close(point);
            /* Initial mounts retain the root's initial reference. */
            if (result) vfs_close(root);
            return (uint64_t)result;
        }

        if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0) {
            char tcanon[256];
            const char *ct = mount_canonicalize(target, tcanon, sizeof(tcanon));
            vfs_node_t *mountpoint = vfs_resolve_path(ct);
            if (!mountpoint)
                return (uint64_t)-2;
            if (!strcmp(fstype, "tmpfs")) {
                int result = mount_tmpfs_at(mountpoint, ct, data_ptr);
                vfs_close(mountpoint);
                return (uint64_t)result;
            }
            vfs_close(mountpoint);
            tmpfs_mount_at(ct);
            return 0;
        }

        if (strcmp(fstype, "cgroup2") == 0) {
            return cgroup2_mount_at(target, fstype);
        }

        if (strcmp(fstype, "cgroup") == 0) {
            return cgroup2_mount_at(target, fstype);
        }

        if (strcmp(fstype, "securityfs") == 0 ||
            strcmp(fstype, "debugfs") == 0 ||
            strcmp(fstype, "pstore") == 0 ||
            strcmp(fstype, "configfs") == 0 ||
            strcmp(fstype, "mqueue") == 0 ||
            strcmp(fstype, "fusectl") == 0) {
            vfs_node_t *mountpoint = vfs_resolve_path(target);
            if (mountpoint) {
                vfs_close(mountpoint);
                return 0;
            }
            return (uint64_t)-2;
        }
    }

    /* retain the earlier restricted overwrite mount mode */
    bool exfat_overwrite = false;
    if (fstype && !strcmp(fstype, "exfat") && data_ptr && !(flags & 1)) {
        char option[sizeof("overwrite")];
        long len = strncpy_from_user(option, (const void *)data_ptr, sizeof(option));
        if (len < 0)
            return (uint64_t)-14;
        exfat_overwrite = len == sizeof("overwrite") - 1 &&
                          memcmp(option, "overwrite", sizeof(option)) == 0;
    }
    if (!source)
        return (uint64_t)-14;

    vfs_node_t *mountpoint = vfs_resolve_path(target);
    if (!mountpoint)
        return (uint64_t)-2;
    if ((mountpoint->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
        vfs_close(mountpoint);
        return (uint64_t)-20;
    }

    struct block_device *dev = block_get_by_name(source);
    if (!dev) {
        vfs_close(mountpoint);
        return (uint64_t)-6;
    }

    /* The block filesystems adopt mountpoint in place.  Keep the pre-mount
     * image so umount can put the directory back. */
    vfs_node_t *saved = kmalloc(sizeof(vfs_node_t));
    if (!saved) {
        block_put(dev);
        vfs_close(mountpoint);
        return (uint64_t)-12;
    }
    memcpy(saved, mountpoint, sizeof(vfs_node_t));

    static const char *const fat_order[] = {"vfat", NULL};
    static const char *const exfat_order[] = {"exfat", NULL};
    static const char *const ext4_order[] = {"ext4", "ext2", NULL};
    static const char *const ext2_order[] = {"ext2", "ext4", NULL};
    static const char *const any_order[] = {"ext4", "ext2", "vfat", "exfat", NULL};
    const char *const *order = any_order;
    if (fstype) {
        if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "fat32") == 0 ||
            strcmp(fstype, "fat16") == 0 || strcmp(fstype, "fat12") == 0 ||
            strcmp(fstype, "msdos") == 0)
            order = fat_order;
        else if (strcmp(fstype, "exfat") == 0)
            order = exfat_order;
        else if (strcmp(fstype, "ext4") == 0)
            order = ext4_order;
        else if (strcmp(fstype, "ext2") == 0 || strcmp(fstype, "ext3") == 0)
            order = ext2_order;
    }

    /* The lookup reference is ours alone; mounts take their own. */
    for (; *order; order++) {
        if (sys_mount_block_try(dev, mountpoint, saved, target, *order,
                                flags, exfat_overwrite)) {
            block_put(dev);
            return 0;
        }
    }

    /* All probes failed before touching mountpoint: drop our resolver ref.
     * On success the filesystem adopts the node in place (clearing
     * FS_PERSISTENT), so that reference is what keeps the mounted root alive
     * and must not be dropped. */
    block_put(dev);
    kfree(saved);
    vfs_close(mountpoint);
    return (uint64_t)-22;
}

static uint64_t sys_umount2(uint64_t target_ptr, uint64_t flags,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *task = sched_get_current();
    bool ns_ok = vfs_namespace_active() && capability_mount_ns_admin(task);
    if (!ns_ok && !capability_has_init_ns(task, CAP_SYS_ADMIN)) return (uint64_t)-1;
    char path[512];
    if (!target_ptr || !is_user_range((void *)target_ptr, 1)) return (uint64_t)-14;
    long n = strncpy_from_user(path, (void *)target_ptr, sizeof(path));
    if (n < 0) return (uint64_t)-14;
    if ((uint64_t)n >= sizeof(path)) return (uint64_t)-36;
    vfs_node_t *point = vfs_resolve_path_at(task && task->cwd_node ? task->cwd_node : fs_root, path);
    if (!point) return (uint64_t)-2;
    int result;
    if (fuse_is_root(point)) {
        if (!capability_has_init_ns(task, CAP_SYS_ADMIN)) {
            vfs_close(point);
            return (uint64_t)-1;
        }
        result = !(flags & 2) && fuse_mount_busy(point) ? -16 :
            vfs_initial_unmount(point, (unsigned)flags);
        if (!result) fuse_detach_mount(point);
    } else if (vfs_namespace_active()) result = vfs_namespace_umount(point, (unsigned)flags);
    else {
        if (!capability_has_init_ns(task, CAP_SYS_ADMIN)) result = -1;
        else result = vfs_inplace_unmount(point, (unsigned)flags);
    }
    vfs_close(point);
    return (uint64_t)result;
}

void syscall_register_fd(void) {
  syscall_register(SYS_READ, sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_PREAD64, sys_pread64);
  syscall_register(SYS_PWRITE64, sys_pwrite64);
  syscall_register(SYS_READV, sys_readv);
  syscall_register(SYS_WRITEV, sys_writev);
  syscall_register(SYS_PREADV, sys_preadv);
  syscall_register(SYS_PWRITEV, sys_pwritev);
  syscall_register(SYS_OPEN, sys_open);
  syscall_register(SYS_CREAT, sys_creat);
  syscall_register(SYS_OPENAT, sys_openat);
  syscall_register(437, sys_openat2);
  syscall_register(442, sys_mount_setattr);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_CLOSE_RANGE, sys_close_range);
  syscall_register(SYS_DUP, sys_dup);
  syscall_register(SYS_DUP2, sys_dup2);
  syscall_register(SYS_DUP3, sys_dup3);
  syscall_register(SYS_LSEEK, sys_lseek);
  syscall_register(SYS_FCNTL, sys_fcntl);
  syscall_register(SYS_FTRUNCATE, sys_ftruncate);
  syscall_register(SYS_FALLOCATE, sys_fallocate);
  syscall_register(SYS_FLOCK, sys_flock);
  syscall_register(SYS_FSYNC, sys_fsync);
  syscall_register(SYS_FDATASYNC, sys_fdatasync);
  syscall_register(SYS_SYNC, sys_sync);
  syscall_register(SYS_SYNCFS, sys_syncfs);
  syscall_register(SYS_SYNC_FILE_RANGE, sys_sync_file_range);
  syscall_register(SYS_MOUNT, sys_mount);
  syscall_register(SYS_UMOUNT2, sys_umount2);
  syscall_register(SYS_SENDFILE, sys_sendfile);
  syscall_register(SYS_COPY_FILE_RANGE, sys_copy_file_range);
  syscall_register(SYS_SPLICE, sys_splice);
  syscall_register(SYS_FADVISE64, sys_fadvise64);
  syscall_register(SYS_READAHEAD, sys_readahead);
}
