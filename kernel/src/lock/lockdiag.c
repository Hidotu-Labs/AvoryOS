/* ===========================================================================
 * lockdiag — who is stopped, who is waiting on them, and what they hold.
 *
 * Before this file, a hang produced nothing: serial.log simply ended.  The
 * shootdown ack watchdog (mm/tlb_shootdown.c) was the only timeout anywhere in
 * the kernel, so every other stall — a spin inside a page fault, a lost futex
 * wake, a reap worker that never gets to run — looked identical from the
 * outside: a dead screen and a silent serial line.
 *
 * What this does:
 *
 *   - Every CPU stamps a heartbeat from its own LAPIC timer tick.  The tick is
 *     the one thing a healthy kernel always returns to, so a stale stamp means
 *     that core has not left a non-maskable context in LOCKDIAG_HUNG_CPU_MS.
 *   - Any live core that notices a stale stamp claims the report (one atomic
 *     exchange) and probes every other core with an IPI.  A core answers the
 *     probe only if it can take interrupts, so the probe separates "slow" from
 *     "masked and spinning", which is the distinction that matters.
 *   - The report is printed with serial_write_sync() only.  It takes no locks
 *     at all: the lock that hung the machine may be serial_lock, and calling
 *     klogf() here would hang the report too and leave the log ending
 *     mid-sentence — which is the failure mode this file exists to remove.
 *   - Stuck cores cannot cooperate, so their state is read from memory the way
 *     a debugger would: cpu_info, its current thread, the per-thread ktrack
 *     breadcrumb and last syscall, and a scan of that core's kernel stack for
 *     return addresses (the same best-effort unwind cpu/kpf_dump.c uses,
 *     because the kernel is built at -O2 without frame pointers).
 *
 * Cost when nothing is wrong: a couple of stores per timer tick, a few relaxed
 * atomics per futex operation, and nothing at all in the allocation paths.
 * =========================================================================== */

#include "lockdiag.h"

#if LOCKDIAG

#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../cpu/isr.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../lib/tsc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "spinlock.h"
#include <stddef.h>

/* mm/tlb_shootdown.c: per-call-site shootdown accounting. */
uint64_t tlb_shootdown_caller_info(int slot, uint64_t *ip, uint64_t *total,
                                   uint64_t *under_vmm_lock);

/* The kernel image is linked at this base; anything at or above it is kernel
 * code, which is the filter the stack scans use to spot return addresses. */
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL
extern char __kernel_text_start[], __kernel_text_end[];
#include "fs/ext4/ext4.h"

static ext3_journal_state_t *lockdiag_root_journal(void) {
  /* Root directories have no regular-file read callback. Identify their
   * mount through statfs instead of requiring ext4_read_impl. */
  if (!fs_root || !fs_root->device ||
      (fs_root->statfs != ext4_statfs_impl &&
       fs_root->statfs != ext2_statfs_impl))
    return NULL;
  ext2_mount_t *mnt = fs_root->device;
  return &mnt->journal;
}

/* NOTE on units: tsc_get_mhz() returns *megahertz*, i.e. TSC ticks per
 * MICROSECOND, so one millisecond is mhz * 1000 ticks.  mm/tlb_shootdown.c
 * multiplies its timeout by mhz alone, which makes its "10 s" watchdog fire
 * after 10 ms; the arithmetic here is written with that in mind. */
#define LOCKDIAG_TICKS_PER_MS(mhz) ((mhz) * 1000ULL)

extern spinlock_t tid_lock;            /* sched/sched_thread.c */
extern struct thread *global_thread_list; /* sched/sched_thread.c */

/* Bucket-wide wake traffic; collisions and normal no-waiter wakes mean these
 * totals alone cannot establish that a particular thread lost a wakeup. */
uint64_t futex_bucket_stats(uint32_t bucket, uint64_t *wakes,
                            uint64_t *no_match);

/* syscalls/syscall.c */
const char *syscall_get_name(uint64_t num);

/* drivers/serial.c: queued-but-unsent bytes, to tell a dead log from a dead
 * kernel. */
uint32_t serial_pending_bytes(void);

/* --- spotlighted locks --------------------------------------------------- */

static lockdiag_spot_t g_spots[LOCKDIAG_SPOT_COUNT] = {
    [LOCKDIAG_SPOT_VMM] = {.name = "vmm_lock"},
    [LOCKDIAG_SPOT_SHOOTDOWN] = {.name = "shootdown_lock"},
    [LOCKDIAG_SPOT_GPU_POLL] = {.name = "gpu_poll_lock"},
    [LOCKDIAG_SPOT_GPU_PRESENT] = {.name = "gpu_present_lock"},
    [LOCKDIAG_SPOT_GPU_CURSOR] = {.name = "gpu_cursor_lock"},
    [LOCKDIAG_SPOT_GPU_SHAPE] = {.name = "gpu_cursor_shape_lock"},
};

const char *lockdiag_spot_name(int id) {
  if (id < 0 || id >= LOCKDIAG_SPOT_COUNT)
    return "?";
  return g_spots[id].name ? g_spots[id].name : "?";
}

lockdiag_spot_t *lockdiag_spot(int id) {
  if (id < 0 || id >= LOCKDIAG_SPOT_COUNT)
    return (lockdiag_spot_t *)0;
  return &g_spots[id];
}

void lockdiag_spot_take(int id, uint64_t wait_cycles, uint64_t ip) {
  if (id < 0 || id >= LOCKDIAG_SPOT_COUNT)
    return;
  lockdiag_spot_t *s = &g_spots[id];
  struct cpu_info *c = cpu_get_current();

  __atomic_add_fetch(&s->acquires, 1, __ATOMIC_RELAXED);
  if (wait_cycles) {
    /* Only the contended path pays for the max/CAS bookkeeping. */
    lockdiag_stat_max(&s->max_wait_cycles, wait_cycles);
    uint64_t mhz = tsc_get_mhz();
    if (mhz &&
        wait_cycles > LOCKDIAG_TICKS_PER_MS(mhz) * LOCKDIAG_SLOW_ACQUIRE_MS)
      __atomic_add_fetch(&s->slow_waits, 1, __ATOMIC_RELAXED);
  }

  /* Nested acquisition by the same owner keeps the original timestamp: the
   * interesting question is how long the lock has been held in total. */
  if (__atomic_add_fetch(&s->depth, 1, __ATOMIC_RELAXED) != 1)
    return;

  __atomic_store_n(&s->owner_cpu, c ? c->cpu_id : 0xFFFFFFFFULL,
                   __ATOMIC_RELAXED);
  struct thread *t = c ? c->current_thread : (struct thread *)0;
  __atomic_store_n(&s->owner_tid, t ? t->tid : 0xFFFFFFFFULL, __ATOMIC_RELAXED);
  __atomic_store_n(&s->taken_ip, ip, __ATOMIC_RELAXED);
  __atomic_store_n(&s->taken_tsc, rdtsc(), __ATOMIC_RELEASE);
}

void lockdiag_spot_drop(int id) {
  if (id < 0 || id >= LOCKDIAG_SPOT_COUNT)
    return;
  lockdiag_spot_t *s = &g_spots[id];

  uint64_t depth = __atomic_load_n(&s->depth, __ATOMIC_RELAXED);
  if (depth == 0)
    return; /* released without a recorded acquisition (untracked path) */
  if (depth > 1) {
    __atomic_sub_fetch(&s->depth, 1, __ATOMIC_RELAXED);
    return;
  }
  uint64_t taken = __atomic_load_n(&s->taken_tsc, __ATOMIC_RELAXED);
  if (taken)
    lockdiag_stat_max(&s->max_hold_cycles, rdtsc() - taken);
  __atomic_store_n(&s->taken_tsc, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s->owner_cpu, 0xFFFFFFFFULL, __ATOMIC_RELEASE);
  __atomic_store_n(&s->depth, 0, __ATOMIC_RELEASE);
}

bool lockdiag_spot_held_by_self(int id) {
  if (id < 0 || id >= LOCKDIAG_SPOT_COUNT)
    return false;
  struct cpu_info *c = cpu_get_current();
  if (!c || !__atomic_load_n(&g_spots[id].depth, __ATOMIC_ACQUIRE))
    return false;
  return __atomic_load_n(&g_spots[id].owner_cpu, __ATOMIC_ACQUIRE) ==
         c->cpu_id;
}

/* vmm_lock is held while we wait for shootdown acknowledgements: every remote
 * core takes its page fault handler through that same lock, so it is waiting
 * for an acknowledgement that core can no longer send. */
bool lockdiag_self_holds_vmm_lock(uint32_t self_id) {
  (void)self_id;
  return lockdiag_spot_held_by_self(LOCKDIAG_SPOT_VMM);
}

static lockdiag_stats_t g_stats;

lockdiag_stats_t *lockdiag_stats(void) { return &g_stats; }

/* --- contended-acquire tracking ----------------------------------------- */

typedef struct {
  volatile uint64_t lock;
  volatile uint64_t ip;
  volatile uint64_t since_tsc;
  volatile uint32_t active;
  uint32_t _pad;
  uint64_t _reserved[4]; /* keep each slot on its own cache line */
} spin_wait_t __attribute__((aligned(64)));

static spin_wait_t g_spin_wait[MAX_CPUS];

