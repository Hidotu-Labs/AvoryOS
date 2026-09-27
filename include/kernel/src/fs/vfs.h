#ifndef FS_VFS_H
#define FS_VFS_H

#include "../lib/list.h"
#include "../lib/radix_tree.h"
#include "../lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_FILE 0x01
#define FS_DIRECTORY 0x02
#define FS_CHARDEV 0x03
#define FS_BLOCKDEV 0x04
#define FS_PIPE 0x05
#define FS_SYMLINK 0x06
#define FS_SOCKET 0x07
#define FS_MOUNTPOINT 0x08
#define FS_EPOLL 0x09
#define FS_PERSISTENT 0x10
#define FS_NONBLOCK 0x20
#define FS_DENTRY_NOCACHE 0x40
#define FS_PAGE_CACHE 0x80
#define FS_TYPE_MASK 0x0F

/* Live-object markers.  A node or cached page that has been freed has its
 * marker cleared, and anything that reaches the object through a stale pointer
 * can say so instead of trusting random memory.  Both values are also checked
 * against the HHDM/managed-RAM window first, so a wild address is rejected
 * before it is dereferenced. */
#define VFS_NODE_MAGIC 0x4E4F4445u /* "NODE" */
#define VFS_PAGE_MAGIC 0x50414745u /* "PAGE" */

// Poll Events
#define POLLIN     0x0001
#define POLLPRI    0x0002  /* Out-of-band / secondary readable (pidfds use it) */
#define POLLRDNORM 0x0040  /* Normal data readable (same as EPOLLRDNORM) */
#define POLLOUT    0x0004
#define POLLWRNORM 0x0100  /* Normal data writable (same as EPOLLWRNORM) */
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020

struct vfs_node;
typedef struct vfs_page {
  uint32_t offset;       // Byte offset within the file (page aligned)
  uint32_t magic;        // VFS_PAGE_MAGIC while the object is live
  uint64_t frame_phys;   // Physical address of the frame
  bool dirty;            // True if data has been modified but not written back
  bool evicted;          // Removed from the tree; free after final transient ref
  bool loading;          // One owner is filling this page from the filesystem
  bool uptodate;         // Frame contains a complete, valid file page
  bool writeback;        // Filesystem write is currently using this frame
  uint32_t refs;         // Transient users; the tree owns a separate cache ref
  uint64_t last_used;    // Monotonic access stamp used for cache reclaim
  uint64_t dirty_seq;    // Detects modifications racing with writeback
} vfs_page_t;

struct dirent {
  char name[128];
  uint32_t ino;
  uint8_t d_type; // DT_* value; readdir backends fill this in
};

/* Directory entry types (Linux DT_* values). */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

/* Map a vfs_node flags word to the directory entry type reported by readdir. */
static inline uint8_t vfs_dtype(uint32_t flags) {
  switch (flags & FS_TYPE_MASK) {
  case FS_FILE:      return DT_REG;
  case FS_DIRECTORY: return DT_DIR;
  case FS_CHARDEV:   return DT_CHR;
  case FS_BLOCKDEV:  return DT_BLK;
  case FS_SYMLINK:   return DT_LNK;
  case FS_SOCKET:    return DT_SOCK;
  case FS_PIPE:      return DT_FIFO;
  default:           return DT_UNKNOWN;
  }
}

typedef uint32_t (*read_type_t)(struct vfs_node *, uint32_t, uint32_t,
                                uint8_t *);
typedef uint32_t (*write_type_t)(struct vfs_node *, uint32_t, uint32_t,
                                 uint8_t *);
typedef void (*open_type_t)(struct vfs_node *);
typedef void (*close_type_t)(struct vfs_node *);
typedef struct vfs_node *(*open_instance_type_t)(struct vfs_node *);
typedef int (*ioctl_type_t)(struct vfs_node *, uint32_t request, uint64_t arg);
typedef struct dirent *(*readdir_type_t)(struct vfs_node *, uint32_t);
typedef struct vfs_node *(*finddir_type_t)(struct vfs_node *, char *name);
typedef int (*create_type_t)(struct vfs_node *, char *name,
                             uint16_t permission);
typedef int (*mkdir_type_t)(struct vfs_node *, char *name, uint16_t permission);
typedef int (*mknod_type_t)(struct vfs_node *, char *name, uint16_t permission,
                            uint32_t flags, void *device);
