/* Phase 1 — chunk 6 core tests: ww_mutex/rwsem/semaphore/wait_bit/tasklet,
 * device+devres, pointer format strings, bitmap/kstrtox, and the DRM-like
 * integration sequence (mutex -> ww_mutex -> workqueue -> hrtimer -> RCU). */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tasklet.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>

#include <linuxkpi/log.h>

/* ── ww_mutex contention ────────────────────────────────────────────────── */

static struct ww_class ww_cls;
static DEFINE_WW_MUTEX(ww_lock, &ww_cls);

struct ww_test {
  volatile int counter;
  volatile int done;
};

static int ww_thread(void *arg) {
  struct ww_test *t = arg;
  struct ww_acquire_ctx ctx;

  ww_acquire_init(&ctx, &ww_cls);
  for (int i = 0; i < 300; i++) {
    ww_mutex_lock(&ww_lock, &ctx);
    t->counter++;
    ww_mutex_unlock(&ww_lock);
  }
  ww_acquire_done(&ctx);

  __atomic_add_fetch(&t->done, 1, __ATOMIC_RELEASE);
  return 0;
}

static bool test_ww_mutex(void) {
  static struct ww_test t;

  t.counter = 0;
  t.done = 0;

  struct task_struct *a = kthread_run(ww_thread, &t, "kpi/ww1");
  struct task_struct *b = kthread_run(ww_thread, &t, "kpi/ww2");
  if (IS_ERR(a) || IS_ERR(b))
    return false;

  unsigned long deadline = jiffies + msecs_to_jiffies(5000);
  while (__atomic_load_n(&t.done, __ATOMIC_ACQUIRE) < 2 &&
         time_before(jiffies, deadline))
    msleep(1);

  bool ok = __atomic_load_n(&t.done, __ATOMIC_ACQUIRE) == 2 && t.counter == 600;
  kthread_stop(a);
  kthread_stop(b);
  return ok;
}

/* ── ww_mutex: multi-lock ordered acquisition (TTM/amdgpu BO style) ─────── */

#define WW_BO_LOCKS 4
static struct ww_class ww_multi_cls;
static struct ww_mutex ww_multi[WW_BO_LOCKS];

struct ww_multi_test {
  volatile int iterations;
  volatile int done;
  volatile int bad;
};

static int ww_multi_thread(void *arg) {
  struct ww_multi_test *t = arg;
  struct ww_acquire_ctx ctx;

  ww_acquire_init(&ctx, &ww_multi_cls);
  for (int i = 0; i < 150; i++) {
    int n = 0;

    /* Every thread acquires in the same global lock order - the invariant
     * TTM/amdgpu keep when reserving a BO set.  Wounding is not implemented
     * in this LinuxKPI (see linux/ww_mutex.h), so the order is what makes
     * multi-lock acquisition deadlock-free; this stress proves the ordered
     * pattern works under real contention. */
    for (int k = 0; k < WW_BO_LOCKS; k++) {
      if (ww_mutex_lock(&ww_multi[k], &ctx) == 0)
        n++;
      else
        break;
    }
    for (int k = n - 1; k >= 0; k--)
      ww_mutex_unlock(&ww_multi[k]);
    if (n != WW_BO_LOCKS)
      __atomic_store_n(&t->bad, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&t->iterations, 1, __ATOMIC_RELEASE);
  }
  ww_acquire_done(&ctx);
  __atomic_add_fetch(&t->done, 1, __ATOMIC_RELEASE);
  return 0;
}

