#ifndef FS_VFS_H
#define FS_VFS_H

#include "../lib/list.h"
#include "../lib/radix_tree.h"
#include "../lock/spinlock.h"
#include "vfs_rcu.h"
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
#define FS_EPOLL 0x09
#define FS_PERSISTENT 0x10
#define FS_NONBLOCK 0x20
#define FS_DENTRY_NOCACHE 0x40
#define FS_PAGE_CACHE 0x80
/* Mount markers live outside FS_TYPE_MASK.  A node used as a mountpoint is
 * still whatever it was (almost always a directory), and every type check in
 * the kernel is `flags & FS_TYPE_MASK`; setting a type bit here corrupted the
 * directory bit into 0x0A and made operations like mkdirat() fail with
 * ENOTDIR on /run after userland mounted a tmpfs over it. */
#define FS_MOUNTPOINT 0x100
/* The filesystem removed this name but live descriptors, mappings or caches
 * still reference the node.  vfs_close() destroys the device payload through
 * ->destroy and frees the node when the last reference drops. */
#define FS_DELETED 0x200
/* Filesystem-owned ephemeral root: destroy payload on its final reference. */
#define FS_AUTODESTROY 0x400
/* Filesystem backing is forced read-only because its on-disk feature set
 * exceeds this driver's writable support. Unlike per-mount MS_RDONLY, this
 * cannot be cleared by a remount. */
#define FS_READONLY 0x800
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
  bool loading;          // One owner is filling this page from the filesystem
  bool uptodate;         // Frame contains a complete, valid file page
  bool writeback;        // Filesystem write is currently using this frame
  uint32_t refs;         // Packed: transient count | VFS_PAGE_EVICTED
  uint64_t last_used;    // Access stamp used for cache reclaim (per-CPU order)
  uint64_t dirty_seq;    // Detects modifications racing with writeback
  /* Deferred-free link (fs/vfs_rcu.c): a lockless vfs_cache_lookup() may have
   * observed this page in the radix tree and be about to take a reference
   * when the last put releases it, so the object and its frame are handed to
   * the grace-period queue instead of being freed inline. */
  struct vfs_rcu_head rcu;
} vfs_page_t;

/* Transient-reference word.  The low 31 bits count transient users (the tree
 * itself owns the page, not a reference); the top bit is the eviction marker.
 * A page is freed exactly once, by whichever transition below takes it to
 * "zero + marker":
 *
 *  - vfs_page_put() drops a transient ref; a result of exactly the marker
 *    means this put owned the last reference of an already-evicted page.
 *  - vfs_page_evict() marks the page evicted after it left the tree; a result
 *    of 0 means it was removed while no transient user held it.
 *  - vfs_page_tryref() (the lockless lookup path) adds a transient ref only
 *    while the marker is clear, so it can never revive an evicted page whose
 *    release already observed zero users.
 *
 * Packing both into one word is what lets a plain ref drop stay lock-free:
 * two racing owners cannot both observe "zero + marker", so exactly one of
 * them frees.  Keep every access to `refs` on these helpers. */
#define VFS_PAGE_EVICTED (1u << 31)

static inline uint32_t vfs_page_ref_count(const vfs_page_t *page) {
  return __atomic_load_n(&((vfs_page_t *)page)->refs, __ATOMIC_RELAXED) &
         ~VFS_PAGE_EVICTED;
}

static inline bool vfs_page_is_evicted(const vfs_page_t *page) {
  return (__atomic_load_n(&((vfs_page_t *)page)->refs, __ATOMIC_RELAXED) &
          VFS_PAGE_EVICTED) != 0;
}

static inline void vfs_page_get(vfs_page_t *page) {
  __atomic_add_fetch(&page->refs, 1, __ATOMIC_RELAXED);
}

/* Drop a transient ref; returns true when the caller must free the page. */
static inline bool vfs_page_put(vfs_page_t *page) {
  return __atomic_sub_fetch(&page->refs, 1, __ATOMIC_ACQ_REL) ==
         VFS_PAGE_EVICTED;
}

/* Mark the page evicted after it left the tree; returns true when the caller
 * must free it.  Must be called exactly once per page, after tree removal. */
static inline bool vfs_page_evict(vfs_page_t *page) {
  return __atomic_fetch_or(&page->refs, VFS_PAGE_EVICTED, __ATOMIC_ACQ_REL) == 0;
}

/* Try to take a transient ref without reviving an evicted page.  Returns false
 * when the page is already evicted (its release is in flight), which is the
 * state a lockless vfs_cache_lookup() can observe after losing the race to
 * vfs_cache_invalidate().  Mirrors vfs_node_tryref(). */
