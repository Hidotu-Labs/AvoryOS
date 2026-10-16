/* Host tests of real journal transactions: write-set staging/dedup, journal
 * ordering, multi-descriptor commits, fair FIFO ownership handoff, recovery
 * replay, collisions, nesting, and allocation/I/O failures. */
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
static void rawspinlock_acquire(rawspinlock_t *s) {
  while (__atomic_exchange_n(&s->locked, 1, __ATOMIC_ACQUIRE)) {
  }
}
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
static _Thread_local struct thread *current_thread;
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
  while (current_thread->state == THREAD_BLOCKED) {
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
}
void wait_queue_add_tail(wait_queue_t *q, wait_queue_entry_t *e) {
  pthread_mutex_lock(&wait_mutex);
  e->wq = q;
  e->next = NULL;
  wait_queue_entry_t **p = &q->head;
  while (*p) p = &(*p)->next;
  *p = e;
  pthread_mutex_unlock(&wait_mutex);
  if (release_on_add) {
    /* The owner finishes (unlocks) while this waiter is mid-registration;
     * the recheck after add must pick up the handoff or the free lock. */
    ext2_mount_t *mnt = release_on_add;
    release_on_add = NULL;
    struct thread fake = {.tid = mnt->journal.owner_tid,
                          .state = THREAD_RUNNING};
    struct thread *saved = current_thread;
    current_thread = &fake;
    assert(ext3_journal_stop(mnt) == 0);
    current_thread = saved;
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
bool wait_queue_handoff(wait_queue_t *q,
                        void (*publish)(struct thread *, void *), void *arg) {
  assert(!irq_enabled);
  pthread_mutex_lock(&wait_mutex);
  bool found = false;
  wait_queue_entry_t **p = &q->head;
  while (*p) {
    wait_queue_entry_t *e = *p;
    if (e->thread && e->thread->state != THREAD_DEAD &&
        e->thread->state != THREAD_ZOMBIE) {
      *p = e->next;
      e->next = NULL;
      publish(e->thread, arg);
      assert(!strcmp(((ext3_journal_state_t *)arg)->phase, "handoff"));
      assert(((ext3_journal_state_t *)arg)->owner_ip == 0);
      assert(((ext3_journal_state_t *)arg)->started_ms == lapic_timer_get_ms());
      assert(!strcmp(((ext3_journal_state_t *)arg)->owner_comm,
                     e->thread->comm));
      e->thread->wakeup_ticks = 0;
      if (e->thread->state == THREAD_BLOCKED ||
          e->thread->state == THREAD_SLEEPING) {
        e->thread->state = THREAD_RUNNING;
        pthread_cond_broadcast(&wait_cond);
      }
      e->wq = NULL;
      found = true;
      break;
    }
    p = &e->next;
  }
  pthread_mutex_unlock(&wait_mutex);
  return found;
}
void sched_wakeup(struct thread *t) {
  pthread_mutex_lock(&wait_mutex);
  if (t && (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING)) {
    t->state = THREAD_RUNNING;
    pthread_cond_broadcast(&wait_cond);
  }
  pthread_mutex_unlock(&wait_mutex);
}
struct thread *sched_get_current(void) { return current_thread; }
static _Thread_local uint64_t test_clock_ms;
uint64_t lapic_timer_get_ms(void) { return test_clock_ms; }
uint64_t lapic_timer_get_ticks(void) { return 0; }
void klogf(const char *fmt, ...) { (void)fmt; }
void klog_puts(const char *text) { (void)text; }
void klog_uint64(uint64_t value) { (void)value; }
static unsigned stall_reports;
void lockdiag_dump_all(const char *reason) {
  assert(irq_enabled);
  assert(!strcmp(reason, "JBD2 transaction wait exceeded 5 seconds"));
  __atomic_add_fetch(&stall_reports, 1, __ATOMIC_RELAXED);
}
static unsigned allocations, live_allocations;
static int fail_allocation;
void *kmalloc(size_t n) {
  allocations++;
  void *p = fail_allocation == (int)allocations ? NULL : malloc(n);
  if (p) live_allocations++;
  return p;
}
void kfree(void *p) { if (p) live_allocations--; free(p); }
static unsigned writes, reads, mappings;
static uint32_t write_blocks[2048];
static unsigned char write_images[2048][4096];
#define MOCK_DISK_BLOCKS 4096
static unsigned char mock_disk[MOCK_DISK_BLOCKS][1024];
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
  reads++;
  if (fail_read == (int)reads) return -1;
  if ((int)block < MOCK_DISK_BLOCKS)
    memcpy(buf, mock_disk[block], mnt->block_size);
  else
    memset(buf, 0, mnt->block_size);
  return 0;
}
int ext2_write_block(ext2_mount_t *mnt, uint32_t block, const void *buf) {
  assert(writes < 2048);
  write_blocks[writes] = block;
  memcpy(write_images[writes], buf, mnt->block_size);
  writes++;
  if (fail_write == (int)writes) return -1;
  if ((int)block < MOCK_DISK_BLOCKS)
    memcpy(mock_disk[block], buf, mnt->block_size);
  return 0;
}
int ext2_write_block_phase(ext2_mount_t *mnt, uint32_t block,
                           const void *buf, const char **phase) {
  assert(phase == &mnt->journal.phase);
  *phase = "metadata-home-io";
  return ext2_write_block(mnt, block, buf);
}
int ext2_write_block_traced(ext2_mount_t *mnt, uint32_t block,
                            const void *buf, const char **phase,
                            const char *const stages[4]) {
  assert(phase == &mnt->journal.phase);
  assert(!strcmp(*phase, "commit-descriptor-map"));
  assert(!strcmp(stages[0], "commit-descriptor-cache-alloc"));
  assert(!strcmp(stages[1], "commit-descriptor-cache-publish"));
  assert(!strcmp(stages[2], "commit-descriptor-io"));
  assert(!strcmp(stages[3], "commit-descriptor-cache-finish"));
  *phase = stages[2];
  int result = ext2_write_block(mnt, block, buf);
  *phase = stages[3];
  return result;
}

/* The real implementation stages every member through the device's batch op
 * (or the ordered single-request fallback).  Here each member goes through the
 * single-block mock in order, so the write_blocks/write_images logs and the
 * fail_write index keep the same meaning they had before batching. */
int ext2_write_blocks_batch(ext2_mount_t *mnt, ext2_block_write_t *entries,
                            uint32_t count) {
  int result = 0;
  for (uint32_t i = 0; i < count; i++) {
    entries[i].result = ext2_write_block(mnt, entries[i].block_num,
                                         entries[i].buffer);
    if (entries[i].result != 0)
      result = -1;
  }
  return result;
}

static void put_journal_header(uint32_t block, uint32_t type, uint32_t sequence) {
  jbd_header_t h = {.h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER),
                    .h_blocktype = __builtin_bswap32(type),
                    .h_sequence = __builtin_bswap32(sequence)};
  memcpy(mock_disk[block], &h, sizeof(h));
}
static void put_tag(uint32_t desc_block, unsigned index, uint32_t block,
                    uint32_t flags) {
  jbd_block_tag_t tag = {.t_blocknr = __builtin_bswap32(block),
                         .t_flags = __builtin_bswap32(flags)};
  memcpy(mock_disk[desc_block] + sizeof(jbd_header_t) + index * sizeof(tag),
         &tag, sizeof(tag));
}

