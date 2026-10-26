#include "../fs/mount_namespace.h"
#include "pid_namespace.h"
#include "net_namespace.h"
#include "capability.h"
#include "seccomp.h"
#include "sched.h"
#include "sched_internal.h"
#include "namespace.h"
#include "hal/hal.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/procfs.h"
#include "../fs/vfs.h"
#include "../fs/mount_namespace.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../cpu/fpu.h"
#include "../smp/cpu.h"

uint32_t next_tid = 1;
spinlock_t tid_lock = SPINLOCK_INIT;
struct thread *global_thread_list = NULL;
/* Atomic mirror of global_thread_list's length: +1 at the insert below and
 * at the idle-thread insert in sched.c, -1 at the unlink in sched_reap.c. */
uint32_t global_thread_count = 0;

/* --- O(1) Hierarchical PID / TID Bitmap Allocator ---
 * Uses hardware tzcnt/bsfl (__builtin_ctzll) to provide instant O(1) PID
 * allocation and recycling without linear scans.
 */
#define PID_MAX_LIMIT 65536
#define PID_MAX_DEFAULT 32768
#define PID_RESERVED 2

static uint64_t pid_bitmap[PID_MAX_LIMIT / 64];
static uint64_t pid_summary[PID_MAX_LIMIT / 64 / 64];
static uint64_t pid_top_summary = 0;
static uint32_t last_allocated_pid = 1;
static bool pid_bitmap_initialized = false;
static struct thread *tid_map[PID_MAX_LIMIT];

static inline void pid_set_bit(uint32_t pid) {
  if (__builtin_expect(pid >= PID_MAX_LIMIT, 0)) return;
  uint32_t word = pid / 64;
  uint32_t bit = pid % 64;
  pid_bitmap[word] |= (1ULL << bit);
  if (__builtin_expect(pid_bitmap[word] == ~0ULL, 0)) {
    uint32_t sword = word / 64;
    uint32_t sbit = word % 64;
    pid_summary[sword] |= (1ULL << sbit);
    if (__builtin_expect(pid_summary[sword] == ~0ULL, 0)) {
      pid_top_summary |= (1ULL << sword);
    }
  }
}

static inline void pid_clear_bit(uint32_t pid) {
  if (__builtin_expect(pid >= PID_MAX_LIMIT, 0)) return;
  uint32_t word = pid / 64;
  uint32_t bit = pid % 64;
  pid_bitmap[word] &= ~(1ULL << bit);
  uint32_t sword = word / 64;
  uint32_t sbit = word % 64;
  pid_summary[sword] &= ~(1ULL << sbit);
  pid_top_summary &= ~(1ULL << sword);
}

static inline bool pid_is_allocated(uint32_t pid) {
  if (pid >= PID_MAX_LIMIT) return false;
  return (pid_bitmap[pid / 64] & (1ULL << (pid % 64))) != 0;
}

static void pid_bitmap_init(void) {
  if (pid_bitmap_initialized)
    return;
  memset(pid_bitmap, 0, sizeof(pid_bitmap));
  memset(pid_summary, 0, sizeof(pid_summary));
  pid_top_summary = 0;
  memset(tid_map, 0, sizeof(tid_map));

  // Mark PID 0 as reserved
  pid_set_bit(0);
  pid_bitmap_initialized = true;
}

