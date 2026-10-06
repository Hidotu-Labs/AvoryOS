#include "ext3.h"
#include "fs/ext2/ext2_internal.h"
#include "console/klog.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "mm/heap.h"
#include "apic/lapic_timer.h"
#include <stdbool.h>

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb,
                                ext2_inode_t *j_inode);

static ext3_journal_state_t *ext3_journal_state(ext2_mount_t *mnt) {
  return &mnt->journal;
}

/* Called only with the transaction lock held. Recovery uses uncached
 * mappings; normal transactions never change the journal inode itself. */
static uint32_t ext3_journal_phys(ext2_mount_t *mnt, uint32_t logical) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  if (trans->journal_blocks && logical >= trans->journal_blocks)
    return 0;
  uint32_t slot = logical % EXT3_JOURNAL_MAP_SLOTS;
  if (trans->map_keys[slot] == logical + 1)
    return trans->map_blocks[slot];
  uint32_t phys = ext2_get_block_num(mnt, &trans->journal_inode, logical);
  if (!phys || phys >= mnt->sb.s_blocks_count)
    return 0;
  trans->map_blocks[slot] = phys;
  trans->map_keys[slot] = logical + 1;
  return phys;
}

static int ext3_finish_transaction(ext3_journal_state_t *trans, int result) {
  trans->active = false;
  trans->owner_tid = 0;
  trans->depth = 0;
  rawspinlock_release(&trans->lock);
  return result;
}

void ext3_init_journal(ext2_mount_t *mnt) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  rawspinlock_init(&trans->lock);
  trans->active = false;
  trans->owner_tid = 0;
  trans->depth = 0;
  trans->io_failed = false;
  trans->journal_blocks = 0;
  memset(trans->map_keys, 0, sizeof(trans->map_keys));
  if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
    return;
  }

  uint32_t journal_ino = mnt->sb.s_journal_inum;
  if (journal_ino == 0) {
    klog_puts("[EXT3] Journal feature enabled but journal inode is 0\n");
    return;
  }

  if (ext2_read_inode(mnt, journal_ino, &trans->journal_inode) != 0) {
    klog_puts("[EXT3] Failed to read journal inode\n");
    return;
  }

  uint32_t disk_block =
      ext2_get_block_num(mnt, &trans->journal_inode, 0);
  if (!disk_block) {
    klog_puts("[EXT3] Journal inode has no blocks\n");
    return;
  }

  uint8_t *buf = kmalloc(mnt->block_size);
  if (!buf) {
    klog_puts("[EXT3] Out of memory for journal read\n");
    return;
  }

  if (ext2_read_block(mnt, disk_block, buf) != 0) {
    klog_puts("[EXT3] Failed to read journal block\n");
    kfree(buf);
    return;
  }

  jbd_superblock_t *jsb = (jbd_superblock_t *)buf;

  uint32_t magic = __builtin_bswap32(jsb->s_header.h_magic);
  uint32_t type = __builtin_bswap32(jsb->s_header.h_blocktype);
  uint32_t block_size = __builtin_bswap32(jsb->s_blocksize);
  uint32_t incompat = __builtin_bswap32(jsb->s_feature_incompat);
  if (magic != EXT3_JOURNAL_MAGIC_NUMBER ||
      (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) ||
      block_size != mnt->block_size || incompat != 0) {
    klog_puts("[JBD2] Unsupported journal format.\n");
    kfree(buf);
    return;
  }

  trans->sequence = __builtin_bswap32(jsb->s_sequence);
  trans->journal_blocks = __builtin_bswap32(jsb->s_maxlen);
  uint32_t start = __builtin_bswap32(jsb->s_start);

  klog_puts("[JBD2] Found valid journal.\n");
  klog_puts("       Sequence:  ");
  klog_uint64(trans->sequence);
  klog_puts("\n");
  klog_puts("       Start:     ");
  klog_uint64(start);
  klog_puts("\n");

  if (start != 0) {
    klog_puts("[EXT3] Journal requires recovery!\n");
    if (ext3_recover_journal(mnt, jsb, &trans->journal_inode) == 0) {
      trans->sequence++;
      jsb->s_sequence = __builtin_bswap32(trans->sequence);
      jsb->s_start = 0;
      ext2_write_block(mnt, disk_block, buf);
    }
  } else {
    klog_puts("[EXT3] Journal is clean.\n");
  }

  kfree(buf);
}

