/* Deterministic host check of real ext2 block-cache read/write functions.
 * A fake device holds an old read until a newer write and colliding eviction
 * have completed. No real disks or kernel IRQ instructions are used.
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
#ifdef TEST_EXT2_SOURCE
#include TEST_EXT2_SOURCE
#else
#include "../../kernel/src/fs/ext2/ext2_block.c"
#endif
#undef sched_yield
void kernel_sched_yield(void) { sched_yield(); }
struct thread *sched_get_current(void) { return NULL; }
void klogf(const char *fmt, ...) { (void)fmt; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
static pthread_mutex_t disk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t disk_cond = PTHREAD_COND_INITIALIZER;
static unsigned char disk_image[1024];
static int captured, released, read_count, fail_write;
static int inode_phase, fail_start;
static pthread_mutex_t transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned transaction_depth;
static const unsigned block_a = 11;
static ext2_mount_t mount;
/* Model the journal's owner/nesting exclusion while testing the real inode
 * RMW functions. The fake device stores full blocks, including neighbors. */
int ext3_journal_start(ext2_mount_t *mnt) {
  (void)mnt;
  if (fail_start)
    return -1;
  if (!transaction_depth)
    pthread_mutex_lock(&transaction_lock);
  transaction_depth++;
  return 0;
}
int ext3_journal_stop(ext2_mount_t *mnt) {
  (void)mnt;
  assert(transaction_depth);
  if (!--transaction_depth)
    pthread_mutex_unlock(&transaction_lock);
  return 0;
}
int ext3_journal_block(ext2_mount_t *mnt, uint32_t block, const void *buf) {
  assert(transaction_depth);
  sched_yield();
  return ext2_write_block(mnt, block, buf);
}
static int device_read(struct block_device *dev, uint64_t lba, uint32_t count,
                       void *buf) {
  (void)dev;
  assert(count == 2);
  if (inode_phase)
    assert(transaction_depth);
  pthread_mutex_lock(&disk_lock);
  if (lba / 2 == block_a)
    memcpy(buf, disk_image, sizeof(disk_image));
  else
    memset(buf, 0x33, 1024);
  if (lba / 2 == block_a && ++read_count == 1) {
    captured = 1;
    pthread_cond_broadcast(&disk_cond);
    while (!released)
      pthread_cond_wait(&disk_cond, &disk_lock);
  }
  pthread_mutex_unlock(&disk_lock);
  return 0;
}
static int device_write(struct block_device *dev, uint64_t lba, uint32_t count,
                        const void *buf) {
  (void)dev;
  assert(count == 2 && lba / 2 == block_a);
  pthread_mutex_lock(&disk_lock);
  if (!fail_write)
    memcpy(disk_image, buf, sizeof(disk_image));
  pthread_mutex_unlock(&disk_lock);
  return fail_write ? -1 : 0;
}
static unsigned char read_result[1024];
static void *held_reader(void *unused) {
  (void)unused;
  assert(ext2_read_block(&mount, block_a, read_result) == 0);
  return NULL;
}
static void *inode_writer(void *arg) {
  unsigned ino = (unsigned)(uintptr_t)arg;
  for (unsigned i = 0; i < 2000; i++) {
    ext2_inode_t inode = {0};
    inode.i_mode = EXT2_S_IFREG | 0600;
    inode.i_links_count = 1;
    inode.i_uid = ino;
    inode.i_size = i;
    /* Exercise both unwrapped callers (post-create chown historically) and
     * callers that already own an enclosing transaction. */
    if (i & 1)
      assert(ext3_journal_start(&mount) == 0);
    assert(ext2_write_inode(&mount, ino, &inode) == 0);
    if (i & 1)
      assert(ext3_journal_stop(&mount) == 0);
    assert(transaction_depth == 0);
  }
  return NULL;
}
int main(void) {
  memset(disk_image, 0x11, sizeof(disk_image));
  struct block_device dev = {.read_sectors = device_read,
                             .write_sectors = device_write};
  mount.dev = &dev;
  mount.block_size = 1024;
  mount.sb.s_blocks_count = 10000;
  spinlock_init(&mount.cache_lock);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, held_reader, NULL));
  pthread_mutex_lock(&disk_lock);
  while (!captured)
    pthread_cond_wait(&disk_cond, &disk_lock);
  pthread_mutex_unlock(&disk_lock);
  unsigned char newer[1024], collision[1024];
  memset(newer, 0x22, sizeof(newer));
  assert(ext2_write_blocks(&mount, block_a, 1, newer) == 0);
  assert(ext2_read_block(&mount, block_a + EXT2_CACHE_SIZE, collision) == 0);
  assert(mount.cache[block_a % EXT2_CACHE_SIZE].num ==
         block_a + EXT2_CACHE_SIZE);
  pthread_mutex_lock(&disk_lock);
  released = 1;
  pthread_cond_broadcast(&disk_cond);
  pthread_mutex_unlock(&disk_lock);
  assert(!pthread_join(thread, NULL));
  assert(!memcmp(read_result, newer, sizeof(newer)));
  assert(read_count == 2);
  puts("PASS overlapping read/write plus cache collision: old image discarded "
       "and reread");
  fail_write = 1;
  memset(newer, 0x44, sizeof(newer));
  assert(ext2_write_block(&mount, block_a, newer) == -1);
  assert(mount.cache[block_a % EXT2_CACHE_SIZE].writes_inflight == 0);
  assert(ext2_read_block(&mount, block_a, read_result) == 0);
  for (unsigned i = 0; i < sizeof(read_result); i++)
    assert(read_result[i] == 0x22);
  puts("PASS failed write: speculative image invalidated and disk contents "
       "reread");
  ext2_bgd_t group = {.bg_inode_table = block_a};
  mount.bgdt = &group;
  mount.groups_count = 1;
  mount.inodes_per_group = 64;
  mount.inode_size = sizeof(ext2_inode_t);
  fail_write = 0;
  memset(disk_image, 0, sizeof(disk_image));
  mount.cache[block_a % EXT2_CACHE_SIZE].num = 0;
  inode_phase = 1;
  pthread_t writers[4];
  for (unsigned i = 0; i < 4; i++)
    assert(!pthread_create(&writers[i], NULL, inode_writer,
                           (void *)(uintptr_t)(i + 1)));
  for (unsigned i = 0; i < 4; i++)
    assert(!pthread_join(writers[i], NULL));
  inode_phase = 0;
  for (unsigned i = 1; i <= 4; i++) {
    ext2_inode_t inode;
    assert(!ext2_read_inode(&mount, i, &inode));
    assert(inode.i_mode == (EXT2_S_IFREG | 0600));
    assert(inode.i_uid == i && inode.i_links_count == 1 &&
           inode.i_size == 1999);
    assert(!memcmp(disk_image + (i - 1) * mount.inode_size, &inode,
                   sizeof(inode)));
  }
  puts("PASS inode RMW: 8000 updates across four neighboring inodes preserve "
       "every image");
  ext2_inode_t inode = {.i_mode = EXT2_S_IFREG | 0600};
  assert(ext2_write_inode(&mount, 65, &inode) == -1);
  assert(transaction_depth == 0);
  fail_write = 1;
  assert(ext2_write_inode(&mount, 1, &inode) == -1);
  assert(transaction_depth == 0);
  fail_start = 1;
  assert(ext2_write_inode(&mount, 1, &inode) == -1);
  assert(transaction_depth == 0);
  puts("PASS inode error paths: invalid group, failed write and failed start "
       "release ownership");
  for (unsigned i = 0; i < EXT2_CACHE_SIZE; i++)
    free(mount.cache[i].data);
}