static uint32_t sched_find_free_pid(uint32_t start) {
  const uint32_t max_pid = PID_MAX_DEFAULT;
  if (start >= max_pid)
    start = PID_RESERVED;

  uint32_t word = start / 64;
  uint32_t bit = start % 64;

  // 1. Current word from 'bit'
  uint64_t mask = (bit == 0) ? ~0ULL : ~((1ULL << bit) - 1);
  uint64_t w = pid_bitmap[word] | ~mask;
  if (w != ~0ULL) {
    uint32_t free_bit = __builtin_ctzll(~w);
    uint32_t pid = word * 64 + free_bit;
    if (pid < max_pid)
      return pid;
  }

  // 2. Scan remaining words in current summary block (up to 64 words)
  uint32_t sword = word / 64;
  uint32_t sbit = (word % 64) + 1;
  if (sbit < 64) {
    uint64_t smask = ~((1ULL << sbit) - 1);
    uint64_t sw = pid_summary[sword] | ~smask;
    if (sw != ~0ULL) {
      uint32_t free_sword_bit = __builtin_ctzll(~sw);
      uint32_t free_word_idx = sword * 64 + free_sword_bit;
      uint32_t free_bit = __builtin_ctzll(~pid_bitmap[free_word_idx]);
      uint32_t pid = free_word_idx * 64 + free_bit;
      if (pid < max_pid)
        return pid;
    }
  }

  // 3. Scan remaining summary blocks using top_summary
  uint32_t max_swords = max_pid / 4096;
  uint32_t top_bit = sword + 1;
  if (top_bit < max_swords) {
    uint64_t top_mask = ~((1ULL << top_bit) - 1);
    uint64_t tw = pid_top_summary | ~top_mask;
    if (tw != ~0ULL) {
      uint32_t free_sword = __builtin_ctzll(~tw);
      if (free_sword < max_swords) {
        uint32_t free_sword_bit = __builtin_ctzll(~pid_summary[free_sword]);
        uint32_t free_word = free_sword * 64 + free_sword_bit;
        uint32_t free_bit = __builtin_ctzll(~pid_bitmap[free_word]);
        uint32_t pid = free_word * 64 + free_bit;
        if (pid < max_pid)
          return pid;
      }
    }
  }

  // 4. Wrap around and search from PID_RESERVED
  if (start > PID_RESERVED) {
    start = PID_RESERVED;
    word = start / 64;
    bit = start % 64;

    mask = (bit == 0) ? ~0ULL : ~((1ULL << bit) - 1);
    w = pid_bitmap[word] | ~mask;
    if (w != ~0ULL) {
      uint32_t pid = word * 64 + __builtin_ctzll(~w);
      if (pid < max_pid)
        return pid;
    }

    sword = word / 64;
    sbit = (word % 64) + 1;
    if (sbit < 64) {
      uint64_t smask = ~((1ULL << sbit) - 1);
      uint64_t sw = pid_summary[sword] | ~smask;
      if (sw != ~0ULL) {
        uint32_t free_sword_bit = __builtin_ctzll(~sw);
        uint32_t free_word_idx = sword * 64 + free_sword_bit;
        uint32_t pid = free_word_idx * 64 + __builtin_ctzll(~pid_bitmap[free_word_idx]);
        if (pid < max_pid)
          return pid;
      }
    }

    top_bit = sword + 1;
    if (top_bit < max_swords) {
      uint64_t top_mask = ~((1ULL << top_bit) - 1);
      uint64_t tw = pid_top_summary | ~top_mask;
      if (tw != ~0ULL) {
        uint32_t free_sword = __builtin_ctzll(~tw);
        if (free_sword < max_swords) {
          uint32_t free_sword_bit = __builtin_ctzll(~pid_summary[free_sword]);
          uint32_t free_word = free_sword * 64 + free_sword_bit;
          uint32_t pid = free_word * 64 + __builtin_ctzll(~pid_bitmap[free_word]);
          if (pid < max_pid)
            return pid;
        }
      }
    }
  }

  return 0; // Exhausted
}

uint32_t sched_alloc_tid_locked(void) {
  if (__builtin_expect(!pid_bitmap_initialized, 0)) {
    pid_bitmap_init();
  }

  // PID 1 is reserved for init process
  if (!pid_is_allocated(1)) {
    pid_set_bit(1);
    last_allocated_pid = 1;
    next_tid = 2;
    return 1;
  }

  uint32_t pid = sched_find_free_pid(last_allocated_pid + 1);
  if (__builtin_expect(pid == 0, 0)) {
    for (uint32_t p = PID_RESERVED; p < PID_MAX_LIMIT; p++) {
      if (!pid_is_allocated(p)) {
        pid = p;
        break;
      }
    }
    if (pid == 0)
      return 0;
  }

  pid_set_bit(pid);
  last_allocated_pid = pid;
  next_tid = pid + 1;
  return pid;
}

void sched_free_tid_locked(uint32_t tid) {
  if (tid < PID_RESERVED || tid >= PID_MAX_LIMIT)
    return;
  pid_clear_bit(tid);
  tid_map[tid] = NULL;
}

extern void thread_stub(void); // Defined in switch.asm
extern void signal_send(struct thread *t, int sig);
extern void signal_notify_thread(struct thread *t, int sig);

#define KSTACK_CACHE_PER_CPU 2
#define KSTACK_GLOBAL_POOL_MAX 16
#define KSTACK_CANARY 0x5AFE57AC05AFE57ACULL
#define KSTACK_CANARY_WORDS 8

struct kstack_cpu_cache {
  void *stacks[KSTACK_CACHE_PER_CPU];
  uint32_t count;
  spinlock_t lock;
};

static struct kstack_cpu_cache kstack_cpu_caches[MAX_CPUS];
static void *kstack_global_pool[KSTACK_GLOBAL_POOL_MAX];
static uint32_t kstack_global_count = 0;
static spinlock_t kstack_global_lock = SPINLOCK_INIT;

/* Diagnostic statistics */
static uint64_t kstack_stat_alloc_cache = 0;
static uint64_t kstack_stat_alloc_pmm = 0;
static uint64_t kstack_stat_free_cache = 0;
static uint64_t kstack_stat_free_pmm = 0;
static uint64_t kstack_stat_reclaimed = 0;