static void *transaction_worker(void *opaque) {
  ext2_mount_t *mnt = opaque;
  current_thread = &owner;
  owner.tid = (uint32_t)(uintptr_t)pthread_self();
  memcpy(owner.comm, "journal-writer", sizeof("journal-writer"));
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
  current_thread = &owner;
  owner.tid = 2;
  memcpy(owner.comm, "journal-waiter", sizeof("journal-waiter"));
  owner.state = THREAD_RUNNING;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  return NULL;
}

static void *interrupted_worker(void *opaque) {
  ext2_mount_t *mnt = opaque;
  current_thread = &owner;
  owner.tid = 2;
  memcpy(owner.comm, "signal-waiter", sizeof("signal-waiter"));
  owner.state = THREAD_RUNNING;
  owner.pending_signals = 1ULL << (SIGUSR1 - 1);
  assert(ext3_journal_start(mnt) == -4);
  owner.pending_signals = 0;
  return NULL;
}

/* FIFO handoff: workers queue in creation order, record when start() grants
 * them ownership, and hold the transaction until their gate opens so the main
 * thread can observe the queue between grants. */
typedef struct {
  ext2_mount_t *mnt;
  unsigned slot;
} fifo_arg_t;
static pthread_mutex_t fifo_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fifo_cond = PTHREAD_COND_INITIALIZER;
static uint32_t fifo_started[4];
static uint32_t fifo_acquired[4];
static uint32_t fifo_finished[4];
static unsigned fifo_started_count, fifo_acquired_count, fifo_finished_count;
static bool fifo_gate[4];

