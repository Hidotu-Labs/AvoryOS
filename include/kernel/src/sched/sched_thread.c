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

extern void thread_stub(void); // Defined in switch.asm

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
  if (cpu->current_thread) {
    cpu->current_thread->state = THREAD_DEAD;
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

  // 1. Push the thread exit function (simulating a return address)
  stack_top -= 8;
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
      uint32_t eax = 0xFFFFFFFF, edx = 0xFFFFFFFF;
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
  snapshot->uid = t->uid;
  snapshot->gid = t->gid;
  snapshot->euid = t->euid;
  snapshot->egid = t->egid;
  snapshot->suid = t->suid;
  snapshot->sgid = t->sgid;
  memcpy(snapshot->comm, t->comm, sizeof(snapshot->comm));
  snapshot->comm[sizeof(snapshot->comm) - 1] = 0;

  if (t->mm) {
    if (t->mm->brk_current > t->mm->brk_base)
      snapshot->virt_bytes = t->mm->brk_current - t->mm->brk_base;
    uint64_t mmap_used = 0x800000000000ULL - t->mm->mmap_next_addr;
    if ((int64_t)mmap_used > 0)
      snapshot->virt_bytes += mmap_used;
    snapshot->resident_bytes = snapshot->virt_bytes / 2;
  }

  spinlock_release(&tid_lock);
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
  spinlock_acquire(&tid_lock);
  uint16_t count = 0;
  struct thread *curr = global_thread_list;
  while (curr) {
    count++;
    curr = curr->global_next;
  }
  spinlock_release(&tid_lock);
  return count;
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
