#include "ext3.h"
#include "hal/hal.h"
#include "fs/ext2/ext2_internal.h"
#include "console/klog.h"
#include "lib/string.h"
#include "sched/sched.h"
#include "mm/heap.h"
#include "apic/lapic_timer.h"
#include "lock/lockdiag.h"
#include <stdbool.h>

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb,
                                ext2_inode_t *j_inode);

static ext3_journal_state_t *ext3_journal_state(ext2_mount_t *mnt) {
  return &mnt->journal;
}

/* Every journaled mount, so a dead owner can be reclaimed without a global
 * mount-list walk. Populated by ext3_init_journal (mount time, one entry per
 * mount); release path only reads. */
#define JOURNAL_MOUNT_MAX 8
static ext2_mount_t *g_journal_mounts[JOURNAL_MOUNT_MAX];
static unsigned g_journal_mount_count;
static uint64_t g_journal_last_stall_report_ms;

/* Capture transient stalls while they still exist. Several writers can warn
 * about one owner; cap full reports to one per 30 s across every mount. No
 * journal/cache/wait-queue lock may be held by this caller. */
static void ext3_report_stall(void) {
  uint64_t now = lapic_timer_get_ms();
  uint64_t previous = __atomic_load_n(&g_journal_last_stall_report_ms,
                                     __ATOMIC_RELAXED);
  if ((previous && (now <= previous || now - previous < 30000)) ||
      !__atomic_compare_exchange_n(&g_journal_last_stall_report_ms, &previous,
                                   now, false, __ATOMIC_RELAXED,
                                   __ATOMIC_RELAXED))
    return;
  lockdiag_dump_all("JBD2 transaction wait exceeded 5 seconds");
}

static void journal_register_mount(ext2_mount_t *mnt) {
  for (unsigned i = 0; i < g_journal_mount_count; i++) {
    if (g_journal_mounts[i] == mnt)
      return;
  }
  unsigned slot = __atomic_fetch_add(&g_journal_mount_count, 1, __ATOMIC_RELAXED);
  if (slot < JOURNAL_MOUNT_MAX)
    __atomic_store_n(&g_journal_mounts[slot], mnt, __ATOMIC_RELEASE);
}

/* Cumulative independent atomic samples: no journal lock or I/O on read. */
bool ext3_perf_snapshot(unsigned index, struct block_device **dev,
                         struct storage_latency *wait,
                         struct storage_latency *hold) {
  if (index >= JOURNAL_MOUNT_MAX || index >=
      __atomic_load_n(&g_journal_mount_count, __ATOMIC_ACQUIRE))
    return false;
  ext2_mount_t *mnt = __atomic_load_n(&g_journal_mounts[index], __ATOMIC_ACQUIRE);
  if (!mnt) return false;
  *dev = mnt->dev;
  storage_latency_snapshot(&mnt->journal.perf_wait, wait);
  storage_latency_snapshot(&mnt->journal.perf_hold, hold);
  return true;
}

static void ext3_grant_next(ext3_journal_state_t *trans);
static void ext3_discard_write_set(ext2_mount_t *mnt,
                                   ext3_journal_state_t *trans);

/* Abort a transaction whose owner will never finish it, or hand off a grant
 * whose receiver died before consuming it. Staged images are discarded; the
 * filesystem stays consistent and live waiters proceed. */
