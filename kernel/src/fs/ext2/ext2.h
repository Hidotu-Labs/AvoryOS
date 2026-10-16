#ifndef FS_EXT2_H
#define FS_EXT2_H

#include "drivers/storage/block.h"
#include "fs/vfs.h"
#include "lock/spinlock.h"
#include "sched/wait.h"
#include <stdbool.h>
#include <stdint.h>

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INODE 2
#define EXT2_DIRECT_BLOCKS 12
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_FEATURE_COMPAT_DIR_INDEX 0x0020
#define EXT2_INDEX_FL 0x00001000

#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFLNK 0xA000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFIFO 0x1000

#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_CHRDEV 3
#define EXT2_FT_BLKDEV 4
#define EXT2_FT_FIFO 5
#define EXT2_FT_SOCK 6
#define EXT2_FT_SYMLINK 7

typedef struct {
  uint32_t s_inodes_count;
  uint32_t s_blocks_count;
  uint32_t s_r_blocks_count;
  uint32_t s_free_blocks_count;
  uint32_t s_free_inodes_count;
  uint32_t s_first_data_block;
  uint32_t s_log_block_size;
  uint32_t s_log_frag_size;
  uint32_t s_blocks_per_group;
  uint32_t s_frags_per_group;
  uint32_t s_inodes_per_group;
  uint32_t s_mtime;
  uint32_t s_wtime;
  uint16_t s_mnt_count;
  uint16_t s_max_mnt_count;
  uint16_t s_magic;
  uint16_t s_state;
  uint16_t s_errors;
  uint16_t s_minor_rev_level;
  uint32_t s_lastcheck;
  uint32_t s_checkinterval;
  uint32_t s_creator_os;
  uint32_t s_rev_level;
  uint16_t s_def_resuid;
  uint16_t s_def_resgid;
  uint32_t s_first_ino;
  uint16_t s_inode_size;
  uint16_t s_block_group_nr;
  uint32_t s_feature_compat;
  uint32_t s_feature_incompat;
  uint32_t s_feature_ro_compat;
  uint8_t s_uuid[16];
  char s_volume_name[16];
  uint8_t s_last_mounted[64];
  uint32_t s_algo_bitmap;
  uint8_t s_prealloc_blocks;
  uint8_t s_prealloc_dir_blocks;
  uint16_t s_reserved_gdt_blocks;
  uint8_t s_journal_uuid[16];
  uint32_t s_journal_inum;
  uint32_t s_journal_dev;
  uint32_t s_last_orphan;
  uint32_t s_hash_seed[4];
  uint8_t s_def_hash_version;
  uint8_t s_jnl_backup_type;
  uint16_t s_desc_size;
  uint32_t s_default_mount_opts;
  uint32_t s_first_meta_bg;
  uint32_t s_mkfs_time;
  uint32_t s_jnl_blocks[17];
  uint8_t s_padding[664];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
  uint32_t bg_block_bitmap;
  uint32_t bg_inode_bitmap;
  uint32_t bg_inode_table;
  uint16_t bg_free_blocks_count;
  uint16_t bg_free_inodes_count;
  uint16_t bg_used_dirs_count;
  uint16_t bg_pad;
  uint8_t bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
  uint16_t i_mode;
  uint16_t i_uid;
  uint32_t i_size;
  uint32_t i_atime;
  uint32_t i_ctime;
  uint32_t i_mtime;
  uint32_t i_dtime;
  uint16_t i_gid;
  uint16_t i_links_count;
  uint32_t i_blocks;
  uint32_t i_flags;
  uint32_t i_osd1;
  uint32_t i_block[15];
  uint32_t i_generation;
  uint32_t i_file_acl;
  uint32_t i_dir_acl;
  uint32_t i_faddr;
  uint8_t i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
  uint32_t inode;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[];
} __attribute__((packed)) ext2_dirent_t;

#define EXT3_JOURNAL_MAP_SLOTS 64

/* Host tests include this header directly; keep the lock/type dependencies
 * implicit through ext2.h's own includes. Journal descriptor tag capacity is
 * (block_size - header) / tag = 126 at 1 KiB. A transaction whose write-set
 * exceeds one descriptor uses consecutive descriptor/payload groups, capped
 * at EXT3_JOURNAL_SEG_MAX groups before it aborts instead of writing home
 * blocks unjournaled. */
#define EXT3_JOURNAL_SEG_MAX 8
#define EXT3_TXN_CHUNK_ENTRIES 32

