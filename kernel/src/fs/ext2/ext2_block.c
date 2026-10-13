#include "ext2_internal.h"
#include "apic/lapic_timer.h"
#include "drivers/timer/rtc.h"
#include "fs/ext4/ext4_extent.h"
#include "console/klog.h"
#include "sched/sched.h"

uint32_t ext2_current_time(void) {
  /* Never touch the CMOS RTC here: rtc_get_timestamp() costs ~50us (eight
   * register reads through ports 0x70/0x71 plus the update-in-progress wait)
   * and this runs on every write/create/mkdir for inode timestamps.  The RTC
   * snapshot taken at boot plus monotonic uptime yields the same
   * second-granularity wall time with no port I/O. */
  return (uint32_t)(rtc_get_boot_timestamp() + lapic_timer_get_ms() / 1000);
}

int ext2_read_block(ext2_mount_t *mnt, uint32_t block_num, void *buffer) {
  if (block_num == 0) {
    memset(buffer, 0, mnt->block_size);
    return 0;
  }

  uint32_t idx = block_num % EXT2_CACHE_SIZE;
read_retry:
  spinlock_acquire(&mnt->cache_lock);
  if (mnt->cache[idx].data && mnt->cache[idx].num == block_num) {
    memcpy(buffer, mnt->cache[idx].data, mnt->block_size);
    spinlock_release(&mnt->cache_lock);
    return 0;
  }
  if (mnt->cache[idx].writes_inflight) {
    spinlock_release(&mnt->cache_lock);
    sched_yield();
    goto read_retry;
  }
  uint64_t write_seq = mnt->cache[idx].write_seq;
  spinlock_release(&mnt->cache_lock);

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba         = byte_offset / 512;
  uint32_t sectors     = mnt->block_size / 512;
  int err = mnt->dev->read_sectors(mnt->dev, lba, sectors, buffer);
  if (err) {
    static uint32_t err_count;
    if (__atomic_add_fetch(&err_count, 1, __ATOMIC_RELAXED) <= 8) {
      union { uint32_t u; char s[4]; } alias = { .u = block_num };
      struct thread *ct = sched_get_current();
      klogf("[EXT2] block %u read failed: lba=%llu sectors=%u err=%d"
            " ascii='%c%c%c%c' bs=%u nblocks=%u comm='%s' tid=%u tgid=%u\n",
            block_num, (unsigned long long)lba, sectors, err,
            alias.s[0] >= 32 && alias.s[0] < 127 ? alias.s[0] : '.',
            alias.s[1] >= 32 && alias.s[1] < 127 ? alias.s[1] : '.',
            alias.s[2] >= 32 && alias.s[2] < 127 ? alias.s[2] : '.',
            alias.s[3] >= 32 && alias.s[3] < 127 ? alias.s[3] : '.',
            mnt->block_size, mnt->sb.s_blocks_count,
            (ct && ct->comm[0]) ? ct->comm : "?",
            ct ? ct->tid : 0, ct ? ct->tgid : 0);
    }
    return err;
  }

  spinlock_acquire(&mnt->cache_lock);
  if (mnt->cache[idx].data && mnt->cache[idx].num == block_num) {
    /* A writer populated this block while the device read was in flight.
     * Keep and return the newer cached version instead of restoring stale data. */
    memcpy(buffer, mnt->cache[idx].data, mnt->block_size);
  } else {
    if (mnt->cache[idx].write_seq != write_seq ||
        mnt->cache[idx].writes_inflight) {
      /* The disk image may predate a write whose cache entry was evicted.
       * Do not return it or use it as a later metadata RMW's starting image. */
      spinlock_release(&mnt->cache_lock);
      sched_yield();
      goto read_retry;
    }
    /* This is a direct-mapped cache.  An occupied slot belonging to another
     * block must be replaced; retaining it makes later writes to that block
     * invisible to the cache and can return stale extent metadata. */
    if (!mnt->cache[idx].data)
      mnt->cache[idx].data = kmalloc(mnt->block_size);
    if (mnt->cache[idx].data) {
      mnt->cache[idx].num = block_num;
      memcpy(mnt->cache[idx].data, buffer, mnt->block_size);
    }
  }
  spinlock_release(&mnt->cache_lock);

  return 0;
}