static inline bool vfs_page_tryref(vfs_page_t *page) {
  uint32_t refs = __atomic_load_n(&page->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (refs & VFS_PAGE_EVICTED)
      return false;
    if (__atomic_compare_exchange_n(&page->refs, &refs, refs + 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return true;
  }
}

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
  struct vfs_node *view_backing; /* Namespace path view; callbacks use the backing inode. */
  struct vfs_node *view_parent; /* Owned path ancestry, not the backing inode parent. */
  uint64_t mount_id; // Stable VFS mount identity, inherited during lookup
  uint64_t filesystem_mount_flags; // Immutable backing RO/NOSUID/NODEV/NOEXEC policy
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
  /* Optional append operation. Returns the actual end position on success;
   * disk backends select persistent EOF under their transaction ownership. */
  uint32_t (*append)(struct vfs_node *, uint32_t, uint8_t *, uint32_t *);
  open_type_t open;
  close_type_t close;
  close_type_t destroy; // Release device payload once a retired node is final
  open_instance_type_t open_instance; // Optional per-open node factory
  readdir_type_t readdir;
  finddir_type_t finddir;
  create_type_t create;
  mkdir_type_t mkdir;
  mknod_type_t mknod;
  unlink_type_t unlink;
  rmdir_type_t rmdir;
  readlink_type_t readlink;
  struct vfs_node *(*get_link)(struct vfs_node *); /* Owned proc magic-link target. */
  symlink_type_t symlink;
  rename_type_t rename;
  chmod_type_t chmod;
  chown_type_t chown;
  truncate_type_t truncate;
  mmap_type_t mmap; // Device-specific mmap handler
  /* A direct backing-buffer handler cannot supply private CoW ownership.
   * Route private mappings through the regular file page-cache fault path. */
  bool mmap_shared_only;
  /* Direct private mappings take one PMM reference per PTE, including CoW
   * copies. munmap must release these just like generic file-cache views. */
  bool mmap_private_pmm_refs;
  /* Direct shared mappings also take one PMM reference per PTE.  The backing
   * storage can be released or moved while a mapping is still live (tmpfs
   * truncate, memfd growth realloc), so a mapping without its own reference
   * would keep writable PTEs to a frame the PMM has already re-issued. */
  bool mmap_shared_pmm_refs;
  poll_type_t poll; // Device-specific poll handler
  statfs_type_t statfs;
  uint32_t (*get_nlink)(struct vfs_node *); // Optional dynamic link count
  ioctl_type_t ioctl; // Device-specific ioctl handler
  fallocate_type_t fallocate;
  void *wait_queue; // Pointer to wait_queue_t for poll() wakeups
  /* In-memory extended-attribute list head (fs/xattr.c, opaque
   * xattr_entry_t *). NULL when the node carries no xattrs. vfs_node_init()
   * zeroes it; vfs_close()/vfs_node_retire() release it. */
  void *xattrs;

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

  /* Direct Radix Level-1 Leaf Cache for Fast Page Cache Lookup */
  uint64_t pc_cache_chunk; // Page index >> 6 (identifies 256KB chunk)
  void *pc_cache_leaf;     // Cached level-1 radix node pointer
  uint32_t pc_cache_seq;   // Tree mutation seq when cached

  /* Adaptive on-demand readahead tracking */
  uint32_t ra_prev_offset;
  uint32_t ra_window;
  uint32_t ra_seq_hits;

  /* Optional sequential readdir cursor used by filesystems with indexed API. */
  uint32_t readdir_cursor_index;
  uint32_t readdir_cursor_offset;
  spinlock_t readdir_cursor_lock;

  /* Deferred-reclamation link (fs/vfs_rcu.c).  Valid only while the node sits
   * on the free queue after its last reference dropped; overwritten by the
   * next queueing and never read once the node is gone.  Keeping the link in
   * the node avoids an allocation on the last-close path, which must not fail.
   */
  struct vfs_rcu_head rcu;
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
uint32_t vfs_append(vfs_node_t *node, uint32_t size, uint8_t *buffer,
                    uint32_t *end_offset);
void vfs_open(vfs_node_t *node);
void vfs_close(vfs_node_t *node);

/* The filesystem unlinked `node`.  When nothing references it anymore its
 * ->destroy callback releases the device payload and the node is freed;
 * otherwise the last vfs_close() does both.  Backends that kfree() the node
 * outright in unlink/rmdir must call this instead. */
void vfs_node_retire(vfs_node_t *node);

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
struct dirent *vfs_readdir_buffer(void);
vfs_node_t *vfs_finddir(vfs_node_t *node, char *name);
void vfs_dentry_invalidate(vfs_node_t *parent, const char *name);
// For successful create/mkdir/mknod/symlink of a previously free name: cheap
// and provably sufficient, see vfs.c.
void vfs_dentry_invalidate_created(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path);
vfs_node_t *vfs_resolve_path(const char *path);
void vfs_path_cache_drop(vfs_node_t *dir, const char *path);
int vfs_create(vfs_node_t *node, char *name, uint16_t permission);
int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission);
int vfs_unlink(vfs_node_t *node, char *name);
int vfs_rmdir(vfs_node_t *node, char *name);
int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size);
int vfs_symlink(vfs_node_t *node, char *name, char *target);
int vfs_rename(vfs_node_t *node, char *old_name, char *new_name);
/* Cross-directory rename of a leaf node within one filesystem instance
 * (tmpfs and ext2/ext4 regular files/symlinks). Returns 0 or a
 * negative errno. */