void lockdiag_spin_begin(const void *lock, uint64_t ip) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return;
  spin_wait_t *w = &g_spin_wait[c->cpu_id];
  w->lock = (uint64_t)lock;
  w->ip = ip;
  w->since_tsc = rdtsc();
  __atomic_store_n(&w->active, 1, __ATOMIC_RELEASE);
}

void lockdiag_spin_end(const void *lock) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return;
  spin_wait_t *w = &g_spin_wait[c->cpu_id];
  /* Only clear our own wait: an interrupt may have nested another contended
   * acquire on this CPU in between, and that one's record must survive. */
  if (__atomic_load_n(&w->lock, __ATOMIC_RELAXED) == (uint64_t)lock)
    __atomic_store_n(&w->active, 0, __ATOMIC_RELEASE);
}

bool lockdiag_get_spin_wait(uint32_t cpu_id, uint64_t *lock_out, uint64_t *ip_out, uint64_t *since_tsc_out) {
  if (cpu_id >= MAX_CPUS)
    return false;
  const spin_wait_t *w = &g_spin_wait[cpu_id];
  if (__atomic_load_n(&w->active, __ATOMIC_ACQUIRE) && w->lock) {
    if (lock_out) *lock_out = w->lock;
    if (ip_out) *ip_out = w->ip;
    if (since_tsc_out) *since_tsc_out = w->since_tsc;
    return true;
  }
  return false;
}

/* --- wait-queue holder tracking ----------------------------------------- */

#define WQ_HOLD_SLOTS 24

typedef struct {
  volatile uint64_t wq;
  volatile uint64_t tid;
  volatile uint64_t cpu;
  volatile uint64_t since_tsc;
  volatile uint64_t ip;
} wq_hold_t;

static wq_hold_t g_wq_holds[WQ_HOLD_SLOTS];

void lockdiag_wq_take(const void *wq, uint64_t ip) {
  if (!wq)
    return;
  struct cpu_info *c = cpu_get_current();
  struct thread *t = c ? c->current_thread : (struct thread *)0;
  for (int i = 0; i < WQ_HOLD_SLOTS; i++) {
    if (__atomic_load_n(&g_wq_holds[i].wq, __ATOMIC_RELAXED) == (uint64_t)wq)
      return; /* already recorded (nested or re-acquire) */
  }
  for (int i = 0; i < WQ_HOLD_SLOTS; i++) {
    if (__atomic_load_n(&g_wq_holds[i].wq, __ATOMIC_RELAXED) == 0) {
      g_wq_holds[i].tid = t ? t->tid : 0xFFFFFFFFULL;
      g_wq_holds[i].cpu = c ? c->cpu_id : 0xFFFFFFFFULL;
      g_wq_holds[i].ip = ip;
      g_wq_holds[i].since_tsc = rdtsc();
      __atomic_store_n(&g_wq_holds[i].wq, (uint64_t)wq, __ATOMIC_RELEASE);
      return;
    }
  }
  /* Table full: this is a diagnostic, not correctness. */
}

void lockdiag_wq_drop(const void *wq) {
  if (!wq)
    return;
  for (int i = 0; i < WQ_HOLD_SLOTS; i++) {
    if (__atomic_load_n(&g_wq_holds[i].wq, __ATOMIC_RELAXED) == (uint64_t)wq)
      __atomic_store_n(&g_wq_holds[i].wq, 0, __ATOMIC_RELEASE);
  }
}

/* The output helpers are defined further down with the rest of the report
 * machinery; declare what is needed here so this section can sit with the
 * tracking state. */
static void ld_str(const char *s);
static void ld_hex(uint64_t v);
static void ld_dec(uint64_t v);
static uint64_t cycles_to_ms(uint64_t cycles, uint64_t mhz);

/* Printed with the report: for every queue currently inside a wait_queue_*
 * critical section, who is in there and for how long.  A holder that is a
 * zombie, or that holds one of these for seconds, is the answer to "why can
 * nobody wake this queue". */
static void dump_wq_holds(uint64_t now_tsc, uint64_t mhz) {
  int any = 0;
  for (int i = 0; i < WQ_HOLD_SLOTS; i++) {
    uint64_t wq = __atomic_load_n(&g_wq_holds[i].wq, __ATOMIC_ACQUIRE);
    if (!wq)
      continue;
    if (!any) {
      ld_str("\n[LOCKDIAG] wait-queue holders:");
      any = 1;
    }
    ld_str("\n[LOCKDIAG]   wq=0x");
    ld_hex(wq);
    ld_str(" held by tid=");
    ld_dec(__atomic_load_n(&g_wq_holds[i].tid, __ATOMIC_RELAXED));
    ld_str(" cpu=");
    ld_dec(__atomic_load_n(&g_wq_holds[i].cpu, __ATOMIC_RELAXED));
    ld_str(" for=");
    ld_dec(cycles_to_ms(now_tsc -
                        __atomic_load_n(&g_wq_holds[i].since_tsc,
                                        __ATOMIC_RELAXED),
                        mhz));
    ld_str("ms entered-at=");
    ld_hex(__atomic_load_n(&g_wq_holds[i].ip, __ATOMIC_RELAXED));
  }
  if (!any)
    ld_str("\n[LOCKDIAG] wait-queue holders: none");
}

/* --- heartbeats and probe state ----------------------------------------- */

static volatile uint64_t hb_tsc[MAX_CPUS];
static volatile uint64_t hb_ticks[MAX_CPUS];

static volatile uint8_t probe_vector_state; /* 0 = no vector allocated yet */
static volatile uint8_t probe_seq;
static volatile uint8_t probe_ack[MAX_CPUS];
static volatile uint64_t probe_rip[MAX_CPUS];
static volatile uint64_t probe_rsp[MAX_CPUS];
static volatile uint64_t probe_rflags[MAX_CPUS];

/* One-shot: once a hang has been reported the box is frozen on purpose, and a
 * second report would only interleave with the first. */
static volatile uint32_t report_latched;
static volatile uint8_t inside_reporter[MAX_CPUS];

static uint64_t g_init_tsc;

/* Per-CPU and cache-line isolated, so the reporting core never shares a line
 * with the cores it is describing. */
typedef struct {
  const void *locks[6];
  uint64_t ips[6];
  volatile int depth;
} holds_t __attribute__((aligned(64)));

#if LOCKDIAG_OWNER
static holds_t g_holds[MAX_CPUS];
#endif

/* --- tiny lock-free output layer ---------------------------------------- */
/* serial_write_sync() bypasses the ring buffer and serial_lock: it is the only
 * writer that still works when a CPU is parked holding the serial lock. */

static void ld_str(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  serial_write_sync(s, n);
}

static void ld_hex(uint64_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (unsigned i = 0; i < 16; i++)
    buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
  serial_write_sync(buf, 18);
}

static void ld_dec(uint64_t v) {
  char buf[24];
  int i = (int)sizeof(buf);
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
  serial_write_sync(&buf[i], (size_t)(sizeof(buf) - (size_t)i));
}

static void ld_pad(uint64_t v, int width) {
  char buf[24];
  int i = (int)sizeof(buf);
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
  int digits = (int)sizeof(buf) - i;
  while (digits++ < width && i > 0)
    buf[--i] = ' ';
  serial_write_sync(&buf[i], (size_t)(sizeof(buf) - (size_t)i));
}

/* comm is 16 bytes and not guaranteed NUL terminated, and a corrupted one must
 * not take the report down with it. */
static void ld_comm(const char *comm) {
  ld_str("'");
  if (comm) {
    for (int i = 0; i < 16 && comm[i]; i++) {
      char c = comm[i];
      char out = (c >= 0x20 && c < 0x7F) ? c : '?';
      serial_write_sync(&out, 1);
    }
  }
  ld_str("'");
}

static const char *state_name(thread_state_t s) {
  switch (s) {
  case THREAD_RUNNING:
    return "RUNNING";
  case THREAD_READY:
    return "READY";
  case THREAD_BLOCKED:
    return "BLOCKED";
  case THREAD_SLEEPING:
    return "SLEEPING";
  case THREAD_DEAD:
    return "DEAD";
  case THREAD_ZOMBIE:
    return "ZOMBIE";
  default:
    return "?";
  }
}

static uint64_t cycles_to_ms(uint64_t cycles, uint64_t mhz) {
  return mhz ? cycles / LOCKDIAG_TICKS_PER_MS(mhz) : 0;
}

/* --- owner shadow stacks (LOCKDIAG_OWNER) ------------------------------- */

#if LOCKDIAG_OWNER

void lockdiag_hold_note(const void *lock, uint64_t ip) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return;
  holds_t *h = &g_holds[c->cpu_id];
  int d = h->depth;
  if (d >= (int)(sizeof(h->locks) / sizeof(h->locks[0])))
    return;
  h->locks[d] = lock;
  h->ips[d] = ip;
  h->depth = d + 1;
}

void lockdiag_hold_drop(const void *lock) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return;
  holds_t *h = &g_holds[c->cpu_id];
  /* Normally the innermost entry is the one being released; scanning covers
   * the error paths that unwind out of order. */
  for (int i = h->depth - 1; i >= 0; i--) {
    if (h->locks[i] != lock)
      continue;
    for (int j = i; j < h->depth - 1; j++) {
      h->locks[j] = h->locks[j + 1];
      h->ips[j] = h->ips[j + 1];
    }
    h->depth--;
    return;
  }
}