int ext2_write_block(ext2_mount_t *mnt, uint32_t block_num,
                     const void *buffer) {
  if (block_num == 0)
    return -1;

  uint32_t idx = block_num % EXT2_CACHE_SIZE;
  spinlock_acquire(&mnt->cache_lock);
  mnt->cache[idx].write_seq++;
  mnt->cache[idx].writes_inflight++;
  if (!mnt->cache[idx].data)
    mnt->cache[idx].data = kmalloc(mnt->block_size);
  if (mnt->cache[idx].data) {
    mnt->cache[idx].num = block_num;
    memcpy(mnt->cache[idx].data, buffer, mnt->block_size);
  }
  spinlock_release(&mnt->cache_lock);

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba         = byte_offset / 512;
  uint32_t sectors     = mnt->block_size / 512;
  int err = mnt->dev->write_sectors(mnt->dev, lba, sectors, buffer);
  spinlock_acquire(&mnt->cache_lock);
  mnt->cache[idx].write_seq++;
  mnt->cache[idx].writes_inflight--;
  if (err && mnt->cache[idx].num == block_num)
    mnt->cache[idx].num = 0;
  spinlock_release(&mnt->cache_lock);
  return err;
}

/* Publish and version every member before issuing a bounded contiguous data
 * transfer. Even colliding readers must detect writes across cache eviction.
 * Journal ownership serializes filesystem writers, as for write_block(). */
int ext2_write_blocks(ext2_mount_t *mnt, uint32_t first, uint32_t count,
                      const void *buffer) {
  if (!mnt || !buffer || !mnt->dev || !mnt->dev->write_sectors ||
      !first || !count || count > 32 || !mnt->block_size ||
      mnt->block_size % 512 || first >= mnt->sb.s_blocks_count ||
      count > mnt->sb.s_blocks_count - first)
    return -1;
  spinlock_acquire(&mnt->cache_lock);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = (first + i) % EXT2_CACHE_SIZE;
    mnt->cache[idx].write_seq++;
    mnt->cache[idx].writes_inflight++;
    if (!mnt->cache[idx].data)
      mnt->cache[idx].data = kmalloc(mnt->block_size);
    if (mnt->cache[idx].data) {
      mnt->cache[idx].num = first + i;
      memcpy(mnt->cache[idx].data, (const uint8_t *)buffer +
             (size_t)i * mnt->block_size, mnt->block_size);
    }
  }
  spinlock_release(&mnt->cache_lock);
  uint32_t sectors_per_block = mnt->block_size / 512;
  int err = mnt->dev->write_sectors(mnt->dev,
      (uint64_t)first * sectors_per_block, count * sectors_per_block, buffer);
  spinlock_acquire(&mnt->cache_lock);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = (first + i) % EXT2_CACHE_SIZE;
    mnt->cache[idx].write_seq++;
    mnt->cache[idx].writes_inflight--;
    if (err && mnt->cache[idx].num == first + i)
      mnt->cache[idx].num = 0;
  }
  spinlock_release(&mnt->cache_lock);
  return err;
}

int ext2_write_superblock(ext2_mount_t *mnt) {
  uint8_t buf[1024];
  int err = mnt->dev->read_sectors(mnt->dev, 2, 2, buf);
  if (err)
    return -1;
  memcpy(buf, &mnt->sb, sizeof(ext2_superblock_t));
  return ext3_journal_block(mnt, 1, buf);
}

