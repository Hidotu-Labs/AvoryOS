#include "ext2_internal.h"
#include "fs/ext4/ext4.h"
#include "syscalls/syscall.h"

extern uint32_t      ext2_read_impl(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
extern uint32_t      ext2_write_impl(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
extern int           ext2_truncate_impl(vfs_node_t *, uint32_t);
extern uint64_t      ext2_mmap_impl(vfs_node_t *, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t);
extern struct dirent *ext2_readdir_impl(vfs_node_t *, uint64_t);
extern vfs_node_t   *ext2_finddir_impl(vfs_node_t *, char *);
extern int           ext2_create_impl(vfs_node_t *, char *, uint16_t);
extern int           ext2_mkdir_impl(vfs_node_t *, char *, uint16_t);
extern int           ext2_unlink_impl(vfs_node_t *, char *);
extern int           ext2_rmdir_impl(vfs_node_t *, char *);
extern int           ext2_readlink_impl(vfs_node_t *, char *, uint32_t);
extern int           ext2_symlink_impl(vfs_node_t *, char *, char *);
extern int           ext2_link_impl(vfs_node_t *, char *, vfs_node_t *);
extern int           ext2_rename_impl(vfs_node_t *, char *, char *);
extern int           ext2_chmod_impl(vfs_node_t *, uint16_t);
extern int           ext2_chown_impl(vfs_node_t *, uint32_t, uint32_t);
extern int           ext2_mknod_impl(vfs_node_t *, char *, uint16_t, uint32_t,
                                     void *);

/*
 * Live VFS nodes per inode.
 *
 * ext2 builds a fresh vfs_node on every lookup that misses the dentry cache,
 * so one inode can have several nodes at once: the one an open descriptor or
 * mapping holds, and the one vfs_unlink() resolved to retire.  Freeing an
 * unlinked inode when *a* node went away released its inode and blocks while
 * another node was still writing them; the inode was then reused (a Firefox
 * "lock" symlink received file data) and the blocks were later freed twice,
 * inflating the group free counters.  Only the last registered node may
 * release an unlinked inode.  Nodes are registered when handed to the VFS by
 * ext2_finddir_vfs(); ext2's own internal lookups are not counted.
 */
typedef struct ext2_live_inode {
  ext2_mount_t *mnt;
  uint32_t ino;
  uint32_t nodes;
  struct ext2_live_inode *next;
} ext2_live_inode_t;

#define EXT2_LIVE_BUCKETS 1024
static ext2_live_inode_t *ext2_live[EXT2_LIVE_BUCKETS];
static spinlock_t ext2_live_lock = SPINLOCK_INIT;

static inline uint32_t ext2_live_hash(ext2_mount_t *mnt, uint32_t ino) {
  return (uint32_t)(((uintptr_t)mnt >> 6) ^ (ino * 2654435761u)) &
         (EXT2_LIVE_BUCKETS - 1);
}

static bool ext2_live_get(ext2_mount_t *mnt, uint32_t ino) {
  ext2_live_inode_t *fresh = kmalloc(sizeof(*fresh)); // outside the lock
  uint32_t h = ext2_live_hash(mnt, ino);
  spinlock_acquire(&ext2_live_lock);
  ext2_live_inode_t *e = ext2_live[h];
  while (e && !(e->mnt == mnt && e->ino == ino))
    e = e->next;
  if (e) {
    e->nodes++;
  } else if (fresh) {
    fresh->mnt = mnt;
    fresh->ino = ino;
    fresh->nodes = 1;
    fresh->next = ext2_live[h];
    ext2_live[h] = fresh;
    e = fresh;
    fresh = NULL;
  }
  spinlock_release(&ext2_live_lock);
  kfree(fresh);
  return e != NULL;
}

/* Drop one registration; true when it was the inode's last live node. */
static bool ext2_live_put(ext2_mount_t *mnt, uint32_t ino) {
  uint32_t h = ext2_live_hash(mnt, ino);
  ext2_live_inode_t *gone = NULL;
  bool last = false;
  spinlock_acquire(&ext2_live_lock);
  for (ext2_live_inode_t **pp = &ext2_live[h]; *pp; pp = &(*pp)->next) {
    ext2_live_inode_t *e = *pp;
    if (e->mnt != mnt || e->ino != ino)
      continue;
    if (--e->nodes == 0) {
      *pp = e->next;
      gone = e;
      last = true;
    }
    break;
  }
  spinlock_release(&ext2_live_lock);
  kfree(gone);
  return last;
}

/* Free an inode whose last name is gone (links 0, dtime set). */
static void ext2_reclaim_unlinked_inode(ext2_mount_t *mnt, uint32_t ino) {
  if (mnt->read_only || ext3_journal_start(mnt) != 0)
    return;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, ino, &inode) == 0 && inode.i_links_count == 0 &&
      inode.i_dtime != 0 && (inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    uint16_t type = inode.i_mode & 0xF000;
    /* A fast symlink keeps its target text where block pointers live, and a
     * socket owns no blocks: never walk those as a block map. */
    bool fast_symlink = type == EXT2_S_IFLNK && inode.i_blocks == 0;
    if (type != EXT2_S_IFSOCK && !fast_symlink)
      ext2_free_all_blocks(mnt, &inode);
    ext2_write_inode(mnt, ino, &inode);
    ext2_free_inode(mnt, ino);
  }
  ext3_journal_stop(mnt);
}

/* close/destroy hook of a registered node.  vfs_close() calls close() and
 * then, for retired nodes, destroy(); clearing both makes this run once. */
static void ext2_node_release(vfs_node_t *node) {
  node->close = NULL;
  node->destroy = NULL;
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (mnt && ext2_live_put(mnt, node->inode))
    ext2_reclaim_unlinked_inode(mnt, node->inode);
}

static void ext2_node_release_close(vfs_node_t *node) { ext2_node_release(node); }

vfs_node_t *ext2_finddir_vfs(vfs_node_t *parent, char *name) {
  vfs_node_t *node = ext2_finddir_impl(parent, name);
  if (!node || (node->flags & FS_TYPE_MASK) == FS_DIRECTORY)
    return node;
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (mnt && ext2_live_get(mnt, node->inode)) {
    node->close = ext2_node_release_close;
    node->destroy = ext2_node_release;
  }
  /* Without a registration the node never frees its inode: an allocation
   * failure leaks an unlinked inode rather than risking a live one. */
  return node;
}

static uint32_t ext2_get_nlink_impl(vfs_node_t *node) {
  if (!node || !node->device || node->inode == 0)
    return 1;
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode) != 0)
    return 1;
  return inode.i_links_count;
}