void kstack_cache_init(void) {
  for (uint32_t i = 0; i < MAX_CPUS; i++) {
    kstack_cpu_caches[i].count = 0;
    spinlock_init(&kstack_cpu_caches[i].lock);
  }
  spinlock_init(&kstack_global_lock);
  kstack_global_count = 0;
}

size_t kstack_cache_total_cached(void) {
  size_t total = __atomic_load_n(&kstack_global_count, __ATOMIC_RELAXED);
  uint32_t count = cpu_get_count();
  if (count > MAX_CPUS)
    count = MAX_CPUS;
  for (uint32_t i = 0; i < count; i++) {
    total += __atomic_load_n(&kstack_cpu_caches[i].count, __ATOMIC_RELAXED);
  }
  return total;
}

static inline void kstack_arm_canary(void *stack) {
  uint64_t *canaries = (uint64_t *)stack;
  for (int i = 0; i < KSTACK_CANARY_WORDS; i++) {
    canaries[i] = KSTACK_CANARY;
  }
}

static inline bool kstack_verify_canary(void *stack) {
  uint64_t *canaries = (uint64_t *)stack;
  for (int i = 0; i < KSTACK_CANARY_WORDS; i++) {
    if (canaries[i] != KSTACK_CANARY)
      return false;
  }
  return true;
}

void *thread_stack_alloc(void) {
  void *stack = NULL;
  struct cpu_info *cpu = cpu_get_current();

  /* 1. Fast path: check current CPU's local lookaside cache */
  if (cpu && cpu->cpu_id < MAX_CPUS) {
    struct kstack_cpu_cache *pc = &kstack_cpu_caches[cpu->cpu_id];
    spinlock_acquire(&pc->lock);
    if (pc->count > 0) {
      stack = pc->stacks[--pc->count];
      __atomic_add_fetch(&kstack_stat_alloc_cache, 1, __ATOMIC_RELAXED);
    }
    spinlock_release(&pc->lock);
  }

  /* 2. Secondary path: check global overflow pool */
  if (!stack && __atomic_load_n(&kstack_global_count, __ATOMIC_RELAXED) > 0) {
    spinlock_acquire(&kstack_global_lock);
    if (kstack_global_count > 0) {
      stack = kstack_global_pool[--kstack_global_count];
      __atomic_add_fetch(&kstack_stat_alloc_cache, 1, __ATOMIC_RELAXED);
    }
    spinlock_release(&kstack_global_lock);
  }

  /* 3. Slow path: on-demand allocate order-2 physical pages from PMM */
  if (!stack) {
    void *phys = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (!phys)
      return NULL;
    stack = (void *)((uint64_t)phys + pmm_get_hhdm_offset());
    __atomic_add_fetch(&kstack_stat_alloc_pmm, 1, __ATOMIC_RELAXED);
  }

  /* Sanitize stack: zero frames and arm bottom canary zone */
  memset(stack, 0, THREAD_STACK_SIZE);
  kstack_arm_canary(stack);

  return stack;
}

void thread_stack_release(uint64_t stack_base) {
  if (!stack_base)
    return;

  void *stack = (void *)stack_base;

  /* Check stack overflow canary zone at the bottom of the stack */
  if (!kstack_verify_canary(stack)) {
    klogf("[KSTACK] WARNING: Kernel stack overflow detected on stack %p (canary corrupted)!\n",
          stack);
    /* Corrupted stack: do not recycle into cache, free directly to PMM */
    void *phys = (void *)(stack_base - pmm_get_hhdm_offset());
    pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
    __atomic_add_fetch(&kstack_stat_free_pmm, 1, __ATOMIC_RELAXED);
    return;
  }

  struct cpu_info *cpu = cpu_get_current();

  /* 1. Try to cache on current CPU's local lookaside cache */
  if (cpu && cpu->cpu_id < MAX_CPUS) {
    struct kstack_cpu_cache *pc = &kstack_cpu_caches[cpu->cpu_id];
    spinlock_acquire(&pc->lock);
    if (pc->count < KSTACK_CACHE_PER_CPU) {
      pc->stacks[pc->count++] = stack;
      spinlock_release(&pc->lock);
      __atomic_add_fetch(&kstack_stat_free_cache, 1, __ATOMIC_RELAXED);
      return;
    }
    spinlock_release(&pc->lock);
  }

  /* 2. Try to cache in global overflow pool */
  spinlock_acquire(&kstack_global_lock);
  if (kstack_global_count < KSTACK_GLOBAL_POOL_MAX) {
    kstack_global_pool[kstack_global_count++] = stack;
    spinlock_release(&kstack_global_lock);
    __atomic_add_fetch(&kstack_stat_free_cache, 1, __ATOMIC_RELAXED);
    return;
  }
  spinlock_release(&kstack_global_lock);

  /* 3. Cache full: free back to PMM buddy allocator */
  void *phys = (void *)(stack_base - pmm_get_hhdm_offset());
  pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
  __atomic_add_fetch(&kstack_stat_free_pmm, 1, __ATOMIC_RELAXED);
}