int ext2_write_bgdt(ext2_mount_t *mnt) {
  uint32_t bgdt_block    = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size     = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t blocks_needed = (bgdt_size + mnt->block_size - 1) / mnt->block_size;

  uint8_t *tmp = kmalloc(blocks_needed * mnt->block_size);
  if (!tmp)
    return -1;

  memset(tmp, 0, blocks_needed * mnt->block_size);
  memcpy(tmp, mnt->bgdt, bgdt_size);

  for (uint32_t i = 0; i < blocks_needed; i++) {
    int err = ext3_journal_block(mnt, bgdt_block + i,
                                 tmp + i * mnt->block_size);
    if (err) {
      kfree(tmp);
      return -1;
    }
  }

  kfree(tmp);
  return 0;
}

/*
 * Lazy bgdt/superblock writeback.
 *
 * A single block allocation used to rewrite the complete group-descriptor
 * table *and* the superblock, each inside its own journal transaction, so
 * creating a 1 MiB file rewrote the same counters hundreds of times.  The
 * allocators now only adjust the in-memory copies and set a dirty flag;
 * ext3_journal_stop() calls ext2_flush_metadata() once per transaction, which
 * keeps the crash-recovery guarantees (the bgdt/superblock blocks are added
 * to the same descriptor as the bitmap change) while collapsing the writes.
 *
 * Snapshot dirty descriptor-block indexes under cache_lock. Marks made during
 * a flush remain pending; only affected blocks are written unless the bounded
 * list overflows or a previous flush failed, which triggers a full-table retry.
 */
void ext2_mark_metadata_dirty(ext2_mount_t *mnt) {
  if (!mnt)
    return;
  spinlock_acquire(&mnt->cache_lock);
  mnt->bgdt_all_dirty = true;
  mnt->metadata_dirty = true;
  spinlock_release(&mnt->cache_lock);
}

void ext2_mark_group_dirty(ext2_mount_t *mnt, uint32_t group) {
  if (!mnt || group >= mnt->groups_count)
    return;
  uint32_t block = group * sizeof(ext2_bgd_t) / mnt->block_size;
  spinlock_acquire(&mnt->cache_lock);
  mnt->metadata_dirty = true;
  if (!mnt->bgdt_all_dirty) {
    uint32_t i;
    for (i = 0; i < mnt->dirty_bgd_count; i++)
      if (mnt->dirty_bgd_blocks[i] == block)
        break;
    if (i == mnt->dirty_bgd_count) {
      if (i < EXT2_DIRTY_BGD_MAX)
        mnt->dirty_bgd_blocks[mnt->dirty_bgd_count++] = block;
      else
        mnt->bgdt_all_dirty = true;
    }
  }
  spinlock_release(&mnt->cache_lock);
}

int ext2_flush_metadata(ext2_mount_t *mnt) {
  if (!mnt)
    return -1;
  uint32_t dirty[EXT2_DIRTY_BGD_MAX];
  spinlock_acquire(&mnt->cache_lock);
  if (!mnt->metadata_dirty) {
    spinlock_release(&mnt->cache_lock);
    return 0;
  }
  bool all = mnt->bgdt_all_dirty;
  uint32_t count = mnt->dirty_bgd_count;
  memcpy(dirty, mnt->dirty_bgd_blocks, count * sizeof(uint32_t));
  mnt->metadata_dirty = false;
  mnt->bgdt_all_dirty = false;
  mnt->dirty_bgd_count = 0;
  spinlock_release(&mnt->cache_lock);

  int result = 0;
  if (all) {
    result = ext2_write_bgdt(mnt);
  } else if (count) {
    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf) {
      result = -1;
    } else {
      uint32_t size = mnt->groups_count * sizeof(ext2_bgd_t);
      uint32_t first = mnt->sb.s_first_data_block + 1;
      for (uint32_t i = 0; i < count && result == 0; i++) {
        uint32_t offset = dirty[i] * mnt->block_size;
        uint32_t bytes = size - offset;
        if (bytes > mnt->block_size)
          bytes = mnt->block_size;
        memset(buf, 0, mnt->block_size);
        memcpy(buf, (uint8_t *)mnt->bgdt + offset, bytes);
        result = ext3_journal_block(mnt, first + dirty[i], buf);
      }
      kfree(buf);
    }
  }
  if (result == 0)
    result = ext2_write_superblock(mnt);
  if (result != 0)
    ext2_mark_metadata_dirty(mnt); /* Retain dirty state for retry after error. */
  return result;
}

