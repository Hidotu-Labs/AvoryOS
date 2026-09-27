#include "sched.h"
#include "sched_internal.h"
#include "hal/hal.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/procfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../cpu/features.h"
#include "../smp/cpu.h"

uint32_t next_tid = 1;
spinlock_t tid_lock = SPINLOCK_INIT;
struct thread *global_thread_list = NULL;
/* Atomic mirror of global_thread_list's length: +1 at the insert below and
 * at the idle-thread insert in sched.c, -1 at the unlink in sched_reap.c. */
uint32_t global_thread_count = 0;

extern void thread_stub(void); // Defined in switch.asm
extern void signal_send(struct thread *t, int sig);

void *thread_stack_alloc(void) {
  void *phys = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
  if (!phys)
    return NULL;
  return (void *)((uint64_t)phys + pmm_get_hhdm_offset());
}

void thread_stack_release(uint64_t stack_base) {
  if (!stack_base)
    return;
  void *phys = (void *)(stack_base - pmm_get_hhdm_offset());
  pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
}

static void thread_exit(void) {
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  struct thread *t = cpu ? cpu->current_thread : NULL;

  if (t) {
    /* Let the LinuxKPI bridge detach its task shadow from this thread while
     * the native struct is still valid (kthread_stop() may resolve the
     * shadow after the thread is gone). */
    extern void linuxkpi_thread_exiting(void *thread) __attribute__((weak));
    if (linuxkpi_thread_exiting)
      linuxkpi_thread_exiting(t);

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

  // Initialize FPU/XSAVE state
  memset(t->fpu_state, 0, sizeof(t->fpu_state));

  static uint8_t fpu_init_done = 0;
  static uint8_t initial_fpu_state[4096] __attribute__((aligned(64)));
  if (!fpu_init_done) {
    memset(initial_fpu_state, 0, sizeof(initial_fpu_state));
    if (cpu_has_xsave()) {
      uint64_t mask = __atomic_load_n(&cpu_xsave_mask, __ATOMIC_ACQUIRE);
      uint32_t eax = (uint32_t)mask, edx = (uint32_t)(mask >> 32);
      __asm__ volatile("fninit; xsave64 %0" : "=m"(initial_fpu_state) : "a"(eax), "d"(edx));
    } else {
      __asm__ volatile("fninit; fxsave64 %0" : "=m"(initial_fpu_state));
    }
    fpu_init_done = 1;
  }
  memcpy(t->fpu_state, initial_fpu_state, sizeof(t->fpu_state));

  /* Publish only after all allocations and context initialization succeed. */
  spinlock_acquire(&tid_lock);
  t->tid = next_tid++;
  t->tgid = t->tid;
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
  snapshot->uid = t->uid;
  snapshot->gid = t->gid;
  snapshot->euid = t->euid;
  snapshot->egid = t->egid;
  snapshot->suid = t->suid;
  snapshot->sgid = t->sgid;
  snapshot->cpu_index = t->cpu_index;
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

  // Orphans from a terminating process go to the BSP idle task as init.
  if (!adopter) {
    adopter = global_thread_list;
    while (adopter && adopter->tid != 1)
      adopter = adopter->global_next;
  }

  struct thread *child = parent->children;
  while (child) {
    struct thread *next_sibling = child->sibling_next;
    if (child->pdeath_signal > 0 && child->pdeath_signal <= 64) {
      signal_send(child, child->pdeath_signal);
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
}