void ext3_journal_release_owner(uint32_t tid) {
  if (!tid)
    return;
  unsigned n = __atomic_load_n(&g_journal_mount_count, __ATOMIC_ACQUIRE);
  if (n > JOURNAL_MOUNT_MAX)
    n = JOURNAL_MOUNT_MAX;
  for (unsigned i = 0; i < n; i++) {
    ext2_mount_t *mnt = __atomic_load_n(&g_journal_mounts[i], __ATOMIC_ACQUIRE);
    if (!mnt)
      continue;
    ext3_journal_state_t *trans = &mnt->journal;
    if (!__atomic_load_n(&trans->active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&trans->owner_tid, __ATOMIC_RELAXED) != tid)
      continue;

    /* The previous owner finished and handed the still-held lock to this
     * thread, which died before its ext3_journal_start() consumed the grant.
     * Nothing was staged; pass ownership on. */
    rawspinlock_acquire(&trans->sched_lock);
    bool pending_grant = trans->granted_tid == tid;
    if (pending_grant)
      trans->granted_tid = 0;
    rawspinlock_release(&trans->sched_lock);
    if (pending_grant) {
      klogf("[JBD2] granted owner tid=%u died before starting, handing off\n",
            tid);
      ext3_grant_next(trans);
      continue;
    }

    /* Owner recorded but lock free means it finished concurrently; take and
     * return avoids stealing a new owner's lock. */
    if (rawspinlock_try_acquire(&trans->lock)) {
      rawspinlock_release(&trans->lock);
      continue;
    }
    klogf("[JBD2] owner tid=%u died holding the transaction, aborting "
          "(depth=%u blocks=%u phase=%s)\n",
          tid, trans->depth, trans->blocks_in_trans,
          trans->phase ? trans->phase : "unknown");
    storage_latency_record(&trans->perf_hold,
        lapic_timer_get_ms() - trans->started_ms, true);
    ext3_discard_write_set(mnt, trans);
    trans->phase = "aborted-owner-died";
    trans->depth = 0;
    trans->blocks_in_trans = 0;
    trans->io_failed = true;
    ext3_grant_next(trans);
  }
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

static uint32_t ext3_tag_capacity(const ext2_mount_t *mnt) {
  if (mnt->block_size <= sizeof(jbd_header_t) + sizeof(jbd_block_tag_t))
    return 0;
  return (mnt->block_size - sizeof(jbd_header_t)) / sizeof(jbd_block_tag_t);
}

/* Logical journal position of the descriptor block that carries write-set
 * entry `index`. Descriptors are interleaved before the payloads they
 * describe: [desc0][payloads0][desc1][payloads1]... */
static uint32_t ext3_desc_pos(uint32_t index, uint32_t tagcap) {
  return 1 + (index / tagcap) * (tagcap + 1);
}

/* Logical journal position of the payload copy of entry `index`. */
static uint32_t ext3_payload_pos(uint32_t index, uint32_t tagcap) {
  return 2 + index + index / tagcap;
}

/* Consume a pending ownership grant for this tid. The grant keeps the
 * transaction lock held and `active` true across the handoff. */
static bool ext3_take_grant(ext3_journal_state_t *trans, uint32_t tid) {
  if (!tid)
    return false;
  if (__atomic_load_n(&trans->granted_tid, __ATOMIC_ACQUIRE) != tid)
    return false;
  rawspinlock_acquire(&trans->sched_lock);
  bool mine = trans->granted_tid == tid;
  if (mine)
    trans->granted_tid = 0;
  rawspinlock_release(&trans->sched_lock);
  return mine;
}

static void ext3_discard_write_set(ext2_mount_t *mnt,
                                   ext3_journal_state_t *trans) {
  spinlock_acquire(&mnt->cache_lock);
  ext3_txn_chunk_t *chunk = trans->txn_head;
  trans->txn_head = NULL;
  trans->txn_tail = NULL;
  trans->txn_count = 0;
  spinlock_release(&mnt->cache_lock);

  while (chunk) {
    ext3_txn_chunk_t *next = chunk->next;
    for (uint32_t i = 0; i < chunk->count; i++)
      kfree(chunk->entries[i].image);
    kfree(chunk);
    chunk = next;
  }
}

static void ext3_publish_grant(struct thread *next, void *arg) {
  ext3_journal_state_t *trans = arg;
  trans->started_ms = lapic_timer_get_ms();
  trans->owner_ip = 0; /* The recipient has not entered an operation yet. */
  memcpy(trans->owner_comm, next->comm, sizeof(trans->owner_comm));
  trans->owner_comm[sizeof(trans->owner_comm) - 1] = 0;
  trans->phase = "handoff";
  __atomic_store_n(&trans->owner_tid, next->tid, __ATOMIC_RELAXED);
  __atomic_store_n(&trans->granted_tid, next->tid, __ATOMIC_RELEASE);
}

/* Called with trans->lock held. Ownership moves to the oldest waiter; the
 * lock stays continuously held so a fresh writer cannot barge in. With no
 * waiters the transaction goes idle and the lock is released. */
static void ext3_grant_next(ext3_journal_state_t *trans) {
  /* Serialize selection as well as publication with an interrupted waiter's
   * grant check. Once it removes its entry and takes sched_lock, no grant
   * selected before that removal may still be waiting to be published. */
  hal_irq_state_t irq_flags = hal_irq_save();
  rawspinlock_acquire(&trans->sched_lock);
  if (!wait_queue_handoff(&trans->waiters, ext3_publish_grant, trans)) {
    __atomic_store_n(&trans->owner_tid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&trans->active, false, __ATOMIC_RELEASE);
    trans->phase = "idle";
    rawspinlock_release(&trans->lock);
  }
  rawspinlock_release(&trans->sched_lock);
  hal_irq_restore(irq_flags);
}

static int ext3_finish_transaction(ext2_mount_t *mnt,
                                   ext3_journal_state_t *trans, int result) {
  storage_latency_record(&trans->perf_hold,
      lapic_timer_get_ms() - trans->started_ms, result != 0);
  ext3_discard_write_set(mnt, trans);
  /* Do not leave a finished transaction reserved while this CPU schedules
   * something else before the successor is selected and woken. Allocation
   * cleanup above stays outside this short IRQ-masked handoff. */
  hal_irq_state_t irq_flags = hal_irq_save();
  trans->depth = 0;
  trans->blocks_in_trans = 0;
  trans->phase = "handoff-pending";
  ext3_grant_next(trans);
  hal_irq_restore(irq_flags);
  sched_critical_exit();
  return result;
}

void ext3_init_journal(ext2_mount_t *mnt) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  rawspinlock_init(&trans->lock);
  wait_queue_init(&trans->waiters);
  memset(&trans->perf_wait, 0, sizeof(trans->perf_wait));
  memset(&trans->perf_hold, 0, sizeof(trans->perf_hold));
  trans->started_ms = trans->owner_ip = 0;
  trans->metadata_submitted = trans->metadata_unchanged = 0;
  trans->owner_comm[0] = 0;
  trans->phase = "idle";
  trans->active = false;
  trans->owner_tid = 0;
  trans->depth = 0;
  trans->io_failed = false;
  trans->journal_blocks = 0;
  trans->txn_head = trans->txn_tail = NULL;
  trans->txn_count = 0;
  rawspinlock_init(&trans->sched_lock);
  trans->granted_tid = 0;
  memset(trans->map_keys, 0, sizeof(trans->map_keys));
  if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
    return;
  }
  journal_register_mount(mnt);

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
  if (__atomic_load_n(&trans->active, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&trans->owner_tid, __ATOMIC_RELAXED) == tid) {
    trans->depth++;
    return 0;
  }

  /* Protect acquisition too: FIFO handoff reserves the mount for a woken
   * waiter before it resumes to consume the grant. A low-priority recipient
   * must not strand that reservation behind unrelated foreground work. */
  sched_critical_enter();

  /*
   * Only one thread may own the transaction at a time; it holds ownership
   * from this call until the matching stop().  Waiters queue in arrival
   * order and the finishing owner hands the (still held) lock to the oldest
   * one, so a stream of fresh short transactions cannot starve a blocked
   * writer the way a free-for-all try-acquire could.
   */
  uint64_t wait_start = lapic_timer_get_ms();
  bool warned = false;
  wait_queue_entry_t entry = {.thread = current};

  for (;;) {
    if (ext3_take_grant(trans, tid))
      break;
    if (!__atomic_load_n(&trans->active, __ATOMIC_ACQUIRE) &&
        rawspinlock_try_acquire(&trans->lock))
      break;

    if (!warned && lapic_timer_get_ms() - wait_start >= 5000) {
      klogf("[JBD2] transaction wait tid=%u owner=%u comm='%s' active=%u "
            "depth=%u blocks=%u hold_ms=%llu phase=%s owner_ip=%llx "
            "submitted=%llu unchanged=%llu granted=%u\n",
            tid, trans->owner_tid, trans->owner_comm, trans->active,
            trans->depth, trans->blocks_in_trans,
            (unsigned long long)(lapic_timer_get_ms() - trans->started_ms),
            trans->phase ? trans->phase : "unknown",
            (unsigned long long)trans->owner_ip,
            (unsigned long long)trans->metadata_submitted,
            (unsigned long long)trans->metadata_unchanged,
            __atomic_load_n(&trans->granted_tid, __ATOMIC_ACQUIRE));
      warned = true;
      ext3_report_stall();
    }
    if (!current) {
      sched_yield(); /* early boot has no scheduler thread to park */
      continue;
    }
    /* Publish blocked state before registering, then recheck the grant and
     * the lock. An unlock before registration is caught by the recheck; a
     * handoff after it wakes this entry. */
    hal_irq_state_t irq_flags = hal_irq_save();
    current->wakeup_ticks = lapic_timer_get_ticks() + 5000;
    current->state = THREAD_BLOCKED;
    wait_queue_add_tail(&trans->waiters, &entry);
    bool acquired = ext3_take_grant(trans, tid) ||
                    (!__atomic_load_n(&trans->active, __ATOMIC_ACQUIRE) &&
                     rawspinlock_try_acquire(&trans->lock));
    if (acquired) {
      current->wakeup_ticks = 0;
      current->state = THREAD_RUNNING;
    }
    hal_irq_restore(irq_flags);
    if (!acquired) {
      /* The old owner can decide to release with an empty queue in the window
       * between our registration and this recheck; nobody would ever grant us
       * then.  Never park while active is false: retry instead, the lock is
       * released momentarily. */
      if (!__atomic_load_n(&trans->active, __ATOMIC_ACQUIRE)) {
        current->wakeup_ticks = 0;
        current->state = THREAD_RUNNING;
        wait_queue_remove(&trans->waiters, &entry);
        sched_yield();
        continue;
      }
      sched_yield();
    }
    current->wakeup_ticks = 0;
    current->state = THREAD_RUNNING;
    wait_queue_remove(&trans->waiters, &entry);
    if (acquired)
      break;
    /* A SIGKILLed waiter must die even while the journal is wedged (e.g.
     * owner died before the reaper reclaims it). Otherwise kill -9 loops
     * forever on the 5 s safety wake and shutdown hangs with it. */
    if (thread_has_pending_signal(current)) {
      /* A signal can wake us at the same time stop() hands us ownership.
       * We have removed our queue entry, but a handoff may already have
       * selected it. Wait for publication, then forward that unused grant;
       * returning EINTR with it would leave active=true, depth=0 forever. */
      hal_irq_state_t handoff_flags = hal_irq_save();
      rawspinlock_acquire(&trans->sched_lock);
      bool granted = trans->granted_tid == tid;
      if (granted)
        trans->granted_tid = 0;
      rawspinlock_release(&trans->sched_lock);
      if (granted)
        ext3_grant_next(trans);
      hal_irq_restore(handoff_flags);
      storage_latency_record(&trans->perf_wait,
          lapic_timer_get_ms() - wait_start, true);
      sched_critical_exit();
      return -4;
    } /* EINTR */
  }
  storage_latency_record(&trans->perf_wait,
      lapic_timer_get_ms() - wait_start, false);

  /* A granted waiter already owns trans->lock; a fresh acquirer just took it
   * after seeing active == false. */
  __atomic_store_n(&trans->owner_tid, tid, __ATOMIC_RELAXED);
  trans->started_ms = lapic_timer_get_ms();
  trans->owner_ip = (uint64_t)__builtin_return_address(0);
  trans->phase = "operation";
  memset(trans->owner_comm, 0, sizeof(trans->owner_comm));
  if (current)
    memcpy(trans->owner_comm, current->comm, sizeof(trans->owner_comm) - 1);
  __atomic_store_n(&trans->active, true, __ATOMIC_RELEASE);
  trans->depth = 1;
  trans->blocks_in_trans = 0;
  trans->io_failed = false;
  trans->start_block = 1;
  /* The previous owner discards its write-set before granting, so this is
   * empty; discard defensively rather than commit another writer's images. */
  if (trans->txn_count)
    ext3_discard_write_set(mnt, trans);

  return 0;
}