size_t thread_stack_cache_reclaim(size_t max_reclaim) {
  size_t reclaimed_stacks = 0;
  if (max_reclaim == 0)
    max_reclaim = 64;

  /* 1. Drain from global pool first */
  while (reclaimed_stacks < max_reclaim) {
    void *stack = NULL;
    spinlock_acquire(&kstack_global_lock);
    if (kstack_global_count > 0) {
      stack = kstack_global_pool[--kstack_global_count];
    }
    spinlock_release(&kstack_global_lock);

    if (!stack)
      break;

    void *phys = (void *)((uint64_t)stack - pmm_get_hhdm_offset());
    pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
    reclaimed_stacks++;
  }

  /* 2. Drain from per-CPU caches if more reclaim needed */
  if (reclaimed_stacks < max_reclaim) {
    uint32_t count = cpu_get_count();
    if (count > MAX_CPUS)
      count = MAX_CPUS;
    for (uint32_t i = 0; i < count && reclaimed_stacks < max_reclaim; i++) {
      struct kstack_cpu_cache *pc = &kstack_cpu_caches[i];
      while (reclaimed_stacks < max_reclaim) {
        void *stack = NULL;
        spinlock_acquire(&pc->lock);
        if (pc->count > 0) {
          stack = pc->stacks[--pc->count];
        }
        spinlock_release(&pc->lock);

        if (!stack)
          break;

        void *phys = (void *)((uint64_t)stack - pmm_get_hhdm_offset());
        pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
        reclaimed_stacks++;
      }
    }
  }

  if (reclaimed_stacks > 0) {
    __atomic_add_fetch(&kstack_stat_reclaimed, reclaimed_stacks, __ATOMIC_RELAXED);
  }

  return reclaimed_stacks * (THREAD_STACK_SIZE / PAGE_SIZE);
}

static void thread_exit(void) {
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  struct thread *t = cpu ? cpu->current_thread : NULL;

  if (t) {
    /* Hand the thread to the reaper the same way a detached user thread
     * does: mark it DEAD and queue it, then switch away.  The reaper frees
     * the kernel stack, mm, fd table and struct thread once this context is
     * off-CPU.  Without this, every kernel thread that returns from its
     * entry point (all the LinuxKPI self-test workers, drm_sched threads,
     * ...) stayed in the global thread list forever. */
    t->state = THREAD_DEAD;
    sched_queue_reap(t);
  }

  while (1) {
    sched_yield();
  }
}