int vfs_rename_cross(vfs_node_t *old_parent, char *old_name,
                     vfs_node_t *new_parent, char *new_name);
int vfs_chmod(vfs_node_t *node, uint16_t permission);
int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid);
int vfs_truncate(vfs_node_t *node, uint32_t size);
int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len);
int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device);
int vfs_poll(vfs_node_t *node, int events);
void vfs_node_init(vfs_node_t *node);
int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target);
/* `path` is the mountpoint as userspace sees it ("/run", "/tmp", ...).  It is
 * what /proc/mounts reports and what mountinfo(1)-style consumers match on;
 * pass NULL to fall back to the mountpoint node's own name. */
int vfs_mount_ex(vfs_node_t *mountpoint, vfs_node_t *target,
                 const char *dev_name, const char *fs_type,
                 const char *path);
int vfs_initial_unmount(vfs_node_t *root, unsigned flags);
int vfs_statfs(vfs_node_t *node, void *buf);

/* In-memory extended attributes (fs/xattr.c). Generic per-node store backing
 * setxattr/getxattr/listxattr/removexattr so cp -a, tar, and Baloo don't spin
 * on EOPNOTSUPP. Only the user.* namespace is stored; anything else reports
 * EOPNOTSUPP. Returns 0 on success or a negative errno (-61 ENODATA, -17
 * EEXIST, -34 ERANGE, -7 E2BIG, -12 ENOMEM, -95 EOPNOTSUPP). get/list follow
 * Linux sizing semantics: a NULL/zero-size buffer returns the required size.
 * NOTE: ext2/ext4/fat32 hand out ephemeral nodes per lookup (dentry-cached),
 * so attributes there live for the cache lifetime, not across reboot. tmpfs
 * and ramfs nodes are persistent and fully correct. On-disk xattr blocks are
 * future work. */
int vfs_setxattr(vfs_node_t *node, const char *name, const void *value,
                 uint32_t size, int flags);
int vfs_getxattr(vfs_node_t *node, const char *name, void *value,
                 uint32_t size);
int vfs_listxattr(vfs_node_t *node, char *list, uint32_t size);
int vfs_removexattr(vfs_node_t *node, const char *name);
/* Release every xattr on `node` (NULL-safe). Called from vfs_close() and
 * vfs_node_retire() before the node is freed. */
void vfs_xattr_clear(vfs_node_t *node);

typedef struct vfs_mount_info {
  uint64_t mount_id;
  uint64_t parent_id;
  uint64_t flags;
  char mountpoint[128];
  char target[128];
  char dev_name[64];
  char fs_type[32];
} vfs_mount_info_t;

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count);

// Page Cache API
bool vfs_cache_page_valid(const vfs_page_t *page);
/* Install the deferred-node-reclamation hook on a page cache tree.  Called
 * once per node from vfs_node_init(), before the tree is shared. */
void vfs_page_cache_tree_init(struct radix_tree *tree);
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
// OOM hook for mm (see heap.c heap_oom_reclaim): reclaim up to `target`
// clean unmapped pages from the current process's mapped files.
size_t mm_file_cache_reclaim(size_t target);
size_t vfs_dentry_reclaim_pages(size_t target);
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

/* Boot-time checks and benchmarks, gated on the kernel command line through
 * these entry points (`vfs_selftest=1` / `vfs_bench=1`).  They run once from
 * kmain after the root filesystem is mounted. */
void vfs_selftest_maybe_run(void);
/* Boot-time fill-integrity check, runs unconditionally once (see page_cache.c
 * for why it earned that). */
void vfs_cache_fill_selftest(void);
/* Second half of `vfs_selftest=1`: proves that no /proc resolution is
 * entered into the full-path cache.  kmain() calls it after procfs_init(),
 * because it needs the mounted /proc. */
void vfs_procfs_selftest(void);
void vfs_bench_maybe_run(void);
bool vfs_cache_ref_selftest(void);
/* `vfs_selftest=1`: the lockless page lookup and its deferred reclamation,
 * including the eviction race the tryref protocol exists for. */
bool vfs_cache_lockless_selftest(void);
void vfs_cache_bench(void);

#endif