static void dump_holds(uint32_t cpu) {
  const holds_t *h = &g_holds[cpu];
  if (h->depth <= 0)
    return;
  ld_str("\n[LOCKDIAG]   holds(innermost first):");
  for (int i = h->depth - 1; i >= 0; i--) {
    ld_str(" lock@");
    ld_hex((uint64_t)h->locks[i]);
    ld_str("<-");
    ld_hex(h->ips[i]);
  }
}
#else
static void dump_holds(uint32_t cpu) { (void)cpu; }
#endif /* LOCKDIAG_OWNER */

/* --- probe IPI ---------------------------------------------------------- */

static void probe_isr(struct registers *regs) {
  struct cpu_info *c = cpu_get_current();
  if (!c || c->cpu_id >= MAX_CPUS)
    return;
  probe_rip[c->cpu_id] = regs->rip;
  probe_rsp[c->cpu_id] = regs->rsp;
  probe_rflags[c->cpu_id] = regs->rflags;
  /* Answering is proof of life on its own: the scheduler arms the LAPIC timer
   * one-shot per deadline, so a core that is merely idle could still be
   * between fallback ticks when its heartbeat is checked.  The idle loops
   * re-arm a 1 s fallback tick, but this stamp is what makes a quiet machine
   * distinguishable from a wedged one without waiting for that tick. */
  __atomic_store_n(&hb_tsc[c->cpu_id], rdtsc(), __ATOMIC_RELAXED);
  __atomic_store_n(&probe_ack[c->cpu_id],
                   __atomic_load_n(&probe_seq, __ATOMIC_RELAXED),
                   __ATOMIC_RELEASE);
}

/* probe_seq wraps at 256.  A core that answered a *newer* probe by the time
 * this waiter looks has still proven it can take interrupts, so an ack at or
 * after `seq` (mod 256) counts.  Comparing with == missed that ack whenever
 * two cores probed concurrently and the target stored the newer sequence,
 * which named a healthy core as hung. */
static inline bool probe_ack_reached(uint8_t ack, uint8_t seq) {
  return (int8_t)(ack - seq) >= 0;
}

uint64_t lockdiag_probe_cpus(uint32_t timeout_ms) {
  struct cpu_info *self = cpu_get_current();
  uint64_t mask = 0;
  if (!self)
    return 0;
  if (self->cpu_id < MAX_CPUS)
    mask |= 1ULL << self->cpu_id;
  if (!probe_vector_state)
    return mask;

  uint8_t seq = __atomic_add_fetch(&probe_seq, 1, __ATOMIC_RELAXED);
  uint32_t count = cpu_get_count();
  lapic_send_ipi_all_but_self(probe_vector_state);

  uint64_t mhz = tsc_get_mhz();
  uint64_t deadline = mhz ? rdtsc() + LOCKDIAG_TICKS_PER_MS(mhz) * timeout_ms : 0;
  uint64_t spins = deadline ? 0 : 200000000ULL;

  for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
    if (i == self->cpu_id)
      continue;
    struct cpu_info *c = cpu_get_info(i);
    if (!c || (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP))
      continue;
    while (!probe_ack_reached(
        (uint8_t)__atomic_load_n(&probe_ack[i], __ATOMIC_ACQUIRE), seq)) {
      if (deadline) {
        if (rdtsc() > deadline)
          break;
      } else if (--spins == 0) {
        break;
      }
      hal_cpu_relax();
    }
    if (probe_ack_reached(
            (uint8_t)__atomic_load_n(&probe_ack[i], __ATOMIC_ACQUIRE), seq))
      mask |= 1ULL << i;
  }
  return mask;
}

/* --- report ------------------------------------------------------------- */

/* Best-effort unwind of a CPU's kernel stack: the most recent words that look
 * like kernel return addresses.  Without frame pointers this is the only
 * unwind that does not need the target to cooperate, and the target is exactly
 * the CPU that cannot cooperate. */
static void dump_stack(uint64_t low, uint64_t high, int max_entries) {
  int shown = 0;
  ld_str("\n[LOCKDIAG]   stack:");
  uint64_t top = high & ~7ULL;
  for (uint64_t a = top; a >= low && shown < max_entries; a -= 8) {
    uint64_t v = *(const volatile uint64_t *)a;
    /* Below the image base is data or stale stack litter, never a frame. */
    if (v >= (uint64_t)__kernel_text_start &&
        v < (uint64_t)__kernel_text_end) {
      ld_str(" ");
      ld_hex(v);
      shown++;
    }
    if (a <= low + 8)
      break;
  }
  if (!shown)
    ld_str(" (none)");
}

/* Suspended threads have a saved RSP: start there, not at the oldest
 * syscall frames at the top of the stack. Only print actual text addresses. */
static void dump_parked_stack(const struct thread *t) {
  if (!t->stack_base || t->stack_size < 8 || t->rsp < t->stack_base ||
      t->rsp >= t->stack_base + t->stack_size)
    return;
  ld_str("\n[LOCKDIAG]   parked-stack (newest first):");
  unsigned shown = 0;
  uint64_t high = t->stack_base + t->stack_size - 8;
  for (uint64_t a = (t->rsp + 7) & ~7ULL; a <= high && shown < 16; a += 8) {
    uint64_t v = *(const volatile uint64_t *)a;
    if (v >= (uint64_t)__kernel_text_start && v < (uint64_t)__kernel_text_end) {
      ld_str(" "); ld_hex(v); shown++;
    }
  }
  if (!shown) ld_str(" (none)");
}

#define LOCKDIAG_USER_FRAMES 16
#define LOCKDIAG_USER_SCAN_WORDS 256
struct lockdiag_user_frame {
  uint64_t ip, offset, slot;
  char module[48];
};
struct lockdiag_user_wait {
  uint64_t rsp;
  struct lockdiag_user_frame frames[LOCKDIAG_USER_FRAMES];
  unsigned count, word_count;
  uint32_t words[4];
};

/* Never switch CR3, fault in a page or acquire vmm_lock in the reporter.
 * mm->lock is held by the collector to stabilize VMAs; a raw, RAM-checked
 * walk reads the parked process rather than the reporting CPU's mappings.
 * A concurrent COW can still make a sample stale: this is diagnostic only. */
static bool lockdiag_user_read(const struct thread *t, uint64_t addr,
                               void *out, unsigned bytes) {
  if (!addr || addr > 0x00007FFFFFFFFFFFULL ||
      (bytes != 4 && bytes != 8) || (addr & (bytes - 1)))
    return false;
  struct vma *v = vma_find(&t->mm->vmas, addr);
  if (!v || !(v->prot & 1) || v->end - addr < bytes)
    return false;
  uint64_t entries[4] = {0};
  uint64_t phys = vmm_debug_walk(t->cr3, addr, entries);
  if (!phys || !pmm_is_managed(phys & ~0xFFFULL))
    return false;
  for (unsigned i = 0; i < 4; i++) {
    if (!(entries[i] & PAGE_FLAG_USER))
      return false;
    if ((i == 1 || i == 2) && (entries[i] & PAGE_FLAG_PS))
      break;
  }
  const void *src = (const void *)(uintptr_t)(phys + pmm_get_hhdm_offset());
  if (bytes == 4)
    *(uint32_t *)out = *(const volatile uint32_t *)src;
  else
    *(uint64_t *)out = *(const volatile uint64_t *)src;
  return true;
}

static void lockdiag_user_add_frame(const struct thread *t,
                                    struct lockdiag_user_wait *sample,
                                    uint64_t ip, uint64_t slot) {
  if (!ip || ip > 0x00007FFFFFFFFFFFULL ||
      sample->count >= LOCKDIAG_USER_FRAMES)
    return;
  struct vma *v = vma_find(&t->mm->vmas, ip);
  if (!v || !(v->prot & 4))
    return;
  for (unsigned i = 0; i < sample->count; i++)
    if (sample->frames[i].ip == ip)
      return;
  struct lockdiag_user_frame *f = &sample->frames[sample->count++];
  f->ip = ip;
  f->slot = slot;
  f->offset = ip - v->start + v->offset;
  vfs_node_t *node = v->file_node;
  const char *name = vfs_node_is_alive(node) ? node->name : "<anonymous>";
  unsigned i = 0;
  for (; i + 1 < sizeof(f->module) && name[i]; i++) {
    char c = name[i];
    f->module[i] = c >= 0x20 && c < 0x7F ? c : '?';
  }
  f->module[i] = '\0';
}

/* tid_lock pins the thread and its mm. Only try the mm lock: a hung owner
 * must never hang the reporter. Snapshot into fixed storage, then drop it
 * before slow serial output. No application state is modified. */
static bool lockdiag_collect_user_wait(const struct thread *t,
                                       struct lockdiag_user_wait *sample) {
  sample->count = sample->word_count = 0;
  sample->rsp = t->last_syscall_user_rsp;
  if (!t->mm || !t->cr3 || !t->in_syscall ||
      t->state != THREAD_BLOCKED || !spinlock_try_acquire(&t->mm->lock))
    return false;
  lockdiag_user_add_frame(t, sample, t->last_syscall_user_rip, 0);
  uint64_t sp = sample->rsp;
  if (sp && sp <= 0x00007FFFFFFFFFFFULL) {
    sp = (sp + 7) & ~7ULL;
    for (unsigned i = 0; i < LOCKDIAG_USER_SCAN_WORDS &&
                         sample->count < LOCKDIAG_USER_FRAMES; i++) {
      uint64_t ip;
      if (!lockdiag_user_read(t, sp + i * 8ULL, &ip, 8))
        break;
      lockdiag_user_add_frame(t, sample, ip, sp + i * 8ULL);
    }
  }
  if (t->in_futex_wait) {
    uint64_t addr = t->last_syscall_args[0];
    for (unsigned i = 0; i < 4; i++) {
      if (!lockdiag_user_read(t, addr + i * 4ULL, &sample->words[i], 4))
        break;
      sample->word_count++;
    }
  }
  spinlock_release(&t->mm->lock);
  return true;
}