typedef int (*unlink_type_t)(struct vfs_node *, char *name);
typedef int (*rmdir_type_t)(struct vfs_node *, char *name);
typedef int (*readlink_type_t)(struct vfs_node *, char *buf, uint32_t size);
typedef int (*symlink_type_t)(struct vfs_node *, char *name, char *target);
typedef int (*rename_type_t)(struct vfs_node *, char *old_name, char *new_name);
typedef int (*chmod_type_t)(struct vfs_node *, uint16_t permission);
typedef int (*chown_type_t)(struct vfs_node *, uint32_t uid, uint32_t gid);
typedef int (*truncate_type_t)(struct vfs_node *, uint32_t);
typedef uint64_t (*mmap_type_t)(struct vfs_node *, uint64_t addr,
                                uint64_t length, uint64_t prot, uint64_t flags,
                                uint64_t offset);
typedef int (*poll_type_t)(struct vfs_node *, int events);
typedef int (*fallocate_type_t)(struct vfs_node *, int mode, uint32_t offset,
                                uint32_t len);
// statfs structure (Linux x86_64 ABI compatible)
// All fields are 'long' (8 bytes on x86_64).
// f_fsid is __kernel_fsid_t = int[2] = exactly 8 bytes — NOT uint64_t[2].
struct statfs_buf {
  int64_t f_type;
  int64_t f_bsize;
  int64_t f_blocks;
  int64_t f_bfree;
  int64_t f_bavail;
  int64_t f_files;
  int64_t f_ffree;
  int32_t f_fsid[2]; /* __kernel_fsid_t: two int32 = 8 bytes total */
  int64_t f_namelen;
  int64_t f_frsize;
  int64_t f_flags;
  int64_t f_spare[4];
};

typedef int (*statfs_type_t)(struct vfs_node *, struct statfs_buf *buf);

typedef struct vfs_node {
  char name[128];
  uint32_t mask; // Permissions
  uint32_t uid;
  uint32_t gid;
  uint32_t flags; // Node type
  uint32_t inode;
  uint32_t length; // Size of file
  uint32_t impl;   // Implementation-defined
  void *device;    // Optional binding to driver block device or ramfs specific
                   // struct

  uint32_t atime; // Access time
  uint32_t mtime; // Modification time
  uint32_t ctime; // Creation time

  read_type_t read;
  write_type_t write;
  open_type_t open;
  close_type_t close;
  open_instance_type_t open_instance; // Optional per-open node factory
  readdir_type_t readdir;
  finddir_type_t finddir;
  create_type_t create;
  mkdir_type_t mkdir;
  mknod_type_t mknod;
  unlink_type_t unlink;
  rmdir_type_t rmdir;
  readlink_type_t readlink;
  symlink_type_t symlink;
  rename_type_t rename;
  chmod_type_t chmod;
  chown_type_t chown;
  truncate_type_t truncate;
  mmap_type_t mmap; // Device-specific mmap handler
  poll_type_t poll; // Device-specific poll handler
  statfs_type_t statfs;
  ioctl_type_t ioctl; // Device-specific ioctl handler
  fallocate_type_t fallocate;
  void *wait_queue; // Pointer to wait_queue_t for poll() wakeups

  struct list_head ep_watchers; // List of epitem_t watching this node
  spinlock_t ep_lock;           // Lock for ep_watchers

  struct vfs_node *ptr; // Used by mountpoints and symlinks
  /* Reference count for memory management.
   *
   * Touched from every CPU at once: two threads routinely open() and close()
   * the same node (a shared /dev/tty, a PTY master held by a shell and its
   * child, or a dentry/path-cache hit racing the last close), and vfs_close()
   * runs without any lock at all.  Plain ++/-- lost updates, which drove the
   * count to zero while a reference was still live and kfree()'d a node that
   * somebody was about to read through.  Use the helpers below. */
  uint32_t refcount;
  uint32_t magic; // VFS_NODE_MAGIC while the node is live

  struct radix_tree pages; // Cached pages keyed by file page index
  spinlock_t pages_lock;   // Serializes compound cache/value-lifetime changes

  /* Optional sequential readdir cursor used by filesystems with indexed API. */
  uint32_t readdir_cursor_index;
  uint32_t readdir_cursor_offset;
  spinlock_t readdir_cursor_lock;
} vfs_node_t;

extern vfs_node_t *fs_root;
bool vfs_in_group(uint32_t gid);
bool vfs_access(vfs_node_t *node, uint32_t requested);
bool vfs_may_remove(vfs_node_t *parent, vfs_node_t *target);

