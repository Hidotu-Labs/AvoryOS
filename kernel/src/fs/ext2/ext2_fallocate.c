#include "ext2_internal.h"
#include "fs/ext4/ext4_extent.h"

#define EXT2_FALLOCATE_BATCH_BLOCKS 32
#define EXT4_FALLOCATE_BATCH_BLOCKS 256

/* Reserve initialized blocks in bounded transactions. Newly reserved blocks
 * are zeroed before mapping them; existing mappings and bytes are untouched. */
int ext2_fallocate_impl(vfs_node_t *node, int mode, uint32_t offset,
                        uint32_t len) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device || !len)
    return -22;
  if (mode != 0 && mode != 1)
    return -95;
  if (len > UINT32_MAX - offset)
    return -27;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (mnt->read_only)
    return -30;
  if (!mnt->block_size)
    return -5;
  uint32_t zero_size = mnt->block_size * EXT2_FALLOCATE_BATCH_BLOCKS;
  if (zero_size / EXT2_FALLOCATE_BATCH_BLOCKS != mnt->block_size)
    return -27;
  uint8_t *zero = kmalloc(zero_size);
  if (!zero)
    return -12;
  memset(zero, 0, zero_size);
  uint32_t end = offset + len;
  uint32_t logical = offset / mnt->block_size;
  uint32_t last = (end - 1) / mnt->block_size;
  int error = 0;

  while (logical <= last && !error) {
    if (ext3_journal_start(mnt)) {
      error = -5;
      break;
    }
    ext2_inode_t inode;
    mnt->journal.phase = "fallocate-inode-read";
    bool inode_valid = ext2_read_inode(mnt, node->inode, &inode) == 0;
    bool changed = false;
    if (!inode_valid) {
      error = -5;
    } else {
      bool extents = ext4_inode_has_extents(&inode);
      uint32_t budget = extents ? EXT4_FALLOCATE_BATCH_BLOCKS
                                : EXT2_FALLOCATE_BATCH_BLOCKS;
      /* Bound both data volume and mapping operations: fragmentation must
       * not turn a larger reservation into hundreds of metadata updates. */
      for (unsigned operations = 0;
           operations < EXT2_FALLOCATE_BATCH_BLOCKS && budget && logical <= last;
           operations++) {
        uint64_t block = 0;
        uint32_t run = 1;
        if (extents)
          block = ext4_get_extent_run(mnt, &inode, logical, &run);
        else if (ext2_get_block_num_checked(mnt, &inode, logical, &block)) {
          error = -5;
          break;
        }
        if (block) {
          if (run > last - logical + 1) run = last - logical + 1;
          if (run > budget) run = budget;
          logical += run;
          budget -= run;
          continue;
        }

        uint32_t reserved = 1;
        if (extents) {
          uint32_t wanted = last - logical + 1;
          if (wanted > budget) wanted = budget;
          if (wanted > run) wanted = run;
          uint64_t goal = logical
              ? ext4_get_block_num(mnt, &inode, logical - 1) : 0;
          if (goal) goal++;
          block = ext2_alloc_run_hint(mnt, goal, wanted, &reserved);
        } else {
          block = ext2_alloc_block(mnt);
        }
        if (!block || !reserved) {
          error = -28;
          break;
        }
        mnt->journal.phase = "fallocate-zero";
        int zero_result = 0;
        /* A reservation may exceed the cache-safe device transfer bound.
         * Reuse the small zero buffer, then publish the complete mapping. */
        for (uint32_t done = 0; done < reserved;) {
          uint32_t count = reserved - done;
          if (count > EXT2_FALLOCATE_BATCH_BLOCKS)
            count = EXT2_FALLOCATE_BATCH_BLOCKS;
          zero_result = extents
              ? ext2_write_blocks(mnt, block + done, count, zero)
              : ext2_write_block(mnt, block + done, zero);
          if (zero_result) break;
          done += count;
        }
        if (zero_result) {
          for (uint32_t n = 0; n < reserved; n++)
            ext2_free_block(mnt, block + n);
          error = -5;
          break;
        }
        /* Mapping helpers can change indirect/tree roots before failing.
         * Persist that inode snapshot even on the error path. */
        changed = true;
        mnt->journal.phase = "fallocate-mapping";
        int result = extents
            ? ext4_extent_insert(mnt, &inode, node->inode, logical, block,
                                 (uint16_t)reserved)
            : ext2_set_block_num(mnt, &inode, logical, block);
        if (result) {
          /* A failed metadata write can already have reached the disk. Do
           * not return these blocks to the allocator for unrelated reuse. */
          error = -5;
          break;
        }
        inode.i_blocks += reserved * (mnt->block_size / 512);
        logical += reserved;
        budget -= reserved;
      }
      /* KEEP_SIZE never modifies the persistent size. For mode 0, publish
       * the extension only after every requested block has been reserved. */
      if (!error && logical > last && !mode && end > inode.i_size) {
        inode.i_size = end;
        changed = true;
      }
      if (changed) {
        inode.i_ctime = ext2_current_time();
        mnt->journal.phase = "fallocate-inode-update";
        if (ext2_write_inode(mnt, node->inode, &inode))
          error = -5;
      }
    }
    if (ext3_journal_stop(mnt))
      error = -5;
    if (inode_valid && error != -5) {
      node->length = inode.i_size;
      node->ctime = inode.i_ctime;
    }
  }
  kfree(zero);
  return error;
}