static bool test_ww_mutex_multi(void) {
  static struct ww_multi_test t;
  struct task_struct *th[3];

  for (int i = 0; i < WW_BO_LOCKS; i++)
    ww_mutex_init(&ww_multi[i], &ww_multi_cls);
  t.iterations = 0;
  t.done = 0;
  t.bad = 0;

  for (int i = 0; i < 3; i++) {
    th[i] = kthread_run(ww_multi_thread, &t, "kpi/wwm");
    if (IS_ERR(th[i]))
      return false;
  }

  unsigned long deadline = jiffies + msecs_to_jiffies(10000);
  while (__atomic_load_n(&t.done, __ATOMIC_ACQUIRE) < 3 &&
         time_before(jiffies, deadline))
    msleep(1);

  bool ok = __atomic_load_n(&t.done, __ATOMIC_ACQUIRE) == 3 &&
            __atomic_load_n(&t.bad, __ATOMIC_ACQUIRE) == 0 &&
            t.iterations == 3 * 150;
  for (int i = 0; i < 3; i++)
    kthread_stop(th[i]);
  for (int i = 0; i < WW_BO_LOCKS; i++)
    ww_mutex_destroy(&ww_multi[i]);
  return ok;
}

/* ── rwsem: readers share, writer excludes ──────────────────────────────── */

static DECLARE_RWSEM(core_rwsem);
static volatile int rwsem_readers_sum;
static volatile int rwsem_writer_sum;
static volatile int rwsem_threads_done;

static int rwsem_reader_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < 200; i++) {
    down_read(&core_rwsem);
    rwsem_readers_sum++;
    up_read(&core_rwsem);
  }
  __atomic_add_fetch(&rwsem_threads_done, 1, __ATOMIC_RELEASE);
  return 0;
}

static int rwsem_writer_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < 100; i++) {
    down_write(&core_rwsem);
    rwsem_writer_sum++;
    up_write(&core_rwsem);
  }
  __atomic_add_fetch(&rwsem_threads_done, 1, __ATOMIC_RELEASE);
  return 0;
}

static bool test_rwsem(void) {
  rwsem_readers_sum = 0;
  rwsem_writer_sum = 0;
  rwsem_threads_done = 0;

  struct task_struct *r1 = kthread_run(rwsem_reader_thread, NULL, "kpi/rs1");
  struct task_struct *r2 = kthread_run(rwsem_reader_thread, NULL, "kpi/rs2");
  struct task_struct *w = kthread_run(rwsem_writer_thread, NULL, "kpi/rsw");
  if (IS_ERR(r1) || IS_ERR(r2) || IS_ERR(w))
    return false;

  unsigned long deadline = jiffies + msecs_to_jiffies(5000);
  while (__atomic_load_n(&rwsem_threads_done, __ATOMIC_ACQUIRE) < 3 &&
         time_before(jiffies, deadline))
    msleep(1);

  bool ok = __atomic_load_n(&rwsem_threads_done, __ATOMIC_ACQUIRE) == 3 &&
            rwsem_readers_sum == 400 && rwsem_writer_sum == 100;
  kthread_stop(r1);
  kthread_stop(r2);
  kthread_stop(w);
  return ok;
}

/* ── counting semaphore ─────────────────────────────────────────────────── */

static struct semaphore core_sem;

static int sem_up_thread(void *arg) {
  struct semaphore *s = arg;
  msleep(15);
  up(s);
  return 0;
}

static bool test_semaphore(void) {
  sema_init(&core_sem, 0);

  struct task_struct *t = kthread_run(sem_up_thread, &core_sem, "kpi/sem");
  if (IS_ERR(t))
    return false;

  unsigned long start = jiffies;
  int ret = down_interruptible(&core_sem);
  unsigned long waited = jiffies - start;

  kthread_stop(t);
  return ret == 0 && waited >= 10;

  /* NOTE: down() of an already-zero semaphore succeeds only after up(). */
}

/* ── wait_on_bit / wake_up_bit ──────────────────────────────────────────── */

static unsigned long core_bit_word;

static int bit_waker_thread(void *arg) {
  (void)arg;
  msleep(15);
  clear_bit(0, &core_bit_word);
  wake_up_bit(&core_bit_word, 0);
  return 0;
}

static bool test_wait_bit(void) {
  /* wait_on_bit() waits until the bit is clear (Linux semantics). */
  set_bit(0, &core_bit_word);

  struct task_struct *t = kthread_run(bit_waker_thread, NULL, "kpi/wbit");
  if (IS_ERR(t))
    return false;

  unsigned long start = jiffies;
  wait_on_bit(&core_bit_word, 0, TASK_UNINTERRUPTIBLE);
  bool ok = !test_bit(0, &core_bit_word) && (jiffies - start) >= 10;

  kthread_stop(t);
  return ok;
}

