/* Host tests of real journal transactions: scratch/mapping reuse,
 * request ordering, collisions, nesting, and allocation/I/O failures. */
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

static void rawspinlock_init(rawspinlock_t *s) { s->locked = 0; }
static bool rawspinlock_try_acquire(rawspinlock_t *s) {
  return !__atomic_exchange_n(&s->locked, 1, __ATOMIC_ACQUIRE);
}
static void rawspinlock_release(rawspinlock_t *s) {
  __atomic_store_n(&s->locked, 0, __ATOMIC_RELEASE);
}
#define sched_yield kernel_sched_yield
#ifdef TEST_JOURNAL_SOURCE
#include TEST_JOURNAL_SOURCE
#else
#include "../../kernel/src/fs/ext3/ext3.c"
#endif
#undef sched_yield

static _Thread_local struct thread owner = {.tid = 1, .state = THREAD_RUNNING};
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static unsigned slept;
static _Thread_local bool irq_enabled = true;
hal_irq_state_t hal_irq_save(void) { bool old = irq_enabled; irq_enabled = false; return old; }
void hal_irq_restore(hal_irq_state_t flags) { irq_enabled = flags; }
static ext2_mount_t *release_on_add;
void kernel_sched_yield(void) {
  assert(irq_enabled);
  pthread_mutex_lock(&wait_mutex);
  while (owner.state == THREAD_BLOCKED) {
    slept++;
    pthread_cond_broadcast(&wait_cond);
    pthread_cond_wait(&wait_cond, &wait_mutex);
  }
  pthread_mutex_unlock(&wait_mutex);
  sched_yield();
}
void wait_queue_init(wait_queue_t *q) { memset(q, 0, sizeof(*q)); }
void wait_queue_add(wait_queue_t *q, wait_queue_entry_t *e) {
  pthread_mutex_lock(&wait_mutex);
  e->wq = q; e->next = q->head; q->head = e;
  pthread_mutex_unlock(&wait_mutex);
  if (release_on_add) {
    ext2_mount_t *mnt = release_on_add;
    release_on_add = NULL;
    uint32_t saved = owner.tid;
    owner.tid = mnt->journal.owner_tid;
    assert(ext3_journal_stop(mnt) == 0);
    owner.tid = saved;
  }
}
void wait_queue_remove(wait_queue_t *q, wait_queue_entry_t *e) {
  pthread_mutex_lock(&wait_mutex);
  wait_queue_entry_t **p = &q->head;
  while (*p && *p != e) p = &(*p)->next;
  if (*p) *p = e->next;
  e->wq = NULL;
  pthread_mutex_unlock(&wait_mutex);
}
void wait_queue_wake_all(wait_queue_t *q) {
  pthread_mutex_lock(&wait_mutex);
  for (wait_queue_entry_t *e = q->head; e; e = e->next)
    e->thread->state = THREAD_RUNNING;
  pthread_cond_broadcast(&wait_cond);
  pthread_mutex_unlock(&wait_mutex);
}
struct thread *sched_get_current(void) { return &owner; }
static _Thread_local uint64_t test_clock_ms;
uint64_t lapic_timer_get_ms(void) { return test_clock_ms; }
uint64_t lapic_timer_get_ticks(void) { return 0; }
void klogf(const char *fmt, ...) { (void)fmt; }
static unsigned allocations;
static int fail_allocation;
void *kmalloc(size_t n) {
  allocations++;
  return fail_allocation == (int)allocations ? NULL : malloc(n);
}
void kfree(void *p) { free(p); }
static unsigned writes, reads, mappings;
static uint32_t write_blocks[2048];
static unsigned char write_images[2048][4096];
static int fail_write, fail_read, fail_mapping = -1;
static int flush_error;
int ext2_flush_metadata(ext2_mount_t *mnt) {
  (void)mnt;
  return flush_error;
}
uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t logical) {
  (void)mnt;
  (void)inode;
  mappings++;
  return fail_mapping == (int)logical ? 0 : 100 + logical;
}
int ext2_read_block(ext2_mount_t *mnt, uint32_t block, void *buf) {
  (void)block;
  reads++;
  memset(buf, 0, mnt->block_size);
  return fail_read == (int)reads ? -1 : 0;
}
int ext2_write_block(ext2_mount_t *mnt, uint32_t block, const void *buf) {
  (void)mnt;
  assert(writes < 2048);
  write_blocks[writes] = block;
  memcpy(write_images[writes], buf, mnt->block_size);
  writes++;
  return fail_write == (int)writes ? -1 : 0;
}
static void *transaction_worker(void *opaque) {
  ext2_mount_t *mnt = opaque;
  owner.tid = (uint32_t)(uintptr_t)pthread_self();
  unsigned char data[1024] = {0};
  memcpy(data, &owner.tid, sizeof(owner.tid));
  for (unsigned i = 0; i < 32; i++) {
    assert(ext3_journal_start(mnt) == 0);
    assert(ext3_journal_start(mnt) == 0);
    assert(ext3_journal_block(mnt, 7, data) == 0);
    assert(ext3_journal_stop(mnt) == 0 && mnt->journal.active);
    assert(ext3_journal_stop(mnt) == 0);
  }
  return NULL;
}