struct thread *sched_create_kernel_thread(void (*entry)(void),
                                          struct cpu_info *explicit_cpu,
                                          bool enqueue) {
  // Allocate thread struct
  struct thread *t = kmalloc(sizeof(struct thread));
  if (!t)
    return NULL;

  memset(t, 0, sizeof(struct thread));
  spinlock_init(&t->namespace_lock);
  t->start_time_ms = lapic_timer_get_ms();
  if (t->start_time_ms == 0)
    t->start_time_ms = 1;
  if (!sched_ensure_files(t)) {
    kfree(t);
    return NULL;
  }

  // Default comm for kernel threads; overwritten by execve for user processes
  strcpy(t->comm, "kthread");
  t->cwd_path[0] = '/';
  t->cwd_node = fs_root;
  struct thread *current = sched_get_current();
  if (current) {
    strcpy(t->cwd_path, current->cwd_path);
    t->cwd_node = current->cwd_node;
    mempolicy_copy(&t->numa_policy, &current->numa_policy);
  } else {
    mempolicy_init(&t->numa_policy);
  }
  if (t->cwd_node)
    vfs_open(t->cwd_node);
  t->umask = 0022;
  t->uid = t->gid = t->euid = t->egid = t->suid = t->sgid = 0;
  t->fsuid = t->fsgid = 0;
  t->supplementary_group_count = 0;

  // Root kernel threads get their own MM by default.
  // fork/clone/exec will replace/refcount this later as needed.
  t->mm = kmalloc(sizeof(struct mm_struct));
  if (t->mm) {
    memset(t->mm, 0, sizeof(struct mm_struct));
    vma_list_init(&t->mm->vmas);
    t->mm->ref_count = 1;
    t->mm->pcid = pcid_alloc();
    spinlock_init(&t->mm->lock);
  }

  uint32_t cpu_count = cpu_get_count();
  t->cpu_affinity = (1ULL << cpu_count) - 1;
  if (cpu_count == 64)
    t->cpu_affinity = ~0ULL;

  /* Keep the requested/home CPU as a placement hint even for suspended
   * threads (fork/clone and worker setup enqueue only after initialization).
   * A zeroed cpu_index silently biases those later balanced enqueues toward
   * the BSP regardless of which CPU created the thread. */
  struct cpu_info *home_cpu = explicit_cpu ? explicit_cpu : cpu_get_current();
  if (home_cpu && home_cpu->cpu_id < cpu_count)
    t->cpu_index = home_cpu->cpu_id;

  t->state = THREAD_READY;
  eevfd_entity_init(&t->se, 0, 0);
  t->priority = SCHED_PRIORITY_DEFAULT;
  t->static_priority = SCHED_PRIORITY_DEFAULT;
  t->nice_value = 0;
  t->stack_size = THREAD_STACK_SIZE;

  t->stack_base = (uint64_t)thread_stack_alloc();

  if (!t->stack_base) {
    sched_release_files(t);
    if (t->cwd_node)
      vfs_close(t->cwd_node);
    if (t->mm) {
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);
    }
    kfree(t);
    return NULL;
  }

  uint64_t stack_top = t->stack_base + THREAD_STACK_SIZE;
  stack_top &= ~0xFULL; // Align stack

  // 1. Build thread_stub's synthetic frame.  The stub is entered by
  //    switch_context's `ret`, so it must land 16-byte aligned with
  //    [rsp] = thread_exit: its `call entry` then pushes a proper SysV
  //    frame (entry sees rsp % 16 == 8) and its fall-through `ret` enters
  //    thread_exit the same way.  An 8-byte-aligned frame here leaves every
  //    callee off by 8, which faults on the aligned SSE spills the AMD DML
  //    code uses (`movaps` -> #GP).
  stack_top -= 16;
  *(uint64_t *)stack_top = (uint64_t)thread_exit;

  // 2. Setup context frame
  stack_top -= sizeof(struct context);
  struct context *ctx = (struct context *)stack_top;
  memset(ctx, 0, sizeof(struct context));

  // Store entry function in r12 which thread_stub will call
  ctx->r12 = (uint64_t)entry;

  // `switch_context` does `ret`, popping this address
  ctx->ret_addr = (uint64_t)thread_stub;

  t->rsp = stack_top;

  fpu_init_thread(t);

  /* Publish only after all allocations and context initialization succeed. */
  spinlock_acquire(&tid_lock);
  t->tid = sched_alloc_tid_locked();
  t->tgid = t->tid;
  if (__builtin_expect(t->tid < PID_MAX_LIMIT, 1)) {
    tid_map[t->tid] = t;
  }
  t->ss_flags = SS_DISABLE;
  t->parent = current;
  if (current) {
    t->sibling_next = current->children;
    current->children = t;
    t->pgid = current->pgid;
    t->sid = current->sid;
  } else {
    t->pgid = t->tid;
    t->sid = t->tid;
  }
  t->global_next = global_thread_list;
  t->global_prev = NULL;
  if (global_thread_list) {
    global_thread_list->global_prev = t;
  }
  global_thread_list = t;
  __atomic_add_fetch(&global_thread_count, 1, __ATOMIC_RELAXED);
  spinlock_release(&tid_lock);

  // Balance and add to a CPU's runqueue conditionally
  if (enqueue) {
    sched_enqueue_thread(t, explicit_cpu);
  }

  return t;
}

struct thread *find_thread_by_tid_locked(uint32_t tid) {
  if (__builtin_expect(tid < PID_MAX_LIMIT, 1)) {
    return tid_map[tid];
  }
  // Fallback for special kernel/idle threads (tid >= 0x7FFFFFF0)
  struct thread *curr = global_thread_list;
  while (curr) {
    if (curr->tid == tid)
      return curr;
    curr = curr->global_next;
  }
  return NULL;
}

struct thread *sched_get_thread_by_tid(uint32_t tid) {
  spinlock_acquire(&tid_lock);
  struct thread *curr = find_thread_by_tid_locked(tid);
  spinlock_release(&tid_lock);
  return curr;
}