#define EXT2_INODE_STACK_BUF_MAX 4096

int ext2_read_inode(ext2_mount_t *mnt, uint32_t inode_num,
                    ext2_inode_t *out) {
  if (inode_num == 0)
    return -1;

  uint32_t group  = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index  = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block    = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset         = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block  = byte_offset_in_table % mnt->block_size;

  uint8_t stack_buf[EXT2_INODE_STACK_BUF_MAX];
  bool heap_used = (mnt->block_size > EXT2_INODE_STACK_BUF_MAX);
  uint8_t *block_buf = heap_used ? kmalloc(mnt->block_size) : stack_buf;
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    if (heap_used) kfree(block_buf);
    return -1;
  }

  memcpy(out, block_buf + offset_within_block, sizeof(ext2_inode_t));
  if (heap_used) kfree(block_buf);
  return 0;
}

static int ext2_write_inode_in_transaction(ext2_mount_t *mnt, uint32_t inode_num,
                                          const ext2_inode_t *inode) {
  if (inode_num == 0)
    return -1;

  uint32_t group  = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index  = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block    = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset         = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block  = byte_offset_in_table % mnt->block_size;

  uint8_t stack_buf[EXT2_INODE_STACK_BUF_MAX];
  bool heap_used = (mnt->block_size > EXT2_INODE_STACK_BUF_MAX);
  uint8_t *block_buf = heap_used ? kmalloc(mnt->block_size) : stack_buf;
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    if (heap_used) kfree(block_buf);
    return -1;
  }

  memcpy(block_buf + offset_within_block, inode, sizeof(ext2_inode_t));
  err = ext3_journal_block(mnt, inode_table_block + block_offset, block_buf);
  if (heap_used) kfree(block_buf);
  return err;
}

int ext2_write_inode(ext2_mount_t *mnt, uint32_t inode_num,
                     const ext2_inode_t *inode) {
  if (!mnt || !inode || inode_num == 0)
    return -1;
  /* An inode-table block contains several independent files. Every caller,
   * including post-create chown/chmod, must own the transaction before reading
   * that block or it can overwrite a neighbor initialized by another CPU. */
  if (ext3_journal_start(mnt) != 0)
    return -1;
  int result = ext2_write_inode_in_transaction(mnt, inode_num, inode);
  int stop_result = ext3_journal_stop(mnt);
  return result != 0 ? result : stop_result;
}

uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                             uint32_t logical_block) {
  if (inode->i_flags & EXT4_EXTENTS_FL)
    return ext4_get_block_num(mnt, inode, logical_block);
  uint32_t ptrs_per_block = mnt->block_size / 4;

  if (logical_block < EXT2_DIRECT_BLOCKS) {
    uint32_t result = inode->i_block[logical_block];
    if (result != 0 && result >= mnt->sb.s_blocks_count) {
      union { uint32_t u; char s[4]; } alias = { .u = result };
      struct thread *ct = sched_get_current();
      klogf("[EXT2] bad direct block: logical=%u result=%u ascii='%c%c%c%c'"
            " i_block0=%u i_block1=%u nblocks=%u comm='%s' tid=%u\n",
            logical_block, result,
            alias.s[0] >= 32 && alias.s[0] < 127 ? alias.s[0] : '.',
            alias.s[1] >= 32 && alias.s[1] < 127 ? alias.s[1] : '.',
            alias.s[2] >= 32 && alias.s[2] < 127 ? alias.s[2] : '.',
            alias.s[3] >= 32 && alias.s[3] < 127 ? alias.s[3] : '.',
            inode->i_block[0], inode->i_block[1],
            mnt->sb.s_blocks_count,
            (ct && ct->comm[0]) ? ct->comm : "?",
            ct ? ct->tid : 0);
    }
    return result;
  }

  logical_block -= EXT2_DIRECT_BLOCKS;

  if (logical_block < ptrs_per_block) {
    if (inode->i_block[12] == 0)
      return 0;
    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[12], indirect);
    uint32_t result = indirect[logical_block];
    kfree(indirect);
    return result;
  }

  logical_block -= ptrs_per_block;

  if (logical_block < ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[13] == 0)
      return 0;
    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[13], dindirect);
    uint32_t indirect_block = dindirect[logical_block / ptrs_per_block];
    kfree(dindirect);
    if (indirect_block == 0)
      return 0;
    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, indirect_block, indirect);
    uint32_t result = indirect[logical_block % ptrs_per_block];
    kfree(indirect);
    return result;
  }

  logical_block -= ptrs_per_block * ptrs_per_block;

  if (logical_block <
      (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[14] == 0)
      return 0;
    uint32_t *tindirect = kmalloc(mnt->block_size);
    if (!tindirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[14], tindirect);

    uint32_t idx1          = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem           = logical_block % (ptrs_per_block * ptrs_per_block);
    uint32_t dindirect_blk = tindirect[idx1];
    kfree(tindirect);
    if (dindirect_blk == 0)
      return 0;

    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return 0;
    ext2_read_block(mnt, dindirect_blk, dindirect);

    uint32_t idx2           = rem / ptrs_per_block;
    uint32_t indirect_block = dindirect[idx2];
    kfree(dindirect);
    if (indirect_block == 0)
      return 0;

    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, indirect_block, indirect);
    uint32_t result = indirect[rem % ptrs_per_block];
    kfree(indirect);
    return result;
  }

  return 0;
}

