/* Exercise real ext2_write_impl with a memory disk and nested transaction
 * model: commit counts, bounded batches, concurrent inode snapshots and errors. */
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
#define ARCH_UACCESS_H

typedef struct { volatile uint32_t locked; uint64_t saved_flags; } spinlock_t;
typedef struct { volatile uint32_t locked; } rawspinlock_t;
#define SPINLOCK_INIT {0}
#define RAWSPINLOCK_INIT {0}
static void spinlock_acquire(spinlock_t *s) {
  while (__atomic_exchange_n(&s->locked, 1, __ATOMIC_ACQUIRE)) {}
}
static void spinlock_release(spinlock_t *s) {
  __atomic_store_n(&s->locked, 0, __ATOMIC_RELEASE);
}
static bool user_buffer;
static bool is_user_ptr(uint64_t p) { (void)p; return user_buffer; }
unsigned long copy_from_user(void *, const void *, unsigned long);
unsigned long copy_to_user(void *, const void *, unsigned long);
unsigned long clear_user(void *, unsigned long);
#define sched_yield kernel_sched_yield
#ifdef TEST_WRITE_SOURCE
#include TEST_WRITE_SOURCE
#else
#include "../../kernel/src/fs/ext2/ext2_file.c"
#endif
#undef sched_yield

#define BS 1024
#define BLOCKS 512
static ext2_mount_t mount;
static ext2_inode_t images[2];
static uint32_t indirect_maps[2][64];
static unsigned char disk[BLOCKS][BS];
static unsigned next_block, commits, starts, inode_reads, data_writes;
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned depth;
static int fail_start, fail_stop, fail_alloc, fail_map, fail_set, fail_read,
           fail_write, fail_inode_read, fail_inode_write, fail_scratch;
static long copy_limit = -1;