bool sched_get_thread_snapshot(uint32_t tid,
                               struct sched_thread_snapshot *snapshot) {
  if (!snapshot)
    return false;

  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t) {
    spinlock_release(&tid_lock);
    return false;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->tid = t->tid;
  snapshot->tgid = t->tgid;
  snapshot->parent_tid = t->parent ? t->parent->tid : 0;
  snapshot->pgid = t->pgid;
  snapshot->state = t->state;
  snapshot->runtime_total = t->runtime_total;
  snapshot->runtime_user_ms =
      __atomic_load_n(&t->runtime_user_ms, __ATOMIC_RELAXED);
  snapshot->runtime_system_ms =
      __atomic_load_n(&t->runtime_system_ms, __ATOMIC_RELAXED);
  snapshot->start_time_ms = t->start_time_ms;
  snapshot->uts_namespace_id = uts_namespace_id(t);
  snapshot->ipc_namespace_id = ipc_namespace_id(t);
  snapshot->pid_namespace_id = pid_namespace_id(t);
  snapshot->net_namespace_id = net_namespace_id(t);
  snapshot->mount_namespace_id = vfs_namespace_id(t);
  (void)capability_has(t, 0);
  snapshot->cap_inheritable = t->cap_inheritable;
  snapshot->cap_permitted = t->cap_permitted;
  snapshot->cap_effective = t->cap_effective;
  snapshot->cap_bounding = t->cap_bounding;
  snapshot->cap_ambient = t->cap_ambient;
  snapshot->no_new_privs = __atomic_load_n(&t->no_new_privs, __ATOMIC_ACQUIRE);
  snapshot->seccomp_mode = seccomp_mode(t);
  snapshot->uid = t->uid;
  snapshot->gid = t->gid;
  snapshot->euid = t->euid;
  snapshot->egid = t->egid;
  snapshot->suid = t->suid;
  snapshot->sgid = t->sgid;
  snapshot->cpu_index = t->cpu_index;
  snapshot->last_syscall_num = t->last_syscall_num;
  snapshot->last_syscall_ret = t->last_syscall_ret;
  for (int i = 0; i < 6; i++)
    snapshot->last_syscall_args[i] = t->last_syscall_args[i];
  snapshot->last_poll_sec = t->last_poll_sec;
  snapshot->last_poll_nsec = t->last_poll_nsec;
  snapshot->last_poll_nfds = t->last_poll_nfds;
  for (int i = 0; i < 8; i++) {
    snapshot->last_poll_fds[i] = t->last_poll_fds[i];
    snapshot->last_poll_events[i] = t->last_poll_events[i];
    for (int j = 0; j < 48; j++) {
      snapshot->last_poll_paths[i][j] = t->last_poll_paths[i][j];
      if (!t->last_poll_paths[i][j])
        break;
    }
  }
  memcpy(snapshot->comm, t->comm, sizeof(snapshot->comm));
  snapshot->comm[sizeof(snapshot->comm) - 1] = 0;
  memcpy(snapshot->exe_path, t->exe_path, sizeof(snapshot->exe_path));
  snapshot->exe_path[sizeof(snapshot->exe_path) - 1] = 0;
  memcpy(snapshot->cwd_path, t->cwd_path, sizeof(snapshot->cwd_path));
  snapshot->cwd_path[sizeof(snapshot->cwd_path) - 1] = 0;

  spinlock_release(&tid_lock);
  return true;
}

/*
 * Virtual + resident bytes for /proc/<pid>/{status,statm}.
 *
 * Both numbers used to be invented right here in the snapshot:
 *
 *   virt_bytes  = (brk span) + (0x800000000000 - mmap_next_addr)
 *   resident    = virt_bytes / 2
 *
 * mmap grows *upward* from MMAP_REGION_BASE, so the second term was the arena
 * still free rather than what was mapped - a fresh process claimed ~1 TiB of
 * virtual memory, and the figure *fell* as the process allocated more.  The
 * resident value was then half of that fiction: a WebKit web process reported
 * ~448 GB of RSS from /proc/self/statm, which WTF::memoryFootprint() compares
 * against its 8 GB kill threshold, so the memory monitor killed the process on
 * every poll ("Unable to shrink memory footprint ... below the kill thresold"),
 * and the webview reported web process terminated reason=memory-limit.
 *
 * Now: virtual size is the sum of the VMAs, resident size is counted from the
 * page tables.  Both walks happen after dropping tid_lock - this lock masks
 * interrupts for its holder and is taken on every scheduler lookup, so it
 * cannot be held across a page-table walk.
 *
 * The mm reference taken here is what keeps the page tables alive while the
 * lock is dropped; mm_put() hands it back and frees them if we turn out to be
 * the last holder.
 */