/* ── tasklet (workqueue-backed) ─────────────────────────────────────────── */

static volatile int core_tasklet_ran;
static void core_tasklet_fn(unsigned long data) {
  core_tasklet_ran = (int)data;
}
static DECLARE_TASKLET(core_tasklet, core_tasklet_fn, 7);

static bool test_tasklet(void) {
  core_tasklet_ran = 0;
  tasklet_schedule(&core_tasklet);

  unsigned long deadline = jiffies + msecs_to_jiffies(1000);
  while (!core_tasklet_ran && time_before(jiffies, deadline))
    msleep(1);

  tasklet_kill(&core_tasklet);
  return core_tasklet_ran == 7;
}

/* ── device + devres ────────────────────────────────────────────────────── */

static int core_devres_ran;
static void core_devres_action(void *data) {
  core_devres_ran = (int)(long)data;
}

static bool test_device_devres(void) {
  struct class *cls = class_create("kpitest");
  struct device *dev = device_create(cls, NULL, 0, NULL, "kpitest%d", 0);
  bool ok;

  if (IS_ERR(dev) || !cls)
    return false;

  void *p = devm_kzalloc(dev, 128, GFP_KERNEL);
  ok = p != NULL;
  if (p) {
    for (int i = 0; i < 128; i++)
      ok = ok && ((char *)p)[i] == 0;
  }

  core_devres_ran = 0;
  ok = ok && devm_add_action(dev, core_devres_action, (void *)3) == 0;

  device_destroy(cls, 0); /* releases devres */
  class_destroy(cls);
  return ok && core_devres_ran == 3;
}

/* ── pointer format extensions ──────────────────────────────────────────── */

static void pv_helper(char *buf, size_t size, const char *fmt, ...) {
  struct {
    const char *fmt;
    va_list *va;
  } vaf;
  va_list ap;

  va_start(ap, fmt);
  vaf.fmt = fmt;
  vaf.va = &ap;
  snprintf(buf, size, "pre %pV post", &vaf);
  va_end(ap);
}

static bool test_formats(void) {
  char buf[128];
  const u8 mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  const u8 bytes[4] = {0xde, 0xad, 0xbe, 0xef};
  const u64 pa = 0x123456789abcdef0ULL;

  snprintf(buf, sizeof(buf), "%pM", mac);
  if (strcmp(buf, "00:11:22:33:44:55") != 0)
    return false;

  snprintf(buf, sizeof(buf), "%*ph", 4, bytes);
  if (strcmp(buf, "de ad be ef") != 0)
    return false;

  snprintf(buf, sizeof(buf), "%pa", &pa);
  if (strcmp(buf, "0x123456789abcdef0") != 0)
    return false;

  snprintf(buf, sizeof(buf), "%pe", ERR_PTR(-EINVAL));
  if (strcmp(buf, "EINVAL") != 0)
    return false;

  pv_helper(buf, sizeof(buf), "val=%d", 42);
  if (strcmp(buf, "pre val=42 post") != 0)
    return false;

  /* "%.*s" (precision from an argument) is what stock drm_printf_indent()
   * prepends; if the parser does not consume the precision and string
   * arguments, the following %s reads an integer and faults (seen at C3). */
  snprintf(buf, sizeof(buf), "%.*sallocated by = %s", 2, "\t\t\t\t\tX",
           "kpi/tests");
  if (strcmp(buf, "\t\tallocated by = kpi/tests") != 0)
    return false;

  /* '+'/' '/'#' flags must be consumed too: an unrecognized flag consumes
   * no argument and shifts every later conversion the same way. */
  snprintf(buf, sizeof(buf), "%+d %#x % d %05d", 5, 0x2a, 7, -3);
  if (strcmp(buf, "+5 0x2a  7 -0003") != 0)
    return false;

  /* Buffer-full paths: the literal emit used to stop advancing the format
   * pointer once the destination was full (the side-effecting `*fmt++` was
   * skipped with the store), so any format longer than the buffer spun
   * forever.  Hit at C6 via alloc_workqueue() formatting the 27-char
   * "amdgpu_dm_hpd_rx_offload_wq" into its 24-byte WQ_NAME_LEN buffer.  A
   * truncating format must terminate and still return the full length. */
  {
    char small[8];
    int n;

    n = snprintf(small, sizeof(small), "amdgpu_dm_hpd_rx_offload_wq");
    if (n != 27 || strcmp(small, "amdgpu_") != 0)
      return false;

    n = snprintf(small, sizeof(small), "%020d", 5);
    if (n != 20 || strcmp(small, "0000000") != 0)
      return false;
  }

  return true;
}

