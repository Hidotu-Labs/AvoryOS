/* Host check of real ext2 dirty-group tracking and metadata writeback.
 * Fake journal I/O counts and validates submitted blocks, with failure and
 * concurrent-mark injection. No real disks or kernel IRQ instructions are used.
 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef strchr
#undef strrchr
#undef strstr
#define LOCK_SPINLOCK_H

/* Zero-initialized host locks model kernel bucket exclusion without cli/sti. */
typedef struct {
  volatile uint32_t locked;
  uint64_t saved_flags;
} spinlock_t;
typedef struct {
  volatile uint32_t locked;
} rawspinlock_t;
#define SPINLOCK_INIT {0}
#define RAWSPINLOCK_INIT {0}
static void spinlock_init(spinlock_t *s) { s->locked = 0; }
static void spinlock_acquire(spinlock_t *s) {
  while (__atomic_exchange_n(&s->locked, 1, __ATOMIC_ACQUIRE)) {
  }
}
static void spinlock_release(spinlock_t *s) {
  __atomic_store_n(&s->locked, 0, __ATOMIC_RELEASE);
}

#define sched_yield kernel_sched_yield
#ifdef TEST_METADATA_SOURCE
#include TEST_METADATA_SOURCE
#else
#include "../../kernel/src/fs/ext2/ext2_block.c"
#endif
#undef sched_yield

void kernel_sched_yield(void) { sched_yield(); }
struct thread *sched_get_current(void) { return NULL; }
void klogf(const char *fmt, ...) { (void)fmt; }
static int allocation_failure;
void *kmalloc(size_t n) { return allocation_failure ? NULL : malloc(n); }
void kfree(void *p) { free(p); }
int ext3_journal_start(ext2_mount_t *mnt) {
  (void)mnt;
  return 0;
}
int ext3_journal_stop(ext2_mount_t *mnt) {
  (void)mnt;
  return 0;
}
static unsigned writes;
static uint32_t written[256];
static int fail_block = -1, mark_during_flush;
static int device_read(struct block_device *dev, uint64_t lba, uint32_t count,
                       void *buf) {
  (void)dev;
  assert(lba == 2 && count == 2);
  memset(buf, 0x77, 1024);
  return 0;
}
int ext3_journal_block(ext2_mount_t *mnt, uint32_t block, const void *buf) {
  assert(writes < 256);
  written[writes++] = block;
  if (block != 1) {
    uint32_t offset = (block - 2) * 1024;
    uint32_t bytes = mnt->groups_count * sizeof(ext2_bgd_t) - offset;
    if (bytes > 1024)
      bytes = 1024;
    assert(!memcmp(buf, (uint8_t *)mnt->bgdt + offset, bytes));
    for (unsigned i = bytes; i < 1024; i++)
      assert(((const unsigned char *)buf)[i] == 0);
  } else {
    assert(!memcmp(buf, &mnt->sb, sizeof(mnt->sb)));
  }
  if (mark_during_flush && block != 1) {
    mark_during_flush = 0;
    ext2_mark_group_dirty(mnt, 64);
  }
  return block == (uint32_t)fail_block ? -1 : 0;
}
static void reset_writes(void) {
  writes = 0;
  memset(written, 0, sizeof(written));
}
int main(void) {
  ext2_mount_t *mnt = calloc(1, sizeof(*mnt));
  assert(mnt);
  struct block_device dev = {.read_sectors = device_read};
  mnt->dev = &dev;
  mnt->block_size = 1024;
  mnt->groups_count = 4096;
  mnt->sb.s_first_data_block = 1;
  mnt->bgdt = calloc(mnt->groups_count, sizeof(ext2_bgd_t));
  assert(mnt->bgdt);
  for (unsigned i = 0; i < mnt->groups_count; i++)
    mnt->bgdt[i].bg_free_blocks_count = i % 8192;
  assert(ext2_flush_metadata(mnt) == 0 && writes == 0);
  ext2_mark_group_dirty(mnt, 1000);
  ext2_mark_group_dirty(mnt, 1001);
  assert(mnt->dirty_bgd_count == 1);
  assert(ext2_flush_metadata(mnt) == 0 && writes == 2);
  assert(written[0] == 2 + 1000 * sizeof(ext2_bgd_t) / 1024 && written[1] == 1);
  puts("PASS sparse counter update: one descriptor + superblock instead of 128 "
       "descriptors + superblock");
  reset_writes();
  ext2_mark_group_dirty(mnt, 0);
  ext2_mark_group_dirty(mnt, 64);
  assert(ext2_flush_metadata(mnt) == 0 && writes == 3);
  puts("PASS duplicate groups deduplicate; distinct descriptor blocks remain "
       "included");
  reset_writes();
  for (unsigned i = 0; i < EXT2_DIRTY_BGD_MAX + 1; i++)
    ext2_mark_group_dirty(mnt, i * 32);
  assert(mnt->bgdt_all_dirty);
  assert(ext2_flush_metadata(mnt) == 0 && writes == 129);
  puts("PASS dirty-list overflow retains every group through full-table "
       "fallback");
  reset_writes();
  ext2_mark_group_dirty(mnt, 0);
  fail_block = 2;
  assert(ext2_flush_metadata(mnt) == -1 && mnt->metadata_dirty &&
         mnt->bgdt_all_dirty);
  fail_block = -1;
  reset_writes();
  assert(ext2_flush_metadata(mnt) == 0 && writes == 129);
  reset_writes();
  allocation_failure = 1;
  ext2_mark_group_dirty(mnt, 0);
  assert(ext2_flush_metadata(mnt) == -1 && mnt->metadata_dirty);
  allocation_failure = 0;
  assert(ext2_flush_metadata(mnt) == 0);
  puts("PASS write and allocation failures retain dirty state for retry");
  reset_writes();
  mark_during_flush = 1;
  ext2_mark_group_dirty(mnt, 0);
  assert(ext2_flush_metadata(mnt) == 0 && mnt->metadata_dirty);
  reset_writes();
  assert(ext2_flush_metadata(mnt) == 0 && writes == 2 && written[0] == 4);
  puts("PASS group marked during flush remains pending for next flush");
  /* A partial last descriptor block must not read beyond the table. */
  mnt->groups_count = 33;
  reset_writes();
  ext2_mark_group_dirty(mnt, 32);
  assert(ext2_flush_metadata(mnt) == 0 && writes == 2 && written[0] == 3);
  puts(
      "PASS partial final descriptor block preserves contents and pads safely");
  free(mnt->bgdt);
  free(mnt);
}