bool sched_get_mem_snapshot(uint32_t tid, uint64_t *virt_bytes,
                            uint64_t *resident_bytes) {
  if (virt_bytes)
    *virt_bytes = 0;
  if (resident_bytes)
    *resident_bytes = 0;

  struct mm_struct *mm = NULL;
  uint64_t cr3 = 0;

  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (t && t->mm) {
    /* A thread found under tid_lock is still linked, so the reaper has not
     * reached its mm drop yet - this reference is taken on a live mm. */
    mm = t->mm;
    cr3 = t->cr3;
    __atomic_add_fetch(&mm->ref_count, 1, __ATOMIC_ACQ_REL);
  }
  spinlock_release(&tid_lock);

  if (!mm)
    return false;

  spinlock_acquire(&mm->lock);
  uint64_t virt = vma_total_bytes(&mm->vmas);
  spinlock_release(&mm->lock);

  uint64_t resident = vmm_user_resident_bytes(cr3);

  /* A few user pages are mapped without a VMA covering them (vdso, vsyscall,
   * the signal trampoline are installed directly by the fault/exec paths), so
   * RSS can legitimately edge past VSZ by pages.  Never report that: VmSize
   * below VmRSS reads as >100% to htop and ps. */
  if (virt < resident)
    virt = resident;

  if (virt_bytes)
    *virt_bytes = virt;
  if (resident_bytes)
    *resident_bytes = resident;

  mm_put(mm, cr3);
  return true;
}

size_t sched_read_thread_auxv(uint32_t tid, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  if (!buffer || size == 0)
    return 0;

  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !t->mm || t->mm->auxv_count == 0) {
    spinlock_release(&tid_lock);
    return 0;
  }

  size_t total_bytes = t->mm->auxv_count * sizeof(uint64_t);
  if (offset >= total_bytes) {
    spinlock_release(&tid_lock);
    return 0;
  }

  if (offset + size > total_bytes)
    size = total_bytes - offset;

  memcpy(buffer, ((uint8_t *)t->mm->saved_auxv) + offset, size);
  spinlock_release(&tid_lock);
  return size;
}

size_t sched_read_thread_cmdline(uint32_t tid, uint32_t offset, uint32_t size,
                                uint8_t *buffer) {
  if (!buffer || size == 0)
    return 0;

  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !t->mm || t->mm->cmdline_len == 0) {
    spinlock_release(&tid_lock);
    return 0;
  }

  size_t total = t->mm->cmdline_len;
  if (offset >= total) {
    spinlock_release(&tid_lock);
    return 0;
  }
  if (offset + size > total)
    size = total - offset;

  memcpy(buffer, ((uint8_t *)t->mm->saved_cmdline) + offset, size);
  spinlock_release(&tid_lock);
  return size;
}

size_t sched_thread_cmdline_size(uint32_t tid) {
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  size_t total = (t && t->mm) ? t->mm->cmdline_len : 0;
  spinlock_release(&tid_lock);
  return total;
}

bool sched_get_nth_thread_tid(uint32_t index, uint32_t *tid) {
  if (!tid)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t && index--)
    t = t->global_next;
  if (t)
    *tid = t->tid;
  spinlock_release(&tid_lock);
  return t != NULL;
}

uint32_t sched_get_group_threads(uint32_t tgid, uint32_t index, uint32_t *tid) {
  uint32_t count = 0;
  if (tid) *tid = 0;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if ((t->tgid ? t->tgid : t->tid) != tgid ||
        t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
      continue;
    if (tid && count == index) *tid = t->tid;
    count++;
  }
  spinlock_release(&tid_lock);
  return count;
}

uint16_t sched_get_thread_count(void) {
  /* Count is maintained at insert/unlink time - no list walk, no tid_lock
   * (the walk was half of sys_sysinfo's 1.5us). */
  return (uint16_t)__atomic_load_n(&global_thread_count, __ATOMIC_RELAXED);
}

uint16_t sched_get_runnable_thread_count(void) {
  spinlock_acquire(&tid_lock);
  uint16_t count = 0;
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (t->state == THREAD_RUNNING || t->state == THREAD_READY)
      count++;
  spinlock_release(&tid_lock);
  return count;
}

/*
 * Sum the runtime of every non-idle thread to derive user-mode CPU ms.
 * idle_ms = total_elapsed_ms * ncpus - user_ms (clamped ≥ 0).
 * Both outputs are in milliseconds.
 */
void sched_get_total_cpu_ms(uint64_t *out_user_ms, uint64_t *out_idle_ms) {
  uint64_t user_ms = 0;

  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (!t->is_idle)
      user_ms += t->runtime_total;
  }
  spinlock_release(&tid_lock);

  uint32_t ncpus = cpu_get_count();
  if (ncpus == 0) ncpus = 1;

  uint64_t elapsed_ms = lapic_timer_get_ms();
  uint64_t total_ms   = elapsed_ms * (uint64_t)ncpus;

  if (user_ms > total_ms)
    user_ms = total_ms;

  if (out_user_ms) *out_user_ms = user_ms;
  if (out_idle_ms) *out_idle_ms = total_ms - user_ms;
}