static void *fifo_worker(void *opaque) {
  fifo_arg_t *arg = opaque;
  ext2_mount_t *mnt = arg->mnt;
  current_thread = &owner;
  owner.tid = (uint32_t)(uintptr_t)pthread_self();
  owner.state = THREAD_RUNNING;
  pthread_mutex_lock(&fifo_mutex);
  fifo_started[fifo_started_count++] = owner.tid;
  pthread_cond_broadcast(&fifo_cond);
  pthread_mutex_unlock(&fifo_mutex);

  assert(ext3_journal_start(mnt) == 0);

  pthread_mutex_lock(&fifo_mutex);
  fifo_acquired[fifo_acquired_count++] = owner.tid;
  pthread_cond_broadcast(&fifo_cond);
  while (!fifo_gate[arg->slot])
    pthread_cond_wait(&fifo_cond, &fifo_mutex);
  pthread_mutex_unlock(&fifo_mutex);

  assert(ext3_journal_stop(mnt) == 0);

  pthread_mutex_lock(&fifo_mutex);
  fifo_finished[fifo_finished_count++] = owner.tid;
  pthread_cond_broadcast(&fifo_cond);
  pthread_mutex_unlock(&fifo_mutex);
  return NULL;
}

static void wait_fifo_acquired(unsigned count) {
  pthread_mutex_lock(&fifo_mutex);
  while (fifo_acquired_count < count)
    pthread_cond_wait(&fifo_cond, &fifo_mutex);
  pthread_mutex_unlock(&fifo_mutex);
}

/* Wait until the waiter at `position` (FIFO from head) is registered and
 * blocked. Reads the queue under the harness's wait_mutex. */
static uint32_t wait_waiter(ext2_mount_t *mnt, unsigned position) {
  for (unsigned spin = 0; spin < 200000000; spin++) {
    pthread_mutex_lock(&wait_mutex);
    wait_queue_entry_t *e = mnt->journal.waiters.head;
    for (unsigned i = 0; i < position && e; i++) e = e->next;
    bool ready = e && e->thread && e->thread->state == THREAD_BLOCKED;
    uint32_t tid = ready ? e->thread->tid : 0;
    pthread_mutex_unlock(&wait_mutex);
    if (ready) return tid;
    sched_yield();
  }
  assert(!"waiter did not register");
  return 0;
}