static void dump_user_wait(const struct thread *t) {
  struct lockdiag_user_wait sample;
  if (!lockdiag_collect_user_wait(t, &sample)) {
    ld_str("\n[LOCKDIAG]   user-wait: unavailable (not parked in syscall or mm busy)");
    return;
  }
  ld_str("\n[LOCKDIAG]   user-wait rsp="); ld_hex(sample.rsp);
  if (sample.word_count) {
    ld_str(" futex-words (sampled, starting at uaddr)=");
    for (unsigned i = 0; i < sample.word_count; i++) {
      if (i) ld_str(",");
      ld_hex(sample.words[i]);
    }
  }
  ld_str("\n[LOCKDIAG]   user-code candidates (stack scan, not an unwind):");
  for (unsigned i = 0; i < sample.count; i++) {
    const struct lockdiag_user_frame *f = &sample.frames[i];
    ld_str("\n[LOCKDIAG]     "); ld_hex(f->ip);
    ld_str(" "); ld_str(f->module);
    ld_str(" file+"); ld_hex(f->offset);
    ld_str(" stack-slot="); ld_hex(f->slot);
  }
  if (!sample.count) ld_str(" (none mapped executable)");
}

/* Bounded sanitized path print for the poll snapshot (written by the owner
 * thread in its own syscall context; a concurrent update can only tear one
 * entry, so cap the scan and replace non-printables). */
static void ld_poll_path(const char *p) {
  ld_str(" path='");
  if (p) {
    for (int i = 0; i < 47 && p[i]; i++) {
      char c = p[i];
      char out = (c >= 0x20 && c < 0x7F) ? c : '?';
      serial_write_sync(&out, 1);
    }
  }
  ld_str("'");
}

/* Poll snapshot for a blocked poll/ppoll/epoll waiter: which fds it sleeps
 * on. Join stalls park Main in futex while workers park in poll/recv; the
 * fd + path names which peer or pipe never became ready. */
static void dump_poll_wait(const struct thread *t) {
  uint64_t nfds = t->last_poll_nfds;
  if (!nfds)
    return;
  ld_str(" [poll nfds=");
  ld_dec(nfds > 64 ? 64 : nfds);
  unsigned n = nfds > 4 ? 4 : (unsigned)nfds;
  for (unsigned i = 0; i < n; i++) {
    ld_str(" fd");
    ld_dec(i);
    ld_str("=");
    if (t->last_poll_fds[i] < 0)
      ld_str("-");
    else
      ld_dec((uint64_t)(int64_t)t->last_poll_fds[i]);
    ld_str(" ev=");
    ld_hex((uint64_t)(uint16_t)t->last_poll_events[i]);
    ld_poll_path(t->last_poll_paths[i]);
  }
  if (nfds > n) {
    ld_str(" ... +");
    ld_dec(nfds - n);
    ld_str(" more");
  }
  ld_str("]");
}

/* FD + byte-count for blocked read/write-family syscalls: identifies which
 * cache/log file a join worker is stuck writing (ext4/journal/NVMe path). */
static void dump_fd_wait(const struct thread *t) {
  uint64_t n = t->last_syscall_num;
  if (n != 0 && n != 1 && n != 17 && n != 18)
    return; /* read, write, pread64, pwrite64 */
  uint64_t fd = t->last_syscall_args[0] & 0xFFFFFFFFULL;
  char path[128] = {0};
  uint64_t inode = 0;
  uint64_t fd_flags = 0, node_flags = 0;
  bool sampled = false;
  if (fd < MAX_FDS && t->files && spinlock_try_acquire(&t->files->lock)) {
    vfs_node_t *node = t->files->fds[fd];
    if (vfs_node_is_alive(node)) {
      inode = node->inode;
      fd_flags = t->files->fd_flags[fd];
      node_flags = node->flags;
      const char *name = t->files->fd_paths[fd] &&
                          t->files->fd_paths[fd]->value[0] ?
                          t->files->fd_paths[fd]->value : node->name;
      unsigned i = 0;
      for (; i + 1 < sizeof(path) && name[i]; i++) {
        char c = name[i];
        path[i] = c >= 0x20 && c < 0x7F ? c : '?';
      }
      path[i] = '\0';
      sampled = true;
    }
    spinlock_release(&t->files->lock);
  }
  ld_str(" [fd=");
  ld_dec(fd);
  ld_str(" count=");
  ld_dec(t->last_syscall_args[2]);
  if (t->in_syscall) {
    uint64_t now = lapic_timer_get_ms();
    ld_str(" syscall_ms=");
    ld_dec(now > t->last_io_started_ms ? now - t->last_io_started_ms : 0);
  }
  if (n == 17 || n == 18) {
    ld_str(" offset="); ld_dec(t->last_syscall_args[3]);
  }
  if (sampled) {
    ld_str(" inode="); ld_dec(inode);
    ld_str(" path='"); ld_str(path); ld_str("'");
    ld_str(" fd_flags="); ld_hex(fd_flags);
    ld_str(" node_flags="); ld_hex(node_flags);
    ld_str(" (sampled)");
  }
  ld_str("]");
}

static void dump_thread(const struct thread *t) {
  if (!t) {
    ld_str(" (no thread)");
    return;
  }
  ld_str("tid=");
  ld_dec(t->tid);
  ld_str(" tgid=");
  ld_dec(t->tgid);
  ld_str(" comm=");
  ld_comm(t->comm);
  ld_str(" state=");
  ld_str(state_name(t->state));
  if (t->last_kernel_func || t->last_subsystem) {
    ld_str("\n[LOCKDIAG]   ktrack=");
    ld_str(t->last_subsystem ? t->last_subsystem : "?");
    ld_str(" ");
    ld_str(t->last_kernel_file ? t->last_kernel_file : "?");
    ld_str(":");
    ld_dec(t->last_kernel_line);
    ld_str(" ");
    ld_str(t->last_kernel_func ? t->last_kernel_func : "?");
  }
  if (t->last_syscall_num) {
    ld_str(" last-syscall=");
    ld_dec(t->last_syscall_num);
    ld_str(" ret=");
    ld_dec((uint64_t)t->last_syscall_ret);
  }
}

static void dump_cpu(uint32_t id, uint64_t ack_mask, uint64_t now_tsc,
                     uint64_t mhz) {
  struct cpu_info *c = cpu_get_info(id);
  struct cpu_info *self = cpu_get_current();
  if (!c)
    return;

  ld_str("\n[CPU ");
  ld_pad(id, 2);
  ld_str("] status=");
  ld_dec(c->status);
  ld_str(" ticks=");
  ld_dec(__atomic_load_n(&hb_ticks[id], __ATOMIC_RELAXED));

  uint64_t last = __atomic_load_n(&hb_tsc[id], __ATOMIC_RELAXED);
  if (last && mhz && now_tsc > last) {
    ld_str(" last-tick=");
    ld_dec(cycles_to_ms(now_tsc - last, mhz));
    ld_str("ms-ago");
  }

  bool acked = (ack_mask & (1ULL << id)) != 0;
  bool reporting = self && id == self->cpu_id;
  if (reporting)
    ld_str("\n[LOCKDIAG]   reporting core (alive by construction; its own RIP"
           " is not sampled)");
  if (acked && !reporting) {
    ld_str("\n[LOCKDIAG]   PROBE-ACK rip=");
    ld_hex(probe_rip[id]);
    ld_str(" rsp=");
    ld_hex(probe_rsp[id]);
    ld_str(" IF=");
    ld_str((probe_rflags[id] & 0x200ULL) ? "1" : "0");
  } else if (!acked) {
    ld_str("\n[LOCKDIAG]   PROBE-UNANSWERED: not executing kernel code with "
           "interrupts enabled (spinning in a critical section, or dead)");
  }

  /* What this core was waiting for when it stopped answering.  This is the one
   * datum that turns "spinning in a critical section" into a name: the call
   * site resolves to the function that took the object's lock, e.g.
   * virtq_submit_internal -> vq->lock, wait_queue_wake_all -> the queue's lock. */
  {
    const spin_wait_t *w = &g_spin_wait[id];
    if (__atomic_load_n(&w->active, __ATOMIC_ACQUIRE) && w->lock) {
      ld_str("\n[LOCKDIAG]   WAITING-ON lock=");
      ld_hex(w->lock);
      ld_str(" at=");
      ld_hex(w->ip);
      uint64_t since = w->since_tsc;
      if (since && mhz && now_tsc > since) {
        ld_str(" for=");
        ld_dec(cycles_to_ms(now_tsc - since, mhz));
        ld_str("ms");
      }
    }
  }

  ld_str("\n[LOCKDIAG]   cur ");
  dump_thread(c->current_thread);
  dump_holds(id);

  for (int s = 0; s < LOCKDIAG_SPOT_COUNT; s++) {
    if (!__atomic_load_n(&g_spots[s].depth, __ATOMIC_RELAXED))
      continue;
    if (__atomic_load_n(&g_spots[s].owner_cpu, __ATOMIC_RELAXED) != id)
      continue;
    ld_str("\n[LOCKDIAG]   HOLDS ");
    ld_str(lockdiag_spot_name(s));
    ld_str(" held=");
    ld_dec(cycles_to_ms(now_tsc -
                        __atomic_load_n(&g_spots[s].taken_tsc, __ATOMIC_RELAXED),
                        mhz));
    ld_str("ms taken-from=");
    ld_hex(__atomic_load_n(&g_spots[s].taken_ip, __ATOMIC_RELAXED));
  }

  if (c->stack_top)
    dump_stack(c->stack_top - CPU_STACK_SIZE, c->stack_top, 14);
}

