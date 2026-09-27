#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

vfs_node_t *fs_root = 0;

bool vfs_in_group(uint32_t gid) {
  struct thread *t = sched_get_current();
  if (!t) return gid == 0;
  if (t->fsgid == gid || t->egid == gid || t->gid == gid) return true;
  for (uint32_t i = 0; i < t->supplementary_group_count; i++)
    if (t->supplementary_groups[i] == gid) return true;
  return false;
}

bool vfs_access(vfs_node_t *node, uint32_t requested) {
  if (!node) return false;
  struct thread *t = sched_get_current();
  if (!t || t->euid == 0) {
    if ((requested & 1) && !(node->mask & 0111) &&
        (node->flags & FS_TYPE_MASK) != FS_DIRECTORY) return false;
    return true;
  }
  uint32_t bits = node->mask & 7;
  if (t->fsuid == node->uid) bits = (node->mask >> 6) & 7;
  else if (vfs_in_group(node->gid)) bits = (node->mask >> 3) & 7;
  return (bits & requested) == requested;
}

bool vfs_may_remove(vfs_node_t *parent, vfs_node_t *target) {
  struct thread *t = sched_get_current();
  if (!t || t->euid == 0) return true;
  if (!vfs_access(parent, 3)) return false;
  if ((parent->mask & 01000) && t->fsuid != parent->uid &&
      (!target || t->fsuid != target->uid)) return false;
  return true;
}

typedef struct vfs_mount_entry {
  vfs_node_t *mountpoint;
  vfs_node_t *target;
  char dev_name[64];
  char fs_type[32];
  struct vfs_mount_entry *next;
} vfs_mount_entry_t;

static vfs_mount_entry_t *vfs_mount_list = NULL;

/* Bounded 4-way dentry cache. Positive entries own a child reference;
 * positive and negative entries both own a parent reference. */
#define VFS_DENTRY_CACHE_SIZE 16384
#define VFS_DENTRY_WAYS 4
#define VFS_DENTRY_BUCKETS (VFS_DENTRY_CACHE_SIZE / VFS_DENTRY_WAYS)

typedef struct vfs_dentry_cache_entry {
  vfs_node_t *parent;
  vfs_node_t *child;
  uint32_t hash;
  uint16_t name_len;
  bool valid;
  char name[128];
} vfs_dentry_cache_entry_t;

typedef struct vfs_dentry_cache_bucket {
  vfs_dentry_cache_entry_t entries[VFS_DENTRY_WAYS];
  uint8_t next_victim;
  spinlock_t lock;
} vfs_dentry_cache_bucket_t;

static vfs_dentry_cache_bucket_t vfs_dentry_cache[VFS_DENTRY_BUCKETS];

static uint32_t vfs_dentry_hash(vfs_node_t *parent, const char *name) {
  uint64_t hash = ((uint64_t)(uintptr_t)parent >> 4) ^ 1469598103934665603ULL;
  while (*name) {
    hash ^= (uint8_t)*name++;
    hash *= 1099511628211ULL;
  }
  return (uint32_t)(hash ^ (hash >> 32));
}

static vfs_node_t *vfs_dentry_lookup(vfs_node_t *parent, const char *name,
                                     bool *negative) {
  *negative = false;
  if (!parent || !name || (parent->flags & FS_DENTRY_NOCACHE))
    return NULL;

  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->name_len == len &&
        entry->parent == parent && memcmp(entry->name, name, len) == 0) {
      vfs_node_t *child = entry->child;
      if (child)
        vfs_node_ref(child);
      else
        *negative = true;
      spinlock_release(&bucket->lock);
      return child;
    }
  }
  spinlock_release(&bucket->lock);
  return NULL;
}