/* Decode a plain-inode pointer path. At most three pointer blocks are
 * needed; keep arithmetic wide for the triple-indirect capacity. */
static int ext2_pointer_path(ext2_mount_t *mnt, uint32_t logical,
                              uint32_t *root, uint32_t indices[3]) {
  uint64_t ptrs = mnt->block_size / sizeof(uint32_t);
  if (!ptrs)
    return -1;
  if (logical < EXT2_DIRECT_BLOCKS) {
    *root = logical;
    return 0;
  }
  uint64_t offset = logical - EXT2_DIRECT_BLOCKS;
  for (uint32_t depth = 1; depth <= 3; depth++) {
    uint64_t capacity = ptrs;
    for (uint32_t n = 1; n < depth; n++) capacity *= ptrs;
    if (offset < capacity) {
      *root = 11 + depth;
      for (uint32_t n = 0; n < depth; n++) {
        capacity /= ptrs;
        indices[n] = (uint32_t)(offset / capacity);
        offset %= capacity;
      }
      return (int)depth;
    }
    offset -= capacity;
  }
  return -1;
}

/* Unlike the legacy hole-or-error lookup, writes must not allocate a new
 * data block when an existing indirect block could not be read. */
int ext2_get_block_num_checked(ext2_mount_t *mnt, ext2_inode_t *inode,
                                uint32_t logical, uint32_t *out) {
  if (!mnt || !inode || !out || (inode->i_flags & EXT4_EXTENTS_FL))
    return -1;
  *out = 0;
  uint32_t root, indices[3];
  int depth = ext2_pointer_path(mnt, logical, &root, indices);
  if (depth < 0)
    return -1;
  uint32_t block = inode->i_block[root];
  uint32_t *buf = depth ? kmalloc(mnt->block_size) : NULL;
  if (depth && !buf)
    return -1;
  int result = 0;
  for (int level = 0; level < depth && block; level++) {
    if (block >= mnt->sb.s_blocks_count || ext2_read_block(mnt, block, buf) != 0) {
      result = -1;
      break;
    }
    block = buf[indices[level]];
  }
  if (block >= mnt->sb.s_blocks_count && block)
    result = -1;
  if (!result)
    *out = block;
  kfree(buf);
  return result;
}