static void dump_spots(uint64_t now_tsc, uint64_t mhz) {
  ld_str("\n[LOCKDIAG] lock spotlight:");
  for (int s = 0; s < LOCKDIAG_SPOT_COUNT; s++) {
    const lockdiag_spot_t *sp = &g_spots[s];
    uint64_t depth = __atomic_load_n(&sp->depth, __ATOMIC_RELAXED);
    uint64_t taken = __atomic_load_n(&sp->taken_tsc, __ATOMIC_RELAXED);
    ld_str("\n[LOCKDIAG]   ");
    ld_str(lockdiag_spot_name(s));
    ld_str(depth ? " HELD by cpu=" : " free");
    if (depth) {
      ld_dec(__atomic_load_n(&sp->owner_cpu, __ATOMIC_RELAXED));
      ld_str(" tid=");
      ld_dec(__atomic_load_n(&sp->owner_tid, __ATOMIC_RELAXED));
      ld_str(" for=");
      ld_dec(taken ? cycles_to_ms(now_tsc - taken, mhz) : 0);
      ld_str("ms taken-from=");
      ld_hex(__atomic_load_n(&sp->taken_ip, __ATOMIC_RELAXED));
    }
    ld_str(" | acquires=");
    ld_dec(__atomic_load_n(&sp->acquires, __ATOMIC_RELAXED));
    ld_str(" max-wait=");
    ld_dec(cycles_to_ms(__atomic_load_n(&sp->max_wait_cycles, __ATOMIC_RELAXED),
                        mhz));
    ld_str("ms max-hold=");
    ld_dec(cycles_to_ms(
        __atomic_load_n(&sp->max_hold_cycles, __ATOMIC_RELAXED), mhz));
    ld_str("ms slow-waits=");
    ld_dec(__atomic_load_n(&sp->slow_waits, __ATOMIC_RELAXED));
  }
}

void lockdiag_dump_all(const char *reason) {
  uint64_t mhz = tsc_get_mhz();
  uint64_t now = rdtsc();
  uint32_t count = cpu_get_count();
  struct cpu_info *self = cpu_get_current();

  ld_str("\n\n[LOCKDIAG] ================= HANG REPORT =================");
  ld_str("\n[LOCKDIAG] reason: ");
  ld_str(reason ? reason : "unspecified");
  ld_str("\n[LOCKDIAG] reporter cpu=");
  ld_dec(self ? self->cpu_id : 0xFFFFFFFFULL);
  ld_str(" cpus=");
  ld_dec(count);
  ld_str(" tsc_mhz=");
  ld_dec(mhz);
  ld_str(" since-lockdiag-init=");
  ld_dec(g_init_tsc ? cycles_to_ms(now - g_init_tsc, mhz) : 0);
  ld_str("ms");

  uint64_t ack = lockdiag_probe_cpus(LOCKDIAG_PROBE_TIMEOUT_MS);

  ld_str("\n[LOCKDIAG] probe acks: cpu");
  ld_dec(count > 0 ? count - 1 : 0);
  ld_str("..cpu0 = ");
  for (int i = 31; i >= 0; i--) {
    if ((uint32_t)i >= count)
      continue;
    char ch = (ack & (1ULL << i)) ? 'Y' : 'N';
    serial_write_sync(&ch, 1);
  }

  for (uint32_t i = 0; i < count && i < MAX_CPUS; i++)
    dump_cpu(i, ack, now, mhz);

  dump_spots(now, mhz);
  dump_wq_holds(now, mhz);

  const lockdiag_stats_t *s = &g_stats;
  ld_str("\n[LOCKDIAG] counters: futex waits=");
  ld_dec(s->futex_waits);
  ld_str(" wakes=");
  ld_dec(s->futex_wakes);
  ld_str(" wake-no-match=");
  ld_dec(s->futex_wake_nomatch);
  ld_str(" timeouts=");
  ld_dec(s->futex_timeouts);
  ld_str(" zero-timeout=");
  ld_dec(s->futex_zero_timeout);
  ld_str("\n[LOCKDIAG]            wait_queue adds=");
  ld_dec(s->wq_adds);
  ld_str(" wakes=");
  ld_dec(s->wq_wakes);
  ld_str(" | shootdowns=");
  ld_dec(s->shootdowns);
  ld_str(" slow-acks=");
  ld_dec(s->shootdown_slow_acks);
  ld_str(" max-ack-wait=");
  ld_dec(s->shootdown_max_ack_ns);
  ld_str("ns shot-down-under-vmm_lock=");
  ld_dec(s->shootdown_under_vmm_lock);
  ld_str("\n[LOCKDIAG]            hung-cpu-reports=");
  ld_dec(s->hung_cpu_reports);
  ld_str(" stuck-waiter-warns=");
  ld_dec(s->stuck_task_reports);

  /* Which call sites started these shootdowns, and how many of them did so
   * while holding vmm_lock: that is what turns "2079 shootdowns" into a name
   * that can be fixed. */
  ld_str("\n[LOCKDIAG] shootdown callers:");
  int any_callers = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t ip = 0, total = 0, under = 0;
    if (!tlb_shootdown_caller_info(i, &ip, &total, &under))
      break;
    any_callers = 1;
    ld_str("\n[LOCKDIAG]   ");
    ld_hex(ip);
    ld_str(" flushes=");
    ld_dec(total);
    ld_str(" under-vmm_lock=");
    ld_dec(under);
  }
  if (!any_callers)
    ld_str(" none");
  ld_str("\n[LOCKDIAG] owner tracking: ");
#if LOCKDIAG_OWNER
  ld_str("ON (held-lock chains printed per CPU)");
#else
  ld_str("OFF (rebuild with make LOCKDIAG_OWNER=1 to name every lock each CPU "
         "holds and where it took it from)");