void sched_get_cpu_times(uint32_t cpu_id, uint64_t *out_user_ms,
                         uint64_t *out_system_ms, uint64_t *out_idle_ms) {
  struct cpu_info *cpu = cpu_get_info(cpu_id);
  if (!cpu) {
    if (out_user_ms) *out_user_ms = 0;
    if (out_system_ms) *out_system_ms = 0;
    if (out_idle_ms) *out_idle_ms = 0;
    return;
  }
  if (out_user_ms)
    *out_user_ms = __atomic_load_n(&cpu->stats_user_ms, __ATOMIC_RELAXED);
  if (out_system_ms)
    *out_system_ms =
        __atomic_load_n(&cpu->stats_system_ms, __ATOMIC_RELAXED);
  if (out_idle_ms)
    *out_idle_ms = __atomic_load_n(&cpu->stats_idle_ms, __ATOMIC_RELAXED);
}

struct thread *sched_get_thread_list_head(void) { return global_thread_list; }

void sched_reparent_children(struct thread *parent) {
  if (!parent)
    return;

  spinlock_acquire(&tid_lock);

  // Linux process children belong to the thread group for wait purposes.
  // Keep them with a live group member when a pthread exits.
  struct thread *adopter = NULL;
  if (parent->tgid != parent->tid) {
    for (struct thread *candidate = global_thread_list; candidate;
         candidate = candidate->global_next) {
      if (candidate != parent && candidate->tgid == parent->tgid &&
          candidate->state != THREAD_DEAD &&
          candidate->state != THREAD_ZOMBIE) {
        adopter = candidate;
        if (candidate->tid == parent->tgid)
          break;
      }
    }
  }

  /* Reparent within the active PID namespace. An orphan must never escape
   * to a process outside its visible ancestry while its namespace init lives. */
  if (!adopter && parent->pid_ns) {
    for (struct thread *candidate = global_thread_list; candidate; candidate = candidate->global_next)
      if (candidate != parent && pid_namespace_tid(parent, candidate) == 1 &&
          candidate->state != THREAD_DEAD && candidate->state != THREAD_ZOMBIE) {
        adopter = candidate; break;
      }
  }
  // Orphans from a terminating process go to the BSP idle task as init.
  if (!adopter) {
    adopter = global_thread_list;
    while (adopter && adopter->tid != 1)
      adopter = adopter->global_next;
  }

  struct thread *to_reap = NULL;
  /* pdeath_signal targets queued under tid_lock, woken after releasing it:
   * waking takes runqueue/wait-queue locks and must never run with tid_lock
   * held (sys_kill hung this way). Stack cap is ample: one entry per child. */
  struct thread *pdeath_notify[64];
  int pdeath_sig[64];
  uint32_t pdeath_count = 0;
  struct thread *child = parent->children;
  while (child) {
    struct thread *next_sibling = child->sibling_next;
    /* If the child has already terminated, its parent exited without reading
     * its exit status. When reparenting to init (or if adopter is not waiting),
     * reap the dead zombie immediately so orphans do not leak. */
    if (child->state == THREAD_ZOMBIE || child->state == THREAD_DEAD) {
      child->sibling_next = to_reap;
      to_reap = child;
      child->parent = NULL;
      child->state = THREAD_DEAD;
      child = next_sibling;
      continue;
    }
    if (child->pdeath_signal > 0 && child->pdeath_signal <= 64) {
      int psig = child->pdeath_signal;
      child->pending_signals |= (1ULL << (psig - 1));
      /* Sender unknown here; preserve SI_USER-style info. */
      if (pdeath_count < 64) {
        pdeath_notify[pdeath_count] = child;
        pdeath_sig[pdeath_count] = psig;
        pdeath_count++;
      }
    }
    child->parent = adopter;
    if (adopter) {
      child->sibling_next = adopter->children;
      adopter->children = child;
    } else {
      child->sibling_next = NULL;
    }
    child = next_sibling;
  }
  parent->children = NULL;
  spinlock_release(&tid_lock);

  for (uint32_t i = 0; i < pdeath_count; i++)
    signal_notify_thread(pdeath_notify[i], pdeath_sig[i]);

  while (to_reap) {
    struct thread *victim = to_reap;
    to_reap = to_reap->sibling_next;
    victim->sibling_next = NULL;
    sched_queue_reap(victim);
  }
}

int sched_get_mount_snapshot(uint32_t tid, vfs_mount_info_t *mounts, int count) {
  spinlock_acquire(&tid_lock);
  struct thread *target = find_thread_by_tid_locked(tid);
  int result = target && pid_namespace_tid(sched_get_current(), target)
                 ? vfs_get_mounts_task(target, mounts, count) : 0;
  spinlock_release(&tid_lock);
  return result;
}