static void vfs_dentry_insert(vfs_node_t *parent, const char *name,
                              vfs_node_t *child) {
  if (!parent || !name)
    return;

  size_t len_sz = strlen(name);
  if (len_sz >= 128 || (parent->flags & FS_DENTRY_NOCACHE))
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
  vfs_node_t *evicted = NULL;
  vfs_node_t *evicted_parent = NULL;

  spinlock_acquire(&bucket->lock);
  vfs_dentry_cache_entry_t *entry = NULL;
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *candidate = &bucket->entries[i];
    if (candidate->valid && candidate->hash == hash && candidate->name_len == len &&
        candidate->parent == parent && memcmp(candidate->name, name, len) == 0) {
      spinlock_release(&bucket->lock);
      return;
    }
    if (!candidate->valid && !entry)
      entry = candidate;
  }
  if (!entry) {
    entry = &bucket->entries[bucket->next_victim];
    bucket->next_victim = (bucket->next_victim + 1) % VFS_DENTRY_WAYS;
  }
  if (entry->valid) {
    evicted = entry->child;
    evicted_parent = entry->parent;
  }
  vfs_node_ref(parent);
  if (child)
    vfs_node_ref(child);
  entry->parent = parent;
  entry->child = child;
  entry->hash = hash;
  entry->name_len = len;
  memcpy(entry->name, name, len);
  entry->name[len] = '\0';
  entry->valid = true;
  spinlock_release(&bucket->lock);

  if (evicted)
    vfs_close(evicted);
  if (evicted_parent)
    vfs_close(evicted_parent);
}

/* Fast-Path Full Path Resolution Cache (Path Dcache)
 * Maps (base_dir, path_string) -> resolved_vfs_node.
 * Directly bypasses string parsing, loops, and per-component lookup overhead
 * for high-frequency access to binaries, libs, JVM classes, and assets.
 */
#define VFS_PATH_CACHE_SIZE 4096
#define VFS_PATH_CACHE_WAYS 4
#define VFS_PATH_CACHE_BUCKETS (VFS_PATH_CACHE_SIZE / VFS_PATH_CACHE_WAYS)

typedef struct vfs_path_cache_entry {
  vfs_node_t *dir;
  vfs_node_t *node;
  uint32_t hash;
  uint16_t path_len;
  bool valid;
  char path[256];
} vfs_path_cache_entry_t;

typedef struct vfs_path_cache_bucket {
  vfs_path_cache_entry_t entries[VFS_PATH_CACHE_WAYS];
  uint8_t next_victim;
  spinlock_t lock;
} vfs_path_cache_bucket_t;

static vfs_path_cache_bucket_t vfs_path_cache[VFS_PATH_CACHE_BUCKETS];

static uint32_t vfs_path_hash(vfs_node_t *dir, const char *path) {
  uint64_t hash = ((uint64_t)(uintptr_t)dir >> 4) ^ 0xcbf29ce484222325ULL;
  while (*path) {
    hash ^= (uint8_t)*path++;
    hash *= 0x100000001b3ULL;
  }
  return (uint32_t)(hash ^ (hash >> 32));
}

static vfs_node_t *vfs_path_cache_lookup(vfs_node_t *dir, const char *path) {
  if (!dir || !path)
    return NULL;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return NULL;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->path_len == len &&
        entry->dir == dir && memcmp(entry->path, path, len) == 0) {
      vfs_node_t *node = entry->node;
      vfs_open(node);
      spinlock_release(&bucket->lock);
      return node;
    }
  }
  spinlock_release(&bucket->lock);
  return NULL;
}

static void vfs_path_cache_insert(vfs_node_t *dir, const char *path, vfs_node_t *node) {
  if (!dir || !node || !path || (node->flags & FS_DENTRY_NOCACHE))
    return;

  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
  vfs_node_t *evicted_node = NULL;
  vfs_node_t *evicted_dir = NULL;

  spinlock_acquire(&bucket->lock);
  vfs_path_cache_entry_t *entry = NULL;
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *candidate = &bucket->entries[i];
    if (candidate->valid && candidate->hash == hash && candidate->path_len == len &&
        candidate->dir == dir && memcmp(candidate->path, path, len) == 0) {
      spinlock_release(&bucket->lock);
      return;
    }
    if (!candidate->valid && !entry)
      entry = candidate;
  }
  if (!entry) {
    entry = &bucket->entries[bucket->next_victim];
    bucket->next_victim = (bucket->next_victim + 1) % VFS_PATH_CACHE_WAYS;
  }
  if (entry->valid) {
    evicted_node = entry->node;
    evicted_dir = entry->dir;
  }

  vfs_node_ref(dir);
  vfs_node_ref(node);
  entry->dir = dir;
  entry->node = node;
  entry->hash = hash;
  entry->path_len = len;
  memcpy(entry->path, path, len);
  entry->path[len] = '\0';
  entry->valid = true;
  spinlock_release(&bucket->lock);

  if (evicted_node)
    vfs_close(evicted_node);
  if (evicted_dir)
    vfs_close(evicted_dir);
}