#endif

  /* Threads parked on a futex with no timeout armed: the fingerprint of a lost
   * wakeup. */
  /* Every blocked thread, not just futex waiters.  A frozen desktop with no
   * hung CPU is a userspace deadlock, and the picture that proves it is who is
   * parked where: Xorg's reader asleep in a unix socket recv while the client
   * that owns the reply is asleep on a mutex, for example.  Threads are listed
   * with their last syscall and ktrack site, which says what each is waiting
   * for without needing a debugger. */
  int shown = 0, blocked_total = 0;
  int dead_shown = 0, dead_total = 0;
  if (spinlock_try_acquire(&tid_lock)) {
    ld_str("\n[LOCKDIAG] blocked / waiting threads (event loops, I/O, sleep):");
    uint32_t focus_tgid = 0;
    uint32_t journal_owner = 0;
    /* A blocked regular write can hold the journal and stall an entire
     * application. Include that application's futex workers before unrelated
     * desktop event loops, and expose the active root journal owner. */
    ext3_journal_state_t *j = lockdiag_root_journal();
    if (j) {
      journal_owner = __atomic_load_n(&j->owner_tid, __ATOMIC_RELAXED);
      ld_str("\n[LOCKDIAG] root journal: active="); ld_dec(j->active);
      ld_str(" owner="); ld_dec(journal_owner);
      ld_str(" comm="); ld_comm(j->owner_comm);
      ld_str(" depth="); ld_dec(j->depth);
      ld_str(" blocks="); ld_dec(j->blocks_in_trans);
      ld_str(" phase="); ld_str(j->phase ? j->phase : "unknown");
      ld_str(" owner_ip="); ld_hex(j->owner_ip);
      ld_str(" hold_ms=");
      ld_dec(j->active ? lapic_timer_get_ms() - j->started_ms : 0);
      ld_str(" granted=");
      ld_dec(__atomic_load_n(&j->granted_tid, __ATOMIC_ACQUIRE));
      /* A completion may have made the owner READY without it being
       * dispatched. The blocked-only listing below would omit precisely
       * that case, leaving a scheduler delay looking like device latency. */
      for (const struct thread *t = global_thread_list; t; t = t->global_next) {
        if (!journal_owner || t->tid != journal_owner)
          continue;
        ld_str("\n[LOCKDIAG] journal owner task: ");
        dump_thread(t);
        ld_str(" cpu="); ld_dec(t->cpu_index);
        ld_str(" on_cpu=");
        ld_dec(__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE));
        ld_str(" on_rq="); ld_dec(t->se.on_rq);
        ld_str(" ready_since_ms="); ld_dec(t->ready_since_ms);
        ld_str(" wake_at_ms="); ld_dec(t->wakeup_ticks);
        ld_str(" deadline_queued="); ld_dec(t->deadline_queued);
        uint64_t ready_at = __atomic_load_n(&t->ready_since_ms, __ATOMIC_RELAXED);
        uint64_t sampled_ms = lapic_timer_get_ms();
        ld_str(" ready_wait_ms=");
        ld_dec(ready_at && sampled_ms >= ready_at ? sampled_ms - ready_at : 0);
        ld_str(" vruntime="); ld_dec(t->se.vruntime);
        ld_str(" vdeadline="); ld_dec(t->se.deadline);
        ld_str(" slice_ns="); ld_dec(t->se.slice_ns);
        struct cpu_info *owner_cpu = cpu_get_info(t->cpu_index);
        /* Diagnostic snapshots only: taking queue_lock here can deadlock a
         * report requested by a scheduler/lock stall. These fields need not
         * describe the same instant; the next report can show progress. */
        if (owner_cpu) {
          ld_str("\n[LOCKDIAG] journal owner cpu: queued=");
          ld_dec(owner_cpu->eevfd.nr_running);
          ld_str(" vtime="); ld_dec(owner_cpu->eevfd.vtime);
          ld_str(" timer_at_ms="); ld_dec(owner_cpu->timer_deadline_ms);
          ld_str(" quantum_at_ms="); ld_dec(owner_cpu->quantum_deadline_ms);
          const struct thread *running =
              __atomic_load_n(&owner_cpu->current_thread, __ATOMIC_ACQUIRE);
          if (running) {
            ld_str(" current_tid="); ld_dec(running->tid);
            ld_str(" need_resched="); ld_dec(running->need_resched);
            ld_str(" vruntime="); ld_dec(running->se.vruntime);
            ld_str(" vdeadline="); ld_dec(running->se.deadline);
          }
        }
        if (!__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) &&
            (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING ||
             t->state == THREAD_READY))
          dump_parked_stack(t);
        break;
      }
    }
    for (const struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t->state != THREAD_BLOCKED) continue;
      if (t->tid == journal_owner) { focus_tgid = t->tgid; break; }
      if (!focus_tgid && (t->last_syscall_num == 1 || t->last_syscall_num == 18))
        focus_tgid = t->tgid;
    }
    /* Game-join stalls rarely sit in the journal owner: Main parks on a
     * futex while a crowd of workers parks in poll/write behind it, and the
     * journal may be held by an unrelated daemon (Firefox SQLite in one
     * capture). Track the tgid with the most BLOCKED threads as a second
     * focus so the whole client group prints first with full detail. */
    uint32_t busy_tgid = 0;
    uint64_t busy_count = 0;
    {
      uint32_t tgids[16];
      uint64_t counts[16];
      unsigned n = 0;
      for (const struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->state != THREAD_BLOCKED && t->state != THREAD_SLEEPING)
          continue;
        unsigned k = n;
        for (unsigned i = 0; i < n; i++) {
          if (tgids[i] == t->tgid) { k = i; break; }
        }
        if (k == n) {
          if (n >= 16) continue;
          tgids[n] = t->tgid;
          counts[n] = 1;
          n++;
        } else {
          counts[k]++;
        }
      }
      for (unsigned i = 0; i < n; i++) {
        if (counts[i] > busy_count) {
          busy_count = counts[i];
          busy_tgid = tgids[i];
        }
      }
    }
    if (busy_tgid && busy_tgid != focus_tgid) {
      ld_str("\n[LOCKDIAG] busiest blocked group: tgid=");
      ld_dec(busy_tgid);
      ld_str(" threads=");
      ld_dec(busy_count);
      ld_str(" (game-join focus: Main + workers print first)");
    }
    for (int pass = 0; pass < 3; pass++) {
    for (const struct thread *t = global_thread_list; t; t = t->global_next) {
      /* A thread that died while holding a lock leaves it held forever, and it
       * will never appear in the blocked list: the lock spotlight above is what
       * names the holder, this is what says whether the holder still exists. */
      if (t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE) {
        if (pass != 0) continue;
        dead_total++;
        if (dead_shown < 8) {
          ld_str("\n[LOCKDIAG]   [zombie/exited] tid=");
          ld_dec(t->tid);
          ld_str(" tgid=");
          ld_dec(t->tgid);
          ld_str(" comm=");
          ld_comm(t->comm);
          ld_str(" ");
          ld_str(state_name(t->state));
          ld_str(" parent=");
          if (t->parent) {
            ld_dec(t->parent->tid);
            ld_str("/");
            ld_dec(t->parent->tgid);
            ld_str("(");
            ld_comm(t->parent->comm);
            ld_str(")");
          } else {
            ld_str("none");
          }
          ld_str(" exit-signal=");
          ld_dec(t->clone_flags & 0xff);
          ld_str(" reap-queued=");
          ld_dec(__atomic_load_n(&t->reap_queued, __ATOMIC_RELAXED));
          if (t->parent && SIGCHLD > 0 && SIGCHLD <= 64) {
            const struct k_sigaction *chld =
                &t->parent->signal_handlers[SIGCHLD - 1];
            ld_str(" parent-SIGCHLD-handler=");
            ld_hex((uint64_t)(uintptr_t)chld->sa_handler);
            ld_str(" flags=");
            ld_hex(chld->sa_flags);
          }
          dead_shown++;
        }
        continue;
      }
      if (t->state != THREAD_BLOCKED && t->state != THREAD_SLEEPING)
        continue;
      bool focused = t->kpi_data || (focus_tgid && t->tgid == focus_tgid) ||
                     (busy_tgid && t->tgid == busy_tgid) ||
                     strncmp(t->comm, "Xorg", 4) == 0 ||
                     strncmp(t->comm, "glxgears", 8) == 0 ||
                     strncmp(t->comm, "xfce4-ab", 8) == 0 ||
                     strncmp(t->comm, "wine", 4) == 0;
      if ((pass == 0 && !focused) ||
          (pass == 1 && (focused || t->in_futex_wait)) ||
          (pass == 2 && (focused || !t->in_futex_wait)))
        continue;
      blocked_total++;
      if (shown >= 128)
        continue;
      shown++;
      ld_str("\n[LOCKDIAG]   tid=");
      ld_dec(t->tid);
      ld_str(" tgid=");
      ld_dec(t->tgid);
      ld_str(" comm=");
      ld_comm(t->comm);
      ld_str(" ");
      ld_str(state_name(t->state));
#ifdef LINUXKPI_LINUX_IMPORTED
      if (t->kpi_wait_mutex) {
        extern void *linuxkpi_mutex_owner_thread(void *mutex);
        const struct thread *owner = linuxkpi_mutex_owner_thread(t->kpi_wait_mutex);
        ld_str(" [kpi mutex="); ld_hex((uint64_t)t->kpi_wait_mutex);
        ld_str(" owner="); ld_dec(owner ? owner->tid : 0);
        if (owner) {
          ld_str("/"); ld_comm(owner->comm);
          ld_str(" state="); ld_str(state_name(owner->state));
        }
        ld_str("]");
      }
#endif
      if (t->kpi_wait_prepared || t->kpi_timeout_active ||
          strncmp(t->comm, "Xorg", 4) == 0) {
        ld_str(" [kpi prepared="); ld_dec(t->kpi_wait_prepared);
        ld_str(" timeout="); ld_dec(t->kpi_timeout_active);
        ld_str(" wake="); ld_dec(t->kpi_wake_pending);
        ld_str(" block-wake="); ld_dec(t->kpi_block_pending);
        ld_str("]");
      }
      if (t->last_syscall_num) {
        const char *sn = syscall_get_name(t->last_syscall_num);
        ld_str(" in ");
        ld_str(sn ? sn : "syscall");
        ld_str("#");
        ld_dec(t->last_syscall_num);
      }
      if (t->last_kernel_func) {
        ld_str(" at ");
        ld_str(t->last_kernel_file ? t->last_kernel_file : "?");
        ld_str(":");
        ld_dec(t->last_kernel_line);
        ld_str(" ");
        ld_str(t->last_kernel_func);
      }
      uint64_t parked_ms = 0;
      if (t->blocked_since_ms) {
        uint64_t now_ms = lapic_timer_get_ms();
        parked_ms = now_ms > t->blocked_since_ms ? now_ms - t->blocked_since_ms : 0;
      }
      if (t->in_futex_wait) {
        uint64_t wk = 0, nm = 0;
        futex_bucket_stats(t->futex_bucket, &wk, &nm);
        ld_str(" [uaddr="); ld_hex(t->last_syscall_args[0]);
        ld_str(" op="); ld_hex(t->last_syscall_args[1]);
        ld_str(" expected="); ld_hex(t->last_syscall_args[2]);
        ld_str("]");
        ld_str(" [futex bucket=");
        ld_dec(t->futex_bucket);
        ld_str(" wakes=");
        ld_dec(wk);
        ld_str(" no-match=");
        ld_dec(nm);
        if (t->blocked_since_ms) {
          ld_str(" parked=");
          ld_dec(parked_ms);
          ld_str("ms");
        }
        ld_str("]");
      } else {
        /* Non-futex waits never set blocked_since_ms; their deadline is
         * still diagnostic (infinite = -1/0, timed = uptime ms). */
        dump_poll_wait(t);
        dump_fd_wait(t);
      }
      ld_str(" cpu="); ld_dec(t->cpu_index);
      ld_str(" wake_at_ms="); ld_dec(t->wakeup_ticks);
      if (t->wakeup_ticks && t->wakeup_ticks != (uint64_t)-1) {
        uint64_t now_ms = lapic_timer_get_ms();
        if (now_ms > t->wakeup_ticks) {
          ld_str(" OVERDUE+");
          ld_dec(now_ms - t->wakeup_ticks);
          ld_str("ms");
        }
      }
      /* Focused groups always get stacks. Unfocused futex waiters are the
       * noisiest entries, so only long-parked ones (>10 s, e.g. a wedged
       * Main) pay for the scan; every other waiter type always does. */
      if (!t->in_futex_wait || focused || parked_ms > 10000)
        dump_parked_stack(t);
      if (focused && t->mm &&
          (!t->in_futex_wait || parked_ms > 10000))
        dump_user_wait(t);
    }
    }
    spinlock_release(&tid_lock);
    ld_str("\n[LOCKDIAG]   ");
    ld_dec((uint64_t)blocked_total);
    ld_str(" blocked");
    if (blocked_total > shown) {
      ld_str(", first ");
      ld_dec((uint64_t)shown);
      ld_str(" listed");
    }
    if (dead_total) {
      ld_str("; ");
      ld_dec((uint64_t)dead_total);
      ld_str(" zombie/exited (see above)");
    }
  } else {
    /* Saying so is the point: tid_lock is held by somebody, and that somebody
     * is a strong candidate for whoever stopped the machine. */
    ld_str("\n[LOCKDIAG] tid_lock BUSY - cannot enumerate threads; rebuild with "
           "make LOCKDIAG_OWNER=1 to name the holder");
  }

  /* A log that stops while the kernel still runs looks identical to a hang:
   * only the BSP's timer tick drains this ring, so a growing backlog means the
   * tail of the log is sitting in a buffer, not lost. */
  ld_str("\n[LOCKDIAG] serial ring: ");
  ld_dec(serial_pending_bytes());
  ld_str(" bytes queued but not yet on the wire\n");

  /* Missing syscalls and unhandled ioctls diagnostic report */
  extern void sysdiag_dump_report(void);
  sysdiag_dump_report();

  ld_str("\n[LOCKDIAG] ===================================================");
  ld_str("\n[LOCKDIAG] addresses are raw: resolve with "
         "addr2line -f -e kernel/bin-x86_64/kernel <addr>  (or nm -n that file "
         "and search); the same ELF the ISO was built from\n");
}

