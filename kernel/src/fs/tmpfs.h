#ifndef FS_TMPFS_H
#define FS_TMPFS_H

#include "vfs.h"
#include "dirindex.h"
#include "../lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

#define TMPFS_DEFAULT_MAX_BYTES  (64ULL * 1024 * 1024)
#define TMPFS_DEFAULT_MAX_INODES 65536
/* /dev/shm gets a larger budget than the default tmpfs: an MIT-SHM segment is
 * sized per screen, and Mesa/Qt shared buffers are per window or per texture. */
#define TMPFS_SHM_MAX_BYTES  (512ULL * 1024 * 1024)
#define TMPFS_SHM_MAX_INODES 65536
#define TMPFS_MAGIC              0x01021994

typedef struct tmpfs_page {
    uint32_t         page_index;
    uint64_t         phys;
} tmpfs_page_t;

typedef struct tmpfs_sb tmpfs_sb_t;

typedef struct {
    struct radix_tree pages;      /* tmpfs_page_t keyed by page index */
    uint32_t      num_pages;
    spinlock_t    lock;           /* serializes page-tree mutation and reads */
    tmpfs_sb_t   *sb;
} tmpfs_file_t;

/* Children live in the shared directory index (dirindex.h): order list for
 * readdir, name hash for O(1) lookup.  tmpfs_dir_t keeps the index FIRST so
 * node->device can be read as a vfs_dirindex_t by the shared finddir
 * callback, with the superblock pointer after it. */
typedef vfs_child_t tmpfs_child_t;

typedef struct {
    vfs_dirindex_t di;
    tmpfs_sb_t    *sb;
} tmpfs_dir_t;

typedef struct {
    char        target[512];
    tmpfs_sb_t *sb;
} tmpfs_symlink_t;

typedef struct tmpfs_sb {
    uint64_t   max_bytes;
    uint64_t   max_inodes;
    uint64_t   used_bytes;
    uint64_t   used_inodes;
    spinlock_t lock;
} tmpfs_sb_t;

void        tmpfs_mount_at(const char *path);
void        tmpfs_mount_at_sized(const char *path, uint64_t max_bytes, uint64_t max_inodes);
vfs_node_t *tmpfs_create_root(tmpfs_sb_t *sb);

#endif