/*
 * Invalidate the single dentry bucket that can hold (parent, name).
 *
 * vfs_dentry_insert() hashes on (parent, name) and only ever replaces entries
 * inside that one bucket, so an entry for a given pair can never live anywhere
 * else.  Scanning all VFS_DENTRY_BUCKETS for it is therefore pure waste, and it
 * is waste paid on every create/unlink/rename - thousands of strcmp() calls
 * against 128-byte name buffers with IRQs masked per bucket.
 */
static void vfs_dentry_bucket_invalidate(vfs_node_t *parent, const char *name) {
  size_t len_sz = strlen(name);
  if (len_sz >= 128)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_dentry_hash(parent, name);
  uint32_t slot = hash % VFS_DENTRY_BUCKETS;
  vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[slot];
  vfs_node_t *released[VFS_DENTRY_WAYS];
  vfs_node_t *released_parents[VFS_DENTRY_WAYS];
  uint32_t released_count = 0;

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
    vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->name_len == len &&
        entry->parent == parent && memcmp(entry->name, name, len) == 0) {
      released[released_count] = entry->child;
      released_parents[released_count++] = entry->parent;
      entry->valid = false;
      entry->parent = NULL;
      entry->child = NULL;
      entry->hash = 0;
      entry->name_len = 0;
      entry->name[0] = '\0';
    }
  }
  spinlock_release(&bucket->lock);

  for (uint32_t i = 0; i < released_count; i++) {
    if (released[i])
      vfs_close(released[i]);
    vfs_close(released_parents[i]);
  }
}

/* Drop the exact (dir, path) fast-path resolution, if present. */
static void vfs_path_cache_drop(vfs_node_t *dir, const char *path) {
  size_t len_sz = strlen(path);
  if (len_sz >= 256)
    return;

  uint16_t len = (uint16_t)len_sz;
  uint32_t hash = vfs_path_hash(dir, path);
  uint32_t slot = hash % VFS_PATH_CACHE_BUCKETS;
  vfs_path_cache_bucket_t *bucket = &vfs_path_cache[slot];
  vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
  vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
  uint32_t count = 0;

  spinlock_acquire(&bucket->lock);
  for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
    vfs_path_cache_entry_t *entry = &bucket->entries[i];
    if (entry->valid && entry->hash == hash && entry->path_len == len &&
        entry->dir == dir && memcmp(entry->path, path, len) == 0) {
      rel_node[count] = entry->node;
      rel_dir[count++] = entry->dir;
      entry->valid = false;
      entry->dir = NULL;
      entry->node = NULL;
      entry->hash = 0;
      entry->path_len = 0;
      entry->path[0] = '\0';
    }
  }
  spinlock_release(&bucket->lock);

  for (uint32_t i = 0; i < count; i++) {
    vfs_close(rel_node[i]);
    vfs_close(rel_dir[i]);
  }
}

void vfs_path_cache_invalidate(void) {
  for (uint32_t b = 0; b < VFS_PATH_CACHE_BUCKETS; b++) {
    vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
    vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
    uint32_t count = 0;
    vfs_path_cache_bucket_t *bucket = &vfs_path_cache[b];

    spinlock_acquire(&bucket->lock);
    for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
      vfs_path_cache_entry_t *entry = &bucket->entries[i];
      if (entry->valid) {
        rel_node[count] = entry->node;
        rel_dir[count++] = entry->dir;
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
      }
    }
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < count; i++) {
      vfs_close(rel_node[i]);
      vfs_close(rel_dir[i]);
    }
  }
}

/*
 * Selectively invalidate path cache entries containing 'name' as a distinct
 * path component or anchored directly at parent.
 *
 * This preserves unrelated cached paths (e.g. dynamic linkers, shared libraries,
 * and system utilities) when a temporary file or specific directory entry is removed.
 */