void *kmalloc(size_t n) { return fail_scratch ? NULL : malloc(n); }
void kfree(void *p) { free(p); }
uint32_t ext2_current_time(void) { return 42; }
int ext3_journal_start(ext2_mount_t *mnt) {
  assert(mnt == &mount);
  if (!depth) {
    pthread_mutex_lock(&tx_lock);
    starts++;
    if (fail_start) {
      pthread_mutex_unlock(&tx_lock);
      return -1;
    }
  }
  depth++;
  return 0;
}
int ext3_journal_stop(ext2_mount_t *mnt) {
  assert(mnt == &mount && depth);
  if (--depth) return 0;
  commits++;
  int failed = fail_stop;
  pthread_mutex_unlock(&tx_lock);
  return failed ? -1 : 0;
}
int ext2_read_inode(ext2_mount_t *mnt, uint32_t ino, ext2_inode_t *out) {
  assert(mnt == &mount && ino < 2 && depth);
  inode_reads++;
  if (fail_inode_read) return -1;
  *out = images[ino];
  return 0;
}
int ext2_write_inode(ext2_mount_t *mnt, uint32_t ino, const ext2_inode_t *in) {
  assert(depth);
  assert(ext3_journal_start(mnt) == 0);
  if (!fail_inode_write) images[ino] = *in;
  assert(ext3_journal_stop(mnt) == 0);
  return fail_inode_write ? -1 : 0;
}
int ext2_get_block_num_checked(ext2_mount_t *mnt, ext2_inode_t *in,
                              uint32_t logical, uint32_t *out) {
  assert(mnt == &mount && depth && logical < 64);
  if (fail_map) return -1;
  *out = logical < 12 ? in->i_block[logical]
                     : indirect_maps[in->i_generation][logical];
  return 0;
}
int ext2_set_block_num(ext2_mount_t *mnt, ext2_inode_t *in,
                       uint32_t logical, uint32_t block) {
  assert(mnt == &mount && depth && logical < 64);
  if (fail_set) return -1;
  if (logical < 12) in->i_block[logical] = block;
  else indirect_maps[in->i_generation][logical] = block;
  return 0;
}
uint32_t ext2_alloc_block(ext2_mount_t *mnt) {
  assert(depth);
  assert(ext3_journal_start(mnt) == 0);
  uint32_t block = fail_alloc ? 0 : next_block++;
  assert(block < BLOCKS);
  assert(ext3_journal_stop(mnt) == 0);
  return block;
}
int ext2_read_block(ext2_mount_t *mnt, uint32_t block, void *out) {
  assert(mnt == &mount && depth && block && block < BLOCKS);
  if (fail_read) return -1;
  memcpy(out, disk[block], BS);
  return 0;
}
int ext2_write_block(ext2_mount_t *mnt, uint32_t block, const void *in) {
  assert(mnt == &mount && depth && block && block < BLOCKS);
  if (fail_write) return -1;
  memcpy(disk[block], in, BS);
  data_writes++;
  return 0;
}
unsigned long copy_from_user(void *out, const void *in, unsigned long n) {
  unsigned long copied = copy_limit < 0 ? n : (unsigned long)copy_limit;
  if (copied > n) copied = n;
  memcpy(out, in, copied);
  if (copy_limit >= 0) copy_limit -= copied;
  return n - copied;
}
static vfs_node_t node;
static void reset(void) {
  assert(!depth);
  memset(&mount, 0, sizeof(mount));
  memset(images, 0, sizeof(images));
  memset(disk, 0xcc, sizeof(disk));
  memset(indirect_maps, 0, sizeof(indirect_maps));
  images[1].i_generation = 1;
  mount.block_size = BS;
  mount.sb.s_blocks_count = BLOCKS;
  node = (vfs_node_t){.device = &mount, .inode = 0};
  next_block = 1;
  commits = starts = inode_reads = data_writes = 0;
  fail_start = fail_stop = fail_alloc = fail_map = fail_set = fail_read = fail_write = 0;
  fail_inode_read = fail_inode_write = fail_scratch = 0;
  user_buffer = false;
  copy_limit = -1;
}
static void check_unlocked(void) {
  assert(!depth);
  assert(pthread_mutex_trylock(&tx_lock) == 0);
  pthread_mutex_unlock(&tx_lock);
}
static void *neighbor_writer(void *arg) {
  unsigned id = (unsigned)(uintptr_t)arg;
  unsigned char buffer[4096];
  memset(buffer, 0x20 + id, sizeof(buffer));
  for (unsigned i = 0; i < 200; i++)
    assert(ext2_write_impl(&node, id * sizeof(buffer), sizeof(buffer), buffer)
           == sizeof(buffer));
  assert(!depth);
  return NULL;
}
int main(void) {
  unsigned char buffer[33 * BS];
  memset(buffer, 0x55, sizeof(buffer));
  reset();
  assert(ext2_write_impl(&node, 0, 4096, buffer) == 4096);
  assert(commits == 1 && starts == 1 && images[0].i_size == 4096);
  assert(images[0].i_blocks == 8 && data_writes == 4);
  for (unsigned i = 0; i < 4; i++)
    assert(!memcmp(disk[images[0].i_block[i]], buffer + i * BS, BS));
  puts("PASS 4 KiB allocation write: one outer commit instead of five");

  reset();
  assert(ext2_write_impl(&node, 0, sizeof(buffer), buffer) == sizeof(buffer));
  assert(commits == 2 && inode_reads == 2 && images[0].i_size == sizeof(buffer));
  puts("PASS 33-block write uses two batches and rereads inode under ownership");

  reset();
  assert(ext2_write_impl(&node, 900, 400, buffer) == 400);
  assert(images[0].i_size == 1300 && commits == 1);
  for (unsigned i = 0; i < 900; i++) assert(disk[1][i] == 0);
  for (unsigned i = 900; i < BS; i++) assert(disk[1][i] == 0x55);
  for (unsigned i = 0; i < 276; i++) assert(disk[2][i] == 0x55);
  for (unsigned i = 276; i < BS; i++) assert(disk[2][i] == 0);
  puts("PASS unaligned new blocks retain zero prefix and suffix");

  reset();
  assert(ext2_write_impl(&node, 0, BS, buffer) == BS);
  memset(buffer, 0x66, sizeof(buffer));
  user_buffer = true;
  copy_limit = 100;
  assert(ext2_write_impl(&node, 0, BS, buffer) == 100);
  for (unsigned i = 0; i < 100; i++) assert(disk[1][i] == 0x66);
  for (unsigned i = 100; i < BS; i++) assert(disk[1][i] == 0x55);
  copy_limit = 0;
  assert(ext2_write_impl(&node, 0, BS, buffer) == (uint32_t)-14);
  check_unlocked();
  puts("PASS partial user copy preserves old suffix and zero-copy faults release ownership");

  reset();
  images[0].i_size = BS; /* A sparse block inside the existing visible size. */
  user_buffer = true;
  copy_limit = 0;
  assert(ext2_write_impl(&node, 0, BS, buffer) == (uint32_t)-14);
  for (unsigned i = 0; i < BS; i++) assert(disk[1][i] == 0);
  check_unlocked();
  puts("PASS zero-byte user fault initializes newly mapped sparse block");

  for (unsigned failure = 0; failure < 9; failure++) {
    reset();
    /* Read failure needs an existing block rather than a freshly zeroed one. */
    if (failure == 4) {
      assert(ext2_write_impl(&node, 0, BS, buffer) == BS);
      user_buffer = true;
    }
    int *faults[] = {&fail_start, &fail_stop, &fail_map, &fail_inode_read,
                    &fail_read, &fail_write, &fail_inode_write,
                    &fail_alloc, &fail_scratch};
    *faults[failure] = 1;
    uint32_t expected = failure == 7 ? (uint32_t)-28
                       : failure == 8 ? (uint32_t)-12 : (uint32_t)-5;
    assert(ext2_write_impl(&node, 0, BS, buffer) == expected);
    check_unlocked();
  }
  puts("PASS start/stop/mapping/inode/read/write/allocation failures return errors and release ownership");

  reset();
  fail_set = 1;
  assert(ext2_write_impl(&node, 0, BS, buffer) == (uint32_t)-5);
  check_unlocked();

  reset();
  assert(ext2_write_impl(&node, UINT32_MAX - 5, 10, buffer) == (uint32_t)-27);
  assert(!commits);
  pthread_t threads[2];
  for (unsigned i = 0; i < 2; i++)
    assert(!pthread_create(&threads[i], NULL, neighbor_writer, (void *)(uintptr_t)i));
  for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
  assert(images[0].i_size == 8192 && images[0].i_blocks == 16);
  for (unsigned i = 0; i < 8; i++)
    for (unsigned j = 0; j < BS; j++)
      assert(disk[images[0].i_block[i]][j] == 0x20 + i / 4);
  check_unlocked();
  puts("PASS overlapping inode writers serialize snapshots and retain both disjoint extents");
  return 0;
}