vfs_node_t *ext2_make_vfs_node(ext2_mount_t *mnt, uint32_t inode_num,
                                ext2_inode_t *inode) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);

  node->inode  = inode_num;
  node->mask   = inode->i_mode & 0x0FFF;
  node->uid    = inode->i_uid;
  node->gid    = inode->i_gid;
  node->length = inode->i_size;
  node->device = mnt;
  node->atime  = inode->i_atime;
  node->mtime  = inode->i_mtime;
  node->ctime  = inode->i_ctime;

  uint16_t type = inode->i_mode & 0xF000;
  if (type == 0) {
    static uint32_t invalid_inode_logs;
    if (__atomic_add_fetch(&invalid_inode_logs, 1, __ATOMIC_RELAXED) <= 8)
      klogf("[EXT2-INODE] invalid type inode=%u mode=%x links=%u size=%u "
            "table=%llu bs=%u\n", inode_num, inode->i_mode,
            inode->i_links_count, inode->i_size,
            (unsigned long long)ext2_bgd_inode_table(
                &mnt->bgdt[(inode_num - 1) / mnt->inodes_per_group]),
            mnt->block_size);
    kfree(node);
    return NULL;
  }

  if (type == EXT2_S_IFDIR) {
    node->flags   = FS_DIRECTORY;
    node->readdir = ext2_readdir_impl;
    node->finddir = ext2_finddir_vfs;
    node->create  = (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)
                        ? ext4_create_impl : ext2_create_impl;
    node->mkdir   = (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)
                        ? ext4_mkdir_impl : ext2_mkdir_impl;
    node->unlink  = ext2_unlink_impl;
    node->rmdir   = ext2_rmdir_impl;
    node->symlink = ext2_symlink_impl;
    node->link    = ext2_link_impl;
    node->rename  = ext2_rename_impl;
    node->chmod   = ext2_chmod_impl;
    node->chown   = ext2_chown_impl;
    node->mknod   = ext2_mknod_impl;
  } else if (type == EXT2_S_IFREG) {
    node->flags    = FS_FILE | FS_PAGE_CACHE;
    if (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
      node->read  = ext4_read_impl;
      node->write = ext4_write_impl;
    } else {
      node->read  = ext2_read_impl;
      node->write = ext2_write_impl;
    }
    node->truncate = (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)
                         ? ext4_truncate_impl : ext2_truncate_impl;
    node->fallocate = ext2_fallocate_impl;
    node->append = ext2_append_impl;
    node->mmap     = ext2_mmap_impl;
    node->chmod    = ext2_chmod_impl;
    node->chown    = ext2_chown_impl;
  } else if (type == EXT2_S_IFLNK) {
    node->flags    = FS_SYMLINK;
    node->readlink = ext2_readlink_impl;
    node->chmod    = ext2_chmod_impl;
    node->chown    = ext2_chown_impl;
  } else if (type == EXT2_S_IFSOCK) {
    node->flags = FS_SOCKET;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
  } else if (type == EXT2_S_IFIFO) {
    /* node->device stays the mount; fifo_open() keys the pipe by inode. */
    node->flags = FS_PIPE | FS_FIFO_SHARED;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
  }

  if (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)
    node->statfs = ext4_statfs_impl;
  else {
    /* Pure ext2 files/dirs previously had no statfs handler, so
     * statfs()/fstatfs() on any non-root path fell back to a fake 2 GiB
     * value (or, for tools that don't accept the fallback magic, 0 B).
     * ext4_statfs_impl only reads the shared ext2 superblock fields, so it
     * is valid for plain ext2 mounts as well. */
    extern int ext4_statfs_impl(vfs_node_t *, struct statfs_buf *);
    node->statfs = ext4_statfs_impl;
  }

  node->get_nlink = ext2_get_nlink_impl;
  if (mnt->read_only)
    node->flags |= FS_READONLY;

  return node;
}