int ext3_journal_start(ext2_mount_t *mnt) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL))
    return 0;

  struct thread *current = sched_get_current();
  uint32_t tid = current ? current->tid : 0;
  if (trans->active && trans->owner_tid == tid) {
    trans->depth++;
    return 0;
  }

  /*
   * Only the owning thread may nest a transaction. Other writers wait until
   * the complete journal commit/checkpoint sequence has finished.
   */
  uint64_t wait_start = lapic_timer_get_ms();
  bool warned = false;
  while (!rawspinlock_try_acquire(&trans->lock)) {
    if (!warned && lapic_timer_get_ms() - wait_start > 5000) {
      klogf("[JBD2] transaction wait tid=%u owner=%u active=%u depth=%u blocks=%u\n",
            tid, trans->owner_tid, trans->active, trans->depth, trans->blocks_in_trans);
      warned = true;
    }
    sched_yield();
  }
  trans->active = true;
  trans->owner_tid = tid;
  trans->depth = 1;
  trans->blocks_in_trans = 0;
  trans->io_failed = false;

  trans->start_block = 1;

  if (!trans->desc_block_buf)
    trans->desc_block_buf = kmalloc(mnt->block_size);
  if (!trans->desc_block_buf)
    return ext3_finish_transaction(trans, -1);
  memset(trans->desc_block_buf, 0, mnt->block_size);

  jbd_header_t *h = (jbd_header_t *)trans->desc_block_buf;
  h->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
  h->h_blocktype = __builtin_bswap32(JBD_DESCRIPTOR_BLOCK);
  h->h_sequence = __builtin_bswap32(trans->sequence);

  return 0;
}

int ext3_journal_block(ext2_mount_t *mnt, uint32_t block_nr, const void *data) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  if (!trans->active)
    return ext2_write_block(mnt, block_nr, data);
  if (trans->io_failed)
    return -1;

  uint32_t tag_offset = sizeof(jbd_header_t) + (trans->blocks_in_trans *
                                                sizeof(jbd_block_tag_t));
  if (tag_offset + sizeof(jbd_block_tag_t) > mnt->block_size) {
    int result = ext2_write_block(mnt, block_nr, data);
    if (result != 0)
      trans->io_failed = true;
    return result;
  }

  jbd_block_tag_t *tag =
      (jbd_block_tag_t *)(trans->desc_block_buf + tag_offset);
  tag->t_blocknr = __builtin_bswap32(block_nr);
  tag->t_flags = 0;

  uint32_t journal_data_pos =
      trans->start_block + 1 + trans->blocks_in_trans;
  uint32_t phys_pos =
      ext3_journal_phys(mnt, journal_data_pos);

  uint8_t *safe_data = (uint8_t *)data;
  if (*(const uint32_t *)data == __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER)) {
    tag->t_flags |= __builtin_bswap32(JBD_FLAG_ESCAPE);
  }

  if (!phys_pos || ext2_write_block(mnt, phys_pos, safe_data) != 0) {
    trans->io_failed = true;
    return -1;
  }

  /*
   * The transaction's cache is direct-mapped and is only an acceleration,
   * not a complete write-set.  A collision used to make a later allocator
   * reread the pre-transaction bitmap and hand out an extent-tree block a
   * second time.  Keep the home block coherent immediately; the journal
   * record is still written first and is replayable on recovery.
   * ext2_write_block publishes the cache image and tracks overlapping misses.
   */
  if (ext2_write_block(mnt, block_nr, data) != 0) {
    trans->io_failed = true;
    return -1;
  }

  trans->blocks_in_trans++;
  return 0;
}