/* --- detector ----------------------------------------------------------- */

static void reason_stuck_cpu(char *out, size_t outsz, uint32_t victim) {
  const char *pre = "cpu ";
  size_t d = 0;
  while (pre[d] && d + 1 < outsz) {
    out[d] = pre[d];
    d++;
  }
  char digits[8];
  int nd = 0;
  uint32_t v = victim;
  do {
    digits[nd++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v && nd < (int)sizeof(digits));
  while (nd && d + 1 < outsz)
    out[d++] = digits[--nd];
  const char *post = " stopped taking timer ticks";
  for (int i = 0; post[i] && d + 1 < outsz; i++)
    out[d++] = post[i];
  out[d] = '\0';
}

/* --- on-demand report: type "dump" on the serial console ------------------
 *
 * The keyboard trigger depends on the whole USB/xHCI path being alive, and it
 * is polled from the BSP's timer tick - measured on a frozen desktop, pressing
 * right-Ctrl produced nothing because that poll had stopped.  Serial input is
 * independent of USB, X, and the graphics stack, and any CPU that is still
 * taking ticks can read it, so this works when the keyboard path is dead.
 *
 * A four-character sliding window means the word has to be typed, not
 * stumbled into. */
static void check_serial_trigger(void) {
  static char window[4];
  static unsigned n;

  /* Bounded per tick.  On boards whose SuperIO does not decode COM1, the
   * line-status register floats high and reads 0xFF: every poll looks like a
   * received byte and never clears.  An unbounded drain here hangs the timer
   * ISR forever on the very first tick.  A real UART delivers at most a few
   * characters per millisecond, so a small budget loses nothing. */
  for (int budget = 0; budget < 64; budget++) {
    int c = serial_try_get_char();
    if (c < 0)
      return;
    if (c == '\r' || c == '\n')
      continue;
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
    if (n < 4)
      window[n++] = (char)c;
    else {
      window[0] = window[1];
      window[1] = window[2];
      window[2] = window[3];
      window[3] = (char)c;
    }
    if (n == 4 && window[0] == 'd' && window[1] == 'u' && window[2] == 'm' &&
        window[3] == 'p') {
      n = 0;
      lockdiag_dump_all("manual trigger: the word 'dump' on the serial console");
    } else if (n == 4 && window[0] == 'd' && window[1] == 'i' && window[2] == 'a' &&
               window[3] == 'g') {
      n = 0;
      extern void sysdiag_dump_report(void);
      sysdiag_dump_report();
    }
  }
}

void lockdiag_tick(void) {
  /* Before any gating: the serial trigger must work even during the detector's
   * startup grace period. */
  check_serial_trigger();

  struct cpu_info *self = cpu_get_current();
  if (!self || self->cpu_id >= MAX_CPUS)
    return;

  uint64_t now = rdtsc();
  __atomic_store_n(&hb_tsc[self->cpu_id], now, __ATOMIC_RELAXED);
  __atomic_add_fetch(&hb_ticks[self->cpu_id], 1, __ATOMIC_RELAXED);

  uint64_t mhz = tsc_get_mhz();
  if (!mhz || !probe_vector_state)
    return; /* early boot: no calibrated clock to compare against yet */

  /* Give AP bring-up time to start ticking before calling a silent core dead.
   * 10s is minutes below where a real stall matters here. */
  if (now - g_init_tsc < LOCKDIAG_TICKS_PER_MS(mhz) * 10000ULL)
    return;

  if (inside_reporter[self->cpu_id])
    return;

  uint64_t stale = LOCKDIAG_TICKS_PER_MS(mhz) * LOCKDIAG_HUNG_CPU_MS;
  uint32_t count = cpu_get_count();
  uint32_t victim = 0xFFFFFFFFU;

  for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
    if (i == self->cpu_id)
      continue;
    struct cpu_info *c = cpu_get_info(i);
    if (!c || (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP))
      continue;
    uint64_t last = __atomic_load_n(&hb_tsc[i], __ATOMIC_RELAXED);
    if (last == 0)
      continue; /* never ticked: bring-up problem, not an attributable hang */
    if (now > last && now - last > stale) {
      victim = i;
      break;
    }
  }

#if LOCKDIAG_STUCK_TASK_MS
  /* Periodic waiter enumeration: compiled out by default, see
   * LOCKDIAG_STUCK_TASK_MS in lockdiag.h (idle worker pools parked
   * forever are normal, so this only helps when you already know the
   * desktop is wedged). */
  /* Hung futex waiters are looked for whether or not a core is stuck: a lost
   * wakeup freezes a process rather than a CPU, and that is the other half of
   * the "the whole machine just stopped" reports. */
  static uint64_t next_scan_ms;
  uint64_t now_ms = lapic_timer_get_ms();
  if (victim == 0xFFFFFFFFU && now_ms && now_ms >= next_scan_ms) {
    next_scan_ms = now_ms + 1000ULL;
    /* Candidates are collected under tid_lock and printed after releasing it,
     * so serial_lock is never taken underneath tid_lock — the inversion the
     * double-free warning in mm/heap.c still has. */
    struct {
      uint32_t tid, tgid, bucket;
      uint64_t ms, wakes, nomatch;
      char comm[17];
    } cand[4];
    int cand_n = 0;

    if (spinlock_try_acquire(&tid_lock)) {
      for (const struct thread *t = global_thread_list; t; t = t->global_next) {
        if (!t->in_futex_wait || !t->blocked_since_ms)
          continue;
        /* Once per park, not once per second.  Without this the same four idle
         * worker threads printed a fresh "stuck waiter" line every tick and the
         * warning became the noise it is meant to distinguish. */
        if (t->blocked_reported)
          continue;
        if (now_ms < t->blocked_since_ms)
          continue;
        if (now_ms - t->blocked_since_ms < LOCKDIAG_STUCK_TASK_MS)
          continue;
        if (cand_n < (int)(sizeof(cand) / sizeof(cand[0]))) {
          cand[cand_n].tid = t->tid;
          cand[cand_n].tgid = t->tgid;
          cand[cand_n].ms = now_ms - t->blocked_since_ms;
          cand[cand_n].bucket = t->futex_bucket;
          cand[cand_n].wakes = 0;
          cand[cand_n].nomatch = 0;
          futex_bucket_stats(t->futex_bucket, &cand[cand_n].wakes,
                             &cand[cand_n].nomatch);
          for (int c = 0; c < 16; c++) {
            char ch = t->comm[c];
            cand[cand_n].comm[c] = (ch >= 0x20 && ch < 0x7F) ? ch : '\0';
          }
          cand[cand_n].comm[16] = '\0';
          /* Cleared again by futex_wait() on the next park, so a thread that
           * parks and wakes repeatedly is still reported once per park. */
          ((struct thread *)t)->blocked_reported = 1;
          cand_n++;
        }
      }
      spinlock_release(&tid_lock);
    }

    for (int i = 0; i < cand_n; i++) {
      __atomic_add_fetch(&g_stats.stuck_task_reports, 1, __ATOMIC_RELAXED);
      ld_str("\n[LOCKDIAG] parked futex waiter: tid=");
      ld_dec(cand[i].tid);
      ld_str(" tgid=");
      ld_dec(cand[i].tgid);
      ld_str(" comm='");
      ld_str(cand[i].comm);
      ld_str("' blocked=");
      ld_dec(cand[i].ms);
      ld_str("ms bucket=");
      ld_dec(cand[i].bucket);
      ld_str(" wake-attempts=");
      ld_dec(cand[i].wakes);
      ld_str(" consumed-wakes=");
      ld_dec(cand[i].nomatch);
      /* Be careful reading the two counters: they are per *bucket*, and a
       * bucket is shared by every futex whose hash lands in it (256 buckets),
       * so a nonzero wake-attempts says nothing about this particular waiter.
       * The one bucket-level event that is genuinely damning is a wake that
       * matched a waiter and found it already unblocked: the handoff was
       * dropped. Whether a long park is otherwise a stall or a worker pool
       * asleep on an empty queue is not knowable from here - only the owning
       * thread's futex word would say, and that lives in user memory. */
      if (cand[i].nomatch)
        ld_str("  => SUSPECT: a FUTEX_WAKE matched a waiter here and found it "
               "already unblocked (dropped handoff)\n");
      else if (cand[i].wakes == 0)
        ld_str("  => no FUTEX_WAKE has hashed to this bucket since boot\n");
      else
        ld_str("  => no dropped handoff recorded (wake-attempts are per bucket,"
               " shared with unrelated futexes)\n");
    }
  }

#endif /* LOCKDIAG_STUCK_TASK_MS */

  if (victim == 0xFFFFFFFFU)
    return;

  /* A core that stopped ticking is not necessarily hung: the timer is armed
   * one-shot per deadline and the idle loops only add a 1 s fallback tick, so
   * a core can sit between ticks for a moment.  The probe is the arbiter:
   * only a core that cannot answer it is actually stuck. */
  uint64_t ack = lockdiag_probe_cpus(LOCKDIAG_PROBE_TIMEOUT_MS);
  if (ack & (1ULL << victim)) {
    /* It answered: alive, just idle for a while.  Refresh its stamp so the next
     * check starts from now instead of probing every tick. */
    __atomic_store_n(&hb_tsc[victim], rdtsc(), __ATOMIC_RELAXED);
    return;
  }

  /* A stale heartbeat only made this core a suspect.  The one that did not
   * answer the probe is the one to name: with idle APs now ticking once a
   * second, a stale stamp can just be a core between ticks. */
  for (uint32_t i = 0; i < count && i < MAX_CPUS; i++) {
    if (i == self->cpu_id)
      continue;
    struct cpu_info *c = cpu_get_info(i);
    if (!c || (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP))
      continue;
    if (!(ack & (1ULL << i))) {
      victim = i;
      break;
    }
  }

  uint32_t expect = 0;
  if (!__atomic_compare_exchange_n(&report_latched, &expect, 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return; /* another core already reported this hang */

  __atomic_add_fetch(&g_stats.hung_cpu_reports, 1, __ATOMIC_RELAXED);
  inside_reporter[self->cpu_id] = 1;

  char reason[64];
  /* Built by hand: klogf() would take serial_lock, which may be the very lock
   * the hung core is sitting on. */
  reason_stuck_cpu(reason, sizeof(reason), victim);

  lockdiag_dump_all(reason);

  /* The machine is deliberately left exactly as it is: the report is only
   * useful while the stuck cores stay stuck, and QEMU can still be attached. */
}

/* --- init --------------------------------------------------------------- */

/* --- on-demand report: Right-Ctrl three times ----------------------------
 *
 * The detector fires when a core cannot take interrupts.  A userspace deadlock
 * is the opposite case - every core is healthy and ticking, and only the
 * desktop is frozen - so it produces no report at all, which is exactly the
 * "system froze but no deadlock log" case.  A key press is answered by the
 * keyboard interrupt path, so this reaches a live kernel on demand and captures
 * the machine as it looks dead.
 *
 * Why this particular combination: the trigger has to survive the *host* as
 * well as the guest.  Ctrl+Alt+F-key is a VT switch on any Linux host, which
 * swallows it before QEMU ever sees the key (and switching the host console
 * away from the desktop looks exactly like the guest having frozen).  Ctrl+Alt
 * alone is QEMU's mouse-grab release, and Alt+SysRq is the host's magic SysRq.
 * A bare right-hand modifier repeated three times is bound by nothing - not the
 * Linux console, not X, not Wayland, not QEMU - needs no scancode that might be
 * absent from a translation table, and is not something a person produces while
 * typing. */
void lockdiag_keyboard_event(uint16_t code, bool pressed) {
  static volatile unsigned tap_count;
  static volatile uint64_t first_tap_tsc;
  static volatile uint64_t last_tap_tsc;
  static volatile uint64_t last_report_tsc;

  /* Two guards, both needed.  Key auto-repeat (PS/2 resends, and a stuck-key
   * report in general) would otherwise count as taps, so a tap must be
   * separated from the previous one.  And once the report starts, holding the
   * key must not start a second one while the first is still printing. */
  uint64_t mhz = tsc_get_mhz();
  if (!mhz)
    return; /* no calibrated clock to time the window against */
  uint64_t now = rdtsc();

  if (code != LOCKDIAG_TRIGGER_KEY || !pressed)
    return;

  if (last_tap_tsc &&
      (now - last_tap_tsc) < LOCKDIAG_TICKS_PER_MS(mhz) * LOCKDIAG_TRIGGER_DEBOUNCE_MS)
    return;
  last_tap_tsc = now;

  if (last_report_tsc &&
      (now - last_report_tsc) <
          LOCKDIAG_TICKS_PER_MS(mhz) * LOCKDIAG_TRIGGER_COOLDOWN_MS)
    return;

  if (tap_count == 0 ||
      (now - first_tap_tsc) >
          LOCKDIAG_TICKS_PER_MS(mhz) * LOCKDIAG_TRIGGER_WINDOW_MS) {
    tap_count = 1;
    first_tap_tsc = now;
    return;
  }

  if (++tap_count < LOCKDIAG_TRIGGER_TAPS)
    return;

  tap_count = 0;
  last_report_tsc = now;
  lockdiag_dump_all("manual trigger: Right-Ctrl three times");
}

/* Scancode-level entry point, for keyboard paths that report a key before
 * evdev exists: evdev_get_keyboard() returns NULL until evdev_init() runs, and
 * that is skipped when the root mount fails - which is exactly the environment
 * where the machine is hardest to inspect.  PS/2 set-1 scancode 0x1D is left
 * Ctrl, and 0x1D with the E0 prefix is right Ctrl.  Both entry points share one
 * tap counter, so a driver that reports the same press through both paths still
 * needs three distinct presses. */
void lockdiag_keyboard_scancode(uint8_t scancode, bool extended, bool release) {
  if (scancode == 0x1D && extended)
    lockdiag_keyboard_event(LOCKDIAG_TRIGGER_KEY, !release);
}

void lockdiag_init(void) {
  g_init_tsc = rdtsc();

  int vec = interrupt_vector_alloc(probe_isr);
  if (vec >= 0)
    probe_vector_state = (uint8_t)vec;

  klog_puts("[LOCKDIAG] Hang detector armed: probe vector 0x");
  klog_hex32(vec >= 0 ? (uint32_t)vec : 0);
  klog_puts(", hung-CPU threshold ");
  klog_uint64(LOCKDIAG_HUNG_CPU_MS);
  klog_puts(" ms, stuck-waiter threshold ");
#if LOCKDIAG_STUCK_TASK_MS
  klog_uint64(LOCKDIAG_STUCK_TASK_MS);
  klog_puts(" ms; on-demand report: right-Ctrl x3 or \"dump\" on the "
              "serial console, lock owner tracking ");
#else
  klog_puts("off (make LOCKDIAG_STUCK_MS=30000 to enumerate futex waiters; "
            "long parks are listed in a hang report either way), lock owner "
            "tracking ");
#endif
  klog_puts(LOCKDIAG_OWNER ? "ON\n"
                           : "OFF (rebuild with make LOCKDIAG_OWNER=1)\n");
}

#else /* !LOCKDIAG */

void lockdiag_init(void) {}
void lockdiag_tick(void) {}
void lockdiag_keyboard_event(uint16_t code, bool pressed) {
  (void)code;
  (void)pressed;
}
void lockdiag_keyboard_scancode(uint8_t scancode, bool extended, bool release) {
  (void)scancode;
  (void)extended;
  (void)release;
}
void lockdiag_dump_all(const char *reason) { (void)reason; }
uint64_t lockdiag_probe_cpus(uint32_t timeout_ms) {
  (void)timeout_ms;
  return 0;
}
bool lockdiag_self_holds_vmm_lock(uint32_t self_id) {
  (void)self_id;
  return false;
}
void lockdiag_spot_take(int id, uint64_t wait_cycles, uint64_t ip) {
  (void)id;
  (void)wait_cycles;
  (void)ip;
}
void lockdiag_spot_drop(int id) { (void)id; }
bool lockdiag_spot_held_by_self(int id) {
  (void)id;
  return false;
}
lockdiag_stats_t *lockdiag_stats(void) { return (lockdiag_stats_t *)0; }
const char *lockdiag_spot_name(int id) {
  (void)id;
  return "lock";
}
lockdiag_spot_t *lockdiag_spot(int id) {
  (void)id;
  return (lockdiag_spot_t *)0;
}

#endif /* LOCKDIAG */
