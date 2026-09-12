/* LinuxKPI bit waits.  See linux/wait_bit.h for the contract. */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/wait_bit.h>

#define KPI_BIT_WQ_BUCKETS 64

static struct wait_queue_head bit_wqs[KPI_BIT_WQ_BUCKETS];
static bool bit_wqs_ready;

static struct wait_queue_head *bit_wq(void *word) {
  unsigned long hash = ((unsigned long)word >> 3) % KPI_BIT_WQ_BUCKETS;
  return &bit_wqs[hash];
}

static void bit_wqs_init(void) {
  for (int i = 0; i < KPI_BIT_WQ_BUCKETS; i++)
    init_waitqueue_head(&bit_wqs[i]);
  bit_wqs_ready = true;
}

static struct wait_queue_head *bit_wq_ready(void *word) {
  if (!bit_wqs_ready)
    bit_wqs_init();
  return bit_wq(word);
}

int wait_on_bit(unsigned long *word, int bit, unsigned int mode) {
  (void)mode;
  while (test_bit(bit, word))
    wait_event(*bit_wq_ready(word), !test_bit(bit, word));
  return 0;
}

int wait_on_bit_io(unsigned long *word, int bit, unsigned int mode) {
  return wait_on_bit(word, bit, mode);
}

int wait_on_bit_timeout(unsigned long *word, int bit, unsigned int mode,
                        unsigned long timeout) {
  (void)mode;
  while (test_bit(bit, word)) {
    long ret = wait_event_timeout(*bit_wq_ready(word), !test_bit(bit, word),
                                  timeout);
    if (!ret)
      return -EAGAIN;
  }
  return 0;
}

int wait_on_bit_action(unsigned long *word, int bit, int (*action)(void *),
                       unsigned int mode) {
  (void)action;
  return wait_on_bit(word, bit, mode);
}

int wait_on_bit_lock(unsigned long *word, int bit) {
  for (;;) {
    wait_on_bit(word, bit, TASK_UNINTERRUPTIBLE);
    if (!test_and_set_bit(bit, word))
      return 0;
  }
}

int wait_on_bit_lock_io(unsigned long *word, int bit) {
  return wait_on_bit_lock(word, bit);
}

int wait_on_bit_lock_action(unsigned long *word, int bit,
                            int (*action)(void *)) {
  (void)action;
  return wait_on_bit_lock(word, bit);
}

void wake_up_bit(void *word, int bit) {
  (void)bit;
  if (bit_wqs_ready)
    wake_up_all(bit_wq(word));
}

void wake_up_var(void *var) { wake_up_bit(var, 0); }
