/* Host tests of real run allocation and checked block mapping. */
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
#ifdef TEST_ALLOC_SOURCE
#include TEST_ALLOC_SOURCE
#else
#include "../../kernel/src/fs/ext2/ext2_block.c"
#endif
#undef sched_yield
#include "../../kernel/src/fs/ext4/ext4_extent.c"


void kernel_sched_yield(void) { sched_yield(); }
struct thread *sched_get_current(void) { return NULL; }
void klogf(const char *fmt, ...) { (void)fmt; }
void klog_puts(const char *text) { (void)text; }
void klog_uint64(uint64_t value) { (void)value; }
int block_flush(struct block_device *dev) { (void)dev; return 0; }
void *kcalloc(size_t count, size_t size) { return calloc(count, size); }
static int fail_alloc, fail_read = -1, fail_write = -1, fail_start, fail_stop;
void *kmalloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
void kfree(void *p) { free(p); }
static unsigned char disk[128][1024];
static unsigned submissions, commits;
static _Thread_local unsigned depth;
static pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;
static int device_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
  (void)dev;
  assert(count == 2 && lba % 2 == 0 && lba / 2 < 128);
  if ((int)(lba / 2) == fail_read) return -1;
  memcpy(buf, disk[lba / 2], 1024);
  return 0;
}
static int device_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
  (void)dev;
  assert(count == 2 && lba % 2 == 0 && lba / 2 < 128);
  if ((int)(lba / 2) == fail_write) return -1;
  memcpy(disk[lba / 2], buf, 1024);
  return 0;
}
int ext3_journal_start(ext2_mount_t *mnt) {
  (void)mnt;
  if (fail_start) return -1;
  if (!depth) assert(pthread_mutex_lock(&transaction_mutex) == 0);
  depth++;
  return 0;
}
int ext3_journal_stop(ext2_mount_t *mnt) {
  (void)mnt;
  assert(depth);
  if (--depth) return 0;
  commits++;
  assert(pthread_mutex_unlock(&transaction_mutex) == 0);
  return fail_stop ? -1 : 0;
}
int ext3_journal_block(ext2_mount_t *mnt, uint32_t block, const void *buf) {
  submissions++;
  return ext2_write_block(mnt, block, buf);
}
static ext2_mount_t *make_mount(void) {
  ext2_mount_t *mnt = calloc(1, sizeof(*mnt));
  assert(mnt);
  static struct block_device dev = {.read_sectors = device_read, .write_sectors = device_write};
  mnt->dev = &dev;
  mnt->block_size = 1024;
  mnt->groups_count = 1;
  mnt->sb.s_blocks_count = 128;
  mnt->sb.s_blocks_per_group = 128;
  mnt->sb.s_first_data_block = 1;
  mnt->sb.s_free_blocks_count = 119;
  mnt->bgdt = calloc(1, sizeof(*mnt->bgdt));
  assert(mnt->bgdt);
  mnt->bgdt[0].bg_block_bitmap = 2;
  mnt->bgdt[0].bg_free_blocks_count = 119;
  memset(disk, 0, sizeof(disk));
  disk[2][0] = 0xff; /* Eight reserved blocks. */
  submissions = commits = depth = 0;
  fail_alloc = fail_start = fail_stop = 0;
  fail_read = fail_write = -1;
  return mnt;
}
static void drop_mount(ext2_mount_t *mnt) {
  for (unsigned i = 0; i < EXT2_CACHE_SIZE; i++) free(mnt->cache[i].data);
  free(mnt->bgdt);
  free(mnt);
}
static void invalidate(ext2_mount_t *mnt, uint32_t block) {
  mnt->cache[block % EXT2_CACHE_SIZE].num = 0;
}
struct run_worker {
  ext2_mount_t *mnt;
  uint32_t starts[6];
};
static void *reserve_runs(void *opaque) {
  struct run_worker *worker = opaque;
  for (unsigned i = 0; i < 6; i++) {
    uint32_t count;
    worker->starts[i] = ext2_alloc_run_hint(worker->mnt, 9, 4, &count);
    assert(worker->starts[i] && count == 4);
  }
  return NULL;
}
int main(void) {
  ext2_mount_t *mnt = make_mount();
  ext2_inode_t extent_inode = {0};
  ext4_extent_init_inode(&extent_inode);
  mnt->inodes_per_group = 32;
  uint64_t extent_start = 0;
  assert(ext4_alloc_extent(mnt, &extent_inode, 1, 0, 4, &extent_start) == 0);
  assert(extent_start == 65 && submissions == 1 && commits == 1);
  assert(extent_inode.i_blocks == 8);
  for (unsigned i = 0; i < 4; i++)
    assert(ext4_get_block_num(mnt, &extent_inode, i) == 65 + i);
  assert(ext4_get_block_num(mnt, &extent_inode, 4) == 0);
  puts("PASS actual ext4 extent allocator reserves four blocks with one bitmap image and correct extent/accounting");
  drop_mount(mnt);
  mnt = make_mount();
  struct run_worker work[4] = {{.mnt = mnt}, {.mnt = mnt}, {.mnt = mnt}, {.mnt = mnt}};
  pthread_t threads[4];
  for (unsigned i = 0; i < 4; i++)
    assert(pthread_create(&threads[i], NULL, reserve_runs, &work[i]) == 0);
  for (unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
  bool seen[128] = {0};
  for (unsigned i = 0; i < 4; i++)
    for (unsigned j = 0; j < 6; j++)
      for (unsigned k = 0; k < 4; k++) {
        uint32_t block = work[i].starts[j] + k;
        assert(block < 128 && !seen[block]);
        seen[block] = true;
      }
  assert(submissions == 24 && commits == 24 && mnt->sb.s_free_blocks_count == 23);
  puts("PASS four concurrent run allocators reserve 96 unique blocks with exact counters");
  drop_mount(mnt);
  mnt = make_mount();
  uint32_t count = 999;
  assert(ext2_alloc_run_hint(mnt, 9, 4, &count) == 9 && count == 4);
  assert(submissions == 1 && commits == 1 && disk[2][1] == 0x0f);
  assert(mnt->bgdt[0].bg_free_blocks_count == 115 && mnt->sb.s_free_blocks_count == 115);
  assert(mnt->metadata_dirty && mnt->dirty_bgd_count == 1);
  assert(ext2_alloc_block_hint(mnt, 13) == 13 && submissions == 2);
  puts("PASS four contiguous blocks use one bitmap submission; legacy single allocation remains valid");
  drop_mount(mnt);

  mnt = make_mount();
  disk[2][1] = 1u << 2; /* Block11 occupied. */
  assert(ext2_alloc_run_hint(mnt, 9, 4, &count) == 9 && count == 2);
  assert(ext2_alloc_run_hint(mnt, 11, 4, &count) == 12 && count == 4);
  drop_mount(mnt);
  mnt = make_mount();
  assert(ext2_alloc_run_hint(mnt, 126, 4, &count) == 126 && count == 2);
  assert(disk[2][15] == 0x60); /* Never allocate physical128/129. */
  assert(ext2_alloc_run_hint(mnt, 126, 4, &count) == 9 && count == 4);
  drop_mount(mnt);
  mnt = make_mount();
  mnt->bgdt[0].bg_free_blocks_count = 0;
  assert(ext2_alloc_run_hint(mnt, 9, 4, &count) == 9 && count == 4);
  puts("PASS fragmentation, final group bounds, goal wrap, and stale counters");
  drop_mount(mnt);

  mnt = make_mount();
  mnt->cache[9].num = 9;
  mnt->cache[10].num = 10;
  mnt->cache[11].num = 11 + EXT2_CACHE_SIZE;
  assert(ext2_alloc_run_hint(mnt, 9, 4, &count) == 9 && count == 4);
  assert(mnt->cache[9].num == 0 && mnt->cache[10].num == 0);
  assert(mnt->cache[11].num == 11 + EXT2_CACHE_SIZE);
  assert(mnt->cache[9].write_seq == 1 && mnt->cache[11].write_seq == 1);
  puts("PASS every reserved block invalidates matching cached data and advances collision generation");
  drop_mount(mnt);

  for (int which = 0; which < 5; which++) {
    mnt = make_mount();
    if (which == 0) fail_start = 1;
    if (which == 1) fail_alloc = 1;
    if (which == 2) fail_read = 2;
    if (which == 3) fail_write = 2;
    if (which == 4) fail_stop = 1;
    count = 999;
    assert(ext2_alloc_run_hint(mnt, 9, 4, &count) == 0 && count == 0 && depth == 0);
    if (which < 4) assert(mnt->sb.s_free_blocks_count == 119);
    drop_mount(mnt);
  }
  puts("PASS allocation/start/read/bitmap/commit errors return no claimed run and release nesting");

  mnt = make_mount();
  ext2_inode_t inode = {0};
  uint32_t logicals[] = {5, 12, 12 + 256, 12 + 256 + 256 * 256};
  assert(ext3_journal_start(mnt) == 0);
  for (unsigned i = 0; i < 4; i++) {
    assert(ext2_set_block_num(mnt, &inode, logicals[i], 50 + i) == 0);
    uint32_t physical = 999;
    assert(ext2_get_block_num_checked(mnt, &inode, logicals[i], &physical) == 0);
    assert(physical == 50 + i);
  }
  assert(ext3_journal_stop(mnt) == 0);
  uint32_t physical = 999;
  assert(ext2_get_block_num_checked(mnt, &inode, 13, &physical) == 0 && physical == 0);
  invalidate(mnt, inode.i_block[12]);
  fail_read = inode.i_block[12];
  assert(ext2_get_block_num_checked(mnt, &inode, 12, &physical) == -1);
  assert(ext2_set_block_num(mnt, &inode, 12, 60) == -1);
  fail_read = -1;
  fail_write = inode.i_block[12];
  assert(ext2_set_block_num(mnt, &inode, 12, 60) == -1);
  fail_write = -1;
  assert(ext2_get_block_num_checked(mnt, &inode, 12, &physical) == 0 && physical == 51);
  fail_alloc = 1;
  assert(ext2_get_block_num_checked(mnt, &inode, 12, &physical) == -1);
  assert(ext2_set_block_num(mnt, &inode, 12, 60) == -1);
  fail_alloc = 0;
  inode.i_block[5] = 128;
  assert(ext2_get_block_num_checked(mnt, &inode, 5, &physical) == -1);
  assert(ext2_set_block_num(mnt, &inode, UINT32_MAX, 60) == -1);
  puts("PASS checked direct/single/double/triple mappings distinguish holes from I/O errors");
  drop_mount(mnt);
}