int ext2_set_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                       uint32_t logical, uint32_t disk_block) {
  if (!mnt || !inode || (inode->i_flags & EXT4_EXTENTS_FL) ||
      (disk_block && disk_block >= mnt->sb.s_blocks_count))
    return -1;
  uint32_t root, indices[3];
  int depth = ext2_pointer_path(mnt, logical, &root, indices);
  if (depth < 0)
    return -1;
  if (!depth) {
    inode->i_block[root] = disk_block;
    return 0;
  }
  uint32_t *buf = kmalloc(mnt->block_size);
  if (!buf)
    return -1;
  uint32_t block = inode->i_block[root];
  int result = -1;
  if (!block) {
    block = ext2_alloc_block(mnt);
    if (!block)
      goto done;
    memset(buf, 0, mnt->block_size);
    if (ext2_write_block(mnt, block, buf) != 0)
      goto done;
    inode->i_block[root] = block;
  }
  for (int level = 0; level < depth; level++) {
    if (block >= mnt->sb.s_blocks_count || ext2_read_block(mnt, block, buf) != 0)
      goto done;
    uint32_t index = indices[level];
    if (level == depth - 1) {
      buf[index] = disk_block;
      result = ext3_journal_block(mnt, block, buf);
      goto done;
    }
    uint32_t child = buf[index];
    if (!child) {
      child = ext2_alloc_block(mnt);
      if (!child)
        goto done;
      /* Publish a pointer only after its new block has been initialized. */
      uint32_t *zero = kmalloc(mnt->block_size);
      if (!zero)
        goto done;
      memset(zero, 0, mnt->block_size);
      int error = ext2_write_block(mnt, child, zero);
      kfree(zero);
      if (error)
        goto done;
      buf[index] = child;
      if (ext3_journal_block(mnt, block, buf) != 0)
        goto done;
    }
    block = child;
  }
done:
  kfree(buf);
  return result;
}

/* Scan one group's bitmap for a free block, journal it as used and return its
 * block number (0 when the group is full).  The bitmap buffer is provided by
 * the caller and reused across groups.  start_bit is only meaningful for the
 * group containing the allocation goal: it makes the scan wrap within that
 * group so the search still starts near the goal. */
static uint32_t ext2_alloc_in_group(ext2_mount_t *mnt, uint32_t g,
                                    uint32_t start_bit, uint8_t *bitmap,
                                    uint32_t max_blocks, uint32_t *run_count) {
  if (ext2_read_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap) != 0)
    return 0;

  uint32_t blocks_in_group = mnt->sb.s_blocks_per_group;
  if (blocks_in_group > mnt->block_size * 8u)
    return 0;
  if (g == mnt->groups_count - 1) {
    /* Block numbers are offset by first_data_block and the last group is
     * short.  The old formula subtracted only the group offset, which lets
     * the scan walk one block past the end of the filesystem. */
    uint32_t group_start =
        mnt->sb.s_first_data_block + g * mnt->sb.s_blocks_per_group;
    if (group_start >= mnt->sb.s_blocks_count)
      return 0;
    uint32_t remaining = mnt->sb.s_blocks_count - group_start;
    if (remaining < blocks_in_group)
      blocks_in_group = remaining;
  }

  /* Two windows: [start_bit, end) then [0, start_bit).  With start_bit == 0
   * the second pass is empty. */
  for (int pass = 0; pass < 2; pass++) {
    uint32_t b0 = (pass == 0) ? start_bit : 0;
    uint32_t b1 = (pass == 0) ? blocks_in_group : start_bit;
    if (b1 > blocks_in_group)
      b1 = blocks_in_group;

    for (uint32_t b = b0; b < b1; b++) {
      uint32_t byte_idx = b / 8;
      uint8_t bit_mask = (uint8_t)(1u << (b % 8));
      if (bitmap[byte_idx] & bit_mask)
        continue;

      /* Reserve a contiguous run in one bitmap image. Never join across
       * the group boundary, wrap window, or an already allocated bit. */
      uint32_t count = 1;
      while (count < max_blocks && b + count < b1 &&
             !(bitmap[(b + count) / 8] & (1u << ((b + count) % 8))))
        count++;
      for (uint32_t n = 0; n < count; n++)
        bitmap[(b + n) / 8] |= (uint8_t)(1u << ((b + n) % 8));
      if (ext3_journal_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap) != 0)
        return 0;

      mnt->bgdt[g].bg_free_blocks_count =
          mnt->bgdt[g].bg_free_blocks_count > count
              ? mnt->bgdt[g].bg_free_blocks_count - count : 0;
      mnt->sb.s_free_blocks_count = mnt->sb.s_free_blocks_count > count
                                      ? mnt->sb.s_free_blocks_count - count : 0;
      ext2_mark_group_dirty(mnt, g);

      uint32_t allocated_block = mnt->sb.s_first_data_block +
                                 g * mnt->sb.s_blocks_per_group + b;
      spinlock_acquire(&mnt->cache_lock);
      for (uint32_t n = 0; n < count; n++) {
        uint32_t c_idx = (allocated_block + n) % EXT2_CACHE_SIZE;
        mnt->cache[c_idx].write_seq++;
        if (mnt->cache[c_idx].num == allocated_block + n)
          mnt->cache[c_idx].num = 0;
      }
      spinlock_release(&mnt->cache_lock);
      *run_count = count;
      return allocated_block;
    }
  }

  return 0;
}