int ext3_journal_stop(ext2_mount_t *mnt) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  if (!trans->active) {
    /* A mount without a journal has no commit point, so write back the lazy
     * allocator counters here.  On journaled mounts stop() is only reached by
     * the transaction owner, which takes the commit path below. */
    if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL))
      return ext2_flush_metadata(mnt);
    return 0;
  }
  if (trans->depth > 1) {
    trans->depth--;
    return 0;
  }

  /* All group-descriptor and superblock changes made by this transaction are
   * written once, before the descriptor is finalised, so recovery replays
   * them together with the bitmap blocks. */
  if (trans->io_failed || ext2_flush_metadata(mnt) != 0 || trans->io_failed)
    return ext3_finish_transaction(trans, -1);

  /* Read-only nested helpers and unchanged chown calls need no log records. */
  if (trans->blocks_in_trans == 0) {
    return ext3_finish_transaction(trans, 0);
  }

  if (trans->blocks_in_trans > 0) {
    uint32_t last_tag_off =
        sizeof(jbd_header_t) +
        ((trans->blocks_in_trans - 1) * sizeof(jbd_block_tag_t));
    jbd_block_tag_t *tag =
        (jbd_block_tag_t *)(trans->desc_block_buf + last_tag_off);
    tag->t_flags |= __builtin_bswap32(JBD_FLAG_LAST_TAG);
  }

  /* Reuse scratch storage under the mount's transaction lock. The journal
   * superblock is still reread for each commit; caching its contents could
   * hide changes after an I/O failure. */
  if (!trans->commit_block_buf)
    trans->commit_block_buf = kmalloc(mnt->block_size);
  if (!trans->super_block_buf)
    trans->super_block_buf = kmalloc(mnt->block_size);
  if (!trans->commit_block_buf || !trans->super_block_buf)
    return ext3_finish_transaction(trans, -1);
  uint8_t *commit_buf = trans->commit_block_buf;
  memset(commit_buf, 0, mnt->block_size);
  jbd_header_t *ch = (jbd_header_t *)commit_buf;
  ch->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
  ch->h_blocktype = __builtin_bswap32(JBD_COMMIT_BLOCK);
  ch->h_sequence = __builtin_bswap32(trans->sequence);

  uint32_t commit_pos =
      trans->start_block + 1 + trans->blocks_in_trans;
  uint32_t commit_phys =
      ext3_journal_phys(mnt, commit_pos);
  uint32_t desc_phys = ext3_journal_phys(mnt, trans->start_block);
  uint32_t sb_phys = ext3_journal_phys(mnt, 0);
  if (!desc_phys || !commit_phys || !sb_phys)
    return ext3_finish_transaction(trans, -1);
  if (ext2_write_block(mnt, desc_phys, trans->desc_block_buf) != 0 ||
      ext2_write_block(mnt, commit_phys, commit_buf) != 0)
    return ext3_finish_transaction(trans, -1);

  uint8_t *sb_buf = trans->super_block_buf;
  if (ext2_read_block(mnt, sb_phys, sb_buf) != 0)
    return ext3_finish_transaction(trans, -1);
  jbd_superblock_t *jsb = (jbd_superblock_t *)sb_buf;
  jsb->s_start = __builtin_bswap32(trans->start_block);
  if (ext2_write_block(mnt, sb_phys, sb_buf) != 0)
    return ext3_finish_transaction(trans, -1);

  jsb->s_sequence = __builtin_bswap32(trans->sequence + 1);
  jsb->s_start = 0;
  if (ext2_write_block(mnt, sb_phys, sb_buf) != 0)
    return ext3_finish_transaction(trans, -1);
  trans->sequence++;
  return ext3_finish_transaction(trans, 0);
}

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb,
                                ext2_inode_t *j_inode) {
  uint32_t block_size = mnt->block_size;
  uint32_t start_block = __builtin_bswap32(jsb->s_start);
  uint32_t sequence = __builtin_bswap32(jsb->s_sequence);
  uint32_t journal_blocks = __builtin_bswap32(jsb->s_maxlen);

  klog_puts("[EXT3] Starting journal recovery...\n");

  uint8_t *desc_buf = kmalloc(block_size);
  uint8_t *data_buf = kmalloc(block_size);
  if (!desc_buf || !data_buf)
    return -1;

  /* Never replay an incomplete transaction. This implementation writes one
   * descriptor followed by its data blocks and a commit block. */
  uint32_t desc_phys = ext2_get_block_num(mnt, j_inode, start_block);
  if (ext2_read_block(mnt, desc_phys, desc_buf) != 0)
    goto incomplete;
  jbd_header_t *desc_header = (jbd_header_t *)desc_buf;
  if (__builtin_bswap32(desc_header->h_magic) != EXT3_JOURNAL_MAGIC_NUMBER ||
      __builtin_bswap32(desc_header->h_blocktype) != JBD_DESCRIPTOR_BLOCK ||
      __builtin_bswap32(desc_header->h_sequence) != sequence)
    goto incomplete;
  uint32_t tag_count = 0;
  for (uint32_t off = sizeof(jbd_header_t);
       off + sizeof(jbd_block_tag_t) <= block_size;
       off += sizeof(jbd_block_tag_t)) {
    jbd_block_tag_t *tag = (jbd_block_tag_t *)(desc_buf + off);
    tag_count++;
    if (__builtin_bswap32(tag->t_flags) & JBD_FLAG_LAST_TAG)
      break;
  }
  uint32_t commit_pos =
      ((start_block + 1 + tag_count - 1) % (journal_blocks - 1)) + 1;
  uint32_t commit_phys = ext2_get_block_num(mnt, j_inode, commit_pos);
  if (ext2_read_block(mnt, commit_phys, data_buf) != 0)
    goto incomplete;
  jbd_header_t *commit = (jbd_header_t *)data_buf;
  if (__builtin_bswap32(commit->h_magic) != EXT3_JOURNAL_MAGIC_NUMBER ||
      __builtin_bswap32(commit->h_blocktype) != JBD_COMMIT_BLOCK ||
      __builtin_bswap32(commit->h_sequence) != sequence)
    goto incomplete;

  uint32_t curr_journal_block = start_block;
  while (1) {
    uint32_t phys_block = ext2_get_block_num(mnt, j_inode, curr_journal_block);
    if (ext2_read_block(mnt, phys_block, desc_buf) != 0)
      break;

    jbd_header_t *header = (jbd_header_t *)desc_buf;
    if (__builtin_bswap32(header->h_magic) != EXT3_JOURNAL_MAGIC_NUMBER)
      break;
    if (__builtin_bswap32(header->h_sequence) != sequence)
      break;

    uint32_t type = __builtin_bswap32(header->h_blocktype);
    if (type == JBD_DESCRIPTOR_BLOCK) {
      uint32_t tag_offset = sizeof(jbd_header_t);
      while (tag_offset < block_size) {
        jbd_block_tag_t *tag = (jbd_block_tag_t *)(desc_buf + tag_offset);
        uint32_t target_block = __builtin_bswap32(tag->t_blocknr);
        uint32_t flags = __builtin_bswap32(tag->t_flags);

        curr_journal_block = (curr_journal_block % (journal_blocks - 1)) + 1;
        uint32_t data_phys =
            ext2_get_block_num(mnt, j_inode, curr_journal_block);
        ext2_read_block(mnt, data_phys, data_buf);

        if (flags & JBD_FLAG_ESCAPE) {
          uint32_t magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
          memcpy(data_buf, &magic, 4);
        }

        klog_puts("       Recovering block: ");
        klog_uint64(target_block);
        klog_puts("\n");
        ext2_write_block(mnt, target_block, data_buf);

        if (flags & JBD_FLAG_LAST_TAG)
          break;
        tag_offset += sizeof(jbd_block_tag_t);
      }
    } else if (type == JBD_COMMIT_BLOCK) {
      sequence++;
    } else {
      break;
    }

    curr_journal_block = (curr_journal_block % (journal_blocks - 1)) + 1;
    if (curr_journal_block == start_block)
      break;
  }

  klog_puts("[EXT3] Journal recovery complete.\n");
  kfree(desc_buf);
  kfree(data_buf);
  return 0;

incomplete:
  klog_puts("[EXT3] Ignoring incomplete journal transaction.\n");
  kfree(desc_buf);
  kfree(data_buf);
  return -1;
}