static void vfs_path_cache_invalidate_name(vfs_node_t *parent, const char *name) {
  if (!name || name[0] == '\0')
    return;

  size_t nlen = strlen(name);
  if (nlen >= 256)
    return;

  for (uint32_t b = 0; b < VFS_PATH_CACHE_BUCKETS; b++) {
    vfs_node_t *rel_node[VFS_PATH_CACHE_WAYS];
    vfs_node_t *rel_dir[VFS_PATH_CACHE_WAYS];
    uint32_t count = 0;
    vfs_path_cache_bucket_t *bucket = &vfs_path_cache[b];

    spinlock_acquire(&bucket->lock);
    for (uint32_t i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
      vfs_path_cache_entry_t *entry = &bucket->entries[i];
      if (!entry->valid)
        continue;

      bool match = false;
      if (entry->dir == parent && entry->path_len == nlen &&
          memcmp(entry->path, name, nlen) == 0) {
        match = true;
      } else {
        /* Check if 'name' appears as a distinct path component in entry->path:
         * e.g. "name", "name/...", ".../name", or ".../name/..." */
        const char *p = strstr(entry->path, name);
        while (p) {
          bool left_bound = (p == entry->path || *(p - 1) == '/');
          bool right_bound = (p[nlen] == '\0' || p[nlen] == '/');
          if (left_bound && right_bound) {
            match = true;
            break;
          }
          p = strstr(p + 1, name);
        }
      }

      if (match) {
        rel_node[count] = entry->node;
        rel_dir[count++] = entry->dir;
        entry->valid = false;
        entry->dir = NULL;
        entry->node = NULL;
        entry->hash = 0;
        entry->path_len = 0;
        entry->path[0] = '\0';
      }
    }
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < count; i++) {
      vfs_close(rel_node[i]);
      vfs_close(rel_dir[i]);
    }
  }
}

void vfs_dentry_invalidate(vfs_node_t *parent, const char *name) {
  /* With a concrete parent and name, invalidate only entries referencing this
   * name in the path cache, and only the single bucket holding (parent, name)
   * in the dentry cache. A NULL argument means "everything", which genuinely
   * does need the full table scan (e.g. unmounts). */
  if (parent && name) {
    vfs_path_cache_invalidate_name(parent, name);
    vfs_dentry_bucket_invalidate(parent, name);
    return;
  }

  vfs_path_cache_invalidate();

  for (uint32_t b = 0; b < VFS_DENTRY_BUCKETS; b++) {
    vfs_node_t *released[VFS_DENTRY_WAYS];
    vfs_node_t *released_parents[VFS_DENTRY_WAYS];
    uint32_t released_count = 0;
    vfs_dentry_cache_bucket_t *bucket = &vfs_dentry_cache[b];

    spinlock_acquire(&bucket->lock);
    for (uint32_t i = 0; i < VFS_DENTRY_WAYS; i++) {
      vfs_dentry_cache_entry_t *entry = &bucket->entries[i];
      if (entry->valid && (!parent || entry->parent == parent) &&
          (!name || strcmp(entry->name, name) == 0)) {
        released[released_count] = entry->child;
        released_parents[released_count++] = entry->parent;
        entry->valid = false;
        entry->parent = NULL;
        entry->child = NULL;
        entry->hash = 0;
        entry->name_len = 0;
        entry->name[0] = '\0';
      }
    }
    spinlock_release(&bucket->lock);

    for (uint32_t i = 0; i < released_count; i++) {
      if (released[i])
        vfs_close(released[i]);
      vfs_close(released_parents[i]);
    }
  }
}


/*
 * Invalidate after a name was *created* (create/mkdir/mknod/symlink returned
 * success).
 *
 * Every filesystem in this kernel rejects these operations when the name is
 * already taken, so a successful create cannot turn an existing successful
 * resolution stale: the path cache stores only successful resolutions, and a
 * cached resolution of this very name would have made the create fail.  What
 * does have to go is the negative dentry recorded for (parent, name) by the
 * lookup that proved the name was free - otherwise the new file stays invisible
 * until that entry is evicted.
 *
 * This matters because creating a file used to throw away the entire path
 * cache and re-scan all 4096 dentry buckets, which erased the "fast open" win
 * for every binary and shared library in the session.  Removals (unlink/rmdir/
 * rename) keep the conservative full invalidation: a removed name really can
 * invalidate resolutions anchored at any of its ancestor directories.
 */