int ext3_journal_block(ext2_mount_t *mnt, uint32_t block_nr, const void *data) {
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  if (!trans->active)
    return ext2_write_block(mnt, block_nr, data);
  struct thread *current = sched_get_current();
  uint32_t tid = current ? current->tid : 0;
  if (__atomic_load_n(&trans->owner_tid, __ATOMIC_RELAXED) != tid ||
      trans->io_failed)
    return -1;

  if (block_nr == 0) {
    trans->io_failed = true;
    return -1;
  }
  const char *previous_phase = trans->phase;
  trans->metadata_submitted++;

  /* Already in the write-set: keep the latest image only. This is the common
   * allocator pattern (the same bitmap and descriptor blocks are restaged for
   * every allocated block), and collapsing it removes the payload+home write
   * pair per repetition. */
  spinlock_acquire(&mnt->cache_lock);
  for (ext3_txn_chunk_t *chunk = trans->txn_head; chunk; chunk = chunk->next) {
    for (uint32_t i = 0; i < chunk->count; i++) {
      if (chunk->entries[i].block != block_nr)
        continue;
      if (!memcmp(chunk->entries[i].image, data, mnt->block_size))
        trans->metadata_unchanged++;
      else
        memcpy(chunk->entries[i].image, data, mnt->block_size);
      spinlock_release(&mnt->cache_lock);
      return 0;
    }
  }
  spinlock_release(&mnt->cache_lock);

  /* A completed coherent cache image equal to this metadata needs no new
   * journal or home write. In-flight writes and cache collisions are never
   * evidence of an unchanged disk block. */
  uint32_t idx = block_nr % EXT2_CACHE_SIZE;
  spinlock_acquire(&mnt->cache_lock);
  bool unchanged = mnt->cache[idx].num == block_nr &&
                   mnt->cache[idx].data && !mnt->cache[idx].writes_inflight &&
                   memcmp(mnt->cache[idx].data, data, mnt->block_size) == 0;
  spinlock_release(&mnt->cache_lock);
  if (unchanged) {
    trans->metadata_unchanged++;
    return 0;
  }

  uint32_t tagcap = ext3_tag_capacity(mnt);
  uint32_t index = trans->txn_count;
  if (!tagcap || index >= (uint32_t)EXT3_JOURNAL_SEG_MAX * tagcap) {
    trans->io_failed = true;
    return -1;
  }
  uint32_t payload_pos = ext3_payload_pos(index, tagcap);
  uint32_t desc_pos = ext3_desc_pos(index, tagcap);
  uint32_t segments = index / tagcap + 1;
  uint32_t commit_pos = 2 + (index + 1) + segments - 1;
  if (!ext3_journal_phys(mnt, payload_pos) ||
      !ext3_journal_phys(mnt, desc_pos) ||
      !ext3_journal_phys(mnt, commit_pos)) {
    trans->io_failed = true;
    return -1;
  }

  uint8_t *image = kmalloc(mnt->block_size);
  if (!image) {
    trans->io_failed = true;
    return -1;
  }
  memcpy(image, data, mnt->block_size);

  ext3_txn_chunk_t *chunk = NULL;
  if (index % EXT3_TXN_CHUNK_ENTRIES == 0) {
    chunk = kmalloc(sizeof(*chunk));
    if (!chunk) {
      kfree(image);
      trans->io_failed = true;
      return -1;
    }
    chunk->next = NULL;
    chunk->count = 0;
  }

  spinlock_acquire(&mnt->cache_lock);
  if (chunk) {
    if (trans->txn_tail)
      trans->txn_tail->next = chunk;
    else
      trans->txn_head = chunk;
    trans->txn_tail = chunk;
  }
  ext3_txn_entry_t *slot = &trans->txn_tail->entries[trans->txn_tail->count++];
  slot->block = block_nr;
  slot->payload_pos = payload_pos;
  slot->desc_pos = desc_pos;
  slot->image = image;
  trans->txn_count = index + 1;
  spinlock_release(&mnt->cache_lock);

  trans->blocks_in_trans = index + 1;
  trans->phase = previous_phase;
  return 0;
}