/* One unique metadata block staged by the active transaction. `image` is the
 * latest full-block content for that block; `payload_pos`/`desc_pos` are
 * logical journal block indexes assigned when the block first appeared. */
typedef struct {
  uint32_t block;
  uint32_t payload_pos;
  uint32_t desc_pos;
  uint8_t *image;
} ext3_txn_entry_t;

/* Appended-only chunks: readers (ext3_journal_read_staged) walk them under
 * cache_lock while the owner links new chunks, so nothing may move. */
typedef struct ext3_txn_chunk {
  struct ext3_txn_chunk *next;
  uint32_t count;
  ext3_txn_entry_t entries[EXT3_TXN_CHUNK_ENTRIES];
} ext3_txn_chunk_t;

typedef struct {
  bool active;
  bool io_failed;
  uint32_t owner_tid;
  uint32_t depth;
  uint32_t sequence;
  uint32_t start_block;
  ext2_inode_t journal_inode;
  uint8_t *desc_block_buf;
  uint8_t *commit_block_buf;
  uint8_t *super_block_buf;
  /* Write-set of the active transaction, guarded by the mount's cache_lock.
   * txn_head/txn_tail/count are written by the owner only; readers copy the
   * image of a matching block. */
  ext3_txn_chunk_t *txn_head;
  ext3_txn_chunk_t *txn_tail;
  uint32_t txn_count;
  /* The journal inode's allocation is fixed for the lifetime of this mount.
   * Cache only mappings, never mutable journal contents. Keys are logical+1
   * so a zero-initialized slot is empty. Collisions just remap on demand. */
  uint32_t map_keys[EXT3_JOURNAL_MAP_SLOTS];
  uint32_t map_blocks[EXT3_JOURNAL_MAP_SLOTS];
  uint32_t journal_blocks;
  uint32_t blocks_in_trans;
  rawspinlock_t lock;
  /* Fair ownership handoff under lock: the finishing owner keeps `lock`
   * held and grants it to the oldest waiter through granted_tid, so a fresh
   * writer cannot acquire ahead of a queued one. */
  rawspinlock_t sched_lock;
  uint32_t granted_tid;
  wait_queue_t waiters;
  struct storage_latency perf_wait, perf_hold;
  uint64_t started_ms;
  uint64_t owner_ip;
  uint64_t metadata_submitted;
  uint64_t metadata_unchanged;
  char owner_comm[16];
  const char *phase;
} ext3_journal_state_t;

#define EXT2_CACHE_SIZE 2048
#define EXT2_DIRTY_BGD_MAX 64

typedef struct {
  struct block_device *dev;
  ext2_superblock_t sb;
  ext2_bgd_t *bgdt;
  uint32_t block_size;
  uint32_t groups_count;
  uint32_t inodes_per_group;
  uint32_t inode_size;
  vfs_node_t *root_node;
  struct {
    uint32_t num;
    uint8_t *data;
    /* Protected by cache_lock. Misses must not install an image read across
     * a write, even if another block has evicted the writer's cache entry. */
    uint64_t write_seq;
    uint32_t writes_inflight;
  } cache[EXT2_CACHE_SIZE];
  spinlock_t cache_lock;
  /* Group-descriptor table and superblock counters are mutated in memory by
   * the block/inode allocators.  The on-disk copies are only updated once per
   * journal transaction (ext2_flush_metadata, called from
   * ext3_journal_stop), not after every single allocation. */
  bool metadata_dirty;
  /* Descriptor blocks touched by allocator counters. Overflow falls back
   * to the full table; correctness never depends on this list fitting. */
  bool bgdt_all_dirty;
  uint32_t dirty_bgd_count;
  uint32_t dirty_bgd_blocks[EXT2_DIRTY_BGD_MAX];
  ext3_journal_state_t journal;
} ext2_mount_t;

int ext2_mount(struct block_device *dev, vfs_node_t *mountpoint);
int ext2_mount_root(struct block_device *dev);
int ext2_statfs_impl(vfs_node_t *node, struct statfs_buf *buf);
int ext2_read_block(ext2_mount_t *mnt, uint32_t block_num, void *buffer);
int ext2_write_block(ext2_mount_t *mnt, uint32_t block_num, const void *buffer);
int ext2_read_inode(ext2_mount_t *mnt, uint32_t inode_num, ext2_inode_t *out);
uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode, uint32_t logical_block);

#endif