void vfs_dentry_invalidate_created(vfs_node_t *parent, const char *name) {
  if (!parent || !name) {
    vfs_dentry_invalidate(parent, name);
    return;
  }

  vfs_dentry_bucket_invalidate(parent, name);
  vfs_path_cache_drop(parent, name);
}


uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (!node)
    return 0;

  if ((node->flags & (FS_TYPE_MASK | FS_PAGE_CACHE)) ==
      (FS_FILE | FS_PAGE_CACHE))
    return vfs_cache_read(node, offset, size, buffer);

  if (node->read) {
    return node->read(node, offset, size, buffer);
  }
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  if (!node)
    return 0;

  struct thread *t = sched_get_current();
  if (size && t && t->euid != 0 && (node->mask & 06000))
    vfs_chmod(node, (uint16_t)(node->mask & ~06000));

  if (node->write) {
    uint32_t written = node->write(node, offset, size, buffer);
    if (written && (node->flags & FS_PAGE_CACHE))
      vfs_cache_update_or_invalidate(node, offset, written, buffer);
    return written;
  }
  return 0;
}

void vfs_open(vfs_node_t *node) {
  if (!node)
    return;
  vfs_node_ref(node);
  if (node->open) {
    node->open(node);
  }
}

void vfs_close(vfs_node_t *node) {
  if (!node)
    return;

  bool is_pipe = (node->flags & FS_TYPE_MASK) == FS_PIPE;
  void *wq = node->wait_queue;

  if (vfs_node_unref(node)) {
    if (node->close) {
      node->close(node);
    }
    vfs_cache_sync(node);
    vfs_cache_clear(node);
    node->ep_watchers.next = NULL;
    node->ep_watchers.prev = NULL;
    if (!(node->flags & FS_PERSISTENT)) {
      node->magic = 0;
      kfree(node);
    }
    return;
  }

  // For pipes, wake blocked readers/writers AFTER decrementing refcount,
  // so they re-check and detect EOF (refcount <= 1).
  // Only do this when refcount > 0 (node is still alive).
  if (is_pipe && wq) {
    extern void wait_queue_wake_all(void *wq);
    wait_queue_wake_all(wq);
  }
}

struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->readdir) {
    return node->readdir(node, index);
  }
  return 0;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->finddir) {
    bool negative;
    vfs_node_t *cached = vfs_dentry_lookup(node, name, &negative);
    if (cached || negative)
      return cached;

    vfs_node_t *res = node->finddir(node, name);
    if (!res) {
      vfs_dentry_insert(node, name, NULL);
      return 0;
    }

    vfs_mount_entry_t *curr = vfs_mount_list;
    while (curr) {
      if (curr->mountpoint && curr->mountpoint->inode == res->inode &&
          curr->mountpoint->device == res->device) {
        if (curr->target != res) {
          vfs_close(res);
          vfs_open(curr->target);
          vfs_dentry_insert(node, name, curr->target);
          return curr->target;
        }
      }
      curr = curr->next;
    }
    vfs_dentry_insert(node, name, res);
    return res;
  }
  return 0;
}

int vfs_create(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->create) {
    int result = node->create(node, name, permission);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mkdir) {
    int result = node->mkdir(node, name, permission);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_unlink(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->unlink) {
    vfs_dentry_invalidate(node, name);
    return node->unlink(node, name);
  }
  return -1;
}

int vfs_rmdir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rmdir) {
    vfs_dentry_invalidate(node, name);
    return node->rmdir(node, name);
  }
  return -1;
}

int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (node && node->readlink) {
    return node->readlink(node, buf, size);
  }
  return -1;
}

int vfs_symlink(vfs_node_t *node, char *name, char *target) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->symlink) {
    int result = node->symlink(node, name, target);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rename) {
    vfs_dentry_invalidate(node, old_name);
    vfs_dentry_invalidate(node, new_name);
    return node->rename(node, old_name, new_name);
  }
  return -1;
}

int vfs_chmod(vfs_node_t *node, uint16_t permission) {
  if (node && node->chmod) {
    return node->chmod(node, permission);
  }
  return -1;
}