static void *blocked_worker(void *opaque) {
  ext2_mount_t *mnt = opaque;
  owner.tid = 2;
  owner.state = THREAD_RUNNING;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  return NULL;
}

int main(void) {
  ext2_mount_t *mnt = calloc(1, sizeof(*mnt));
  assert(mnt);
  mnt->block_size = 1024;
  mnt->sb.s_blocks_count = 4096;
  mnt->journal.journal_blocks = 1024;
  mnt->sb.s_feature_compat = EXT3_FEATURE_COMPAT_HAS_JOURNAL;
  mnt->journal.sequence = 42;
  assert(ext3_journal_start(mnt) == 0);
  test_clock_ms = 7;
  assert(ext3_journal_stop(mnt) == 0);
  assert(mnt->journal.perf_wait.count == 1 && mnt->journal.perf_wait.total_ms == 0);
  assert(mnt->journal.perf_hold.count == 1 && mnt->journal.perf_hold.total_ms == 7 && mnt->journal.perf_hold.max_ms == 7 && mnt->journal.perf_hold.buckets[3] == 1);
  test_clock_ms = 0;
  assert(writes == 0 && reads == 0 && mnt->journal.sequence == 42);
  assert(!mnt->journal.active && !mnt->journal.lock.locked &&
         !mnt->journal.depth);
  puts("PASS empty transaction: no journal/device requests or sequence "
       "advancement");
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_start(mnt) == 0 && mnt->journal.depth == 2);
  assert(ext3_journal_stop(mnt) == 0 && mnt->journal.depth == 1);
  assert(ext3_journal_stop(mnt) == 0 && !mnt->journal.lock.locked &&
         writes == 0);
  assert(mnt->journal.perf_wait.count == 2 && mnt->journal.perf_hold.count == 2);
  puts("PASS nested empty transactions preserve owner until outer stop; latency counts outer transactions only");
  assert(ext3_journal_start(mnt) == 0);
  unsigned char buf[1024] = {1};
  assert(ext3_journal_block(mnt, 7, buf) == 0 && writes == 2);
  assert(ext3_journal_stop(mnt) == 0);
  assert(writes == 6 && reads == 1 && mnt->journal.sequence == 43);
  assert(!mnt->journal.active && !mnt->journal.lock.locked);
  puts("PASS nonempty transaction still performs existing log/home and commit "
       "writes");
  assert(ext3_journal_start(mnt) == 0);
  flush_error = -1;
  assert(ext3_journal_stop(mnt) == -1 && !mnt->journal.active &&
         !mnt->journal.lock.locked);
  assert(mnt->journal.perf_hold.errors == 1);
  flush_error = 0;
  assert(ext3_journal_start(mnt) == 0 && ext3_journal_stop(mnt) == 0);
  puts("PASS metadata flush failure releases ownership and permits the next "
       "transaction");
  /* Repeating the same transaction must preserve I/O order and payload,
   * while eliminating all scratch allocation and mapping calls. */
  unsigned warm_allocations = allocations, warm_mappings = mappings;
  writes = reads = 0;
  uint32_t seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  assert(allocations == warm_allocations && mappings == warm_mappings);
  assert(writes == 6 && reads == 1);
  uint32_t expected[] = {102, 7, 101, 103, 100, 100};
  for (unsigned i = 0; i < 6; i++) assert(write_blocks[i] == expected[i]);
  assert(!memcmp(write_images[0], buf, sizeof(buf)));
  assert(!memcmp(write_images[1], buf, sizeof(buf)));
  jbd_header_t *header = (jbd_header_t *)write_images[2];
  assert(__builtin_bswap32(header->h_sequence) == seq);
  jbd_block_tag_t *tag = (jbd_block_tag_t *)(write_images[2] + sizeof(*header));
  assert(__builtin_bswap32(tag->t_blocknr) == 7);
  assert(__builtin_bswap32(tag->t_flags) & JBD_FLAG_LAST_TAG);
  assert(__builtin_bswap32(((jbd_superblock_t *)write_images[4])->s_start) == 1);
  assert(((jbd_superblock_t *)write_images[5])->s_start == 0);
  puts("PASS warm transaction: zero scratch allocations/mapping calls; same six writes and payloads");

  /* Complete cached metadata can skip redundant I/O, but neither an active
   * write nor a direct-map collision can stand in for the requested block. */
  mnt->cache[7].num = 7;
  mnt->cache[7].data = buf;
  writes = reads = 0;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == 0 && writes == 0 && reads == 0);
  assert(mnt->journal.sequence == seq && mnt->journal.metadata_unchanged == 1);
  unsigned char changed[1024] = {2};
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, changed) == 0);
  assert(ext3_journal_stop(mnt) == 0 && writes == 6);
  writes = reads = 0;
  mnt->cache[0].data = buf;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 0, buf) == -1);
  assert(ext3_journal_stop(mnt) == -1 && writes == 0);
  mnt->cache[0].data = NULL;
  mnt->cache[7].writes_inflight = 1;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == 0 && writes == 6);
  mnt->cache[7].writes_inflight = 0;
  mnt->cache[7].num = 7 + EXT2_CACHE_SIZE;
  writes = reads = 0;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == 0 && writes == 6);
  mnt->cache[7].data = NULL;
  puts("PASS unchanged metadata skips six writes; in-flight/colliding images retain journaling");

  assert(ext3_journal_start(mnt) == 0);
  owner.tid = 2;
  assert(ext3_journal_stop(mnt) == -1 && mnt->journal.active);
  assert(ext3_journal_block(mnt, 7, buf) == -1);
  release_on_add = mnt;
  unsigned before_sleep = slept;
  assert(ext3_journal_start(mnt) == 0);
  assert(slept == before_sleep && !mnt->journal.waiters.head);
  assert(ext3_journal_stop(mnt) == 0);
  owner.tid = 1;
  puts("PASS unlock during waiter registration is rechecked; non-owner cannot stop transaction");

  for (unsigned fail_owner = 0; fail_owner < 2; fail_owner++) {
    assert(ext3_journal_start(mnt) == 0);
    pthread_t blocked;
    pthread_mutex_lock(&wait_mutex);
    before_sleep = slept;
    pthread_mutex_unlock(&wait_mutex);
    assert(pthread_create(&blocked, NULL, blocked_worker, mnt) == 0);
    pthread_mutex_lock(&wait_mutex);
    while (slept == before_sleep) pthread_cond_wait(&wait_cond, &wait_mutex);
    pthread_mutex_unlock(&wait_mutex);
    mnt->journal.io_failed = fail_owner;
    assert(ext3_journal_stop(mnt) == (fail_owner ? -1 : 0));
    assert(pthread_join(blocked, NULL) == 0);
    assert(!mnt->journal.waiters.head && !mnt->journal.lock.locked);
  }
  puts("PASS contended writers sleep and wake after both successful and failed owner completion");

  /* Cache collisions never return the other logical block's mapping. */
  assert(ext3_journal_start(mnt) == 0);
  for (uint32_t i = 0; i < 130; i++)
    assert(ext3_journal_phys(mnt, i) == 100 + i);
  for (uint32_t i = 130; i-- > 0;)
    assert(ext3_journal_phys(mnt, i) == 100 + i);
  assert(ext3_journal_stop(mnt) == 0);
  puts("PASS mapping collisions resolve the correct physical blocks");

  /* Each of the payload/home/descriptor/commit/publication/retirement
   * writes can fail; none may report successful outer completion. */
  for (int i = 1; i <= 6; i++) {
    writes = reads = 0;
    fail_write = i;
    seq = mnt->journal.sequence;
    assert(ext3_journal_start(mnt) == 0);
    int block_result = ext3_journal_block(mnt, 7, buf);
    assert(block_result == (i <= 2 ? -1 : 0));
    if (i <= 2) {
      assert(ext3_journal_start(mnt) == 0);
      assert(ext3_journal_block(mnt, 8, buf) == -1);
      assert(ext3_journal_stop(mnt) == 0); /* Nested ownership. */
    }
    assert(ext3_journal_stop(mnt) == -1);
    assert(writes == (unsigned)i && mnt->journal.sequence == seq);
    assert(!mnt->journal.active && !mnt->journal.lock.locked);
    fail_write = 0;
    assert(ext3_journal_start(mnt) == 0 && ext3_journal_stop(mnt) == 0);
  }
  writes = reads = 0;
  fail_read = 1;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == -1 && writes == 4);
  assert(mnt->journal.sequence == seq && !mnt->journal.lock.locked);
  fail_read = 0;
  puts("PASS every commit I/O failure is propagated and releases ownership");

  /* Missing, out-of-filesystem, and out-of-journal mappings must never
   * become writes to block zero or successful commits. Failed lookups are
   * not cached, allowing a subsequent valid lookup. */
  memset(mnt->journal.map_keys, 0, sizeof(mnt->journal.map_keys));
  writes = 0;
  fail_mapping = 2;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == -1);
  assert(ext3_journal_stop(mnt) == -1 && writes == 0);
  fail_mapping = -1;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_phys(mnt, 2) == 102);
  assert(ext3_journal_phys(mnt, 1024) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  mnt->journal.journal_blocks = 5000;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_phys(mnt, 3996) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  mnt->journal.journal_blocks = 2;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == -1);
  assert(ext3_journal_stop(mnt) == -1);
  mnt->journal.journal_blocks = 1024;
  puts("PASS invalid journal mappings rejected without device writes");

  mnt->journal.journal_blocks = 1024;
  writes = reads = 0;
  seq = mnt->journal.sequence;
  pthread_t workers[4];
  for (unsigned i = 0; i < 4; i++)
    assert(pthread_create(&workers[i], NULL, transaction_worker, mnt) == 0);
  for (unsigned i = 0; i < 4; i++) assert(pthread_join(workers[i], NULL) == 0);
  assert(writes == 128 * 6 && reads == 128);
  assert(mnt->journal.sequence == seq + 128 && !mnt->journal.lock.locked);
  for (unsigned i = 0; i < 128; i++) {
    unsigned off = i * 6;
    assert(!memcmp(write_images[off], write_images[off + 1], 1024));
    for (unsigned j = 0; j < 6; j++)
      assert(write_blocks[off + j] == expected[j]);
    assert(__builtin_bswap32(((jbd_header_t *)write_images[off + 2])->h_sequence)
           == seq + i);
    assert(__builtin_bswap32(((jbd_header_t *)write_images[off + 3])->h_sequence)
           == seq + i);
  }
  puts("PASS four writers: nested transactions serialize shared buffers and preserve images/sequences");

  /* Fail each cold buffer allocation. Retained allocations are usable on
   * retry, and every error releases the mount's transaction lock. */
  for (int i = 1; i <= 3; i++) {
    ext2_mount_t *cold = calloc(1, sizeof(*cold));
    assert(cold);
    cold->block_size = 1024;
    cold->sb.s_blocks_count = 4096;
    cold->sb.s_feature_compat = EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    allocations = writes = reads = 0;
    fail_allocation = i;
    int start_result = ext3_journal_start(cold);
    if (i == 1) assert(start_result == -1 && writes == 0);
    else {
      assert(start_result == 0);
      assert(ext3_journal_block(cold, 7, buf) == 0);
      assert(ext3_journal_stop(cold) == -1 && writes == 2);
    }
    assert(!cold->journal.active && !cold->journal.lock.locked);
    fail_allocation = 0;
    assert(ext3_journal_start(cold) == 0);
    assert(ext3_journal_block(cold, 7, buf) == 0);
    assert(ext3_journal_stop(cold) == 0);
    free(cold->journal.desc_block_buf);
    free(cold->journal.commit_block_buf);
    free(cold->journal.super_block_buf);
    free(cold);
  }
  puts("PASS scratch allocation failures release ownership and retry succeeds");
  for (uint32_t bs = 2048; bs <= 4096; bs *= 2) {
    ext2_mount_t *large = calloc(1, sizeof(*large));
    assert(large);
    large->block_size = bs;
    large->sb.s_blocks_count = 4096;
    large->sb.s_feature_compat = EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    unsigned char data[4096] = {1};
    uint32_t tags = (bs - sizeof(jbd_header_t)) / sizeof(jbd_block_tag_t);
    writes = reads = 0;
    assert(ext3_journal_start(large) == 0);
    for (uint32_t i = 0; i < tags; i++)
      assert(ext3_journal_block(large, 2000 + i, data) == 0);
    assert(ext3_journal_stop(large) == 0 && writes == 2 * tags + 4);
    for (uint32_t i = 0; i < tags; i++) {
      assert(write_blocks[2 * i] == 102 + i);
      assert(write_blocks[2 * i + 1] == 2000 + i);
      assert(!memcmp(write_images[2 * i], data, bs));
    }
    assert(write_blocks[2 * tags] == 101);
    assert(write_blocks[2 * tags + 1] == 102 + tags);
    free(large->journal.desc_block_buf);
    free(large->journal.commit_block_buf);
    free(large->journal.super_block_buf);
    free(large);
  }
  puts("PASS 2/4 KiB blocks: full descriptors and repeated mapping collisions retain payloads");
  free(mnt->journal.commit_block_buf);
  free(mnt->journal.super_block_buf);
  free(mnt->journal.desc_block_buf);
  free(mnt);
}