/* ── bitmap / kstrtox ───────────────────────────────────────────────────── */

static bool test_bitmap_kstrtox(void) {
  unsigned long mask[2] = {0, 0};
  int v = 0;

  /* bitmap_parselist takes "1-3,5" ranges; bitmap_parse takes hex. */
  if (bitmap_parselist("1-3,5", mask, 64) != 0)
    return false;

  if (!test_bit(1, mask) || !test_bit(2, mask) || !test_bit(3, mask) ||
      !test_bit(5, mask) || test_bit(0, mask) || test_bit(4, mask))
    return false;

  if (kstrtoint("123", 10, &v) != 0 || v != 123)
    return false;

  unsigned long bits = 0;
  bitmap_set(&bits, 3, 4);
  return bits == 0x78UL;
}

/* ── DRM-like integration: mutex -> ww_mutex -> work -> hrtimer -> RCU ──── */

static volatile int integ_work_ran;
static volatile int integ_timer_ran;
static volatile int integ_rcu_ran;

static void integ_work_fn(struct work_struct *work) {
  (void)work;
  integ_work_ran = 1;
}

static enum hrtimer_restart integ_timer_fn(struct hrtimer *timer) {
  (void)timer;
  integ_timer_ran = 1;
  return HRTIMER_NORESTART;
}

static void integ_rcu_fn(struct rcu_head *head) {
  (void)head;
  integ_rcu_ran = 1;
}

static bool test_integration(void) {
  static struct mutex mtx;
  static struct ww_class wc;
  static struct ww_mutex wwl;
  static struct work_struct work;
  static struct hrtimer timer;
  static struct rcu_head rcu;
  struct ww_acquire_ctx ctx;

  mutex_init(&mtx);
  ww_mutex_init(&wwl, &wc);
  INIT_WORK(&work, integ_work_fn);
  hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  timer.function = integ_timer_fn;

  integ_work_ran = integ_timer_ran = integ_rcu_ran = 0;

  mutex_lock(&mtx);
  mutex_unlock(&mtx);

  ww_acquire_init(&ctx, &wc);
  ww_mutex_lock(&wwl, &ctx);
  ww_mutex_unlock(&wwl);
  ww_acquire_done(&ctx);

  queue_work(system_wq, &work);
  flush_work(&work);

  hrtimer_start(&timer, ms_to_ktime(10), HRTIMER_MODE_REL);
  unsigned long deadline = jiffies + msecs_to_jiffies(1000);
  while (!integ_timer_ran && time_before(jiffies, deadline))
    msleep(1);
  hrtimer_cancel(&timer);

  call_rcu(&rcu, integ_rcu_fn);
  rcu_barrier();

  return integ_work_ran == 1 && integ_timer_ran == 1 && integ_rcu_ran == 1;
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase1_core(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"ww_mutex", test_ww_mutex},
      {"ww_mutex multi-lock ordered", test_ww_mutex_multi},
      {"rwsem", test_rwsem},
      {"semaphore", test_semaphore},
      {"wait_bit", test_wait_bit},
      {"tasklet", test_tasklet},
      {"device/devres", test_device_devres},
      {"pointer formats", test_formats},
      {"bitmap/kstrtox", test_bitmap_kstrtox},
      {"integration sequence", test_integration},
  };

  klog_puts("[LINUXKPI] Phase 1 chunk-6 core self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