uint32_t ext2_alloc_run_hint(ext2_mount_t *mnt, uint32_t goal,
                              uint32_t max_blocks, uint32_t *run_count) {
  if (!mnt || !run_count || !max_blocks || !mnt->groups_count)
    return 0;
  *run_count = 0;
  if (ext3_journal_start(mnt) != 0)
    return 0;
  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap) {
    ext3_journal_stop(mnt);
    return 0;
  }

  uint32_t start_group = 0;
  uint32_t start_bit = 0;

  if (goal >= mnt->sb.s_first_data_block && goal < mnt->sb.s_blocks_count) {
    uint32_t rel = goal - mnt->sb.s_first_data_block;
    start_group = rel / mnt->sb.s_blocks_per_group;
    start_bit = rel % mnt->sb.s_blocks_per_group;
    if (start_group >= mnt->groups_count) {
      start_group = 0;
      start_bit = 0;
    }
  }

  uint32_t allocated = 0;

  /* Fast path: trust the in-memory group counters and start at the goal's
   * group. */
  for (uint32_t i = 0; i < mnt->groups_count && !allocated; i++) {
    uint32_t g = (start_group + i) % mnt->groups_count;
    if (mnt->bgdt[g].bg_free_blocks_count == 0)
      continue;
    allocated = ext2_alloc_in_group(mnt, g, i == 0 ? start_bit : 0, bitmap,
                                     max_blocks, run_count);
  }

  /* Slow path: an interrupted write or journal replay can leave a group's
   * free counter stale at 0 while its bitmap still has free blocks.  Ignore
   * the counters for those groups and make one full pass so a filesystem with
   * free space can still allocate. */
  for (uint32_t g = 0; g < mnt->groups_count && !allocated; g++) {
    if (mnt->bgdt[g].bg_free_blocks_count != 0)
      continue; /* already covered by the fast path */
    allocated = ext2_alloc_in_group(mnt, g, 0, bitmap, max_blocks, run_count);
  }

  if (ext3_journal_stop(mnt) != 0) {
    allocated = 0;
    *run_count = 0;
  }
  kfree(bitmap);
  return allocated;
}

uint32_t ext2_alloc_block_hint(ext2_mount_t *mnt, uint32_t goal) {
  uint32_t count;
  return ext2_alloc_run_hint(mnt, goal, 1, &count);
}

uint32_t ext2_alloc_block(ext2_mount_t *mnt) {
  return ext2_alloc_block_hint(mnt, 0);
}