int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  if (node && node->chown) {
    return node->chown(node, uid, gid);
  }
  return -1;
}

int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mknod) {
    int result = node->mknod(node, name, permission, flags, device);
    if (result == 0)
      vfs_dentry_invalidate_created(node, name);
    return result;
  }
  return -1;
}

int vfs_truncate(vfs_node_t *node, uint32_t size) {
  if (node && node->truncate) {
    int result = node->truncate(node, size);
    if (!result && (node->flags & FS_PAGE_CACHE))
      vfs_cache_clear(node);
    return result;
  }
  return -1;
}

int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {
  if (node && node->fallocate) {
    return node->fallocate(node, mode, offset, len);
  }
  if (node && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    // Mode 0: standard allocation (posix_fallocate)
    if (mode == 0) {
      uint32_t required = offset + len;
      if (required > node->length) {
        if (node->truncate)
          return node->truncate(node, required);
        node->length = required;
      }
      return 0;
    }
  }
  return -1;
}

int vfs_poll(vfs_node_t *node, int events) {
  if (node && node->poll) {
    return node->poll(node, events);
  }
  uint32_t type = (node->flags & FS_TYPE_MASK);
  if (type == FS_FILE || type == FS_DIRECTORY) {
    return events & (POLLIN | POLLOUT);
  }
  return 0;
}

#define MAX_SYMLINK_DEPTH 8

vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path) {
  if (!path || !fs_root)
    return 0;

  vfs_node_t *effective_dir = (path[0] == '/') ? fs_root : (dir ? dir : fs_root);

  // 1. Fast Path Full-Path Dcache lookup:
  // Check if we already resolved this exact path string from this directory
  vfs_node_t *cached = vfs_path_cache_lookup(effective_dir, path);
  if (cached) {
    return cached;
  }

  char path_buf[512];
  strncpy(path_buf, path, 511);
  path_buf[511] = '\0';

  vfs_node_t *current = effective_dir;
  vfs_open(current); // Reference for 'current'

  int symlink_depth = 0;
  char *p = path_buf;

#define VFS_PARENT_STACK_DEPTH 32
  vfs_node_t *parent_stack[VFS_PARENT_STACK_DEPTH];
  int stack_top = -1; // Stack is empty initially

  if (path_buf[0] == '/') {
    while (*p == '/')
      p++;
  }

  while (*p) {
    char comp[128];
    int i = 0;

    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    if (strcmp(comp, "..") == 0) {
      if (stack_top >= 0) {
        vfs_close(current);
        current = parent_stack[stack_top];
        stack_top--;
        // Ownership transferred from stack to 'current'
      }
      continue;
    }

    if (strcmp(comp, ".") == 0)
      continue;

    if (!vfs_access(current, 1)) goto fail;

    vfs_node_t *next = vfs_finddir(current, comp);
    if (!next) {
      for (int j = 0; j <= stack_top; j++) {
        vfs_close(parent_stack[j]);
      }
      vfs_close(current);
      return 0;
    }

    if ((next->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      if (++symlink_depth > MAX_SYMLINK_DEPTH) {
        vfs_close(next);
        goto fail;
      }

      char link_target[512];
      int len = vfs_readlink(next, link_target, 511);
      vfs_close(next);

      if (len < 0)
        goto fail;
      link_target[len] = '\0';

      char next_path[512];
      strcpy(next_path, link_target);

      if (*p) {
        int cur_len = (int)strlen(next_path);
        if (cur_len < 510) {
          bool target_ends_in_slash =
              (cur_len > 0 && next_path[cur_len - 1] == '/');
          if (!target_ends_in_slash && *p != '/')
            strcat(next_path, "/");
          else if (target_ends_in_slash && *p == '/')
            p++;
          strncat(next_path, p, 511 - strlen(next_path));
        }
      }
      next_path[511] = '\0';
      strcpy(path_buf, next_path);
      p = path_buf;

      if (path_buf[0] == '/') {
        vfs_close(current);
        for (int j = 0; j <= stack_top; j++)
          vfs_close(parent_stack[j]);
        stack_top = -1;
        current = fs_root;
        vfs_open(current);
        while (*p == '/')
          p++;
      }
      continue;
    }

    // Descent
    if (stack_top < VFS_PARENT_STACK_DEPTH - 1) {
      stack_top++;
      parent_stack[stack_top] = current;
      current = next;
      // 'current' reference transferred to stack, 'next' becomes new 'current'
    } else {
      // Stack overflow - just swap current and lose history
      vfs_close(current);
      current = next;
    }
  }

  // Cleanup stack
  for (int j = 0; j <= stack_top; j++) {
    vfs_close(parent_stack[j]);
  }

  // Cache successfully resolved path
  if (current && strstr(path, "..") == NULL) {
    vfs_path_cache_insert(effective_dir, path, current);
  }

  return current;


fail:
  for (int j = 0; j <= stack_top; j++)
    vfs_close(parent_stack[j]);
  vfs_close(current);
  return 0;
}

vfs_node_t *vfs_resolve_path(const char *path) {
  return vfs_resolve_path_at(fs_root, path);
}

void vfs_node_init(vfs_node_t *node) {
  if (!node)
    return;
  memset(node, 0, sizeof(vfs_node_t));
  INIT_LIST_HEAD(&node->ep_watchers);
  spinlock_init(&node->ep_lock);
  radix_tree_init(&node->pages);
  spinlock_init(&node->pages_lock);
  spinlock_init(&node->readdir_cursor_lock);
  node->refcount = 1;
  node->magic = VFS_NODE_MAGIC;
}

bool vfs_node_is_alive(const vfs_node_t *node) {
  if (!node || !pmm_kernel_ptr_is_managed(node))
    return false;
  return node->magic == VFS_NODE_MAGIC;
}

int vfs_mount_ex(vfs_node_t *mountpoint, vfs_node_t *target,
                 const char *dev_name, const char *fs_type) {
  if (!target)
    return -1;

  vfs_mount_entry_t *entry = kmalloc(sizeof(vfs_mount_entry_t));
  if (!entry)
    return -1;
  entry->mountpoint = mountpoint;
  entry->target = target;
  strncpy(entry->dev_name, dev_name ? dev_name : "none", 63);
  strncpy(entry->fs_type, fs_type ? fs_type : "unknown", 31);
  entry->next = vfs_mount_list;
  vfs_mount_list = entry;

  /* A prior lookup may have cached the node hidden by this mount. */
  vfs_dentry_invalidate(NULL, NULL);

  target->flags |= FS_PERSISTENT;

  if (mountpoint) {
    mountpoint->flags |= FS_MOUNTPOINT | FS_PERSISTENT;
    mountpoint->ptr = target;
  }

  return 0;
}

int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target) {
  return vfs_mount_ex(mountpoint, target, "none", "unknown");
}