int main(void) {
  current_thread = &owner;
  test_clock_ms = 5000;
  ext3_report_stall();
  ext3_report_stall();
  test_clock_ms = 34999;
  ext3_report_stall();
  assert(stall_reports == 1);
  test_clock_ms = 35000;
  ext3_report_stall();
  assert(stall_reports == 2);
  /* A CPU sampled time before a newer report was published: do not treat
   * unsigned subtraction underflow as an expired throttle. */
  test_clock_ms = 34999;
  ext3_report_stall();
  assert(stall_reports == 2);
  test_clock_ms = 0;
  puts("PASS automatic journal stall capture is throttled across waiters");
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
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(writes == 0 && reads == 0); /* staging is memory-only */
  assert(ext3_journal_stop(mnt) == 0);
  assert(writes == 6 && reads == 1 && mnt->journal.sequence == 43);
  assert(!mnt->journal.active && !mnt->journal.lock.locked &&
         !mnt->journal.txn_head && mnt->journal.txn_count == 0);
  puts("PASS nonempty transaction stages in memory, then commits payload, "
       "descriptor, commit record, publish, home and retirement");
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
   * while eliminating all mapping calls and reusing the scratch buffers. */
  unsigned warm_allocations = allocations, warm_mappings = mappings;
  unsigned warm_live = live_allocations;
  writes = reads = 0;
  uint32_t seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == 0);
  /* One image copy and one write-set chunk for the transaction. */
  assert(allocations == warm_allocations + 2 && mappings == warm_mappings);
  assert(live_allocations == warm_live);
  assert(writes == 6 && reads == 1);
  uint32_t expected[] = {102, 101, 103, 100, 7, 100};
  for (unsigned i = 0; i < 6; i++) assert(write_blocks[i] == expected[i]);
  assert(!memcmp(write_images[0], buf, sizeof(buf)));
  assert(!memcmp(write_images[4], buf, sizeof(buf)));
  jbd_header_t *header = (jbd_header_t *)write_images[1];
  assert(__builtin_bswap32(header->h_sequence) == seq);
  jbd_block_tag_t *tag = (jbd_block_tag_t *)(write_images[1] + sizeof(*header));
  assert(__builtin_bswap32(tag->t_blocknr) == 7);
  assert(__builtin_bswap32(tag->t_flags) & JBD_FLAG_LAST_TAG);
  assert(!(__builtin_bswap32(tag->t_flags) &
           ~(uint32_t)(JBD_FLAG_ESCAPE | JBD_FLAG_LAST_TAG)));
  assert(__builtin_bswap32(((jbd_superblock_t *)write_images[3])->s_start) == 1);
  assert(((jbd_superblock_t *)write_images[5])->s_start == 0);
  puts("PASS warm transaction: payload/descriptor/commit/publish/home/clear "
       "order with cached mappings and no leaked images");

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

  /* A metadata block whose first word is the journal magic is escaped in the
   * log copy and restored on replay. */
  unsigned char magic_data[1024] = {0};
  uint32_t magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
  memcpy(magic_data, &magic, 4);
  magic_data[4] = 9;
  writes = reads = 0;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, magic_data) == 0);
  assert(ext3_journal_stop(mnt) == 0 && writes == 6 && mnt->journal.sequence == seq + 1);
  assert(*(uint32_t *)write_images[0] == 0);
  jbd_block_tag_t *esc = (jbd_block_tag_t *)(write_images[1] + sizeof(jbd_header_t));
  assert(__builtin_bswap32(esc->t_flags) & JBD_FLAG_ESCAPE);
  assert(*(uint32_t *)write_images[4] == magic);
  puts("PASS journal magic in metadata is escaped in the log copy and kept in the home image");

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

  /* Pending signals must not strand ownership when a sleeping waiter wakes
   * on a handoff. Also cover forwarding to a successor and withdrawal before
   * any grant. Join the interrupted worker before probing the journal so the
   * original bug fails an assertion instead of hanging the next start(). */
  for (unsigned mode = 0; mode < 3; mode++) {
    assert(ext3_journal_start(mnt) == 0);
    pthread_t interrupted, successor;
    pthread_mutex_lock(&wait_mutex);
    before_sleep = slept;
    pthread_mutex_unlock(&wait_mutex);
    assert(pthread_create(&interrupted, NULL, interrupted_worker, mnt) == 0);
    pthread_mutex_lock(&wait_mutex);
    while (slept == before_sleep) pthread_cond_wait(&wait_cond, &wait_mutex);
    pthread_mutex_unlock(&wait_mutex);
    if (mode == 1) {
      assert(pthread_create(&successor, NULL, transaction_worker, mnt) == 0);
      wait_waiter(mnt, 1);
    }
    if (mode == 2) {
      pthread_mutex_lock(&wait_mutex);
      struct thread *waiter = mnt->journal.waiters.head->thread;
      pthread_mutex_unlock(&wait_mutex);
      sched_wakeup(waiter);
    } else {
      assert(ext3_journal_stop(mnt) == 0);
    }
    assert(pthread_join(interrupted, NULL) == 0);
    assert(mnt->journal.granted_tid != 2 && mnt->journal.owner_tid != 2);
    if (mode == 1)
      assert(pthread_join(successor, NULL) == 0);
    if (mode == 2)
      assert(ext3_journal_stop(mnt) == 0);
    assert(!mnt->journal.active && !mnt->journal.lock.locked &&
           !mnt->journal.granted_tid && !mnt->journal.waiters.head);
    assert(ext3_journal_start(mnt) == 0);
    assert(ext3_journal_stop(mnt) == 0);
  }
  puts("PASS interrupted waiters release or forward concurrent grants and withdraw without a grant");

  /* Cache collisions never return the other logical block's mapping. */
  assert(ext3_journal_start(mnt) == 0);
  for (uint32_t i = 0; i < 130; i++)
    assert(ext3_journal_phys(mnt, i) == 100 + i);
  for (uint32_t i = 130; i-- > 0;)
    assert(ext3_journal_phys(mnt, i) == 100 + i);
  assert(ext3_journal_stop(mnt) == 0);
  puts("PASS mapping collisions resolve the correct physical blocks");

  /* Each of the payload/descriptor/commit/publication/retirement writes can
   * fail; none may report successful outer completion. A failed home
   * checkpoint still retires the committed log record for liveness. */
  for (int i = 1; i <= 6; i++) {
    writes = reads = 0;
    fail_write = i;
    seq = mnt->journal.sequence;
    assert(ext3_journal_start(mnt) == 0);
    assert(ext3_journal_block(mnt, 7, buf) == 0);
    assert(writes == 0 && reads == 0);
    assert(ext3_journal_stop(mnt) == -1);
    assert(writes == (i <= 4 || i == 6 ? (unsigned)i : 6u));
    assert(mnt->journal.sequence == (i == 5 ? seq + 1 : seq));
    assert(!mnt->journal.active && !mnt->journal.lock.locked &&
           !mnt->journal.txn_head);
    fail_write = 0;
    assert(ext3_journal_start(mnt) == 0 && ext3_journal_stop(mnt) == 0);
  }
  writes = reads = 0;
  fail_read = 1;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  assert(ext3_journal_block(mnt, 7, buf) == 0);
  assert(ext3_journal_stop(mnt) == -1 && writes == 3 && reads == 1);
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

  /* The allocator pattern restages the same bitmap/superblock over and over;
   * the write-set must collapse that to one payload and one home write. */
  unsigned char evolving[1024] = {0};
  uint64_t submitted = mnt->journal.metadata_submitted;
  writes = reads = 0;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  for (unsigned i = 0; i < 300; i++) {
    evolving[0] = (unsigned char)i;
    assert(ext3_journal_block(mnt, 7, evolving) == 0);
    assert(writes == 0 && reads == 0);
  }
  assert(mnt->journal.metadata_submitted - submitted == 300);
  assert(mnt->journal.blocks_in_trans == 1);
  assert(ext3_journal_stop(mnt) == 0);
  assert(writes == 6 && reads == 1 && mnt->journal.sequence == seq + 1);
  assert(write_blocks[0] == 102 && !memcmp(write_images[0], evolving, 1024));
  assert(write_blocks[1] == 101 && write_blocks[2] == 103 && write_blocks[3] == 100);
  assert(write_blocks[4] == 7 && !memcmp(write_images[4], evolving, 1024));
  assert(write_blocks[5] == 100);
  puts("PASS repeated staging of one block collapses to one payload and one "
       "home write with zero staging I/O");

  /* More unique blocks than one descriptor has tags: the transaction commits
   * with a second descriptor group instead of falling back to unjournaled
   * home writes. */
  writes = reads = 0;
  seq = mnt->journal.sequence;
  assert(ext3_journal_start(mnt) == 0);
  unsigned char image[1024];
  for (uint32_t i = 0; i < 130; i++) {
    memset(image, 0, sizeof(image));
    image[0] = (unsigned char)i;
    image[1] = (unsigned char)(i >> 8);
    assert(ext3_journal_block(mnt, 2000 + i, image) == 0);
  }
  assert(mnt->journal.blocks_in_trans == 130);
  assert(writes == 0);
  assert(ext3_journal_stop(mnt) == 0);
  assert(writes == 130 + 2 + 1 + 1 + 130 + 1 && reads == 1);
  for (uint32_t i = 0; i < 130; i++)
    assert(write_blocks[i] == 100 + ext3_payload_pos(i, 126));
  assert(write_blocks[130] == 100 + ext3_desc_pos(0, 126));
  assert(write_blocks[131] == 100 + ext3_desc_pos(126, 126));
  assert(write_blocks[132] == 100 + ext3_payload_pos(129, 126) + 1);
  assert(write_blocks[133] == 100);
  for (uint32_t i = 0; i < 130; i++)
    assert(write_blocks[134 + i] == 2000 + i);
  assert(write_blocks[264] == 100);
  jbd_block_tag_t *tags0 = (jbd_block_tag_t *)(write_images[130] + sizeof(jbd_header_t));
  assert(__builtin_bswap32(tags0[125].t_flags) & JBD_FLAG_LAST_TAG);
  assert(__builtin_bswap32(tags0[124].t_flags) == 0);
  jbd_block_tag_t *tags1 = (jbd_block_tag_t *)(write_images[131] + sizeof(jbd_header_t));
  assert(__builtin_bswap32(tags1[0].t_blocknr) == 2126);
  assert(__builtin_bswap32(tags1[3].t_blocknr) == 2129);
  assert(__builtin_bswap32(tags1[3].t_flags) & JBD_FLAG_LAST_TAG);
  assert(!__builtin_bswap32(tags1[4].t_flags) && !__builtin_bswap32(tags1[4].t_blocknr));
  assert(__builtin_bswap32(((jbd_header_t *)write_images[131])->h_sequence) == seq);
  assert(__builtin_bswap32(((jbd_header_t *)write_images[132])->h_blocktype) == JBD_COMMIT_BLOCK);
  puts("PASS 130-block transaction spans two descriptor groups with per-segment LAST_TAG");

  /* FIFO handoff: the oldest waiter gets the lock first, and a fresh arrival
   * cannot barge ahead of queued writers. */
  fifo_gate[0] = fifo_gate[1] = fifo_gate[2] = false;
  fifo_started_count = fifo_acquired_count = fifo_finished_count = 0;
  fifo_arg_t args[3];
  pthread_t workers[3];
  owner.tid = 1;
  owner.state = THREAD_RUNNING;
  assert(ext3_journal_start(mnt) == 0);
  args[0] = (fifo_arg_t){.mnt = mnt, .slot = 0};
  assert(pthread_create(&workers[0], NULL, fifo_worker, &args[0]) == 0);
  uint32_t tid_a = wait_waiter(mnt, 0);
  args[1] = (fifo_arg_t){.mnt = mnt, .slot = 1};
  assert(pthread_create(&workers[1], NULL, fifo_worker, &args[1]) == 0);
  uint32_t tid_b = wait_waiter(mnt, 1);
  assert(tid_a != tid_b);
  assert(ext3_journal_stop(mnt) == 0);
  assert(mnt->journal.owner_tid == tid_a && mnt->journal.active &&
         mnt->journal.lock.locked);
  assert(mnt->journal.granted_tid == 0 || mnt->journal.granted_tid == tid_a);
  wait_fifo_acquired(1);
  /* Fresh arrival C queues behind B, even though A just took ownership. */
  args[2] = (fifo_arg_t){.mnt = mnt, .slot = 2};
  assert(pthread_create(&workers[2], NULL, fifo_worker, &args[2]) == 0);
  uint32_t tid_c = wait_waiter(mnt, 1);
  assert(tid_c != tid_a && tid_c != tid_b);
  pthread_mutex_lock(&wait_mutex);
  wait_queue_entry_t *head = mnt->journal.waiters.head;
  assert(head && head->thread && head->thread->tid == tid_b);
  pthread_mutex_unlock(&wait_mutex);
  /* Releasing A hands the transaction to B, not to the younger C. */
  pthread_mutex_lock(&fifo_mutex);
  fifo_gate[0] = true;
  pthread_cond_broadcast(&fifo_cond);
  pthread_mutex_unlock(&fifo_mutex);
  wait_fifo_acquired(2);
  pthread_mutex_lock(&fifo_mutex);
  assert(fifo_acquired[0] == tid_a && fifo_acquired[1] == tid_b);
  pthread_mutex_unlock(&fifo_mutex);
  pthread_mutex_lock(&wait_mutex);
  head = mnt->journal.waiters.head;
  assert(head && head->thread && head->thread->tid == tid_c &&
         head->thread->state == THREAD_BLOCKED);
  pthread_mutex_unlock(&wait_mutex);
  assert(mnt->journal.owner_tid == tid_b && mnt->journal.granted_tid == 0);
  /* B's stop grants C (oldest remaining), completing strict FIFO order. */
  pthread_mutex_lock(&fifo_mutex);
  fifo_gate[1] = true;
  fifo_gate[2] = true;
  pthread_cond_broadcast(&fifo_cond);
  pthread_mutex_unlock(&fifo_mutex);
  for (unsigned i = 0; i < 3; i++)
    assert(pthread_join(workers[i], NULL) == 0);
  assert(fifo_started_count == 3 && fifo_acquired_count == 3 &&
         fifo_finished_count == 3);
  assert(fifo_acquired[0] == tid_a && fifo_acquired[1] == tid_b &&
         fifo_acquired[2] == tid_c);
  assert(fifo_finished[0] == tid_a && fifo_finished[1] == tid_b &&
         fifo_finished[2] == tid_c);
  assert(!mnt->journal.active && !mnt->journal.lock.locked &&
         !mnt->journal.waiters.head && mnt->journal.granted_tid == 0);
  puts("PASS FIFO handoff: oldest waiter first, fresh arrivals cannot barge");

  writes = reads = 0;
  seq = mnt->journal.sequence;
  owner.tid = 1;
  pthread_t pool[4];
  for (unsigned i = 0; i < 4; i++)
    assert(pthread_create(&pool[i], NULL, transaction_worker, mnt) == 0);
  for (unsigned i = 0; i < 4; i++) assert(pthread_join(pool[i], NULL) == 0);
  assert(writes == 128 * 6 && reads == 128);
  assert(mnt->journal.sequence == seq + 128 && !mnt->journal.lock.locked);
  for (unsigned i = 0; i < 128; i++) {
    unsigned off = i * 6;
    assert(!memcmp(write_images[off], write_images[off + 4], 1024));
    assert(write_blocks[off] == 102 && write_blocks[off + 1] == 101 &&
           write_blocks[off + 2] == 103 && write_blocks[off + 3] == 100 &&
           write_blocks[off + 4] == 7 && write_blocks[off + 5] == 100);
    assert(__builtin_bswap32(((jbd_header_t *)write_images[off + 1])->h_sequence)
           == seq + i);
    assert(__builtin_bswap32(((jbd_header_t *)write_images[off + 2])->h_sequence)
           == seq + i);
  }
  puts("PASS four writers: nested transactions serialize shared buffers and preserve images/sequences");

  /* Every transaction-scoped allocation can fail: image, write-set chunk, or
   * one of the commit scratch buffers. Each failure releases the transaction
   * lock and no staged image survives. */
  unsigned live_before_failures = live_allocations;
  for (int i = 1; i <= 5; i++) {
    ext2_mount_t *cold = calloc(1, sizeof(*cold));
    assert(cold);
    cold->block_size = 1024;
    cold->sb.s_blocks_count = 4096;
    cold->sb.s_feature_compat = EXT3_FEATURE_COMPAT_HAS_JOURNAL;
    allocations = writes = reads = 0;
    fail_allocation = i;
    assert(ext3_journal_start(cold) == 0);
    int block_result = ext3_journal_block(cold, 7, buf);
    assert(ext3_journal_stop(cold) == -1);
    assert(writes == 0 && reads == 0);
    assert(block_result == (i <= 2 ? -1 : 0));
    /* Commit scratch allocated before the failing allocation is deliberately
     * kept for reuse by the mount, so only the write-set must be gone. */
    assert(live_allocations == live_before_failures + (i >= 3 ? 2 : 0));
    assert(!cold->journal.active && !cold->journal.lock.locked &&
           !cold->journal.txn_head && !cold->journal.txn_tail &&
           cold->journal.txn_count == 0);
    fail_allocation = 0;
    assert(ext3_journal_start(cold) == 0);
    assert(ext3_journal_block(cold, 7, buf) == 0);
    assert(ext3_journal_stop(cold) == 0);
    kfree(cold->journal.desc_block_buf);
    kfree(cold->journal.commit_block_buf);
    kfree(cold->journal.super_block_buf);
    assert(live_allocations == live_before_failures);
    free(cold);
  }
  puts("PASS image/chunk/scratch allocation failures release ownership and retry succeeds");
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
    assert(writes == 0);
    assert(ext3_journal_stop(large) == 0);
    assert(writes == 2 * tags + 4 && reads == 1);
    for (uint32_t i = 0; i < tags; i++) {
      assert(write_blocks[i] == 102 + i);
      assert(!memcmp(write_images[i], data, bs));
    }
    assert(write_blocks[tags] == 101);
    assert(write_blocks[tags + 1] == 102 + tags);
    assert(write_blocks[tags + 2] == 100);
    for (uint32_t i = 0; i < tags; i++) {
      assert(write_blocks[tags + 3 + i] == 2000 + i);
      assert(!memcmp(write_images[tags + 3 + i], data, bs));
    }
    assert(write_blocks[2 * tags + 3] == 100);
    kfree(large->journal.desc_block_buf);
    kfree(large->journal.commit_block_buf);
    kfree(large->journal.super_block_buf);
    free(large);
  }
  puts("PASS 2/4 KiB blocks: full descriptors and repeated mapping collisions retain payloads");

  /* Recovery replays only complete transactions, follows multiple descriptor
   * groups, and restores escaped magic words. */
  memset(mock_disk, 0, sizeof(mock_disk));
  ext2_mount_t rmnt = {0};
  rmnt.block_size = 1024;
  rmnt.sb.s_blocks_count = 4096;
  rmnt.journal.journal_blocks = 1024;
  ext2_inode_t jin = {0};
  jbd_superblock_t rjsb = {0};
  rjsb.s_header.h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
  rjsb.s_header.h_blocktype = __builtin_bswap32(JBD_SUPERBLOCK_V1);
  rjsb.s_blocksize = __builtin_bswap32(1024);
  rjsb.s_maxlen = __builtin_bswap32(1024);
  rjsb.s_start = __builtin_bswap32(1);
  unsigned char pat_a[1024], pat_b[1024], pat_c[1024];
  for (unsigned i = 0; i < 1024; i++) {
    pat_a[i] = (unsigned char)(i * 3 + 1);
    pat_b[i] = (unsigned char)(i * 7 + 2);
    pat_c[i] = (unsigned char)(i ^ 0x5a);
  }
  fail_read = fail_write = 0;
  fail_mapping = -1;

  /* One committed transaction, sequence 5. */
  rjsb.s_sequence = __builtin_bswap32(5);
  memset(mock_disk[7], 0xAA, 1024);
  memset(mock_disk[8], 0xAA, 1024);
  put_journal_header(101, JBD_DESCRIPTOR_BLOCK, 5);
  put_tag(101, 0, 7, 0);
  put_tag(101, 1, 8, JBD_FLAG_LAST_TAG);
  memcpy(mock_disk[102], pat_a, 1024);
  memcpy(mock_disk[103], pat_b, 1024);
  put_journal_header(104, JBD_COMMIT_BLOCK, 5);
  writes = reads = 0;
  assert(ext3_recover_journal(&rmnt, &rjsb, &jin) == 0);
  assert(writes == 2);
  assert(!memcmp(mock_disk[7], pat_a, 1024));
  assert(!memcmp(mock_disk[8], pat_b, 1024));

  /* Two descriptor groups in one transaction, sequence 6. */
  rjsb.s_sequence = __builtin_bswap32(6);
  memset(mock_disk[7], 0xAA, 1024);
  memset(mock_disk[8], 0xAA, 1024);
  memset(mock_disk[9], 0xAA, 1024);
  put_journal_header(101, JBD_DESCRIPTOR_BLOCK, 6);
  put_tag(101, 0, 7, 0);
  put_tag(101, 1, 8, JBD_FLAG_LAST_TAG);
  memcpy(mock_disk[102], pat_a, 1024);
  memcpy(mock_disk[103], pat_b, 1024);
  put_journal_header(104, JBD_DESCRIPTOR_BLOCK, 6);
  put_tag(104, 0, 9, JBD_FLAG_LAST_TAG);
  memcpy(mock_disk[105], pat_c, 1024);
  put_journal_header(106, JBD_COMMIT_BLOCK, 6);
  writes = reads = 0;
  assert(ext3_recover_journal(&rmnt, &rjsb, &jin) == 0);
  assert(writes == 3);
  assert(!memcmp(mock_disk[7], pat_a, 1024));
  assert(!memcmp(mock_disk[8], pat_b, 1024));
  assert(!memcmp(mock_disk[9], pat_c, 1024));

  /* Missing commit record: no home block may be touched. */
  rjsb.s_sequence = __builtin_bswap32(7);
  memset(mock_disk[7], 0xAA, 1024);
  memset(mock_disk[8], 0xAA, 1024);
  put_journal_header(101, JBD_DESCRIPTOR_BLOCK, 7);
  put_tag(101, 0, 7, 0);
  put_tag(101, 1, 8, JBD_FLAG_LAST_TAG);
  memcpy(mock_disk[102], pat_a, 1024);
  memcpy(mock_disk[103], pat_b, 1024);
  writes = reads = 0;
  assert(ext3_recover_journal(&rmnt, &rjsb, &jin) == -1);
  assert(writes == 0);
  assert(mock_disk[7][0] == 0xAA && mock_disk[8][0] == 0xAA);

  /* Escaped payload: the log copy has zeroes, replay restores the magic. */
  rjsb.s_sequence = __builtin_bswap32(8);
  memset(mock_disk[7], 0xAA, 1024);
  put_journal_header(101, JBD_DESCRIPTOR_BLOCK, 8);
  put_tag(101, 0, 7, JBD_FLAG_ESCAPE | JBD_FLAG_LAST_TAG);
  memset(mock_disk[102], 0, 1024);
  memcpy(mock_disk[102] + 4, pat_a + 4, 1020);
  put_journal_header(103, JBD_COMMIT_BLOCK, 8);
  writes = reads = 0;
  assert(ext3_recover_journal(&rmnt, &rjsb, &jin) == 0);
  assert(writes == 1);
  assert(*(uint32_t *)mock_disk[7] == __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER));
  assert(!memcmp(mock_disk[7] + 4, pat_a + 4, 1020));
  puts("PASS recovery: committed single/multi-descriptor replay, incomplete "
       "ignored, escaped magic restored");

  kfree(mnt->journal.commit_block_buf);
  kfree(mnt->journal.super_block_buf);
  kfree(mnt->journal.desc_block_buf);
  free(mnt);
  assert(live_allocations == 0);
  puts("PASS all journal allocations released");
  return 0;
}