bool ext3_journal_read_staged(ext2_mount_t *mnt, uint32_t block_nr,
                              void *buffer) {
  if (!mnt || !mnt->block_size || !block_nr || !buffer)
    return false;
  ext3_journal_state_t *trans = ext3_journal_state(mnt);
  bool found = false;
  spinlock_acquire(&mnt->cache_lock);
  for (ext3_txn_chunk_t *chunk = trans->txn_head; chunk && !found;
       chunk = chunk->next) {
    for (uint32_t i = 0; i < chunk->count; i++) {
      if (chunk->entries[i].block != block_nr || !chunk->entries[i].image)
        continue;
      memcpy(buffer, chunk->entries[i].image, mnt->block_size);
      found = true;
      break;
    }
  }
  spinlock_release(&mnt->cache_lock);
  return found;
}

/*
 * Commit I/O batching.  Journal payloads and checkpoint home writes are sets
 * of independent whole blocks, so each set is gathered and handed to the
 * block layer as one batch: a driver with pipelined I/O keeps several commands
 * in flight per submitter, while every other device runs the members through
 * the single-request path in order.  Ordering barriers stay explicit - a
 * batch is always fully waited before the next commit step starts.
 */
#define EXT3_COMMIT_WRITE_BATCH 16u

/* Write one gathered batch and reset the count.  Returns 0 when every member
 * reached its device block, -1 otherwise. */