int vfs_statfs(vfs_node_t *node, void *buf) {
  if (node && node->statfs) {
    return node->statfs(node, buf);
  }
  return -1;
}

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count) {
  int count = 0;

  // Add root if it exists
  if (fs_root && max_count > 0) {
    // Try to find if root is in the mount list first
    bool found = false;
    vfs_mount_entry_t *c = vfs_mount_list;
    while (c) {
      if (!c->mountpoint ||
          (c->mountpoint && strcmp(c->mountpoint->name, "/") == 0)) {
        found = true;
        break;
      }
      c = c->next;
    }

    if (!found) {
      strcpy(buffer[count].mountpoint, "/");
      strcpy(buffer[count].target, "/");
      strcpy(buffer[count].dev_name, "none");
      strcpy(buffer[count].fs_type, "ext3"); // Common default for this OS
      count++;
    }
  }

  vfs_mount_entry_t *curr = vfs_mount_list;
  while (curr && count < max_count) {
    if (curr->mountpoint) {
      strncpy(buffer[count].mountpoint, curr->mountpoint->name, 127);
      buffer[count].mountpoint[127] = '\0';
    } else {
      strcpy(buffer[count].mountpoint, "/");
    }

    strncpy(buffer[count].target, curr->target->name, 127);
    buffer[count].target[127] = '\0';

    strncpy(buffer[count].dev_name, curr->dev_name, 63);
    buffer[count].dev_name[63] = '\0';

    strncpy(buffer[count].fs_type, curr->fs_type, 31);
    buffer[count].fs_type[31] = '\0';

    count++;
    curr = curr->next;
  }
  return count;
}