uint32_t ext2_alloc_inode(ext2_mount_t *mnt) {
  ext3_journal_start(mnt);
  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap) {
    ext3_journal_stop(mnt);
    return 0;
  }

  for (uint32_t g = 0; g < mnt->groups_count; g++) {
    if (mnt->bgdt[g].bg_free_inodes_count == 0)
      continue;

    ext2_read_block(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);

    for (uint32_t i = 0; i < mnt->inodes_per_group; i++) {
      uint32_t byte_idx = i / 8;
      uint8_t  bit_mask = 1 << (i % 8);
      if (!(bitmap[byte_idx] & bit_mask)) {
        bitmap[byte_idx] |= bit_mask;
        ext3_journal_block(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);

        mnt->bgdt[g].bg_free_inodes_count--;
        mnt->sb.s_free_inodes_count--;
        ext2_mark_group_dirty(mnt, g);

        ext3_journal_stop(mnt);
        kfree(bitmap);
        return g * mnt->inodes_per_group + i + 1;
      }
    }
  }

  ext3_journal_stop(mnt);
  kfree(bitmap);
  return 0;
}

int ext2_free_block(ext2_mount_t *mnt, uint32_t block_num) {
  if (block_num == 0)
    return -1;

  uint32_t adjusted = block_num - mnt->sb.s_first_data_block;
  uint32_t group    = adjusted / mnt->sb.s_blocks_per_group;
  uint32_t index    = adjusted % mnt->sb.s_blocks_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return -1;

  ext2_read_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);

  uint32_t byte_idx = index / 8;
  uint8_t  bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask;

  ext3_journal_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);
  kfree(bitmap);

  uint32_t c_idx = block_num % EXT2_CACHE_SIZE;
  spinlock_acquire(&mnt->cache_lock);
  mnt->cache[c_idx].write_seq++;
  if (mnt->cache[c_idx].num == block_num) {
    mnt->cache[c_idx].num = 0;
  }
  spinlock_release(&mnt->cache_lock);

  mnt->bgdt[group].bg_free_blocks_count++;
  mnt->sb.s_free_blocks_count++;
  ext2_mark_group_dirty(mnt, group);
  return 0;
}

int ext2_free_inode(ext2_mount_t *mnt, uint32_t inode_num) {
  if (inode_num == 0)
    return -1;

  uint32_t group = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return -1;

  ext2_read_block(mnt, mnt->bgdt[group].bg_inode_bitmap, bitmap);

  uint32_t byte_idx = index / 8;
  uint8_t  bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask;

  ext3_journal_block(mnt, mnt->bgdt[group].bg_inode_bitmap, bitmap);
  kfree(bitmap);

  mnt->bgdt[group].bg_free_inodes_count++;
  mnt->sb.s_free_inodes_count++;
  ext2_mark_group_dirty(mnt, group);
  return 0;
}

void ext2_free_indirect(ext2_mount_t *mnt, uint32_t indirect_block) {
  if (indirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, indirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_block(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, indirect_block);
}

void ext2_free_dindirect(ext2_mount_t *mnt, uint32_t dindirect_block) {
  if (dindirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, dindirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_indirect(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, dindirect_block);
}

void ext2_free_tindirect(ext2_mount_t *mnt, uint32_t tindirect_block) {
  if (tindirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, tindirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_dindirect(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, tindirect_block);
}

void ext2_free_all_blocks(ext2_mount_t *mnt, ext2_inode_t *inode) {
  if (ext4_inode_has_extents(inode)) {
    ext4_extent_free_all(mnt, inode);
    return;
  }
  if ((inode->i_mode & 0xF000) == EXT2_S_IFLNK && inode->i_blocks == 0) {
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_size = 0;
    return;
  }
  for (int i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
    if (inode->i_block[i]) {
      ext2_free_block(mnt, inode->i_block[i]);
      inode->i_block[i] = 0;
    }
  }
  if (inode->i_block[12]) {
    ext2_free_indirect(mnt, inode->i_block[12]);
    inode->i_block[12] = 0;
  }
  if (inode->i_block[13]) {
    ext2_free_dindirect(mnt, inode->i_block[13]);
    inode->i_block[13] = 0;
  }
  if (inode->i_block[14]) {
    ext2_free_tindirect(mnt, inode->i_block[14]);
    inode->i_block[14] = 0;
  }
  inode->i_blocks = 0;
  inode->i_size   = 0;
}