// Standard API wrapper functions
uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer);
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer);
void vfs_open(vfs_node_t *node);
void vfs_close(vfs_node_t *node);

/* True when `node` is a kernel pointer into managed RAM that still carries the
 * live-node marker.  Safe to call on a wild pointer: the RAM window is checked
 * before the marker is read. */
bool vfs_node_is_alive(const vfs_node_t *node);

/* Refcount helpers - see the note on vfs_node_t::refcount.  Every adjustment
 * of that field goes through these, including the ones made under a dentry or
 * path-cache bucket lock: vfs_close() takes no lock, so a bucket lock excludes
 * nothing. */
static inline void vfs_node_ref(vfs_node_t *node) {
  __atomic_add_fetch(&node->refcount, 1, __ATOMIC_RELAXED);
}

/* Takes a reference only if the node still has one, for cache lookups that
 * cannot tell whether the entry is being dropped on another CPU.  Returns false
 * when the caller must treat the entry as gone. */
static inline bool vfs_node_tryref(vfs_node_t *node) {
  uint32_t old = __atomic_load_n(&node->refcount, __ATOMIC_RELAXED);
  while (old != 0) {
    if (__atomic_compare_exchange_n(&node->refcount, &old, old + 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return true;
  }
  return false;
}

/* Drops a reference; returns true when this was the last one. */
static inline bool vfs_node_unref(vfs_node_t *node) {
  return __atomic_sub_fetch(&node->refcount, 1, __ATOMIC_ACQ_REL) == 0;
}
struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, char *name);
void vfs_dentry_invalidate(vfs_node_t *parent, const char *name);
// For successful create/mkdir/mknod/symlink of a previously free name: cheap
// and provably sufficient, see vfs.c.
void vfs_dentry_invalidate_created(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path);
vfs_node_t *vfs_resolve_path(const char *path);
int vfs_create(vfs_node_t *node, char *name, uint16_t permission);
int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission);
int vfs_unlink(vfs_node_t *node, char *name);
int vfs_rmdir(vfs_node_t *node, char *name);
int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size);
int vfs_symlink(vfs_node_t *node, char *name, char *target);
int vfs_rename(vfs_node_t *node, char *old_name, char *new_name);
int vfs_chmod(vfs_node_t *node, uint16_t permission);
int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid);
int vfs_truncate(vfs_node_t *node, uint32_t size);
int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len);
int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device);
int vfs_poll(vfs_node_t *node, int events);
void vfs_node_init(vfs_node_t *node);
int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target);
int vfs_mount_ex(vfs_node_t *mountpoint, vfs_node_t *target,
                 const char *dev_name, const char *fs_type);
int vfs_statfs(vfs_node_t *node, void *buf);

typedef struct vfs_mount_info {
  char mountpoint[128];
  char target[128];
  char dev_name[64];
  char fs_type[32];
} vfs_mount_info_t;

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count);

// Page Cache API
vfs_page_t *vfs_cache_lookup(vfs_node_t *node, uint32_t offset);
vfs_page_t *vfs_cache_insert(vfs_node_t *node, uint32_t offset, uint64_t frame);
size_t vfs_cache_page_count(void);
void vfs_cache_invalidate(vfs_node_t *node, uint32_t offset);
void vfs_cache_invalidate_range(vfs_node_t *node, uint32_t offset,
                                uint32_t length);
void vfs_cache_update_or_invalidate(vfs_node_t *node, uint32_t offset,
                                    uint32_t length, const uint8_t *buffer);
void vfs_cache_clear(vfs_node_t *node);
void vfs_cache_clear_unused(vfs_node_t *node);
void vfs_cache_sync(vfs_node_t *node);
void vfs_cache_mark_dirty(vfs_node_t *node, uint32_t offset);
size_t vfs_cache_reclaim(vfs_node_t *node, size_t target);
vfs_page_t *vfs_cache_get_or_create(vfs_node_t *node, uint32_t offset);
void vfs_cache_put(vfs_node_t *node, vfs_page_t *page);
bool vfs_cache_phase3_stress_test(void);
uint32_t vfs_cache_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer);
uint32_t vfs_cache_readahead(vfs_node_t *node, uint32_t offset, uint32_t max_bytes);
void vfs_cache_prefetch_async(vfs_node_t *node, uint32_t offset,
                              uint32_t length);
bool vfs_cache_phase4_stress_test(void);
bool vfs_cache_phase5_stress_test(void);

#endif