static int ext3_commit_io_batch(ext2_mount_t *mnt, ext2_block_write_t *batch,
                                uint32_t *count) {
  if (*count == 0)
    return 0;
  uint32_t n = *count;
  *count = 0;
  return ext2_write_blocks_batch(mnt, batch, n);
}

/* Like ext3_commit_io_batch(), but reports the first failed block (in
 * write-set order) so the metadata-home diagnostic keeps its meaning. */
static int ext3_commit_home_batch(ext2_mount_t *mnt, ext2_block_write_t *batch,
                                  uint32_t *count, uint32_t *failed_block) {
  if (*count == 0)
    return 0;
  uint32_t n = *count;
  *count = 0;
  if (ext2_write_blocks_batch(mnt, batch, n) == 0)
    return 0;
  for (uint32_t i = 0; i < n; i++) {
    if (batch[i].result != 0) {
      *failed_block = batch[i].block_num;
      break;
    }
  }
  return -1;
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
  struct thread *current = sched_get_current();
  uint32_t tid = current ? current->tid : 0;
  if (__atomic_load_n(&trans->owner_tid, __ATOMIC_RELAXED) != tid)
    return -1;
  if (trans->depth > 1) {
    trans->depth--;
    return 0;
  }

  trans->phase = "commit-metadata";
  /* All group-descriptor and superblock changes made by this transaction are
   * staged into the write-set first, so recovery replays them together with
   * the bitmap blocks in one descriptor set. */
  if (trans->io_failed || ext2_flush_metadata(mnt) != 0 || trans->io_failed)
    return ext3_finish_transaction(mnt, trans, -1);

  /* Read-only nested helpers and unchanged chown calls need no log records. */
  uint32_t n = trans->txn_count;
  if (n == 0)
    return ext3_finish_transaction(mnt, trans, 0);

  uint32_t tagcap = ext3_tag_capacity(mnt);
  if (!tagcap)
    return ext3_finish_transaction(mnt, trans, -1);
  uint32_t segments = (n + tagcap - 1) / tagcap;
  uint32_t commit_pos = ext3_payload_pos(n - 1, tagcap) + 1;
  uint32_t commit_phys = ext3_journal_phys(mnt, commit_pos);
  uint32_t sb_phys = ext3_journal_phys(mnt, 0);
  if (segments > EXT3_JOURNAL_SEG_MAX || !commit_phys || !sb_phys) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }

  /* Reuse scratch storage under the mount's transaction lock. The journal
   * superblock is still reread for each commit; caching its contents could
   * hide changes after an I/O failure. */
  trans->phase = "commit-scratch";
  if (!trans->desc_block_buf)
    trans->desc_block_buf = kmalloc(mnt->block_size);
  if (!trans->commit_block_buf)
    trans->commit_block_buf = kmalloc(mnt->block_size);
  if (!trans->super_block_buf)
    trans->super_block_buf = kmalloc(mnt->block_size);
  if (!trans->desc_block_buf || !trans->commit_block_buf ||
      !trans->super_block_buf) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }

  /* Journal payloads first: each unique write-set block is copied once, no
   * matter how many times it was restaged during the transaction. A payload
   * whose first word is the journal magic is escaped (zeroed in the log copy,
   * restored by recovery) so log scans never mistake it for a header.  The
   * independent payload blocks are gathered into batches so a pipelined
   * driver keeps more than one command in flight per submitter. */
  trans->phase = "commit-payload";
  ext2_block_write_t payload_batch[EXT3_COMMIT_WRITE_BATCH];
  uint32_t payload_batch_count = 0;
  bool payload_failed = false;
  for (ext3_txn_chunk_t *chunk = trans->txn_head; chunk && !payload_failed;
       chunk = chunk->next) {
    for (uint32_t i = 0; i < chunk->count; i++) {
      ext3_txn_entry_t *entry = &chunk->entries[i];
      uint32_t phys = ext3_journal_phys(mnt, entry->payload_pos);
      if (!phys) {
        payload_failed = true;
        break;
      }
      if (*(const uint32_t *)entry->image ==
          __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER)) {
        /* The escaped copy uses the shared descriptor scratch, so an
         * in-flight batch must drain before it is overwritten. */
        if (ext3_commit_io_batch(mnt, payload_batch, &payload_batch_count) !=
            0) {
          payload_failed = true;
          break;
        }
        memcpy(trans->desc_block_buf, entry->image, mnt->block_size);
        memset(trans->desc_block_buf, 0, sizeof(uint32_t));
        if (ext2_write_block(mnt, phys, trans->desc_block_buf) != 0) {
          payload_failed = true;
          break;
        }
        continue;
      }
      payload_batch[payload_batch_count].block_num = phys;
      payload_batch[payload_batch_count].buffer = entry->image;
      payload_batch_count++;
      if (payload_batch_count == EXT3_COMMIT_WRITE_BATCH &&
          ext3_commit_io_batch(mnt, payload_batch, &payload_batch_count) != 0) {
        payload_failed = true;
        break;
      }
    }
  }
  if (!payload_failed &&
      ext3_commit_io_batch(mnt, payload_batch, &payload_batch_count) != 0)
    payload_failed = true;
  if (payload_failed) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }

  /* One descriptor per tag-capacity group, each terminated by LAST_TAG. */
  trans->phase = "commit-descriptor-build";
  static const char *const descriptor_stages[4] = {
      "commit-descriptor-cache-alloc", "commit-descriptor-cache-publish",
      "commit-descriptor-io", "commit-descriptor-cache-finish"};
  uint32_t index = 0;
  ext3_txn_chunk_t *chunk = trans->txn_head;
  uint32_t chunk_index = 0;
  while (index < n) {
    trans->phase = "commit-descriptor-build";
    uint8_t *desc = trans->desc_block_buf;
    memset(desc, 0, mnt->block_size);
    jbd_header_t *dh = (jbd_header_t *)desc;
    dh->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
    dh->h_blocktype = __builtin_bswap32(JBD_DESCRIPTOR_BLOCK);
    dh->h_sequence = __builtin_bswap32(trans->sequence);

    uint32_t in_segment = 0;
    while (in_segment < tagcap && index < n) {
      while (chunk && chunk_index == chunk->count) {
        chunk = chunk->next;
        chunk_index = 0;
      }
      if (!chunk) {
        trans->io_failed = true;
        return ext3_finish_transaction(mnt, trans, -1);
      }
      ext3_txn_entry_t *entry = &chunk->entries[chunk_index++];
      jbd_block_tag_t *tag = (jbd_block_tag_t *)(desc + sizeof(jbd_header_t) +
          in_segment * sizeof(jbd_block_tag_t));
      tag->t_blocknr = __builtin_bswap32(entry->block);
      tag->t_flags = 0;
      if (*(const uint32_t *)entry->image ==
          __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER))
        tag->t_flags |= __builtin_bswap32(JBD_FLAG_ESCAPE);
      index++;
      in_segment++;
      if (in_segment == tagcap || index == n)
        tag->t_flags |= __builtin_bswap32(JBD_FLAG_LAST_TAG);
    }
    trans->phase = "commit-descriptor-map";
    uint32_t desc_phys = ext3_journal_phys(mnt, ext3_desc_pos(index - 1, tagcap));
    if (!desc_phys || ext2_write_block_traced(mnt, desc_phys, desc,
                            &trans->phase, descriptor_stages) != 0) {
      trans->io_failed = true;
      return ext3_finish_transaction(mnt, trans, -1);
    }
  }

  uint8_t *commit_buf = trans->commit_block_buf;
  memset(commit_buf, 0, mnt->block_size);
  jbd_header_t *ch = (jbd_header_t *)commit_buf;
  ch->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
  ch->h_blocktype = __builtin_bswap32(JBD_COMMIT_BLOCK);
  ch->h_sequence = __builtin_bswap32(trans->sequence);
  trans->phase = "commit-record";
  if (ext2_write_block(mnt, commit_phys, commit_buf) != 0) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }

  /* Publish the log head before any home block is written: a crash from here
   * on is recovered by replaying the images above. */
  uint8_t *sb_buf = trans->super_block_buf;
  trans->phase = "commit-super-read";
  if (ext2_read_block(mnt, sb_phys, sb_buf) != 0) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }
  jbd_superblock_t *jsb = (jbd_superblock_t *)sb_buf;
  jsb->s_start = __builtin_bswap32(trans->start_block);
  trans->phase = "commit-super-publish";
  if (ext2_write_block(mnt, sb_phys, sb_buf) != 0) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }

  /* Checkpoint: one home write for each unique block.  These blocks are
   * independent, so they are gathered into batches like the payloads; a
   * failure reports the first affected block and still retires the log. */
  bool home_failed = false;
  uint32_t failed_block = 0;
  ext2_block_write_t home_batch[EXT3_COMMIT_WRITE_BATCH];
  uint32_t home_batch_count = 0;
  for (chunk = trans->txn_head; chunk && !home_failed; chunk = chunk->next) {
    for (uint32_t i = 0; i < chunk->count; i++) {
      ext3_txn_entry_t *entry = &chunk->entries[i];
      trans->phase = "metadata-home";
      home_batch[home_batch_count].block_num = entry->block;
      home_batch[home_batch_count].buffer = entry->image;
      home_batch_count++;
      if (home_batch_count == EXT3_COMMIT_WRITE_BATCH &&
          ext3_commit_home_batch(mnt, home_batch, &home_batch_count,
                                 &failed_block) != 0) {
        home_failed = true;
        break;
      }
    }
  }
  if (!home_failed &&
      ext3_commit_home_batch(mnt, home_batch, &home_batch_count,
                             &failed_block) != 0)
    home_failed = true;

  /* Retire the committed sequence even when a home write failed: the log
   * record is complete, and leaving s_start set would make every later
   * transaction overwrite the same replay region. The failed block is
   * reported to the caller and its dirty state stays recorded for retry. */
  jsb->s_sequence = __builtin_bswap32(trans->sequence + 1);
  jsb->s_start = 0;
  trans->phase = "commit-super-clear";
  if (ext2_write_block(mnt, sb_phys, sb_buf) != 0) {
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }
  trans->sequence++;
  if (home_failed) {
    klogf("[JBD2] home checkpoint failed after commit, journal retired "
          "block=%u sequence=%u\n", failed_block, trans->sequence - 1);
    trans->io_failed = true;
    return ext3_finish_transaction(mnt, trans, -1);
  }
  return ext3_finish_transaction(mnt, trans, 0);
}

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb,
                                ext2_inode_t *j_inode) {
  uint32_t block_size = mnt->block_size;
  uint32_t start_block = __builtin_bswap32(jsb->s_start);
  uint32_t sequence = __builtin_bswap32(jsb->s_sequence);
  uint32_t journal_blocks = __builtin_bswap32(jsb->s_maxlen);
  uint32_t tagcap = ext3_tag_capacity(mnt);

  klog_puts("[EXT3] Starting journal recovery...\n");

  if (!start_block || !tagcap || journal_blocks < 4 ||
      start_block >= journal_blocks)
    return -1;

  uint8_t *desc_buf = kmalloc(block_size);
  uint8_t *data_buf = kmalloc(block_size);
  if (!desc_buf || !data_buf) {
    kfree(desc_buf);
    kfree(data_buf);
    return -1;
  }

  /* The writer publishes s_start only after the commit record is durable, so
   * a transaction found here should be complete. Still validate the whole
   * record before touching a home block: a partly written descriptor set with
   * no commit record must be ignored, not replayed. */
  uint32_t pos = start_block;
  uint32_t recovered = 0;
  while (pos < journal_blocks) {
    uint32_t scan = pos;
    bool complete = false;
    for (;;) {
      if (scan >= journal_blocks)
        goto incomplete;
      uint32_t phys = ext2_get_block_num(mnt, j_inode, scan);
      if (!phys || ext2_read_block(mnt, phys, desc_buf) != 0)
        goto incomplete;
      jbd_header_t *h = (jbd_header_t *)desc_buf;
      if (__builtin_bswap32(h->h_magic) != EXT3_JOURNAL_MAGIC_NUMBER ||
          __builtin_bswap32(h->h_sequence) != sequence)
        goto incomplete;
      uint32_t type = __builtin_bswap32(h->h_blocktype);
      if (type == JBD_COMMIT_BLOCK) {
        complete = true;
        break;
      }
      if (type != JBD_DESCRIPTOR_BLOCK)
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
      if (!tag_count || tag_count > tagcap)
        goto incomplete;
      scan += 1 + tag_count;
    }
    if (!complete)
      goto incomplete;

    /* The commit record exists: replay every descriptor group, then its
     * payloads, to their home blocks. */
    uint32_t cursor = pos;
    while (cursor != scan) {
      uint32_t desc_phys = ext2_get_block_num(mnt, j_inode, cursor);
      if (!desc_phys || ext2_read_block(mnt, desc_phys, desc_buf) != 0)
        goto incomplete;
      uint32_t next = cursor + 1;
      for (uint32_t off = sizeof(jbd_header_t);
           off + sizeof(jbd_block_tag_t) <= block_size;
           off += sizeof(jbd_block_tag_t)) {
        jbd_block_tag_t *tag = (jbd_block_tag_t *)(desc_buf + off);
        uint32_t target_block = __builtin_bswap32(tag->t_blocknr);
        uint32_t flags = __builtin_bswap32(tag->t_flags);

        uint32_t data_phys = ext2_get_block_num(mnt, j_inode, next);
        if (!data_phys || ext2_read_block(mnt, data_phys, data_buf) != 0)
          goto incomplete;
        next++;

        if (flags & JBD_FLAG_ESCAPE) {
          uint32_t magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
          memcpy(data_buf, &magic, 4);
        }

        klog_puts("       Recovering block: ");
        klog_uint64(target_block);
        klog_puts("\n");
        if (target_block && target_block < mnt->sb.s_blocks_count &&
            ext2_write_block(mnt, target_block, data_buf) != 0)
          goto incomplete;

        if (flags & JBD_FLAG_LAST_TAG)
          break;
      }
      cursor = next;
    }

    recovered++;
    pos = scan + 1;
    sequence++;
  }

  klog_puts("[EXT3] Journal recovery complete.\n");
  kfree(desc_buf);
  kfree(data_buf);
  return recovered ? 0 : -1;

incomplete:
  kfree(desc_buf);
  kfree(data_buf);
  if (recovered) {
    /* Replaying the committed transactions is the goal; the block after the
     * last commit is stale data from an older log generation. */
    klog_puts("[EXT3] Journal recovery complete.\n");
    return 0;
  }
  klog_puts("[EXT3] Ignoring incomplete journal transaction.\n");
  return -1;
}
