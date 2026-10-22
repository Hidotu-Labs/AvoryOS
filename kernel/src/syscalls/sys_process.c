#include "../sched/capability.h"
// Process Syscalls: fork, getpid, exit
#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/fpu.h"
#include "../cpu/msr.h"
#include "../drivers/storage/nvme.h"
#include "../drivers/net/e1000.h"
#include "../drivers/timer/rtc.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"

#include "../sched/sched.h"
#include "../sched/namespace.h"
#include "../sched/pid_namespace.h"
#include "../sched/seccomp.h"
#include "../sched/net_namespace.h"
#include "../fs/mount_namespace.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "sysdiag.h"
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"
#include <stdint.h>
#include <stddef.h>
#ifdef LINUXKPI_LINUX_IMPORTED
#include <linuxkpi/native_pci.h>
#endif

// Wake waiters after CLONE_CHILD_CLEARTID/set_tid_address exit cleanup.
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count);

extern void mm_reset_mmap_state(struct thread *t);
void vma_list_init(struct vma_list *list);
void vma_list_destroy(struct vma_list *list);
static uint8_t nice_to_sched_priority(int nice);

#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define MMAP_REGION_BASE 0x7F0000000000ULL

static void process_mount_cleanup(struct mount_namespace **ns) { vfs_namespace_put(*ns); }
static int process_namespace_inherit(struct thread *child, struct thread *parent,
                                      struct mount_namespace *mount,
                                      struct pid_namespace *pid, uint64_t flags) {
  int err = pid_namespace_inherit(child, parent, pid, (flags & CLONE_THREAD) != 0);
  if (err) return err;
  net_namespace_inherit(child, parent);
  vfs_namespace_clone(child, parent, false);
  if (mount) vfs_namespace_install(child, mount);
  if (child->pid_ns && pid_namespace_tid(child, child) == 1)
    child->pgid = child->sid = child->tid;
  return 0;
}

/* Called before publishing a new userspace child, for fork/clone/vfork alike. */
static void process_inherit_scheduler(struct thread *child,
                                       const struct thread *parent) {
  int nice = parent->nice_value;
  uint8_t policy = parent->sched_policy;
  uint8_t rt_priority = parent->sched_rt_priority;
  if (parent->sched_reset_on_fork) {
    if (policy == 1 || policy == 2) {
      policy = 0;
      rt_priority = 0;
    }
    if (nice < 0)
      nice = 0;
  }
  child->priority = nice_to_sched_priority(nice);
  child->static_priority = child->priority;
  child->nice_value = (int8_t)nice;
  child->sched_policy = policy;
  child->sched_rt_priority = rt_priority;
  child->sched_reset_on_fork = false;
  child->se.custom_slice = false;
  eevfd_set_nice(&child->se, nice);
  if (parent->se.custom_slice && policy == parent->sched_policy)
    eevfd_set_slice(&child->se, parent->se.slice_ns);
}

// Path Normalization
static void path_normalize(const char *base, const char *rel, char *out) {
  char temp[512] = {0};

  // If relative path starts with '/', it replaces base entirely
  if (rel[0] == '/') {
    strcpy(temp, rel);
  } else {
    strcpy(temp, base);
    if (temp[strlen(temp) - 1] != '/')
      strcat(temp, "/");
    strcat(temp, rel);
  }

  // Collapse elements
  char *collapsed[128];
  int count = 0;

  char *p = temp;
  while (*p) {
    while (*p == '/')
      *p++ = '\0';
    if (!*p)
      break;

    char *elem = p;
    while (*p && *p != '/') {
      p++;
    }
    if (*p == '/') {
      *p = '\0';
      p++;
    }

    if (strcmp(elem, ".") == 0) {
      continue;
    } else if (strcmp(elem, "..") == 0) {
      if (count > 0)
        count--;
    } else {
      if (count < 128) {
        collapsed[count++] = elem;
      }
    }
  }

  // Rebuild out path
  out[0] = '\0';
  for (int i = 0; i < count; i++) {
    strcat(out, "/");
    strcat(out, collapsed[i]);
  }
  if (count == 0)
    strcpy(out, "/");
}

// Used to restart bash session on exit
extern void restart_main_session(void) __attribute__((noreturn));

// Assembly trampoline that restores user registers and sysrets to Ring 3
extern void fork_return_to_userspace(struct syscall_regs *regs)
    __attribute__((noreturn));

// MSR for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;

/* Queues the child's exit signal on its parent, as Linux's do_notify_parent().
 * Implemented in sys_signal.c. */
extern void signal_notify_parent_exit(struct thread *child);
extern void signal_notify_thread(struct thread *t, int sig);
extern uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count);
static void robust_handle_death(struct thread *t);

struct pidfd_ctx { uint32_t pid; };

/* Waiters that care about "some process changed state": pollers on a pidfd and
 * blocked waitid(P_PIDFD) callers.  The parent-only wake in process_do_exit()
 * cannot reach them - a pidfd may name any process in the system, and a parent
 * cloned with CLONE_VFORK is still inside sys_clone() when its child exits, so
 * it is not yet registered as a waiter.  One shared queue is enough because
 * every waiter re-evaluates the thread list after being woken.
 *
 * Without this, QProcess::waitForFinished() (which Qt implements as
 * waitid(P_PIDFD, ...) plus poll() on the same descriptor) can park forever.
 * Note that this only covers Qt's pidfd path: QProcess with a
 * childProcessModifier - every kpty spawn - uses the SIGCHLD handler instead,
 * which is reported by signal_notify_parent_exit(). */
static wait_queue_t pidfd_event_wait = { .lock = SPINLOCK_INIT, .head = NULL };

void pidfd_wake_waiters(void) { wait_queue_wake_all(&pidfd_event_wait); }

static bool pidfd_task_state(uint32_t pid, bool *exited) {
  bool found = false;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tid == pid && t->tgid == t->tid) {
      found = true;
      *exited = t->state == THREAD_ZOMBIE || t->state == THREAD_DEAD;
      break;
    }
  }
  spinlock_release(&tid_lock);
  return found;
}

static int pidfd_poll(vfs_node_t *node, int events) {
  struct pidfd_ctx *ctx = node ? node->device : NULL;
  bool exited = true;
  if (!ctx) return -1;
  if (!pidfd_task_state(ctx->pid, &exited)) exited = true;
  /* A terminated pidfd is both readable and priority-readable, exactly like
   * Linux.  Callers differ in which one they ask for: Qt's async watcher polls
   * POLLIN while its blocking forkfd_wait path polls POLLPRI, and a poller
   * asking only for POLLPRI would never be satisfied. */
  return exited ? (events & (POLLIN | POLLPRI | POLLRDNORM)) : 0;
}

static void pidfd_close(vfs_node_t *node) {
  if (node && node->device) {
    kfree(node->device);
    node->device = NULL;
  }
}

/* Linux's extensible PIDFD_GET_INFO ABI. Only advertise fields that this
 * kernel can supply; namespace/cgroup and retained exit data are unsupported. */
struct avory_pidfd_info {
  uint64_t mask, cgroupid;
  uint32_t pid, tgid, ppid;
  uint32_t ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
  int32_t exit_code;
  uint32_t coredump_mask, coredump_signal, coredump_code, coredump_pad;
  uint64_t supported_mask;
};
_Static_assert(sizeof(struct avory_pidfd_info) == 88, "pidfd_info ABI");

static int pidfd_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  if ((request & 0xc000ffffU) != 0xc000ff0bU) return -25;
  unsigned size = (request >> 16) & 0x3fff;
  if (size < 64 || !arg) return -22;
  uint64_t requested;
  if (copy_from_user(&requested, (void *)arg, sizeof(requested))) return -14;
  struct pidfd_ctx *ctx = node ? node->device : NULL;
  if (!ctx) return -9;
  struct avory_pidfd_info info = {0};
  spinlock_acquire(&tid_lock);
  for (struct thread *task = global_thread_list; task; task = task->global_next) {
    if (task->tid != ctx->pid || task->tgid != task->tid) continue;
    info.mask = 3; /* PIDFD_INFO_PID | PIDFD_INFO_CREDS */
    info.pid = task->tid; info.tgid = task->tgid;
    /* Match getppid's visible PID-1 convention for the initial session. */
    info.ppid = task->is_main_session || !task->parent ? 0 :
                (task->parent->is_main_session ? 1 : task->parent->tgid);
    info.ruid = task->uid; info.rgid = task->gid;
    info.euid = task->euid; info.egid = task->egid;
    info.suid = task->suid; info.sgid = task->sgid;
    info.fsuid = task->fsuid; info.fsgid = task->fsgid;
    break;
  }
  spinlock_release(&tid_lock);
  if (!info.mask) return -3;
  if ((requested & (1ULL << 5)) && size >= sizeof(info)) {
    info.mask |= 1ULL << 5;
    info.supported_mask = 3 | (1ULL << 5);
  }
  unsigned count = size < sizeof(info) ? size : sizeof(info);
  if (copy_to_user((void *)arg, &info, count)) return -14;
  if (size > count && clear_user((void *)(arg + count), size - count)) return -14;
  return 0;
}

/* A pidfd is recognised by its poll handler.  Without this check any descriptor
 * whose node happens to carry a ->device (a pty, a drm client, ...) would be
 * read as a struct pidfd_ctx by waitid(P_PIDFD), turning a stray file descriptor
 * into a wait on an arbitrary process. */
static struct pidfd_ctx *pidfd_ctx_of(vfs_node_t *node) {
  if (!fd_is_valid_node(node) || node->poll != pidfd_poll || !node->device)
    return NULL;
  return (struct pidfd_ctx *)node->device;
}

static void pidfd_abort(struct thread *t, int fd, vfs_node_t *node);

/* Reserves a descriptor number and builds its pidfd node, without publishing
 * either: clone(CLONE_PIDFD) has to reserve before the child exists (no
 * allocation may fail after the child was created) and publish only after the
 * child copied the parent's descriptor table (the pidfd must not leak into the
 * child).  While reserved the descriptor is unreachable from user space. */
static int64_t pidfd_reserve(struct thread *t, int *out_fd, vfs_node_t **out_node) {
  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE
  struct pidfd_ctx *ctx = kmalloc(sizeof(*ctx));
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!ctx || !node) {
    if (ctx) kfree(ctx);
    if (node) kfree(node);
    pidfd_abort(t, fd, NULL);
    return (uint64_t)-12; // ENOMEM
  }
  ctx->pid = 0;
  vfs_node_init(node);
  strcpy(node->name, "pidfd");
  node->flags = FS_CHARDEV;
  node->mask = 0600;
  node->device = ctx;
  node->poll = pidfd_poll;
  node->ioctl = pidfd_ioctl;
  node->close = pidfd_close;
  /* So that poll()/ppoll() on the descriptor is woken by an exit instead of
   * only noticing it the next time the poll loop happens to re-scan. */
  node->wait_queue = &pidfd_event_wait;
  *out_fd = fd;
  *out_node = node;
  return 0;
}

/* Gives the reserved descriptor the pid it names and installs it. */
static void pidfd_publish(struct thread *t, int fd, vfs_node_t *node,
                          uint32_t pid, uint64_t flags) {
  const uint64_t PIDFD_NONBLOCK = 0x800;
  struct pidfd_ctx *ctx = (struct pidfd_ctx *)node->device;
  ctx->pid = pid;
  spinlock_acquire(&t->files->lock);
  t->fds[fd] = node;
  spinlock_release(&t->files->lock);
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = FD_FLAGS_CLOEXEC_BIT |
                    (flags & PIDFD_NONBLOCK ? O_NONBLOCK : 0);
}

/* Undoes a reservation that was never published. */
static void pidfd_abort(struct thread *t, int fd, vfs_node_t *node) {
  if (node) {
    if (node->device)
      kfree(node->device);
    kfree(node);
  }
  spinlock_acquire(&t->files->lock);
  t->fds[fd] = NULL;
  spinlock_release(&t->files->lock);
  if ((uint32_t)fd < t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
}

static int64_t pidfd_install(struct thread *t, uint32_t pid, uint64_t flags) {
  int fd = -1;
  vfs_node_t *node = NULL;
  int64_t r = pidfd_reserve(t, &fd, &node);
  if (r < 0)
    return r;
  pidfd_publish(t, fd, node, pid, flags);
  return (uint64_t)fd;
}

static uint64_t sys_pidfd_open(uint64_t pid, uint64_t flags, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  const uint64_t PIDFD_NONBLOCK = 0x800;
  if (pid == 0 || pid > UINT32_MAX || (flags & ~PIDFD_NONBLOCK))
    return (uint64_t)-22;
  pid = pid_namespace_global(sched_get_current(), (uint32_t)pid);
  bool exited = false;
  if (!pid || !pidfd_task_state((uint32_t)pid, &exited)) return (uint64_t)-3;
  return pidfd_install(sched_get_current(), (uint32_t)pid, flags);
}

static uint64_t sys_pidfd_send_signal(uint64_t pidfd, uint64_t sig,
                                      uint64_t info_ptr, uint64_t flags,
                                      uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (flags != 0)
    return (uint64_t)-22; // EINVAL: only 0 supported
  if (sig > 64)
    return (uint64_t)-22;
  if (info_ptr && !vmm_is_user_addr_range_valid(info_ptr, 128))
    return (uint64_t)-14; // EFAULT
  struct thread *current = sched_get_current();
  if (!FD_IN_RANGE(current, pidfd))
    return (uint64_t)-9; // EBADF
  vfs_node_t *node = current->fds[pidfd];
  struct pidfd_ctx *ctx = pidfd_ctx_of(node);
  if (!ctx)
    return (uint64_t)-9;
  uint32_t pid = ctx->pid;
  if (sig == 0)
    return pidfd_task_state(pid, &(bool){0}) ? 0 : (uint64_t)-3;
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *target = NULL;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid == pid || t->tid == pid) {
      target = t;
      break;
    }
  }
  if (!target) {
    spinlock_release(&tid_lock);
    return (uint64_t)-3; // ESRCH
  }
  if (!capability_has(current, CAP_KILL) && current->uid != target->uid &&
      current->euid != target->uid) {
    spinlock_release(&tid_lock);
    return (uint64_t)-1; // EPERM
  }
  if (!pid_namespace_signal_allowed(current, target, (int)sig)) {
    spinlock_release(&tid_lock);
    return 0;
  }
  uint32_t sender_pid = current->tgid ? current->tgid : current->tid;
  target->pending_signals |= (1ULL << (sig - 1));
  target->signal_sender_pid[sig - 1] = sender_pid;
  target->signal_sender_code[sig - 1] = 0; /* SI_USER */
  spinlock_release(&tid_lock);
  signal_notify_thread(target, (int)sig);
  return 0;
}

static uint64_t sys_pidfd_getfd(uint64_t pidfd, uint64_t targetfd,
                                 uint64_t flags, uint64_t a3, uint64_t a4,
                                 uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (flags != 0)
    return (uint64_t)-22;
  struct thread *current = sched_get_current();
  if (!FD_IN_RANGE(current, pidfd))
    return (uint64_t)-9;
  vfs_node_t *pnode = current->fds[pidfd];
  struct pidfd_ctx *ctx = pidfd_ctx_of(pnode);
  if (!ctx)
    return (uint64_t)-9;
  uint32_t pid = ctx->pid;
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *target = NULL;
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t->tgid == pid || t->tid == pid) {
      target = t;
      break;
    }
  }
  if (!target) {
    spinlock_release(&tid_lock);
    return (uint64_t)-3;
  }
  if (!capability_has(current, CAP_SYS_PTRACE) &&
      (current->euid != target->uid || current->euid != target->euid ||
       current->euid != target->suid || current->egid != target->gid ||
       current->egid != target->egid || current->egid != target->sgid ||
       (target->cap_permitted & ~current->cap_permitted))) {
    spinlock_release(&tid_lock);
    return (uint64_t)-1;
  }
  if (!target->files || !target->fds || !FD_IN_RANGE(target, targetfd)) {
    spinlock_release(&tid_lock);
    return (uint64_t)-9;
  }
  spinlock_acquire(&target->files->lock);
  vfs_node_t *node = target->fds[targetfd];
  if (!node || node == FD_RESERVED) {
    spinlock_release(&target->files->lock);
    spinlock_release(&tid_lock);
    return (uint64_t)-9;
  }
  uint64_t description_id = fd_description_id_locked(target, (int)targetfd);
  uint64_t offset = target->fd_offsets[targetfd];
  uint64_t status = target->fd_flags[targetfd] | FD_FLAGS_CLOEXEC_BIT;
  vfs_node_ref(node);
  spinlock_release(&target->files->lock);
  spinlock_release(&tid_lock);
  vfs_open(node);
  vfs_close(node); // release temporary pin
  int new_fd = alloc_fd(current);
  if (new_fd < 0) {
    vfs_close(node);
    return (uint64_t)-24; // EMFILE
  }
  spinlock_acquire(&current->files->lock);
  current->fd_offsets[new_fd] = offset;
  current->fd_flags[new_fd] = status;
  fd_description_set_locked(current, new_fd, description_id);
  current->fds[new_fd] = node;
  spinlock_release(&current->files->lock);
  return (uint64_t)new_fd;
}

// exit / exit_group (shared)
void process_do_exit(uint64_t status) __attribute__((noreturn));

/* Real-vfork rendezvous release (see struct thread::vfork_done). The child's
 * exec success and _exit paths call this; sched_reap_thread() calls it as a
 * backstop so a child killed any other way (SIGKILL via terminate) cannot
 * leave the parent blocked forever. The flag is set before the wakeup and the
 * child's VFORK mark cleared, so repeats and races are harmless. */
void vfork_release_parent(struct thread *child) {
  if (!child || !(child->clone_flags & CLONE_VFORK))
    return;
  struct thread *parent = child->parent;
  if (parent) {
    /* Pair completion with the waiter's check-and-block under its runqueue
     * lock. Publishing the flag and inspecting state without that handshake
     * can miss a parent that is just about to sleep. Started threads remain
     * on their owning CPU (sched_wakeup uses the same queue_lock). */
    struct cpu_info *cpu = cpu_get_info(parent->cpu_index);
    spinlock_acquire(&cpu->queue_lock);
    parent->vfork_release_tsc = rdtsc_fence();
    __atomic_store_n(&parent->vfork_done, true, __ATOMIC_RELEASE);
    bool wake = parent->state == THREAD_BLOCKED;
    spinlock_release(&cpu->queue_lock);
    /* sched_wakeup takes queue_lock itself; never call it while holding it. */
    if (wake) {
      sched_wakeup(parent);
      /* The same-CPU child should give the waiting launcher a scheduling
       * opportunity at syscall exit, even when normal wakeup fairness would
       * let the child keep its slice. No context switch while holding locks. */
      if (cpu == cpu_get_current() && cpu->current_thread == child)
        __atomic_store_n(&child->need_resched, true, __ATOMIC_RELEASE);
    }
  }
  child->clone_flags &= ~CLONE_VFORK;
}

static void vfork_wait_for_child(struct thread *parent) {
  TSC_BEGIN(vfork_wait);
  uint64_t released_at = 0;
  for (;;) {
    struct cpu_info *cpu = cpu_get_current();
    spinlock_acquire(&cpu->queue_lock);
    if (__atomic_load_n(&parent->vfork_done, __ATOMIC_ACQUIRE)) {
      released_at = parent->vfork_release_tsc;
      spinlock_release(&cpu->queue_lock);
      break;
    }
    parent->state = THREAD_BLOCKED;
    spinlock_release(&cpu->queue_lock);
    /* A completion after unlock makes us runnable before yield, or wakes us
     * after the switch. Spurious signal wakeups simply repeat the handshake. */
    sched_yield();
  }
  parent->vfork_child = 0;
  uint64_t resumed_at = rdtsc_fence();
  TSC_END(vfork_wait);
  /* Separate scheduling/wakeup latency from time spent waiting for the
   * child to finish. Clamp defensively if cross-CPU TSCs are not aligned. */
  if (released_at && resumed_at >= released_at) {
    int slot = tsc_probe_register("vfork_resume");
    tsc_probe_record(slot, resumed_at - released_at);
  }

  /* Borrowing children skip their exec dump; show completed rendezvous
   * samples from the parent. This is elapsed time, not CPU time. */
  static uint32_t waits;
  uint32_t n = __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
  if (n <= 4 || n == 16 || n == 64)
    tsc_probe_dump();
}
static void process_clear_child_tid(struct thread *current) {
  uint32_t *tidptr = (uint32_t *)current->tid_address;
  current->tid_address = NULL;
  if (!tidptr)
    return;
  uint32_t zero = 0;
  /* Exit also runs from a user exception, where the syscall's coarse SMAP
   * window is absent. The address may have been unmapped since registration;
   * a guarded user copy handles both that race and demand/CoW faults. */
  if (vmm_is_user_addr_range_writable((uint64_t)tidptr, sizeof(zero)) &&
      copy_to_user(tidptr, &zero, sizeof(zero)) == 0)
    futex_wake_user(tidptr, 1);
}

void process_do_exit(uint64_t status) {
  /* Fatal user exceptions enter with IF clear. Cleanup may wait for I/O or
   * IPIs and must run with the same interrupt state as ordinary sys_exit. */
  hal_irq_enable();
  struct thread *current = sched_get_current();
  if (!current) {
    while (1)
      sched_yield();
  }
  if (current->state == THREAD_DEAD || current->state == THREAD_ZOMBIE) {
    while (1) {
      sched_yield();
    }
  }
  // Robust mutexes held by this thread must be marked OWNER_DIED and woken
  // while the address space is still live; otherwise the next locker spins
  // into musl's a_crash (HLT) on impossible owner state.
  if (current && current->robust_list)
    robust_handle_death(current);
  process_clear_child_tid(current);

  if (current) {
    pid_namespace_exit(current);
    sched_reparent_children(current);
  }

  if (current && !current->is_main_session) {
    klog_debug_puts("\n[SYSCALL] Process exited with status: ");
    klog_debug_uint64(status);
    klog_debug_puts("\n");
  }

  klog_debug_puts("[EXITDBG] begin cleanup\n");

  // Common cleanup. CLONE_FILES tables remain alive until the final thread
  // drops its reference; closing every fd on each pthread exit would break
  // descriptors still in use by its siblings.
  TSC_BEGIN(exit_files);
  if (current) {
    sched_release_files(current);
    klog_debug_puts("[EXITDBG] files released\n");

    if (current->cwd_node) {
      vfs_close(current->cwd_node);
      current->cwd_node = NULL;
    }
    klog_debug_puts("[EXITDBG] cwd released\n");
  }
  TSC_END(exit_files);

  if (current && current->is_forked_child) {
    TSC_BEGIN(exit_publish);
    current->exit_status = (int)status;

    struct thread *parent_to_wake = NULL;

    // Thread-group members are not waitable children. Keeping them as
    // zombies retains their large struct thread and kernel stack forever.
    // Arrange for the scheduler to reclaim them after this context switches
    // away; ordinary fork/clone processes remain zombies for wait4().
    if (current->clone_flags & CLONE_THREAD) {
      spinlock_acquire(&tid_lock);
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t != current && t->tgid == current->tgid && t->tid == current->tgid) {
          t->exit_status = (int)status;
          break;
        }
      }
      spinlock_release(&tid_lock);
      current->state = THREAD_DEAD;
      pidfd_wake_waiters();
      sched_queue_reap(current);
    } else {
      /* Serialize zombie publication with wait4's transition to BLOCKED. */
      klog_debug_puts("[EXITDBG] publishing zombie\n");
      spinlock_acquire(&tid_lock);
      struct thread *parent = current->parent;
      int exit_signal = (int)(current->clone_flags & 0xff);
      bool auto_reap = false;
      if (parent && exit_signal == SIGCHLD) {
        struct k_sigaction *chld_action =
            &parent->signal_handlers[SIGCHLD - 1];
        auto_reap = chld_action->sa_handler == (void *)SIG_IGN ||
                    (chld_action->sa_flags & SA_NOCLDWAIT) != 0;
        /* Keep default-disposition children waitable, including when the
         * parent is a GUI/daemon using pidfds, signalfd, or a later waitpid.
         * Process names and whether a wait syscall is currently blocked do
         * not establish that the parent has abandoned the exit status.
         * Only retain the existing PID-1 fallback for orphan collection. */
        bool parent_group_waiting = false;
        if (parent->waiting_for_child) {
          parent_group_waiting = true;
        } else {
          for (struct thread *w = global_thread_list; w; w = w->global_next) {
            if (w->tgid == parent->tgid && w->waiting_for_child) {
              parent_group_waiting = true;
              break;
            }
          }
        }
        if (!auto_reap &&
            (chld_action->sa_handler == (void *)SIG_DFL || chld_action->sa_handler == NULL) &&
            !parent_group_waiting) {
          if (parent->tgid == 1 || parent->tid == 1) {
            auto_reap = true;
          }
        }
      }
      current->state = auto_reap ? THREAD_DEAD : THREAD_ZOMBIE;
      if (current->parent) {
        uint32_t parent_tgid = current->parent->tgid;
        /* O(1) fast path: the single-threaded parent waiting in wait4() is
         * itself blocked with the flag set.  Checking it first avoids a full
         * global-list walk per exit (wake-one order was already arbitrary).
         * Anything else falls back to the scan below. */
        if (current->parent->tgid == parent_tgid &&
            current->parent->waiting_for_child &&
            current->parent->state == THREAD_BLOCKED) {
          parent_to_wake = current->parent;
        } else {
          for (struct thread *waiter = global_thread_list; waiter;
               waiter = waiter->global_next) {
            if (waiter->tgid == parent_tgid && waiter->waiting_for_child &&
                waiter->state == THREAD_BLOCKED) {
              parent_to_wake = waiter;
              break;
            }
          }
        }
      }
      if (auto_reap && parent) {
        /* No waitable child should remain when SIGCHLD is explicitly ignored
         * or SA_NOCLDWAIT is set. Unlink it before waking waiters so their
         * rescan observes ECHILD instead of parking on a zombie that will be
         * removed asynchronously by the reaper. Keep parent until notification
         * completes; the reaper clears the back-pointer before freeing. */
        if (parent->children == current) {
          parent->children = current->sibling_next;
        } else {
          struct thread *sibling = parent->children;
          while (sibling && sibling->sibling_next != current)
            sibling = sibling->sibling_next;
          if (sibling)
            sibling->sibling_next = current->sibling_next;
        }
        current->sibling_next = NULL;
      }
      spinlock_release(&tid_lock);
      klog_debug_puts("[EXITDBG] exit state published\n");
      /* Anyone waiting on a pidfd for this process (poll or waitid(P_PIDFD))
       * is woken here to observe its terminal state. */
      pidfd_wake_waiters();
      /* ... and the parent's SIGCHLD handler runs here, the way Linux sends
       * do_notify_parent() right after publishing the zombie. */
      signal_notify_parent_exit(current);
      if (auto_reap)
        sched_queue_reap(current);
    }

    /* A vfork parent is blocked in sys_vfork(), before it can enter wait4().
     * Publish the zombie before releasing it so an immediate waitpid() can
     * observe and reap this child. */
    vfork_release_parent(current);

    if (parent_to_wake) {
      klog_debug_puts("[EXITDBG] waking parent\n");
      sched_wakeup(parent_to_wake);
      klog_debug_puts("[EXITDBG] parent wake returned\n");
    }

    // Reaping handles the address-space reference after this task is off-CPU.
    TSC_END(exit_publish);
    if (current->cr3) {
      struct cpu_info *cpu = cpu_get_current();
      cpu_set_active_cr3(cpu->kernel_cr3);
      __asm__ volatile("mov %0, %%cr3" :: "r"(cpu->kernel_cr3) : "memory");
    }

    // Sleep forever; the parent will reap us.
    klog_debug_puts("[EXITDBG] yielding zombie\n");
    while (1) {
      sched_yield();
    }
  }

  // Non-forked (main) process exit
  extern void restart_main_session(void);
  restart_main_session();
  while (1)
    ;
}

static uint64_t __attribute__((noreturn)) sys_exit(uint64_t status, uint64_t a1,
                                                   uint64_t a2, uint64_t a3,
                                                   uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit((int)status);
}

static uint64_t __attribute__((noreturn))
sys_exit_group(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3,
               uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_terminate_thread_group(sched_get_current());
  process_do_exit(status);
}

static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return 0;
  if (tidptr && !vmm_is_user_addr_range_writable(tidptr, sizeof(uint32_t)))
    return (uint64_t)-14;

  // Store the pointer to the user-space TID variable
  // This is used by musl/glibc for thread exit notification
  current->tid_address = (uint64_t *)tidptr;

  // Return the current thread's ID
  return pid_namespace_tid(current, current);
}

static uint64_t sys_gettid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current && current->is_main_session)
    return 1;
  return current ? pid_namespace_tid(current, current) : 0;
}

static uint64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current) {
    if (current->is_main_session)
      return 1;
    return pid_namespace_visible(current, current->tgid);
  }
  return 0;
}
// sys_getcwd
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t size, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  char *buf = (char *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  size_t len = strlen(current->cwd_path) + 1;
  if (size < len)
    return (uint64_t)-34; // ERANGE

  memcpy(buf, current->cwd_path, len);
  return len; // Linux getcwd syscall returns length of copied bytes
}

// sys_chdir
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  char new_path[256];
  path_normalize(current->cwd_path, path, new_path);

  // Validate the directory exists!
  // Note: we want an absolute lookup here
  vfs_node_t *node = vfs_resolve_path_at(current->cwd_node, path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
    vfs_close(node);
    return (uint64_t)-20; // ENOTDIR
  }
  if (!vfs_access(node, 1)) {
    vfs_close(node);
    return (uint64_t)-13;
  }

  // If validation passes, update thread
  if (current->cwd_node)
    vfs_close(current->cwd_node);
  current->cwd_node = node; // vfs_resolve_path_at already opened it

  strncpy(current->cwd_path, new_path, 255);
  current->cwd_path[255] = '\0';

  return 0; // Success
}
extern spinlock_t tid_lock;

#define WNOHANG 1
#define __WNOTHREAD 0x20000000

static uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_ptr, uint64_t options,
                          uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)rusage;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int target_pid = (int)pid;

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;
    struct thread *zombie_owner = NULL;
    bool should_block = false;

    spinlock_acquire(&tid_lock);
    /* Clear the flag now that we hold the lock and are about to rescan.
     * This prevents a spurious re-block if a wakeup arrived between our
     * last sched_yield() return and this spinlock_acquire(). */
    current->waiting_for_child = false;
    struct thread *owner = (options & __WNOTHREAD) ? current : global_thread_list;
    while (owner) {
      if (owner->tgid != current->tgid) {
        owner = owner->global_next;
        continue;
      }

      struct thread *t = owner->children;
      while (t) {
        bool matches = false;
        if (target_pid == -1) {
          matches = true;
        } else if (target_pid > 0) {
          if (pid_namespace_tid(current, t) == (uint32_t)target_pid)
            matches = true;
        } else if (target_pid == 0) {
          if (t->pgid == current->pgid)
            matches = true;
        } else { // target_pid < -1
          if (pid_namespace_visible_locked(current, t->pgid) == (uint32_t)(-target_pid))
            matches = true;
        }

        if (matches) {
          has_matching_children = true;
          if (t->state == THREAD_ZOMBIE) {
            zombie = t;
            zombie_owner = owner;

            // Unlink from its owning thread list while holding the lock.
            if (zombie_owner->children == zombie) {
              zombie_owner->children = zombie->sibling_next;
            } else {
              struct thread *p = zombie_owner->children;
              while (p && p->sibling_next != zombie)
                p = p->sibling_next;
              if (p)
                p->sibling_next = zombie->sibling_next;
            }
            break;
          }
        }
        t = t->sibling_next;
      }

      if (zombie || (options & __WNOTHREAD))
        break;
      owner = owner->global_next;
    }
    /* Publish BLOCKED while child exit is excluded from publishing ZOMBIE.
     * Keep waiting_for_child set until we re-check under the lock so we
     * cannot miss a wakeup that fires between sched_yield() returning and
     * the next spinlock_acquire at the top of the loop. */
    if (!zombie && has_matching_children && !(options & WNOHANG)) {
      current->waiting_for_child = true;
      current->state = THREAD_BLOCKED;
      should_block = true;
    }
    spinlock_release(&tid_lock);

    if (zombie) {
      if (wstatus_ptr != 0) {
        int *wstatus = (int *)wstatus_ptr;
        *wstatus = zombie->termination_signal ? zombie->termination_signal :
                   (zombie->exit_status & 0xFF) << 8;
      }
      uint32_t reaped_pid = pid_namespace_tid(current, zombie);

      // Destruction must wait until the exiting child has switched off its
      // kernel stack.  On SMP the awakened parent can run concurrently with
      // process_do_exit(), so direct reaping here would free a live stack.
      TSC_BEGIN(wait4_reap);
      sched_queue_reap_and_wait(zombie);
      TSC_END(wait4_reap);

      current->waiting_for_child = false;
      return (uint64_t)reaped_pid;
    }

    if (!has_matching_children) {
      current->waiting_for_child = false;
      return (uint64_t)-10; // ECHILD
    }

    // WNOHANG: return 0 immediately if no zombie found
    if (options & WNOHANG) {
      current->waiting_for_child = false;
      return 0;
    }

    // Block and wait for a child to exit.
    // Note: waiting_for_child stays true across sched_yield() so that a
    // wakeup arriving while we are off-CPU is not lost.  It is cleared only
    // at the top of the next iteration once we hold tid_lock again and can
    // safely inspect the child list.
    if (should_block) {
      sched_yield();
      /* waiting_for_child is cleared at the top of the next loop iteration
       * after we reacquire tid_lock and rescan — do NOT clear it here. */
    }
  }
}

/* ── waitid (syscall 247) ──────────────────────────────────────────────────
 * int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options,
 * idtype values (POSIX / Linux):
 *   P_ALL   = 0  – wait for any child
 *   P_PID   = 1  – wait for specific pid
 *   P_PGID  = 2  – wait for any child in process group `id`
 *   P_PIDFD = 3  – wait for process referred to by pidfd
 *
 * We fill a minimal siginfo_t sufficient for glibc/musl to function.
 */
#define P_ALL   0
#define P_PID   1
#define P_PGID  2
#define P_PIDFD 3

#define WEXITED   4
#define WSTOPPED  2
#define WNOWAIT   0x01000000

/* siginfo_t layout – only the fields we need */
struct k_siginfo_child {
  int      si_signo;   /* always SIGCHLD = 17 */
  int      si_errno;
  int      si_code;    /* CLD_EXITED=1, CLD_KILLED=2 */
  int      _pad0;
  uint32_t si_pid;
  uint32_t si_uid;
  int      si_status;  /* exit code or signal */
  int      _pad1;
};
#define CLD_EXITED 1
#define CLD_KILLED 2

static uint64_t sys_waitid(uint64_t idtype_val, uint64_t id_val,
                           uint64_t infop_ptr, uint64_t options,
                           uint64_t rusage, uint64_t a5) {
  (void)rusage; (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int idtype = (int)idtype_val;
  uint32_t id = (uint32_t)id_val;
  uint32_t target_pid = id;

  if (idtype == P_PIDFD) {
    struct pidfd_ctx *pctx =
        (FD_IN_RANGE(current, id) && fd_is_valid_node(current->fds[id])) ? pidfd_ctx_of(current->fds[id]) : NULL;
    if (!pctx)
      return (uint64_t)-9; // EBADF
    target_pid = pctx->pid;
  }

  /* At least one of WEXITED/WSTOPPED must be requested */
  if (!(options & (WEXITED | WSTOPPED)))
    return (uint64_t)-22; // EINVAL

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;
    struct thread *zombie_owner = NULL;
    bool should_block = false;

    spinlock_acquire(&tid_lock);
    /* Clear here under the lock so a wakeup between yield-return and this
     * acquire is not missed — same pattern as sys_wait4. */
    current->waiting_for_child = false;

    /* Search every thread in this process's thread group for matching children */
    for (struct thread *owner = global_thread_list; owner;
         owner = owner->global_next) {
      if (owner->tgid != current->tgid)
        continue;

      for (struct thread *t = owner->children; t; t = t->sibling_next) {
        bool matches = false;
        switch (idtype) {
        case P_ALL:   matches = true; break;
        case P_PID:   matches = (pid_namespace_tid(current, t) == id); break;
        case P_PGID:  matches = (id ? pid_namespace_visible_locked(current, t->pgid) == id : t->pgid == current->pgid); break;
        case P_PIDFD: matches = (t->tid == target_pid || t->tgid == target_pid); break;
        default:
          spinlock_release(&tid_lock);
          return (uint64_t)-22; // EINVAL
        }

        if (!matches)
          continue;

        has_matching_children = true;

        if (t->state == THREAD_ZOMBIE) {
          zombie = t;
          zombie_owner = owner;

          /* Unlink from parent's children list while still holding tid_lock */
          if (!(options & WNOWAIT)) {
            if (zombie_owner->children == zombie) {
              zombie_owner->children = zombie->sibling_next;
            } else {
              struct thread *p = zombie_owner->children;
              while (p && p->sibling_next != zombie)
                p = p->sibling_next;
              if (p)
                p->sibling_next = zombie->sibling_next;
            }
          }
          break;
        }
      }
      if (zombie)
        break;
    }

    if (idtype == P_PIDFD && !has_matching_children) {
      /* P_PIDFD can wait on non-child processes */
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->tid == target_pid || t->tgid == target_pid) {
          has_matching_children = true;
          if (t->state == THREAD_ZOMBIE || t->state == THREAD_DEAD) {
            zombie = t;
            zombie_owner = t->parent;
          }
          break;
        }
      }
    }

    if (!zombie && has_matching_children && !(options & WNOHANG)) {
      current->waiting_for_child = true;
      current->state = THREAD_BLOCKED;
      should_block = true;
    }

    spinlock_release(&tid_lock);

    if (zombie) {
      /* Fill in the siginfo_t if the caller supplied a buffer */
      if (infop_ptr) {
        struct k_siginfo_child *si = (struct k_siginfo_child *)infop_ptr;
        si->si_signo  = 17; /* SIGCHLD */
        si->si_errno  = 0;
        si->si_code   = zombie->termination_signal ? 2 /* CLD_KILLED */ : CLD_EXITED;
        si->si_pid    = pid_namespace_tid(current, zombie);
        si->si_uid    = (uint32_t)zombie->uid;
        si->si_status = zombie->termination_signal ? zombie->termination_signal :
                        zombie->exit_status & 0xFF;
        si->_pad0     = 0;
        si->_pad1     = 0;
      }

      if (!(options & WNOWAIT) && zombie_owner && zombie_owner->tgid == current->tgid) {
        /* Consume the zombie – wait until child is truly off-CPU first */
        sched_queue_reap_and_wait(zombie);
      }

      current->waiting_for_child = false;
      return 0; /* waitid returns 0 on success, not the pid */
    }

    if (!has_matching_children) {
      current->waiting_for_child = false;
      return (uint64_t)-10; // ECHILD
    }

    if (options & WNOHANG) {
      /* No zombie yet; zero out siginfo_t per POSIX/Linux specification */
      if (infop_ptr) {
        struct k_siginfo_child *si = (struct k_siginfo_child *)infop_ptr;
        memset(si, 0, sizeof(*si));
      }
      current->waiting_for_child = false;
      return 0;
    }

    if (should_block) {
      /* A pidfd can name a process this one is not the parent of, so the
       * parent-only wake above cannot be relied on here: queue on the shared
       * pidfd wake-up, and keep a bounded fallback timer so a missed wake can
       * never park the caller forever - the loop re-evaluates every round. */
      wait_queue_entry_t pidfd_entry;
      pidfd_entry.thread = current;
      pidfd_entry.next = NULL;
      pidfd_entry.wq = NULL;
      pidfd_entry.thread_next = NULL;
      pidfd_entry.thread_prev = NULL;
      bool on_pidfd_wq = false;

      if (idtype == P_PIDFD) {
        current->state = THREAD_BLOCKED;
        wait_queue_add(&pidfd_event_wait, &pidfd_entry);
        on_pidfd_wq = true;
        current->wakeup_ticks = lapic_timer_get_ticks() + 10;
      }

      sched_yield();

      if (on_pidfd_wq) {
        wait_queue_remove(&pidfd_event_wait, &pidfd_entry);
        current->wakeup_ticks = 0;
        if (current->state == THREAD_BLOCKED)
          current->state = THREAD_RUNNING;
      }
      /* waiting_for_child stays set — cleared at top of next iteration
       * after we reacquire tid_lock, same as sys_wait4. */
    }
  }
}

// Close descriptors marked FD_CLOEXEC only after the replacement image has
// loaded successfully. Failed execve() must leave the descriptor table intact.
static void exec_close_cloexec(struct thread *t) {
  if (!t || !t->files)
    return;

  spinlock_acquire(&t->files->lock);
  uint32_t cap = t->files->capacity;
  spinlock_release(&t->files->lock);

  for (uint32_t fd = 0; fd < cap; fd++) {
    spinlock_acquire(&t->files->lock);
    if (fd >= t->files->capacity) {
      spinlock_release(&t->files->lock);
      break;
    }
    vfs_node_t *node = t->files->fds[fd];
    if (!fd_is_valid_node(node) ||
        !(t->files->fd_flags[fd] & FD_FLAGS_CLOEXEC_BIT)) {
      spinlock_release(&t->files->lock);
      continue;
    }

    t->files->fds[fd] = NULL;
    t->files->fd_offsets[fd] = 0;
    t->files->fd_flags[fd] = 0;
    fd_path_clear(t, fd);
    if ((uint32_t)fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    vfs_close(node);
  }
}

// sys_execve
#include "../lib/tsc.h"

/* Keep normal Wine launches quiet. Explicit WINEDEBUG settings still win. */
static bool exec_wine_debug_env(const char *path, char ***envp, int *envc) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  if (strcmp(name, "wine") && strcmp(name, "wine64") &&
      strcmp(name, "wine-preloader") && strcmp(name, "wine64-preloader"))
    return true;
  for (int i = 0; i < *envc; i++)
    if (strncmp((*envp)[i], "WINEDEBUG=", 10) == 0) return true;
  const char *setting = "WINEDEBUG=-all";
  size_t setting_size = strlen(setting) + 1;
  char **expanded = kmalloc(((size_t)*envc + 2) * sizeof(*expanded));
  if (!expanded) return false;
  char *value = kmalloc(setting_size);
  if (!value) { kfree(expanded); return false; }
  memcpy(value, setting, setting_size);
  memcpy(expanded, *envp, (size_t)*envc * sizeof(*expanded));
  expanded[*envc] = value;
  expanded[*envc + 1] = NULL;
  kfree(*envp);
  *envp = expanded;
  (*envc)++;
  return true;
}

static const char *exec_vector_entry(const char **vector, int index,
                                      bool compat32) {
  return compat32 ? (const char *)(uintptr_t)((const uint32_t *)vector)[index]
                  : vector[index];
}

static uint64_t sys_execve(struct syscall_regs *regs) {
  bool compat32 = regs->user_cs == 0x1b;
  const char **user_argv = (const char **)regs->rsi;
  const char **user_envp = (const char **)regs->rdx;
  const char *user_path = (const char *)regs->rdi;

  if (!user_path)
    return (uint64_t)-14; // EFAULT

  // Snapshot the pathname before inspecting self-exec aliases or switching CR3.
  char *path = kmalloc(4096);
  if (!path) return (uint64_t)-12;
  long path_len = strncpy_from_user(path, user_path, 4096);
  if (path_len < 0 || path_len >= 4096) {
    kfree(path);
    return path_len < 0 ? (uint64_t)-14 : (uint64_t)-36;
  }

  /* Recording the alias itself as exe_path would make /proc/self/exe point
   * back to itself after exec, breaking the next Chromium service launch. */
  struct thread *exec_caller = sched_get_current();
  if (!strcmp(path, "/proc/self/exe") ||
      !strcmp(path, "/proc/thread-self/exe")) {
    if (!exec_caller || !exec_caller->exe_path[0]) {
      kfree(path);
      return (uint64_t)-2; // ENOENT
    }
    strcpy(path, exec_caller->exe_path);
  }

  TSC_BEGIN(execve_arg_copy);
  int argc = 0;
  if (user_argv)
    while (exec_vector_entry(user_argv, argc, compat32))
      argc++;
  char **k_argv = kmalloc((argc + 1) * sizeof(char *));
  if (!k_argv) {
    kfree(path);
    return (uint64_t)-12;
  }
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(exec_vector_entry(user_argv, i, compat32));
    k_argv[i] = kmalloc(len + 1);
    memcpy(k_argv[i], exec_vector_entry(user_argv, i, compat32), len + 1);
  }
  k_argv[argc] = NULL;

  int envc = 0;
  if (user_envp)
    while (exec_vector_entry(user_envp, envc, compat32))
      envc++;
  char **k_envp = kmalloc((envc + 1) * sizeof(char *));
  if (!k_envp) {
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    return (uint64_t)-12;
  }
  for (int i = 0; i < envc; i++) {
    size_t len = strlen(exec_vector_entry(user_envp, i, compat32));
    k_envp[i] = kmalloc(len + 1);
    memcpy(k_envp[i], exec_vector_entry(user_envp, i, compat32), len + 1);
  }
  k_envp[envc] = NULL;
  if (!exec_wine_debug_env(path, &k_envp, &envc)) {
    kfree(path);
    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++) kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-12;
  }
  TSC_END(execve_arg_copy);

  // Shebang (#!) script interpretation loop (up to 4 levels of recursion)
  struct thread *exec_thread = sched_get_current();
  int shebang_depth = 0;
  uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;
  uint64_t exec_mount_flags = 0;

  while (shebang_depth < 4) {
    vfs_node_t *exec_base = (path[0] == '/') ? fs_root :
        (exec_thread && exec_thread->cwd_node ? exec_thread->cwd_node : fs_root);
    vfs_node_t *exec_node = vfs_resolve_path_at(exec_base, path);
    int exec_err = 0;
    if (!exec_node) {
      exec_err = 2; // ENOENT
    } else if (!vfs_access(exec_node, 1) ||
               (exec_node->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
      exec_err = 13; // EACCES, and Linux also refuses to exec a directory
    }
    if (exec_err) {
      if (exec_node) vfs_close(exec_node);
      klog_puts("[PROC] execve failed path=\"");
      klog_puts(path);
      klog_puts("\" errno=");
      klog_uint64((uint64_t)exec_err);
      klog_puts("\n");
      kfree(path);
      for (int i = 0; i < argc; i++) kfree(k_argv[i]);
      kfree(k_argv);
      for (int i = 0; i < envc; i++) kfree(k_envp[i]);
      kfree(k_envp);
      return (uint64_t)(-exec_err);
    }
    exec_mode = exec_node->mask;
    exec_mount_flags = vfs_mount_flags(exec_node);
    exec_uid = exec_node->uid;
    exec_gid = exec_node->gid;

    char header[256];
    uint32_t hlen = vfs_read(exec_node, 0, sizeof(header) - 1, (uint8_t *)header);
    vfs_close(exec_node);

    if (hlen >= 2 && header[0] == '#' && header[1] == '!') {
      header[hlen] = '\0';
      for (char *c = header; *c; c++) {
        if (*c == '\n' || *c == '\r') {
          *c = '\0';
          break;
        }
      }

      // Skip whitespace after '#!'
      char *p = header + 2;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0') {
        kfree(path);
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
        return (uint64_t)-8; // ENOEXEC
      }

      char *interp_bin = p;
      char *interp_arg = NULL;
      while (*p && *p != ' ' && *p != '\t') p++;
      if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\0') {
          interp_arg = p;
          char *end = p + strlen(p) - 1;
          while (end >= p && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end = '\0';
            end--;
          }
        }
      }

      int added = interp_arg ? 2 : 1;
      int new_argc = argc + added;
      char **new_k_argv = kmalloc((new_argc + 1) * sizeof(char *));
      if (!new_k_argv) {
        kfree(path);
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
        return (uint64_t)-12;
      }

      new_k_argv[0] = kmalloc(strlen(interp_bin) + 1);
      strcpy(new_k_argv[0], interp_bin);

      int dst_idx = 1;
      if (interp_arg) {
        new_k_argv[dst_idx] = kmalloc(strlen(interp_arg) + 1);
        strcpy(new_k_argv[dst_idx], interp_arg);
        dst_idx++;
      }
      new_k_argv[dst_idx] = path; // Reuse script path buffer
      dst_idx++;

      for (int i = 1; i < argc; i++) {
        new_k_argv[dst_idx++] = k_argv[i];
      }
      new_k_argv[new_argc] = NULL;

      kfree(k_argv[0]);
      kfree(k_argv);
      k_argv = new_k_argv;
      argc = new_argc;

      path = kmalloc(strlen(interp_bin) + 1);
      strcpy(path, interp_bin);

      shebang_depth++;
      continue;
    }

    // Binary executable verified
    break;
  }

  TSC_BEGIN(execve_pml4_create);
  uint64_t *new_pml4 = vmm_create_pml4();
  TSC_END(execve_pml4_create);
  if (!new_pml4) {
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-12;
  }

  // EARLY CR3 SWITCH:
  struct thread *current = sched_get_current();
  uint64_t old_cr3 = current->cr3;
  struct mm_struct *old_mm = current->mm;
  struct vma_list old_vmas;
  uint64_t old_brk_base = old_mm->brk_base;
  uint64_t old_brk_current = old_mm->brk_current;
  uint64_t old_mmap_next_addr = old_mm->mmap_next_addr;
  bool old_compat32 = old_mm->compat32;
  bool shared_mm = false;

  /* Pre-allocate the replacement address space.  Dropping the shared reference
   * before knowing that this allocation succeeds is what made the -ENOMEM path
   * below wrong: for a moment this thread held no reference at all to the mm it
   * was still running on, so a sibling exiting in that window could free it
   * under us. */
  struct mm_struct *new_mm = NULL;
  spinlock_acquire(&current->mm->lock);
  bool may_be_shared = (current->mm->ref_count > 1);
  spinlock_release(&current->mm->lock);
  if (may_be_shared) {
    new_mm = kmalloc(sizeof(struct mm_struct));
    if (!new_mm) {
      /* Nothing has been modified yet: free the execve scratch buffers and
       * report the failure, staying on the current address space. */
      kfree(path);
      for (int i = 0; i < argc; i++)
        kfree(k_argv[i]);
      kfree(k_argv);
      for (int i = 0; i < envc; i++)
        kfree(k_envp[i]);
      kfree(k_envp);
      return (uint64_t)-12;
    }
  }

  // Unshare mm_struct if shared (e.g. after vfork or in a thread)
  spinlock_acquire(&current->mm->lock);
  if (new_mm && current->mm->ref_count > 1) {
    shared_mm = true;
    current->mm->ref_count--;
    spinlock_release(&current->mm->lock);

    vma_list_init(&new_mm->vmas);
    new_mm->ref_count = 1;
    new_mm->pcid = pcid_alloc();
    new_mm->brk_base = 0;
    new_mm->brk_current = 0;
    new_mm->mmap_next_addr = MMAP_REGION_BASE;
    spinlock_init(&new_mm->lock);
    current->mm = new_mm;

    vma_list_init(&old_vmas);
  } else {
    spinlock_release(&current->mm->lock);
    /* Every other owner is gone by now, so the pre-allocated mm is not needed. */
    if (new_mm)
      kfree(new_mm);
    // Save the old VMA list before destroying it — we need it to
    // identify MAP_SHARED pages when freeing the old address space.
    old_vmas = current->mm->vmas;
    vma_list_init(&current->mm->vmas); // Reset to empty for the new program
  }

  current->cr3 = (uint64_t)new_pml4;
  cpu_set_active_cr3(current->cr3);
  __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");

  // Reset memory placement for the candidate image. Process-visible state is
  // reset only after loading succeeds so a failed exec is non-destructive.
  mm_reset_mmap_state(current);

  elf_info_t elf_info = {0};
  TSC_BEGIN(execve_elf_load);
  if (!elf_load(path, new_pml4, &elf_info)) {
    TSC_END(execve_elf_load);
    klog_puts("[PROC] execve failed path=\"");
    klog_puts(path);
    klog_puts("\" errno=8 (elf_load)\n");
    current->cr3 = old_cr3;
    cpu_set_active_cr3(current->cr3);
    __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");
    if (shared_mm) {
      // A vfork/CLONE_VM exec temporarily detached from the shared mm.
      // ENOEXEC (including a #! script) must leave the caller on the original
      // address space with its program break intact.
      struct mm_struct *failed_mm = current->mm;
      if (failed_mm->pcid) {
        pcid_free(failed_mm->pcid);
        failed_mm->pcid = 0;
      }
      vma_list_destroy(&failed_mm->vmas);
      kfree(failed_mm);


      current->mm = old_mm;
      spinlock_acquire(&old_mm->lock);
      old_mm->ref_count++;
      spinlock_release(&old_mm->lock);
    } else {
      vma_list_destroy(&current->mm->vmas);
      current->mm->vmas = old_vmas;
      current->mm->brk_base = old_brk_base;
      current->mm->brk_current = old_brk_current;
      current->mm->mmap_next_addr = old_mmap_next_addr;
      current->mm->compat32 = old_compat32;
    }
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    // Note: new_pml4 is leaked if elf_load fails. Fixing that would require
    // vmm_destroy_pml4 or similar, but for now we focus on the reported leak.
    return (uint64_t)-8; // ENOEXEC
  }

  TSC_END(execve_elf_load);

  exec_close_cloexec(current);

  uint32_t old_exec_euid = current->euid;
  if ((exec_mode & 04000) && !current->no_new_privs && !(exec_mount_flags & 2))
    current->euid = current->suid = current->fsuid = exec_uid;
  if ((exec_mode & 02000) && !current->no_new_privs && !(exec_mount_flags & 2))
    current->egid = current->sgid = current->fsgid = exec_gid;
  capability_exec(current, old_exec_euid);

  current->fs_base = 0;
  current->gs_base = 0;

  /* exec replaces the mappings backing a fork-inherited alternate stack.
   * Keeping its registration makes SA_ONSTACK handlers target the old image
   * and can kill a process while delivering a recoverable Wasm SIGILL). */
  current->ss_sp = 0;
  current->ss_size = 0;
  current->ss_flags = SS_DISABLE;
  syscall_user_dispatch_configure(current, 0, 0, 0, 0);

  // Reset custom signal handlers to SIG_DFL after exec; preserve SIG_IGN (POSIX requirement).
  for (int i = 0; i < 64; i++) {
    if ((uint64_t)current->signal_handlers[i].sa_handler != (uint64_t)SIG_IGN) {
      memset(&current->signal_handlers[i], 0, sizeof(struct k_sigaction));
    }
  }
  /* exec preserves pending signals, including full queued siginfo payloads.
   * Dispositions change above, but delivery belongs to the new image. */

  // The old address space is gone, so any robust list registered in it is too.
  current->robust_list = 0;

  // Same for rseq: the old registration pointed into the dead address space
  // (Linux flush_thread() clears it on exec).
  current->rseq_addr = 0;
  current->rseq_len = 0;
  current->rseq_sig = 0;

  // We are now safely loaded into the new address space!
  TSC_BEGIN(execve_stack);
  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, path, (const char **)k_argv,
      (const char **)k_envp, &elf_info);
  TSC_END(execve_stack);

  /* Parent-side vfork_wait already dumps completed samples. Avoid rendering
   * a duplicate exec table while the borrowing child still holds it asleep. */
  bool borrowed_vfork = (current->clone_flags & CLONE_VFORK) != 0;
  /* Successful exec starts with architectural FPU defaults. */
  fpu_reset_current();

  // ENOEXEC fallbacks above retain the original address space and keep waiting.
  vfork_release_parent(current);

  /* Probes are cumulative (fork stats must survive to be averaged), so print
   * the first few launches; fork_probe_maybe_dump() covers later activity. */
  {
    static uint32_t exec_probe_dumps;
    if (!borrowed_vfork && exec_probe_dumps < 4) {
      exec_probe_dumps++;
      tsc_probe_dump();
    }
  }

  uint64_t actual_entry =
      elf_info.interp_base ? elf_info.interp_entry : elf_info.entry;

  // Store the basename of the executable as the thread's comm name
  {
    const char *base = path;
    for (const char *p = path; *p; p++)
      if (*p == '/')
        base = p + 1;
    int ci = 0;
    while (base[ci] && ci < 15) {
      current->comm[ci] = base[ci];
      ci++;
    }
    current->comm[ci] = '\0';
  }

  // Automatic desktop interactive prioritization (KDE Plasma, KWin, Xorg)
  if (strcmp(current->comm, "kwin_x11") == 0 ||
      strcmp(current->comm, "kwin_x11-bin") == 0 ||
      strcmp(current->comm, "kwin_wayland") == 0 ||
      strcmp(current->comm, "kwin") == 0 ||
      strcmp(current->comm, "plasmashell") == 0 ||
      strcmp(current->comm, "Xorg") == 0 ||
      strcmp(current->comm, "Xwayland") == 0) {
    sched_set_priority(current, nice_to_sched_priority(-10), -10);
    eevfd_set_slice(&current->se, EEVFD_INTERACTIVE_SLICE_NS);
  }

  if (current->is_forked_child) {
    klog_proc_exec(current->tid, path);
#if SYSCALL_LOG == 2
    if (klog_path_is_browser(path))
      current->trace_syscalls = true;
#elif SYSCALL_LOG == 3
    if (klog_path_is_sddm(path))
      current->trace_syscalls = true;
#endif
  }

  // Store the full executable path for /proc/self/exe
  strncpy(current->exe_path, path, sizeof(current->exe_path) - 1);
  current->exe_path[sizeof(current->exe_path) - 1] = '\0';

  // Map the vsyscall page into the new address space
  vmm_map_vsyscall_page((uint64_t *)current->cr3);

  // Hand the old address space (from fork) to background teardown now that
  // the new one is loaded.  We've already switched CR3, so no CPU can be
  // walking these tables.  Queued nodes drain on idle CPUs (bounded by the
  // reap-queue safety net), so execve no longer pays the ~60 us page-table
  // walk inline.  The queue steals old_vmas (re-initing it), making the
  // destroy below a no-op in the queued case; on OOM it falls back to the
  // inline free internally.  Shared mm (vfork/thread exec) has nothing to
  // free: old_vmas there is an empty placeholder.
  if (old_cr3 != 0 && !shared_mm) {
    TSC_BEGIN(exec_free_old);
    vmm_queue_free_user_pages_vma(old_cr3, &old_vmas);
    TSC_END(exec_free_old);
  }
  // Now destroy the old VMA tree nodes
  TSC_BEGIN(exec_vma_destroy);
  vma_list_destroy(&old_vmas);
  TSC_END(exec_vma_destroy);

  // Cleanup kernel-side copies
  for (int i = 0; i < argc; i++)
    kfree(k_argv[i]);
  kfree(k_argv);
  for (int i = 0; i < envc; i++)
    kfree(k_envp[i]);
  kfree(k_envp);
  kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);
  struct thread *ct = sched_get_current();
  if (ct) {
    ct->fs_base = 0;
    ct->gs_base = 0;
  }

  // Set the return registers for the syscall exit handler to jump to.
  // We MUST clear all general purpose registers to prevent info leaks from the
  // previous process image (e.g. from the shell into a new glibc program).
  // Note: RCX and R11 are clobbered by the syscall instruction itself and
  // are not present in our struct syscall_regs.
  regs->rip = actual_entry;
  regs->rsp = user_rsp;
  regs->rax = 0;
  regs->rbx = 0;
  regs->rdx = 0;
  regs->rsi = 0;
  regs->rdi = 0;
  regs->rbp = 0;
  regs->r8 = 0;
  regs->r9 = 0;
  regs->r10 = 0;
  regs->r12 = 0;
  regs->r13 = 0;
  regs->r14 = 0;
  regs->r15 = 0;
  regs->user_cs = elf_info.compat32 ? 0x1b : 0x2b;
  regs->user_rcx = regs->user_rsi = regs->user_rdi = 0;
  regs->rflags = 0x202; // IF | reserved bit 1

  return 0;
}

// Fork child kernel thread entry point
// This function runs as a kernel thread.  When scheduled, it switches to the
// child's page table and sysrets to the user-space address where the parent
// called fork().  RAX will be 0 (child return value).
static void fork_child_entry(void) {
  struct thread *self = sched_get_current();
  struct syscall_regs *child_regs = (struct syscall_regs *)self->fork_ctx;

  // Switch to the child's cloned address space if not already loaded by sched_schedule
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & CR3_ADDR_MASK) != (self->cr3 & CR3_ADDR_MASK)) {
    cpu_set_active_cr3(self->cr3);
    __asm__ volatile("mov %0, %%cr3" ::"r"(self->cr3) : "memory");
  }

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Restore user TLS bases — child inherits parent's FS_BASE (musl needs TLS)
  wrmsr(IA32_KERNEL_GS_BASE, self->gs_base);
  wrmsr(IA32_FS_BASE, self->fs_base);

  if (self->child_set_tid) {
    uint32_t visible_tid = pid_namespace_tid(self, self);
    copy_to_user((void *)self->child_set_tid, &visible_tid, sizeof(visible_tid));
    self->child_set_tid = 0;
  }

  // Jump to userspace — this never returns
  fork_return_to_userspace(child_regs);
}

/* Fork is on the critical path of every shell command (bash does fork+exec).
 * Print the cumulative probe table once a representative number of forks has
 * happened so the clone cost is visible alongside the exec sections. */
static void fork_probe_maybe_dump(void) {
  static uint32_t forks;
  uint32_t n = __atomic_add_fetch(&forks, 1, __ATOMIC_RELAXED);
  if (n == 16 || n == 64)
    tsc_probe_dump();
  /* Zero-pool hit rate next to the TSC table: boot is pool-cold by
   * construction (no idle time during the exec storm), steady state should
   * show hits climbing once idle refills. Every 64th fork, additive. */
  if ((n % 64) == 0) {
    uint64_t hits = 0, misses = 0, refilled = 0;
    size_t cached = 0;
    pmm_zero_pool_stats(&hits, &misses, &refilled, &cached);
    klog_puts("[PMM] zero-pool hits=");
    klog_uint64(hits);
    klog_puts(" misses=");
    klog_uint64(misses);
    klog_puts(" refilled=");
    klog_uint64(refilled);
    klog_puts(" cached=");
    klog_uint64((uint64_t)cached);
    klog_puts("\n");
  }
}

// sys_fork (raw handler — receives full register frame)
uint64_t sys_fork(struct syscall_regs *regs) {
  // 1. Get current parent state
  uint64_t *parent_pml4_phys = vmm_get_active_pml4();
  struct thread *parent = sched_get_current();
  if (!regs || !parent || !parent->mm)
    return (uint64_t)-22;

  // 2. Clone the user address space with VMA awareness
  //    Shared mappings share physical pages, private mappings get copied
  TSC_BEGIN(fork_clone);
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(
      parent_pml4_phys, &parent->mm->vmas);
  TSC_END(fork_clone);
  fork_probe_maybe_dump();
  /* Never publish a child that would enter fork_child_entry with CR3 == 0.
   * The VMM clone returns zero when it cannot allocate the child page tables. */
  if (!child_cr3)
    return (uint64_t)-12;

  // 3. Allocate and populate the child's saved register state.
  //    RAX = 0 so the child sees fork() returning 0.
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)(-12);
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  /* Prepare every fallible process resource before publishing a thread. */
  struct mm_struct *child_mm = kmalloc(sizeof(*child_mm));
  if (!child_mm) {
    kfree(child_regs);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)-12;
  }
  memset(child_mm, 0, sizeof(*child_mm));
  vma_list_init(&child_mm->vmas);
  TSC_BEGIN(fork_vma);
  if (!vma_list_clone(&child_mm->vmas, &parent->mm->vmas)) {
    kfree(child_mm);
    kfree(child_regs);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)-12;
  }
  TSC_END(fork_vma);
  child_mm->brk_base = parent->mm->brk_base;
  child_mm->brk_current = parent->mm->brk_current;
  child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
    child_mm->compat32 = parent->mm->compat32;
  child_mm->ref_count = 1;
  child_mm->pcid = pcid_alloc();
  spinlock_init(&child_mm->lock);
  memcpy(child_mm->saved_auxv, parent->mm->saved_auxv,
         sizeof(child_mm->saved_auxv));
  child_mm->auxv_count = parent->mm->auxv_count;
  memcpy(child_mm->saved_cmdline, parent->mm->saved_cmdline,
         sizeof(child_mm->saved_cmdline));
  child_mm->cmdline_len = parent->mm->cmdline_len;
  child_mm->arg_start = parent->mm->arg_start;
  child_mm->arg_end = parent->mm->arg_end;

  // 4. Create a kernel thread for the child process, not yet runnable.
  TSC_BEGIN(fork_thread);
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, NULL, false);
  if (!child) {
    kfree(child_regs);
    pcid_free(child_mm->pcid);
    vma_list_destroy(&child_mm->vmas);
    kfree(child_mm);
    vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
    return (uint64_t)(-12);
  }

  // Clear or free the default MM allocated by sched_create_kernel_thread
  // as we're about to replace it with a cloned one.
  if (child->mm) {
    pcid_free(child->mm->pcid);
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }
  child->mm = child_mm;
  TSC_END(fork_thread);

  // 5. Configure child thread
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->tgid = child->tid; // Fork creates a new process (new thread group)
  /* Linux implements fork() as a clone whose exit signal is SIGCHLD, and the
   * parent is notified through it.  Keep that visible here so the notification
   * does not depend on which entry point created the child. */
  child->clone_flags = SIGCHLD;

  // 6. Copy file descriptors from parent to child (with reference counting)
  TSC_BEGIN(fork_fds);
  if (parent) {
    sched_copy_files(child, parent);
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    // sched_create_kernel_thread() already inherited and referenced the
    // parent's CWD. Do not take a second, unmatched reference here.
    child->cwd_node = parent->cwd_node;
    memcpy(child->signal_handlers, parent->signal_handlers,
           sizeof(child->signal_handlers));
    child->fs_base = parent->fs_base;
    child->gs_base = parent->gs_base;
    child->umask = parent->umask;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->supplementary_group_count = parent->supplementary_group_count;
    memcpy(child->supplementary_groups, parent->supplementary_groups,
           sizeof(child->supplementary_groups));
    child->ctty = parent->ctty; // Inherit controlling terminal

    // Inherit comm name — forked child keeps the parent's name until exec
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));

    // Inherit alternate signal stack
    child->ss_sp = parent->ss_sp;
    child->ss_size = parent->ss_size;
    child->ss_flags = parent->ss_flags;

#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
    child->trace_syscalls = parent->trace_syscalls;
#endif
  }
  TSC_END(fork_fds);

  // 8. Enqueue child only after its address space and inherited state are set.
  // Keep the return value independent of the child allocation in case it exits
  // quickly after being scheduled.
  int ns_error = process_namespace_inherit(child, parent, NULL, NULL, 0);
  if (ns_error) { sched_reap_thread(child); return (uint64_t)ns_error; }
  uint32_t child_tid = pid_namespace_tid(parent, child);
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  sched_enqueue_thread(child, NULL);

  // 9. Return child PID to parent
  return child_tid;
}

static uint64_t sys_clone_internal(struct syscall_regs *regs, uint64_t flags,
                                   uint64_t child_stack, uint64_t ptid,
                                   uint64_t ctid, uint64_t newtls,
                                   uint64_t pidfd_slot) {
  struct thread *parent = sched_get_current();
  if (!parent || !parent->mm)
    return (uint64_t)-22;

  /* Linux clone flag dependencies. */
  if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
    return (uint64_t)-22;
  if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
    return (uint64_t)-22;
  /* Reject combined sandbox requests until every requested namespace exists.
   * In particular, accepting NEWNS with a global mount table is not safe. */
  if (flags & (CLONE_NEWCGROUP | CLONE_NEWUSER | CLONE_NEWTIME))
    return (uint64_t)-1; // EPERM
  if ((flags & CLONE_NEWIPC) && (flags & CLONE_SYSVSEM)) return (uint64_t)-22;
  if ((flags & CLONE_NEWPID) && (flags & (CLONE_THREAD | CLONE_PARENT))) return (uint64_t)-22;
  if ((flags & CLONE_NEWNS) && (flags & CLONE_FS)) return (uint64_t)-22;
  if ((flags & (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET)) &&
      !capability_has(parent, CAP_SYS_ADMIN))
    return (uint64_t)-1;
  struct uts_namespace *new_uts
      __attribute__((cleanup(uts_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWUTS) {
    new_uts = uts_namespace_copy(parent);
    if (!new_uts) return (uint64_t)-12;
  }
  struct ipc_namespace *new_ipc
      __attribute__((cleanup(ipc_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWIPC) {
    new_ipc = ipc_namespace_create();
    if (!new_ipc) return (uint64_t)-12;
  }
  struct mount_namespace *new_mount __attribute__((cleanup(process_mount_cleanup))) = NULL;
  struct pid_namespace *new_pid __attribute__((cleanup(pid_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNS) {
    new_mount = vfs_namespace_prepare(parent);
    if (!new_mount) return (uint64_t)-12;
  }
  if (flags & CLONE_NEWPID) {
    new_pid = pid_namespace_create(parent);
    if (!new_pid) return (uint64_t)-12;
  }
  struct net_namespace *new_net __attribute__((cleanup(net_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNET) {
    new_net = net_namespace_create();
    if (!new_net) return (uint64_t)-12;
  }
  if ((flags & CLONE_VM) && !(flags & CLONE_VFORK) && child_stack == 0)
    return (uint64_t)-22;
  if ((flags & CLONE_PARENT_SETTID) &&
      (!ptid || !vmm_is_user_addr_range_writable(ptid, sizeof(uint32_t))))
    return (uint64_t)-14;
  if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
      (!ctid || !vmm_is_user_addr_range_writable(ctid, sizeof(uint32_t))))
    return (uint64_t)-14;

  /* clone(CLONE_PIDFD) hands the descriptor back through the parent_tid slot
   * (clone3() has a field of its own for it).  Reserve it here, before the
   * child exists, because no allocation may fail once the child has been
   * created; it is installed only after the child copied the parent's
   * descriptor table, so the pidfd does not leak into the child. */
  int pidfd_fd = -1;
  vfs_node_t *pidfd_node = NULL;
  if (pidfd_slot) {
    /* Linux refuses a pidfd for a thread, and one slot cannot report both the
     * pidfd and the parent_tid. */
    if (flags & CLONE_THREAD)
      return (uint64_t)-22;
    if ((flags & CLONE_PARENT_SETTID) && pidfd_slot == ptid)
      return (uint64_t)-22;
    if (!vmm_is_user_addr_range_writable(pidfd_slot, sizeof(int)))
      return (uint64_t)-14;
    int64_t pr = pidfd_reserve(parent, &pidfd_fd, &pidfd_node);
    if (pr < 0)
      return (uint64_t)pr;
  }
#define PIDFD_UNRESERVE()                                                      \
  do {                                                                         \
    if (pidfd_fd >= 0) {                                                       \
      pidfd_abort(parent, pidfd_fd, pidfd_node);                               \
      pidfd_fd = -1;                                                           \
      pidfd_node = NULL;                                                       \
    }                                                                          \
  } while (0)

  struct syscall_regs *child_regs = kmalloc(sizeof(*child_regs));
  if (!child_regs) {
    PIDFD_UNRESERVE();
    return (uint64_t)-12;
  }
  memcpy(child_regs, regs, sizeof(*child_regs));
  child_regs->rax = 0;
  if (child_stack)
    child_regs->rsp = child_stack;

  uint64_t child_cr3 = parent->cr3;
  struct mm_struct *child_mm = parent->mm;
  bool vfork = (flags & CLONE_VFORK) != 0;
  /* CLONE_VFORK borrows the address space (Linux vfork semantics, same as
   * sys_vfork): the child shares mm/CR3 and the caller blocks until the child
   * execs or exits. Only the calling thread sleeps; sibling threads keep
   * running. The child must only call execve or _exit — the standard vfork
   * contract musl posix_spawn honors. exec() unshares via its shared_mm path;
   * _exit/kill release via vfork_release_parent() plus mm refcounting, and
   * the reaper NULLs survivor back-pointers so a reaped parent can't be
   * touched by a late release. VFORK-without-VM keeps the old private-CoW
   * clone as a fallback instead of growing a new EINVAL. */
  bool private_mm = !(flags & CLONE_VM);

  if (private_mm) {
    TSC_BEGIN(fork_clone);
    child_cr3 = vmm_clone_user_mappings_vma(
        (uint64_t *)parent->cr3, &parent->mm->vmas);
    TSC_END(fork_clone);
    if (!child_cr3) {
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }

    child_mm = kmalloc(sizeof(*child_mm));
    if (!child_mm) {
      vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }
    memset(child_mm, 0, sizeof(*child_mm));
    vma_list_init(&child_mm->vmas);
    if (!vma_list_clone(&child_mm->vmas, &parent->mm->vmas)) {
      kfree(child_mm);
      vmm_free_user_pages_vma(child_cr3, &parent->mm->vmas);
      PIDFD_UNRESERVE();
      kfree(child_regs);
      return (uint64_t)-12;
    }
    child_mm->brk_base = parent->mm->brk_base;
    child_mm->brk_current = parent->mm->brk_current;
    child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
    child_mm->compat32 = parent->mm->compat32;
    child_mm->ref_count = 1;
    child_mm->pcid = pcid_alloc();
    spinlock_init(&child_mm->lock);
  }

  struct cpu_info *target_cpu = vfork ? cpu_get_current() : NULL;
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, target_cpu, false);
  if (!child) {
    PIDFD_UNRESERVE();
    if (private_mm) {
      vmm_free_user_pages_vma(child_cr3, &child_mm->vmas);
      if (child_mm->pcid) {
        pcid_free(child_mm->pcid);
        child_mm->pcid = 0;
      }
      vma_list_destroy(&child_mm->vmas);
      kfree(child_mm);
    }
    kfree(child_regs);
    return (uint64_t)-12;
  }

  /* Replace the generic kernel-thread MM only after every clone-owned
   * allocation has succeeded. From this point initialization cannot fail. */
  if (child->mm) {
    if (child->mm->pcid) {
      pcid_free(child->mm->pcid);
      child->mm->pcid = 0;
    }
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }

  if (!private_mm)
    __atomic_add_fetch(&child_mm->ref_count, 1, __ATOMIC_ACQ_REL);

  child->cr3 = child_cr3;
  child->mm = child_mm;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->clone_flags = flags;
  if (new_ipc) {
    ipc_namespace_install(child, new_ipc);
    new_ipc = NULL;
  }
  if (new_uts) {
    uts_namespace_install(child, new_uts);
    new_uts = NULL;
  }
  child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->tid;
  child->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;
  child->gs_base = parent->gs_base;
  child->tid_address =
      (flags & CLONE_CHILD_CLEARTID) ? (uint64_t *)ctid : NULL;
  child->child_set_tid = (flags & CLONE_CHILD_SETTID) ? ctid : 0;
  // A new task starts from the parent's robust list, exactly like Linux; a
  // fresh thread installs its own through set_robust_list(2).
  child->robust_list = parent->robust_list;
  // rseq follows Linux copy_process()/rseq_fork(): the child's registration
  // is always cleared, even for CLONE_THREAD. Each thread owns a struct rseq
  // in its own TLS at a different virtual address; inheriting the parent's
  // pointer makes the child's subsequent rseq(2) registration of its own TLS
  // area fail with EBUSY, which glibc treats as fatal ("rseq registration
  // failed") and aborts (observed as Xorg + all threaded glibc programs
  // dying with SIGABRT). The child re-registers on thread startup.
  child->rseq_addr = 0;
  child->rseq_len = 0;
  child->rseq_sig = 0;

  if (flags & CLONE_FILES) {
    sched_share_files(child, parent);
  } else {
    sched_copy_files(child, parent);
  }

  memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  child->uid = parent->uid;
  child->gid = parent->gid;
  child->euid = parent->euid;
  child->egid = parent->egid;
  child->suid = parent->suid;
  child->sgid = parent->sgid;
  child->fsuid = parent->fsuid;
  child->fsgid = parent->fsgid;
  child->supplementary_group_count = parent->supplementary_group_count;
  memcpy(child->supplementary_groups, parent->supplementary_groups,
         sizeof(child->supplementary_groups));
  child->pgid = parent->pgid;
  memcpy(child->signal_handlers, parent->signal_handlers,
         sizeof(child->signal_handlers));
  if ((flags & CLONE_VM) && !(flags & CLONE_VFORK)) {
    child->ss_sp = 0;
    child->ss_size = 0;
    child->ss_flags = SS_DISABLE;
  } else {
    child->ss_sp = parent->ss_sp;
    child->ss_size = parent->ss_size;
    child->ss_flags = parent->ss_flags;
  }
  memcpy(child->comm, parent->comm, sizeof(child->comm));
  memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
  child->trace_syscalls = parent->trace_syscalls;
#endif

  int ns_error = process_namespace_inherit(child, parent, new_mount, new_pid, flags);
  if (ns_error) { PIDFD_UNRESERVE(); sched_reap_thread(child); return (uint64_t)ns_error; }
  new_mount = NULL;
  if (new_net) { net_namespace_install(child, new_net); new_net = NULL; }
  uint32_t visible_child_tid = pid_namespace_tid(parent, child);
  if (flags & CLONE_PARENT_SETTID)
    __atomic_store_n((uint32_t *)ptid, visible_child_tid, __ATOMIC_RELEASE);
  if ((flags & CLONE_CHILD_SETTID) && (flags & CLONE_VM))
    __atomic_store_n((uint32_t *)ctid, pid_namespace_tid(child, child), __ATOMIC_RELEASE);
  if (pidfd_fd >= 0) {
    /* The child has copied the descriptor table by now, so the pidfd is only
     * the parent's.  Publish it before the child runs: the whole point of a
     * pidfd is to be able to watch a process that may already be gone. */
    pidfd_publish(parent, pidfd_fd, pidfd_node, child->tid, 0);
    int pidfd_user = pidfd_fd;
    copy_to_user((void *)pidfd_slot, &pidfd_user, sizeof(int));
  }
  /* Runqueue publication is the final creation step. */
  /* Snapshot the result before making the child runnable. */
  uint32_t child_tid = child->tid;
  if (vfork) {
    /* Publish the rendezvous before the child is runnable: like sys_vfork's
     * child, it may exec/exit on another CPU immediately after enqueue and
     * must observe valid state. Release comes from the child's exec/_exit
     * (or the reap backstop for kills). */
    parent->vfork_child = child_tid;
    parent->vfork_release_tsc = 0;
    __atomic_store_n(&parent->vfork_done, false, __ATOMIC_RELEASE);
  }
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  sched_enqueue_thread(child, target_cpu);

  if (vfork) {
    vfork_wait_for_child(parent);
  }

  return visible_child_tid;
#undef PIDFD_UNRESERVE
}
// sys_clone (syscall 56)
uint64_t sys_clone(struct syscall_regs *regs) {
  uint64_t flags = regs->rdi;
  uint64_t child_stack = regs->rsi;
  uint64_t ptid = regs->rdx;
  uint64_t ctid = regs->r10;
  uint64_t newtls = regs->r8;

  klog_debugf("[CLONE] flags=%llu stack=0x%llx\n",
              (unsigned long long)flags, (unsigned long long)child_stack);

  /* Legacy clone() has no pidfd field: CLONE_PIDFD reports the descriptor
   * through the parent_tid argument. */
  uint64_t pidfd_slot = (flags & CLONE_PIDFD) ? ptid : 0;

  return sys_clone_internal(regs, flags, child_stack, ptid, ctid, newtls,
                            pidfd_slot);
}

// sys_vfork (syscall 58) — real vfork semantics, shared with
// sys_clone_internal()'s CLONE_VFORK path (same borrow + rendezvous).
//
// The child borrows the parent's address space (same mm, same CR3, refcounted)
// and the calling thread blocks until the child execs or exits. Only the
// calling thread sleeps; sibling threads keep running, matching Linux. The
// child must only call execve or _exit (never return or touch the stack) —
// the standard vfork contract, which musl/glibc posix_spawn honor.
//
uint64_t sys_vfork(struct syscall_regs *regs) {
  struct thread *parent = sched_get_current();
  if (!regs || !parent || !parent->mm)
    return (uint64_t)-22;

  // Child register state: returns 0 in the child, same user stack as parent.
  struct syscall_regs *child_regs = kmalloc(sizeof(*child_regs));
  if (!child_regs)
    return (uint64_t)-12;
  memcpy(child_regs, regs, sizeof(*child_regs));
  child_regs->rax = 0;

  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, NULL, false);
  if (!child) {
    kfree(child_regs);
    return (uint64_t)-12;
  }

  // Drop the placeholder MM and share the parent's address space instead of
  // cloning page tables / VMAs (the whole point of vfork).
  if (child->mm) {
    if (child->mm->pcid)
      pcid_free(child->mm->pcid);
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
  }
  spinlock_acquire(&parent->mm->lock);
  parent->mm->ref_count++;
  spinlock_release(&parent->mm->lock);
  child->mm = parent->mm;
  child->cr3 = parent->cr3;

  // Configure the child exactly like sys_fork (own fd-table copy, inherited
  // credentials/handlers/name), so the only behavioral delta is the shared mm.
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  process_inherit_scheduler(child, parent);
  uts_namespace_inherit(child, parent);
  ipc_namespace_inherit(child, parent);
  child->tgid = child->tid;
  child->clone_flags = CLONE_VFORK | CLONE_VM | SIGCHLD;
  sched_copy_files(child, parent);
  memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  // sched_create_kernel_thread() already inherited and referenced the
  // parent's CWD. Do not take a second, unmatched reference here.
  child->cwd_node = parent->cwd_node;
  memcpy(child->signal_handlers, parent->signal_handlers,
         sizeof(child->signal_handlers));
  child->fs_base = parent->fs_base;
  child->gs_base = parent->gs_base;
  child->umask = parent->umask;
  child->uid = parent->uid;
  child->gid = parent->gid;
  child->euid = parent->euid;
  child->egid = parent->egid;
  child->suid = parent->suid;
  child->sgid = parent->sgid;
  child->fsuid = parent->fsuid;
  child->fsgid = parent->fsgid;
  child->supplementary_group_count = parent->supplementary_group_count;
  memcpy(child->supplementary_groups, parent->supplementary_groups,
         sizeof(child->supplementary_groups));
  child->ctty = parent->ctty;
  memcpy(child->comm, parent->comm, sizeof(child->comm));
  memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
  child->ss_sp = parent->ss_sp;
  child->ss_size = parent->ss_size;
  child->ss_flags = parent->ss_flags;
#if SYSCALL_LOG == 2 || SYSCALL_LOG == 3
  child->trace_syscalls = parent->trace_syscalls;
#endif

  int ns_error = process_namespace_inherit(child, parent, NULL, NULL, 0);
  if (ns_error) { sched_reap_thread(child); return (uint64_t)ns_error; }
  uint32_t visible_child_tid = pid_namespace_tid(parent, child);
  uint32_t child_tid = child->tid;
  // Publish the rendezvous before the child is runnable: it may exec/exit
  // on another CPU immediately after enqueue and must observe valid state.
  parent->vfork_child = child_tid;
  parent->vfork_release_tsc = 0;
  __atomic_store_n(&parent->vfork_done, false, __ATOMIC_RELEASE);
  capability_inherit(child, parent);
  seccomp_inherit(child, parent);
  fpu_copy_thread(child, parent);
  sched_enqueue_thread(child, NULL);

  vfork_wait_for_child(parent);
  return visible_child_tid;
}

struct clone_args {
  uint64_t flags;
  uint64_t pidfd;
  uint64_t child_tid;
  uint64_t parent_tid;
  uint64_t exit_signal;
  uint64_t stack;
  uint64_t stack_size;
  uint64_t tls;
  uint64_t set_tid;
  uint64_t set_tid_size;
  uint64_t cgroup;
};

// sys_clone3 (syscall 435)
uint64_t sys_clone3(struct syscall_regs *regs) {
  uint64_t uargs_ptr = regs->rdi;
  size_t size = (size_t)regs->rsi;

  if (size < 64 || !uargs_ptr)
    return (uint64_t)-22; // EINVAL

  if (!is_user_ptr(uargs_ptr) || !vmm_is_user_addr_range_valid(uargs_ptr, size))
    return (uint64_t)-14; // EFAULT

  struct clone_args kargs;
  memset(&kargs, 0, sizeof(kargs));
  size_t copy_len = size < sizeof(kargs) ? size : sizeof(kargs);
  if (copy_from_user(&kargs, (void *)uargs_ptr, copy_len) != 0)
    return (uint64_t)-14;

  uint64_t flags = kargs.flags | (kargs.exit_signal & 0xff);
  uint64_t child_stack = 0;
  if (kargs.stack) {
    child_stack = kargs.stack + kargs.stack_size;
  }

  uint64_t ptid = kargs.parent_tid;
  uint64_t ctid = kargs.child_tid;
  uint64_t newtls = kargs.tls;

  klog_debugf("[CLONE3] flags=0x%llx stack=0x%llx size=0x%llx ptid=0x%llx ctid=0x%llx tls=0x%llx\n",
              (unsigned long long)flags, (unsigned long long)kargs.stack,
              (unsigned long long)kargs.stack_size, (unsigned long long)ptid,
              (unsigned long long)ctid, (unsigned long long)newtls);

  uint64_t ret = sys_clone_internal(regs, flags, child_stack, ptid, ctid, newtls,
                                    (kargs.flags & CLONE_PIDFD) ? kargs.pidfd : 0);
  return ret;
}

// sys_sysinfo
struct sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  /* Natural x86-64 alignment places totalhigh at 88, not 82. */
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
};

_Static_assert(sizeof(struct sysinfo) == 112, "x86-64 sysinfo ABI size");
_Static_assert(offsetof(struct sysinfo, totalram) == 32 &&
               offsetof(struct sysinfo, procs) == 80 &&
               offsetof(struct sysinfo, totalhigh) == 88 &&
               offsetof(struct sysinfo, freehigh) == 96 &&
               offsetof(struct sysinfo, mem_unit) == 104,
               "x86-64 sysinfo ABI offsets");

static uint64_t sys_sysinfo(uint64_t info_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct sysinfo info;
  /* Initialize ABI padding too. In the packed layout mem_unit occupied byte
   * 98, leaving libc's byte-104 read uninitialized and corrupting RAM sizes
   * returned by sysconf(_SC_PHYS_PAGES). */
  memset(&info, 0, sizeof(info));
  info.uptime = (int64_t)(lapic_timer_get_ms() / 1000);
  info.totalram = pmm_get_total_memory();
  info.freeram = (uint64_t)pmm_get_free_pages() * PAGE_SIZE;

  extern uint64_t shm_total_pages(void) __attribute__((weak));
  extern uint64_t tmpfs_total_pages(void) __attribute__((weak));
  uint64_t shm_pages = shm_total_pages ? shm_total_pages() : 0;
  uint64_t tmpfs_pages = tmpfs_total_pages ? tmpfs_total_pages() : 0;
  info.sharedram = (shm_pages + tmpfs_pages) * PAGE_SIZE;

  extern size_t ext2_buffer_cache_total_kb(void) __attribute__((weak));
  info.bufferram = ext2_buffer_cache_total_kb ? (uint64_t)ext2_buffer_cache_total_kb() * 1024 : 0;

  info.procs = sched_get_thread_count();
  info.mem_unit = 1;
  if (copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
    return (uint64_t)-14; // EFAULT
  return 0;
}

// sys_uname
struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

/* System hostname: set by sethostname(2), reported back through uname(2).
 * OpenRC's `hostname` service fails the whole boot runlevel without it. */
/* Hostname and domain belong to the calling thread's UTS namespace. */

static uint64_t process_set_utsname(uint64_t name_ptr, uint64_t len, int domain) {
  if (len > 64)
    return (uint64_t)-22; // EINVAL: Linux caps the hostname at 64 bytes
  if (len && !name_ptr)
    return (uint64_t)-14; // EFAULT

  char tmp[65];
  if (len && copy_from_user(tmp, (const void *)name_ptr, len) != 0)
    return (uint64_t)-14;
  tmp[len] = '\0';

  return (uint64_t)uts_namespace_set(sched_get_current(), tmp, len, domain);
}

static uint64_t sys_sethostname(uint64_t name_ptr, uint64_t len, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  return process_set_utsname(name_ptr, len, 0);
}

static uint64_t sys_setdomainname(uint64_t name_ptr, uint64_t len, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  return process_set_utsname(name_ptr, len, 1);
}

static uint64_t sys_uname(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct utsname result = {0};
  struct utsname *buf = &result;
  if (!buf_ptr)
    return (uint64_t)-14; // EFAULT

  strcpy(buf->sysname, "Ascension");
  uts_namespace_read(sched_get_current(), buf->nodename, buf->domainname);
  strcpy(buf->release, "2.5.0 Beta");

  /* Reading the RTC costs tens of microseconds of CMOS port I/O, which used
   * to dominate this syscall (and gethostname, which is layered on uname).
   * The date only changes once per second, so rebuild the version string at
   * most once per second off the cheap monotonic clock.  A lost race merely
   * repeats an RTC read; the cached bytes are always self-consistent. */
  static char cached_version[80] = "";
  static uint64_t cached_ms = 0;
  uint64_t now_ms = lapic_timer_get_ms();
  if (!cached_version[0] || now_ms - cached_ms >= 1000) {
    char datetime[32];
    rtc_format_datetime(rtc_get_timestamp(), datetime, sizeof(datetime));
    strcpy(cached_version, "2.5.0 Beta ");
    strcat(cached_version, datetime);
    cached_ms = now_ms;
  }
  strcpy(buf->version, cached_version);

  strcpy(buf->machine, "x86_64");
  return copy_to_user((void *)buf_ptr, &result, sizeof(result)) ? (uint64_t)-14 : 0;
}

// sys_getcpu(unsigned *cpu, unsigned *node, struct getcpu_cache *tcache)
// Returns the calling thread's current CPU index and NUMA node.
// AvoryOS has a single NUMA node (0); tcache is ignored.
static uint64_t sys_getcpu(uint64_t cpu_ptr, uint64_t node_ptr, uint64_t tcache,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tcache;
  (void)a3;
  (void)a4;
  (void)a5;

  struct cpu_info *ci = cpu_get_current();
  uint32_t cpu_id = ci ? ci->cpu_id : 0;
  uint32_t node = 0; // single NUMA node

  /* SMAP-safe copies (the old direct stores faulted under SMAP). */
  if (cpu_ptr &&
      copy_to_user((void *)cpu_ptr, &cpu_id, sizeof(cpu_id)) != 0)
    return (uint64_t)-14;
  if (node_ptr &&
      copy_to_user((void *)node_ptr, &node, sizeof(node)) != 0)
    return (uint64_t)-14;
  return 0;
}


static uint64_t sys_uptime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return lapic_timer_get_ms();
}

static uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t a5) {
  (void)arg3;
  (void)arg4;
  (void)arg5;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  switch (option) {
  case 59: // PR_SET_SYSCALL_USER_DISPATCH
    return (uint64_t)syscall_user_dispatch_configure(current, arg2, arg3,
                                                    arg4, arg5);
  case 1: // PR_SET_PDEATHSIG
    if (arg2 == 0 || (arg2 > 0 && arg2 <= 64)) {
      current->pdeath_signal = (int)arg2;
      return 0;
    }
    return (uint64_t)-22;
  case 2: // PR_GET_PDEATHSIG
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = current->pdeath_signal;
      return 0;
    }
    return (uint64_t)-14;
  case 3: // PR_GET_DUMPABLE
    return 1;
  case 4: // PR_SET_DUMPABLE
    return 0;
  case PR_SET_NAME: {
    if (!arg2 || !vmm_is_user_addr_range_valid(arg2, 1))
      return (uint64_t)-14; // EFAULT

    const char *name = (const char *)arg2;
    // Copy up to 15 chars + null terminator
    strncpy(current->comm, name, 15);
    current->comm[15] = '\0';
    return 0;
  }
  case PR_GET_NAME: {
    if (!arg2 || !vmm_is_user_addr_range_valid(arg2, 16))
      return (uint64_t)-14; // EFAULT

    char *out = (char *)arg2;
    strncpy(out, current->comm, 16);
    return 0;
  }
  case 7: case 8: case 23: case 24: case 27: case 28:
    return capability_prctl(current, option, arg2, arg3, arg4, arg5);
  case 13: // PR_GET_TIMING
    return 0;
  case 14: // PR_SET_TIMING
    return 0;
  case 19: // PR_GET_ENDIAN
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 1; // PR_ENDIAN_LITTLE
      return 0;
    }
    return (uint64_t)-14;
  case 20: // PR_SET_ENDIAN
    return (arg2 == 1) ? 0 : (uint64_t)-22;
  case 21: // PR_GET_SECCOMP
    return seccomp_mode(current);
  case 22: // PR_SET_SECCOMP
    if (arg2 != 2) return (uint64_t)-22;
    return (uint64_t)seccomp_control(1, 0, arg3);
  case 25: // PR_GET_TSC
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 1; // PR_TSC_ENABLE
      return 0;
    }
    return (uint64_t)-14;
  case 26: // PR_SET_TSC
    return 0;
  case 29: // PR_SET_TIMERSLACK
    return 0;
  case 30: // PR_GET_TIMERSLACK
    return 50000; // 50 microseconds
  case 35: { // PR_SET_MM (0x23)
    if (!current->mm)
      return 0;
    if (arg2 == 8 /* PR_SET_MM_ARG_START */) {
      current->mm->arg_start = arg3;
    } else if (arg2 == 9 /* PR_SET_MM_ARG_END */) {
      current->mm->arg_end = arg3;
    }
    return 0;
  }
  case 36: // PR_SET_CHILD_SUBREAPER
    return 0;
  case 37: // PR_GET_CHILD_SUBREAPER
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 0;
      return 0;
    }
    return (uint64_t)-14;
  case 38: // PR_SET_NO_NEW_PRIVS
    if (arg2 != 1 || arg3 || arg4 || arg5) return (uint64_t)-22;
    __atomic_store_n(&current->no_new_privs, true, __ATOMIC_RELEASE);
    return 0;
  case 39: // PR_GET_NO_NEW_PRIVS
    if (arg2 || arg3 || arg4 || arg5) return (uint64_t)-22;
    return __atomic_load_n(&current->no_new_privs, __ATOMIC_ACQUIRE);
  case 41: // PR_SET_THP_DISABLE
    return 0;
  case 42: // PR_GET_THP_DISABLE
    return 0;
  case 47:
    return capability_prctl(current, option, arg2, arg3, arg4, arg5);
  case 40: // PR_GET_TID_ADDRESS
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(uint64_t))) {
      *(uint64_t *)arg2 = (uint64_t)current->tid_address;
      return 0;
    }
    return (uint64_t)-14;
  case 52: // PR_GET_SPECULATION_CTRL
    return 0; // PR_SPEC_NOT_AFFECTED
  case 53: // PR_SET_SPECULATION_CTRL
    return 0;
  case 55: // PR_SET_TAGGED_ADDR_CTRL
    return 0;
  case 56: // PR_GET_TAGGED_ADDR_CTRL
    return 0;
  case 0x59616d61: // PR_SET_PTRACER
    return 0;
  case 0x53564d41: // PR_SET_VMA
    return 0;
  case 0x41555856: { // PR_GET_AUXV (Linux 6.4+)    if (!arg2 || !arg3)
      return (uint64_t)-14; // EFAULT
    if (!current->mm)
      return 0;

    static const uint64_t fallback_auxv[] = {
        6 /* AT_PAGESZ */, 4096,
        17 /* AT_CLKTCK */, 100,
        33 /* AT_SYSINFO_EHDR */, 0x700000000000ULL,
        0 /* AT_NULL */, 0
    };

    const void *src = (current->mm->auxv_count > 0)
                          ? (const void *)current->mm->saved_auxv
                          : (const void *)fallback_auxv;
    size_t aux_bytes = (current->mm->auxv_count > 0)
                           ? (current->mm->auxv_count * sizeof(uint64_t))
                           : sizeof(fallback_auxv);

    size_t to_copy = (arg3 < aux_bytes) ? (size_t)arg3 : aux_bytes;
    if (!vmm_is_user_addr_range_writable(arg2, to_copy))
      return (uint64_t)-14; // EFAULT
    memcpy((void *)arg2, src, to_copy);
    return (uint64_t)to_copy;
  }
  case 0x50544344: { // TEMPORARY "PTCD": dump TSC probe stats (syscall profiler)
    tsc_probe_dump();
    return 0;
  }
  default: {
    char opt_str[32];
    snprintf(opt_str, sizeof(opt_str), "option=0x%llx", (unsigned long long)option);
    sysdiag_record_unimplemented_feature("prctl", opt_str, current, -22);
    return (uint64_t)-22; // EINVAL
  }
  }
}

// sys_umask
static uint64_t sys_umask(struct syscall_regs *regs) {
  uint32_t mask = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  uint32_t old_mask = t->umask;
  t->umask = mask & 0777;
  return old_mask;
}

// sys_getuid
static uint64_t sys_getuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->uid;
}

// sys_getgid
static uint64_t sys_getgid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->gid;
}

// sys_geteuid
static uint64_t sys_geteuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->euid;
}

// sys_getegid
static uint64_t sys_getegid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->egid;
}

// sys_getresuid
static uint64_t sys_getresuid(struct syscall_regs *regs) {
  uint32_t *ruid_ptr = (uint32_t *)regs->rdi;
  uint32_t *euid_ptr = (uint32_t *)regs->rsi;
  uint32_t *suid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (ruid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)ruid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *ruid_ptr = t->uid;
  }
  if (euid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)euid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *euid_ptr = t->euid;
  }
  if (suid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)suid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *suid_ptr = t->suid;
  }

  return 0;
}

// sys_getresgid
static uint64_t sys_getresgid(struct syscall_regs *regs) {
  uint32_t *rgid_ptr = (uint32_t *)regs->rdi;
  uint32_t *egid_ptr = (uint32_t *)regs->rsi;
  uint32_t *sgid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (rgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)rgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *rgid_ptr = t->gid;
  }
  if (egid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)egid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *egid_ptr = t->egid;
  }
  if (sgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)sgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *sgid_ptr = t->sgid;
  }

  return 0;
}

// sys_setresuid
static uint64_t sys_setresuid(struct syscall_regs *regs) {
  uint32_t ruid = (uint32_t)regs->rdi, euid = (uint32_t)regs->rsi;
  uint32_t suid = (uint32_t)regs->rdx;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;
  if (!capability_has(t, CAP_SETUID)) {
    if (ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid && ruid != t->suid) return (uint64_t)-1;
    if (euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid) return (uint64_t)-1;
    if (suid != UINT32_MAX && suid != t->uid && suid != t->euid && suid != t->suid) return (uint64_t)-1;
  }
  if (ruid != UINT32_MAX) t->uid = ruid;
  if (euid != UINT32_MAX) t->euid = euid;
  if (suid != UINT32_MAX) t->suid = suid;
  t->fsuid = t->euid;
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setresgid
static uint64_t sys_setresgid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi, egid = (uint32_t)regs->rsi;
  uint32_t sgid = (uint32_t)regs->rdx;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;
  if (!capability_has(t, CAP_SETGID)) {
    if (rgid != UINT32_MAX && rgid != t->gid && rgid != t->egid && rgid != t->sgid) return (uint64_t)-1;
    if (egid != UINT32_MAX && egid != t->gid && egid != t->egid && egid != t->sgid) return (uint64_t)-1;
    if (sgid != UINT32_MAX && sgid != t->gid && sgid != t->egid && sgid != t->sgid) return (uint64_t)-1;
  }
  if (rgid != UINT32_MAX) t->gid = rgid;
  if (egid != UINT32_MAX) t->egid = egid;
  if (sgid != UINT32_MAX) t->sgid = sgid;
  t->fsgid = t->egid;
  return 0;
}

// sys_setuid (syscall 105)
static uint64_t sys_setuid(struct syscall_regs *regs) {
  uint32_t uid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;
  if (capability_has(t, CAP_SETUID)) {
    t->uid = t->euid = t->suid = t->fsuid = uid;
  } else if (uid == t->uid || uid == t->suid) {
    t->euid = t->fsuid = uid;
  } else {
    return (uint64_t)-1;
  }
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setgid (syscall 106)
static uint64_t sys_setgid(struct syscall_regs *regs) {
  uint32_t gid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (capability_has(t, CAP_SETGID)) {
    t->gid = t->egid = t->sgid = t->fsgid = gid;
  } else if (gid == t->gid || gid == t->sgid) {
    t->egid = t->fsgid = gid;
  } else {
    return (uint64_t)-1;
  }
  return 0;
}

// sys_setreuid (syscall 113)
static uint64_t sys_setreuid(struct syscall_regs *regs) {
  uint32_t ruid = (uint32_t)regs->rdi;
  uint32_t euid = (uint32_t)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  uint32_t old_uid = t->uid, old_euid = t->euid, old_suid = t->suid;

  if (!capability_has(t, CAP_SETUID)) {
    if (ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid)
      return (uint64_t)-1;
    if (euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid)
      return (uint64_t)-1;
  }

  if (ruid != UINT32_MAX) {
    t->uid = ruid;
  }
  if (euid != UINT32_MAX) {
    t->euid = euid;
  }
  if (ruid != UINT32_MAX || (euid != UINT32_MAX && euid != t->uid)) {
    t->suid = t->euid;
  }
  t->fsuid = t->euid;
  capability_uid_changed(t, old_uid, old_euid, old_suid);
  return 0;
}

// sys_setregid (syscall 114)
static uint64_t sys_setregid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi;
  uint32_t egid = (uint32_t)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (!capability_has(t, CAP_SETGID)) {
    if (rgid != UINT32_MAX && rgid != t->gid && rgid != t->egid)
      return (uint64_t)-1;
    if (egid != UINT32_MAX && egid != t->gid && egid != t->egid && egid != t->sgid)
      return (uint64_t)-1;
  }

  if (rgid != UINT32_MAX) {
    t->gid = rgid;
  }
  if (egid != UINT32_MAX) {
    t->egid = egid;
  }
  if (rgid != UINT32_MAX || (egid != UINT32_MAX && egid != t->gid)) {
    t->sgid = t->egid;
  }
  t->fsgid = t->egid;
  return 0;
}

// sys_setfsuid (syscall 122)
static uint64_t sys_setfsuid(struct syscall_regs *regs) {
  uint32_t fsuid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsuid value
  uint32_t old_fsuid = t->fsuid;
  if (capability_has(t, CAP_SETUID) || fsuid == t->uid || fsuid == t->euid ||
      fsuid == t->suid || fsuid == t->fsuid)
    t->fsuid = fsuid;
  return old_fsuid;
}

// sys_setfsgid (syscall 123)
static uint64_t sys_setfsgid(struct syscall_regs *regs) {
  uint32_t fsgid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsgid value
  uint32_t old_fsgid = t->fsgid;
  if (capability_has(t, CAP_SETGID) || fsgid == t->gid || fsgid == t->egid ||
      fsgid == t->sgid || fsgid == t->fsgid)
    t->fsgid = fsgid;
  return old_fsgid;
}

static uint64_t sys_getgroups(struct syscall_regs *regs) {
  int size = (int)regs->rdi;
  uint32_t *list = (uint32_t *)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t || size < 0) return (uint64_t)-22;
  if (size == 0) return t->supplementary_group_count;
  if ((uint32_t)size < t->supplementary_group_count) return (uint64_t)-22;
  if (!list || !vmm_is_user_addr_range_writable((uint64_t)list,
      t->supplementary_group_count * sizeof(uint32_t))) return (uint64_t)-14;
  memcpy(list, t->supplementary_groups,
         t->supplementary_group_count * sizeof(uint32_t));
  return t->supplementary_group_count;
}

static uint64_t sys_setgroups(struct syscall_regs *regs) {
  uint32_t size = (uint32_t)regs->rdi;
  const uint32_t *list = (const uint32_t *)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t || !capability_has(t, CAP_SETGID)) return (uint64_t)-1;
  if (size > MAX_SUPPLEMENTARY_GROUPS) return (uint64_t)-22;
  if (size && (!list || !vmm_is_user_addr_range_valid((uint64_t)list,
      size * sizeof(uint32_t)))) return (uint64_t)-14;
  if (size) memcpy(t->supplementary_groups, list, size * sizeof(uint32_t));
  t->supplementary_group_count = size;
  return 0;
}

// sys_getppid
static uint64_t sys_getppid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t) return 0;
  spinlock_acquire(&tid_lock);
  uint32_t pid = t->parent ? pid_namespace_visible_locked(t, t->parent->tgid) : 0;
  spinlock_release(&tid_lock);
  return pid;
}

// sys_setpgid
static uint64_t sys_setpgid(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t pgid = (uint32_t)regs->rsi;
  struct thread *current = sched_get_current();
  struct thread *target = NULL;
  if (pid) { pid = pid_namespace_global(current, pid); if (!pid) return (uint64_t)-3; }
  if (pgid) { pgid = pid_namespace_global(current, pgid); if (!pgid) return (uint64_t)-1; }

  if (pid == 0) {
    target = current;
  } else {
    // Only allow setting PGID for self or children
    if (current->tid == pid) {
      target = current;
    } else {
      spinlock_acquire(&tid_lock);
      struct thread *c = current->children;
      while (c) {
        if (c->tid == pid) {
          target = c;
          break;
        }
        c = c->sibling_next;
      }
      spinlock_release(&tid_lock);
    }
  }

  if (!target)
    return (uint64_t)-3; // ESRCH

  if (pgid == 0)
    pgid = target->tid;
  target->pgid = pgid;
  return 0;
}

static uint64_t sys_getpgid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (pid == 0)
    return pid_namespace_visible(current, current->pgid);
  struct thread *target = sched_get_thread_by_tid(pid_namespace_global(current, (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  return pid_namespace_visible(current, target->pgid);
}

static uint64_t sys_getsid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-3; // ESRCH
  if (pid == 0)
    return pid_namespace_visible(current, current->sid ? current->sid : current->tgid);
  struct thread *target = sched_get_thread_by_tid(pid_namespace_global(current, (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  return pid_namespace_visible(current, target->sid ? target->sid : target->tgid);
}

// sys_getpgrp
static uint64_t sys_getpgrp(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return pid_namespace_visible(t, t->pgid);
}

// sys_rseq: restartable sequences (Linux rseq(2) ABI).
//
// Registration model (matches Linux): each thread registers one 32-byte
// struct rseq { cpu_id_start, cpu_id, rseq_cs, flags, node_id, mm_cid }.
// The kernel keeps cpu_id/cpu_id_start/node_id current on every return to
// user mode (see rseq_update_cpu_id callers in syscall.c / isr.c) and
// redirects RIP to abort_ip when a preemption/signal/migration interrupts
// an active critical section (see rseq_maybe_abort).
//
// Preemption *between* user instructions always funnels back through a
// return-to-user path (syscall tail, interrupt return, signal delivery),
// so no user-memory access happens in sched_schedule() itself, where the
// runqueue lock is held and IRQs are masked.
#define RSEQ_LEN 32
#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_CPU_ID_UNINITIALIZED 0xFFFFFFFFu
#define RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT 1
#define RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL 2
#define RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE 4

struct rseq_cs_abi {
  uint32_t version;
  uint32_t flags;
  uint64_t start_ip;
  uint64_t post_commit_offset;
  uint64_t abort_ip;
};

bool rseq_update_cpu_id(struct thread *t) {
  if (!t || !t->rseq_addr)
    return true;
  struct cpu_info *cpu = cpu_get_current();
  uint32_t id = cpu ? cpu->cpu_id : 0;
  uint32_t node = 0;
  uint32_t mm_cid = 0;
  // Fault-safe: an unmapped/torn-down rseq area just keeps stale values;
  // the next successful update fixes it. Offsets per Linux struct rseq:
  // cpu_id_start@0, cpu_id@4, flags@16, node_id@20, mm_cid@24.
  unsigned long left = 0;
  left |= copy_to_user((void *)(t->rseq_addr + 0), &id, sizeof(id));
  left |= copy_to_user((void *)(t->rseq_addr + 4), &id, sizeof(id));
  left |= copy_to_user((void *)(t->rseq_addr + 20), &node, sizeof(node));
  left |= copy_to_user((void *)(t->rseq_addr + 24), &mm_cid, sizeof(mm_cid));
  return left == 0;
}

bool rseq_maybe_abort(struct thread *t, uint64_t *rip_inout) {
  if (!t || !t->rseq_addr || !rip_inout)
    return false;
  uint64_t cs_ptr = 0;
  if (copy_from_user(&cs_ptr, (const void *)(t->rseq_addr + 8),
                     sizeof(cs_ptr)) != 0)
    return false;
  if (!cs_ptr)
    return false; // Not inside a critical section.
  struct rseq_cs_abi cs;
  if (copy_from_user(&cs, (const void *)cs_ptr, sizeof(cs)) != 0)
    return false;
  if (cs.version != 0)
    return false;
  uint64_t rip = *rip_inout;
  if (rip < cs.start_ip || rip >= cs.start_ip + cs.post_commit_offset)
    return false; // Outside the critical section: nothing to abort.
  if (cs.flags & (RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT |
                  RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL |
                  RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE))
    return false; // Userspace asked to handle this event itself.
  if (t->rseq_sig != 0) {
    uint32_t sig_at_abort = 0;
    if (copy_from_user(&sig_at_abort, (const void *)cs.abort_ip,
                       sizeof(sig_at_abort)) != 0)
      return false;
    if (sig_at_abort != t->rseq_sig)
      return false; // Signature mismatch: refuse to redirect.
  }
  uint64_t zero = 0;
  copy_to_user((void *)(t->rseq_addr + 8), &zero, sizeof(zero));
  *rip_inout = cs.abort_ip;
  return true;
}

uint64_t sys_rseq(struct syscall_regs *regs) {
  uint64_t addr = regs->rdi;
  uint64_t len = regs->rsi;
  uint64_t flags = regs->rdx;
  uint64_t sig = regs->r10; // 4th syscall arg arrives in r10, not rcx.
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-14; // EFAULT: no thread to register for.

  if (flags & (uint64_t)~(uint64_t)RSEQ_FLAG_UNREGISTER)
    return (uint64_t)-22; // EINVAL: unknown flags.

  if (flags & RSEQ_FLAG_UNREGISTER) {
    // Idempotent: unregistering when nothing is registered succeeds.
    current->rseq_addr = 0;
    current->rseq_len = 0;
    current->rseq_sig = 0;
    return 0;
  }

  if (len != RSEQ_LEN)
    return (uint64_t)-22; // EINVAL: struct rseq is 32 bytes.
  if (addr & 31)
    return (uint64_t)-22; // EINVAL: 32-byte alignment required.
  if (!vmm_is_user_addr_range_writable(addr, RSEQ_LEN))
    return (uint64_t)-14; // EFAULT.
  if (current->rseq_addr && current->rseq_addr != addr)
    return (uint64_t)-16; // EBUSY: already registered elsewhere.

  current->rseq_addr = addr;
  current->rseq_len = RSEQ_LEN;
  current->rseq_sig = (uint32_t)sig;

  // Publish the initial state so glibc never observes UNINITIALIZED:
  // flags/node/mm_cid zero, cpu ids = current CPU. Every copy is checked:
  // returning 0 while leaving cpu_id == UNINITIALIZED makes glibc abort
  // with the same "rseq registration failed" message as a syscall error.
  {
    uint32_t zero = 0;
    unsigned long left = 0;
    left |= copy_to_user((void *)(addr + 16), &zero, sizeof(zero)); // flags
    left |= copy_to_user((void *)(addr + 20), &zero, sizeof(zero)); // node_id
    left |= copy_to_user((void *)(addr + 24), &zero, sizeof(zero)); // mm_cid
    if (left != 0) {
      current->rseq_addr = 0;
      current->rseq_len = 0;
      current->rseq_sig = 0;
      return (uint64_t)-14; // EFAULT.
    }
  }
  if (!rseq_update_cpu_id(current)) {
    current->rseq_addr = 0;
    current->rseq_len = 0;
    current->rseq_sig = 0;
    return (uint64_t)-14; // EFAULT: could not publish cpu_id.
  }
  return 0;
}

// sys_setsid
static uint64_t sys_setsid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  // POSIX: setsid() operates on the calling *process* (thread group), not an
  // individual thread. Use tgid (== tid for the group leader/single-threaded
  // case) so a non-leader thread of a group-leader process still gets EPERM.
  uint32_t pid = current->tgid ? current->tgid : current->tid;
  // POSIX: A process group leader cannot create a new session.
  // PID 1 (init) is pgid 1 by design (see kernel.c), so setsid() from init
  // correctly returns -EPERM; callers are expected to fork first.
  if (pid == current->pgid) {
    return (uint64_t)-1; // -EPERM
  }

  current->sid = pid;
  current->pgid = pid;
  current->ctty = NULL; // Clear controlling terminal

  klog_debugf("[SETSID] New session created: %llu\n", (unsigned long long)current->sid);

  return pid_namespace_visible(current, current->sid);
}

struct kernel_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct kernel_itimerval {
  struct kernel_timeval it_interval;
  struct kernel_timeval it_value;
};

#define ITIMER_REAL 0

static void ms_to_timeval(uint64_t ms, struct kernel_timeval *tv) {
  tv->tv_sec = (int64_t)(ms / 1000);
  tv->tv_usec = (int64_t)((ms % 1000) * 1000);
}

static bool timeval_to_ms(const struct kernel_timeval *tv, uint64_t *ms) {
  if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
    return false;
  if ((uint64_t)tv->tv_sec > (UINT64_MAX - 999) / 1000)
    return false;
  *ms = (uint64_t)tv->tv_sec * 1000 +
        ((uint64_t)tv->tv_usec + 999) / 1000;
  return true;
}

static uint64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr,
                              uint64_t old_value_ptr, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (which != ITIMER_REAL)
    return (uint64_t)-22;
  if (!new_value_ptr ||
      !vmm_is_user_addr_range_valid(new_value_ptr,
                                    sizeof(struct kernel_itimerval)))
    return (uint64_t)-14;
  if (old_value_ptr &&
      !vmm_is_user_addr_range_writable(old_value_ptr,
                                    sizeof(struct kernel_itimerval)))
    return (uint64_t)-14;

  /* Copy input first because old_value is allowed to alias new_value. */
  struct kernel_itimerval new_value;
  memcpy(&new_value, (const void *)new_value_ptr, sizeof(new_value));

  uint64_t value_ms;
  uint64_t interval_ms;
  if (!timeval_to_ms(&new_value.it_value, &value_ms) ||
      !timeval_to_ms(&new_value.it_interval, &interval_ms))
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-22;

  uint64_t now = lapic_timer_get_ms();
  struct kernel_itimerval old_value = {0};
  uint64_t remaining_ms =
      t->it_real_next > now ? t->it_real_next - now : 0;
  ms_to_timeval(remaining_ms, &old_value.it_value);
  ms_to_timeval(t->it_real_interval, &old_value.it_interval);

  t->it_real_value = value_ms;
  t->it_real_interval = interval_ms;
  /* Route through the arm/disarm helpers: the tick fast-path skips the
   * whole itimer scan while active_itimer_count==0, so writing it_real_next
   * directly (as here before) leaves the timer armed but never delivered -
   * alarm(2) would never fire. The helpers also maintain
   * earliest_itimer_deadline and handle re-arm accounting. */
  if (value_ms)
    sched_itimer_arm(t, now + value_ms, interval_ms, value_ms);
  else
    sched_itimer_disarm(t);

  if (old_value_ptr)
    memcpy((void *)old_value_ptr, &old_value, sizeof(old_value));
  return 0;
}

static uint64_t sys_alarm(uint64_t seconds, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t) return 0;
  uint64_t now = lapic_timer_get_ticks();
  uint64_t remaining = t->it_real_next > now ? (t->it_real_next - now + 999) / 1000 : 0;
  /* Same accounting as setitimer: bypassing sched_itimer_arm leaves
   * active_itimer_count at zero and the tick scan never delivers SIGALRM. */
  if (seconds)
    sched_itimer_arm(t, now + seconds * 1000, 0, seconds * 1000);
  else
    sched_itimer_disarm(t);
  return remaining;
}

// getrusage (syscall 98)
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

struct rusage {
  struct { int64_t tv_sec; int64_t tv_usec; } ru_utime; // user CPU time
  struct { int64_t tv_sec; int64_t tv_usec; } ru_stime; // system CPU time
  int64_t ru_maxrss;     // maximum resident set size (KB)
  int64_t ru_ixrss;      // integral shared memory size
  int64_t ru_idrss;      // integral unshared data size
  int64_t ru_isrss;      // integral unshared stack size
  int64_t ru_minflt;     // page reclaims (soft page faults)
  int64_t ru_majflt;     // page faults (hard page faults)
  int64_t ru_nswap;      // swaps
  int64_t ru_inblock;    // block input operations
  int64_t ru_oublock;    // block output operations
  int64_t ru_msgsnd;     // IPC messages sent
  int64_t ru_msgrcv;     // IPC messages received
  int64_t ru_nsignals;   // signals received
  int64_t ru_nvcsw;      // voluntary context switches
  int64_t ru_nivcsw;     // involuntary context switches
};

static uint64_t sys_getrusage(uint64_t who, uint64_t usage_ptr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct rusage *ru = (struct rusage *)usage_ptr;
  if (!ru || !vmm_is_user_addr_range_writable((uint64_t)ru, sizeof(*ru)))
    return (uint64_t)-14; // EFAULT

  int who_int = (int)(int64_t)who;
  if (who_int != RUSAGE_SELF && who_int != RUSAGE_CHILDREN &&
      who_int != RUSAGE_THREAD)
    return (uint64_t)-22; // EINVAL

  memset(ru, 0, sizeof(*ru));

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-22;

  if (who_int == RUSAGE_SELF || who_int == RUSAGE_THREAD) {
    // runtime_total is in LAPIC ticks (1 tick ≈ 1ms)
    // Report as user time (we don't separately track user vs kernel time)
    uint64_t total_ms = t->runtime_total;
    ru->ru_utime.tv_sec = (int64_t)(total_ms / 1000);
    ru->ru_utime.tv_usec = (int64_t)((total_ms % 1000) * 1000);

    // Report resident memory if mm is available
    if (t->mm) {
      // Convert bytes to KB for ru_maxrss (Linux convention)
      uint64_t resident = 0;
      spinlock_acquire(&t->mm->lock);
      // Estimate from brk range + mmap allocations
      if (t->mm->brk_current > t->mm->brk_base)
        resident += t->mm->brk_current - t->mm->brk_base;
      spinlock_release(&t->mm->lock);
      ru->ru_maxrss = (int64_t)(resident / 1024);
      if (ru->ru_maxrss == 0)
        ru->ru_maxrss = 4; // minimum 4 KB
    }
  }
  // RUSAGE_CHILDREN: leave zeroed (we don't accumulate child usage yet)

  return 0;
}

// Resource Limits (getrlimit / prlimit64)
struct rlimit {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

#define RLIM_INFINITY ((uint64_t)-1)

/* The effective limits are still fixed, but every ABI transfer must use
 * fault-safe user copies: Chromium can pass buffers protected with mprotect. */
static struct rlimit resource_limit_value(uint32_t resource) {
  struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
  if (resource == 7) { /* RLIMIT_NOFILE */
    limit.rlim_cur = limit.rlim_max = MAX_FDS;
  } else if (resource == 4) { /* RLIMIT_CORE */
    limit.rlim_cur = 0;
  } else if (resource == 3) { /* RLIMIT_STACK */
    limit.rlim_cur = limit.rlim_max = 8 * 1024 * 1024;
  }
  return limit;
}

static int resource_limit_read(uint64_t user, struct rlimit *limit) {
  if (copy_from_user(limit, (const void *)user, sizeof(*limit)) != 0)
    return -14;
  return limit->rlim_cur > limit->rlim_max ? -22 : 0;
}

static uint64_t sys_getrlimit(uint64_t resource_arg, uint64_t rlim_ptr,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  uint32_t resource = (uint32_t)resource_arg;
  if (resource >= 16) return (uint64_t)-22;
  struct rlimit limit = resource_limit_value(resource);
  return copy_to_user((void *)rlim_ptr, &limit, sizeof(limit)) != 0
             ? (uint64_t)-14 : 0;
}

static uint64_t sys_prlimit64(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t resource = (uint32_t)regs->rsi;
  if (resource >= 16) return (uint64_t)-22;
  struct rlimit requested;
  if (regs->rdx) {
    int error = resource_limit_read(regs->rdx, &requested);
    if (error) return (uint64_t)(int64_t)error;
  }
  if (pid != 0) {
    struct thread *current = sched_get_current();
    struct sched_thread_snapshot target;
    if (!sched_get_thread_snapshot(pid_namespace_global(sched_get_current(), pid), &target)) return (uint64_t)-3;
    if (!current || (target.tgid ? target.tgid : target.tid) !=
                        (current->tgid ? current->tgid : current->tid))
      return (uint64_t)-1; /* Other processes remain unsupported. */
  }
  if (regs->r10) {
    struct rlimit previous = resource_limit_value(resource);
    if (copy_to_user((void *)regs->r10, &previous, sizeof(previous)) != 0)
      return (uint64_t)-14;
  }
  /* Preserve the existing fixed-limit policy for valid setter requests. */
  return 0;
}

static uint64_t sys_setrlimit(uint64_t resource_arg, uint64_t rlim_ptr,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((uint32_t)resource_arg >= 16) return (uint64_t)-22;
  struct rlimit requested;
  return (uint64_t)(int64_t)resource_limit_read(rlim_ptr, &requested);
}

static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)cmd;
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: success
  return 0;
}

// Registration
static uint64_t sys_sched_yield(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_yield_user();
  return 0;
}

static uint64_t sys_sched_setaffinity(uint64_t pid, uint64_t len,
                                      uint64_t user_mask_ptr, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_mask_ptr)
    return (uint64_t)-14; // EFAULT
  if (len < sizeof(uint64_t))
    return (uint64_t)-22; // EINVAL

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  if (!vmm_is_user_addr_range_valid(user_mask_ptr, sizeof(uint64_t)))
    return (uint64_t)-14;

  /* SMAP-safe copy: the old direct dereference faulted under SMAP and made
   * every affinity request fail silently. */
  uint64_t mask = 0;
  if (copy_from_user(&mask, (const void *)user_mask_ptr, sizeof(mask)) != 0)
    return (uint64_t)-14;

  // Sanity check: must have at least one valid CPU in the mask
  uint32_t cpu_count = cpu_get_count();
  uint64_t all_cpus_mask = (1ULL << cpu_count) - 1;
  if (cpu_count >= 64)
    all_cpus_mask = ~0ULL;

  if (!(mask & all_cpus_mask))
    return (uint64_t)-22; // EINVAL

  target->cpu_affinity = mask & all_cpus_mask;
  return 0;
}

static uint64_t sys_sched_getaffinity(uint64_t pid, uint64_t len,
                                      uint64_t user_mask_ptr, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!user_mask_ptr)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  uint32_t cpu_count = cpu_get_count();
  uint64_t mask = target->cpu_affinity;
  if (mask == 0) {
    mask = (1ULL << cpu_count) - 1;
    if (cpu_count >= 64)
      mask = ~0ULL;
  }

  if (len < sizeof(uint64_t)) {
    return (uint64_t)-22; // EINVAL
  }

  size_t copy_bytes = len;
  if (copy_bytes > 128)
    copy_bytes = 128; // Standard Linux cpuset max 1024 bits

  if (!vmm_is_user_addr_range_writable(user_mask_ptr, copy_bytes))
    return (uint64_t)-14; // EFAULT

  /* SMAP-safe: stage in a kernel buffer and copy out. */
  uint8_t kbuf[128];
  memset(kbuf, 0, copy_bytes);
  memcpy(kbuf, &mask, sizeof(mask));
  if (copy_to_user((void *)user_mask_ptr, kbuf, copy_bytes) != 0)
    return (uint64_t)-14; // EFAULT
  return copy_bytes;
}

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define SCHED_FLAG_RESET_ON_FORK 1ULL
#define SCHED_FLAG_KEEP_POLICY 8ULL
#define SCHED_FLAG_KEEP_PARAMS 16ULL
#define SCHED_FLAG_UTIL_CLAMP 96ULL

/* FIFO/RR retain the existing EEVDF compatibility mapping, not RT guarantees. */
static uint64_t process_set_scheduler(struct thread *current,
                                       struct thread *target, uint32_t policy,
                                       int nice, uint32_t priority, bool reset) {
  if (!target)
    return (uint64_t)-3;
  if (policy != SCHED_OTHER && policy != SCHED_BATCH && policy != SCHED_IDLE &&
      policy != SCHED_FIFO && policy != SCHED_RR)
    return policy == 6 ? (uint64_t)-95 : (uint64_t)-22;
  bool rt = policy == SCHED_FIFO || policy == SCHED_RR;
  if ((rt && (priority < 1 || priority > 99)) || (!rt && priority))
    return (uint64_t)-22;
  if (nice < -20) nice = -20;
  if (nice > 19) nice = 19;
  if (!current || (!capability_has(current, CAP_SYS_NICE) &&
      ((current->euid != target->uid && current->euid != target->euid) || rt ||
       nice < target->nice_value || (target->sched_reset_on_fork && !reset))))
    return (uint64_t)-1;
  if (rt) {
    nice = -20 + (99 - priority) / 10;
    if (nice > -10) nice = -10;
  } else if (policy == SCHED_IDLE) {
    nice = 19;
  }
  return sched_set_attributes(target, nice_to_sched_priority(nice), (int8_t)nice,
                               (uint8_t)policy, (uint8_t)priority, reset)
             ? 0 : (uint64_t)-3;
}

static uint64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  int priority;
  if (copy_from_user(&priority, (void *)param_ptr, sizeof(priority)))
    return (uint64_t)-14;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  return process_set_scheduler(current, target, target->sched_policy,
                                target->nice_value, (uint32_t)priority,
                                target->sched_reset_on_fork);
}

static uint64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  struct thread *target = pid == 0 ? sched_get_current() :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  int priority = target->sched_rt_priority;
  return copy_to_user((void *)param_ptr, &priority, sizeof(priority))
             ? (uint64_t)-14 : 0;
}

/* Linux version 0 is 48 bytes; version 1 adds utilization bounds. */
struct linux_sched_attr {
  uint32_t size, policy;
  uint64_t flags;
  int32_t nice;
  uint32_t priority;
  uint64_t runtime, deadline, period;
  uint32_t util_min, util_max;
};
_Static_assert(sizeof(struct linux_sched_attr) == 56, "sched_attr ABI");

static uint64_t sys_sched_setattr(uint64_t pid, uint64_t attr_ptr,
                                  uint64_t flags, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !attr_ptr || (uint32_t)flags)
    return (uint64_t)-22;
  uint32_t size;
  if (copy_from_user(&size, (void *)attr_ptr, sizeof(size)))
    return (uint64_t)-14;
  if (!size) size = 48;
  if (size < 48 || size > 4096) goto bad_size;
  struct linux_sched_attr attr = {0};
  size_t bytes = size < sizeof(attr) ? size : sizeof(attr);
  if (copy_from_user(&attr, (void *)attr_ptr, bytes)) return (uint64_t)-14;
  /* Future extensions may be accepted only if all unknown bytes are zero. */
  for (size_t off = sizeof(attr); off < size;) {
    uint8_t tail[64];
    size_t n = size - off;
    if (n > sizeof(tail)) n = sizeof(tail);
    if (copy_from_user(tail, (void *)(attr_ptr + off), n)) return (uint64_t)-14;
    for (size_t i = 0; i < n; i++) if (tail[i]) goto bad_size;
    off += n;
  }
  if ((int32_t)attr.policy < 0 || (attr.flags & ~127ULL))
    return (uint64_t)-22;
  if ((attr.flags & SCHED_FLAG_UTIL_CLAMP) && size < sizeof(attr))
    return (uint64_t)-22;
  /* No utilization governor or deadline admission control exists yet. */
  if (attr.flags & (2ULL | 4ULL | SCHED_FLAG_UTIL_CLAMP)) return (uint64_t)-95;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  if (attr.flags & SCHED_FLAG_KEEP_POLICY) attr.policy = target->sched_policy;
  if (attr.flags & SCHED_FLAG_KEEP_PARAMS) {
    attr.nice = target->nice_value;
    attr.priority = target->sched_rt_priority;
  }
  return process_set_scheduler(current, target, attr.policy, attr.nice,
                                attr.priority,
                                (attr.flags & SCHED_FLAG_RESET_ON_FORK) != 0);
 bad_size:
  size = sizeof(struct linux_sched_attr);
  if (copy_to_user((void *)attr_ptr, &size, sizeof(size))) return (uint64_t)-14;
  return (uint64_t)-7;
}

static uint64_t sys_sched_getattr(uint64_t pid, uint64_t attr_ptr,
                                  uint64_t size_arg, uint64_t flags,
                                  uint64_t a4, uint64_t a5) {
  (void)a4; (void)a5;
  uint32_t size = (uint32_t)size_arg;
  if ((int32_t)pid < 0 || !attr_ptr || (uint32_t)flags || size < 48 || size > 4096)
    return (uint64_t)-22;
  struct thread *target = pid == 0 ? sched_get_current() :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  struct linux_sched_attr attr = {0};
  attr.size = size < sizeof(attr) ? size : sizeof(attr);
  attr.policy = target->sched_policy;
  attr.nice = target->nice_value;
  attr.priority = target->sched_rt_priority;
  attr.flags = target->sched_reset_on_fork ? SCHED_FLAG_RESET_ON_FORK : 0;
  return copy_to_user((void *)attr_ptr, &attr, attr.size) ? (uint64_t)-14 : 0;
}

static uint64_t sys_sched_get_priority_max(uint64_t policy, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t clean_policy = policy & ~0x40000000ULL;
  if (clean_policy == SCHED_FIFO || clean_policy == SCHED_RR)
    return 99;
  if (clean_policy == SCHED_OTHER || clean_policy == SCHED_BATCH || clean_policy == SCHED_IDLE)
    return 0;
  return (uint64_t)-22; // EINVAL
}

static uint64_t sys_sched_get_priority_min(uint64_t policy, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  uint64_t clean_policy = policy & ~0x40000000ULL;
  if (clean_policy == SCHED_FIFO || clean_policy == SCHED_RR)
    return 1;
  if (clean_policy == SCHED_OTHER || clean_policy == SCHED_BATCH || clean_policy == SCHED_IDLE)
    return 0;
  return (uint64_t)-22; // EINVAL
}

// sys_sched_rr_get_interval (syscall 148)
// Writes the SCHED_RR timeslice quantum for the given pid.  This kernel runs
// an EEVDF scheduler with no fixed per-policy quantum; the reported 100 ms
// matches the tick accounting the scheduler itself uses (1 LAPIC tick ~= 1 ms,
// idle quantum initialised to 100 ticks in sched.c).
static uint64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH
  (void)current;

  if (!tp_ptr || !vmm_is_user_addr_range_writable(tp_ptr, 16))
    return (uint64_t)-14; // EFAULT

  // struct timespec { tv_sec, tv_nsec }: 100 ms quantum.
  ((uint64_t *)tp_ptr)[0] = 0;
  ((uint64_t *)tp_ptr)[1] = 100000000ULL;
  return 0;
}

// sys_times (syscall 100)
// Fills struct tms { utime, stime, cutime, cstime } in clock ticks
// (USER_HZ = 100) and returns elapsed ticks since boot.  User vs system time
// is not tracked separately (getrusage reports the same total as utime) and
// child times are not accumulated (RUSAGE_CHILDREN is zeroed there too).
static uint64_t sys_times(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (buf_ptr) {
    if (!vmm_is_user_addr_range_writable(buf_ptr, 4 * sizeof(long)))
      return (uint64_t)-14; // EFAULT
    long *tms = (long *)buf_ptr;
    tms[0] = (long)(t->runtime_total / 10); // utime: ms -> 100 Hz ticks
    tms[1] = 0;                             // stime: not tracked separately
    tms[2] = 0;                             // cutime: children not accumulated
    tms[3] = 0;                             // cstime: children not accumulated
  }

  return lapic_timer_get_ticks() / 10; // boot-relative ticks at 100 Hz
}

// sys_sched_getscheduler (syscall 145)
// Returns the scheduling policy of the given process.
static uint64_t sys_sched_getscheduler(uint64_t pid, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if ((int32_t)pid < 0) return (uint64_t)-22;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target)
    return (uint64_t)-3; // ESRCH

  return target->sched_policy | (target->sched_reset_on_fork ? 0x40000000U : 0);

}

// sys_sched_setscheduler (syscall 144)
// Sets the scheduling policy and parameters for a process.
// Shares the attribute syscall's validation, policy state and EEVDF mapping.
static uint64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
                                       uint64_t param_ptr, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if ((int32_t)pid < 0 || !param_ptr) return (uint64_t)-22;
  int priority;
  if (copy_from_user(&priority, (void *)param_ptr, sizeof(priority)))
    return (uint64_t)-14;
  struct thread *current = sched_get_current();
  struct thread *target = pid == 0 ? current : sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)pid));
  if (!target) return (uint64_t)-3;
  return process_set_scheduler(current, target, (uint32_t)policy & ~0x40000000U,
                                target->nice_value, (uint32_t)priority,
                                ((uint32_t)policy & 0x40000000U) != 0);
}

// sys_setpriority (syscall 141)
// Sets the nice value for one process and maps it onto 32 kernel priorities.
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

static uint8_t nice_to_sched_priority(int nice);

static uint64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if (which != PRIO_PROCESS) return (uint64_t)-22;
  int nice = (int)(int64_t)prio;
  if (nice < -20) nice = -20;
  if (nice > 19) nice = 19;
  struct thread *current = sched_get_current();
  struct thread *target = who == 0 ? current :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
  if (!target) return (uint64_t)-3;
  if (!capability_has(current, CAP_SYS_NICE) && current->uid != target->uid &&
      current->euid != target->uid) return (uint64_t)-1;
  if (nice < target->nice_value && !capability_has(current, CAP_SYS_NICE))
    return (uint64_t)-13;
  if (!sched_set_priority(target, nice_to_sched_priority(nice), (int8_t)nice))
    return (uint64_t)-3;
  return 0;
}

// sys_getpriority (syscall 140)
// Returns 20 - nice_value (Linux convention: returns value in range 1..40).
static uint8_t nice_to_sched_priority(int nice) {
  return (uint8_t)(((nice + 20) * (SCHED_PRIORITY_LEVELS - 1) + 19) / 39);
}

static uint64_t sys_getpriority(uint64_t which, uint64_t who, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if (which != PRIO_PROCESS) return (uint64_t)-22;
  struct thread *current = sched_get_current();
  struct thread *target = who == 0 ? current :
      sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
  if (!target) return (uint64_t)-3;
  return (uint64_t)(20 - target->nice_value);
}

// ioprio_set (251) / ioprio_get (252).
// There is no block-I/O scheduler here, so priorities are validated for
// Linux-correct errors and otherwise accepted as no-ops; get reports the
// default best-effort/4. baloo_file sets IDLE on its indexer at startup and
// treats anything but success as fatal, so ENOSYS here breaks file indexing.
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_WHO_PGRP 2
#define IOPRIO_WHO_USER 3
#define IOPRIO_CLASS_RT 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_DEFAULT_PRIO ((IOPRIO_CLASS_BE << 13) | 4)

static uint64_t sys_ioprio_set(uint64_t which, uint64_t who, uint64_t ioprio,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
    return (uint64_t)-22;
  uint64_t cls = ioprio >> 13;
  uint64_t data = ioprio & 0x1FFF;
  if (cls > IOPRIO_CLASS_IDLE)
    return (uint64_t)-22;
  if ((cls == IOPRIO_CLASS_RT || cls == IOPRIO_CLASS_BE) && data > 7)
    return (uint64_t)-22;
  struct thread *current = sched_get_current();
  if (which == IOPRIO_WHO_PROCESS && who != 0) {
    struct thread *target = sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who));
    if (!target) return (uint64_t)-3;
    if (current && !capability_has(current, CAP_SYS_NICE) && current->uid != target->uid &&
        current->euid != target->uid) return (uint64_t)-1;
  }
  if (cls == IOPRIO_CLASS_RT && current && !capability_has(current, CAP_SYS_NICE))
    return (uint64_t)-1; // RT I/O class needs privilege, as on Linux
  return 0;
}

static uint64_t sys_ioprio_get(uint64_t which, uint64_t who, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  if (which < IOPRIO_WHO_PROCESS || which > IOPRIO_WHO_USER)
    return (uint64_t)-22;
  if (which == IOPRIO_WHO_PROCESS && who != 0 &&
      !sched_get_thread_by_tid(pid_namespace_global(sched_get_current(), (uint32_t)who)))
    return (uint64_t)-3;
  return (uint64_t)IOPRIO_DEFAULT_PRIO;
}

/* sizeof(struct robust_list_head) on x86-64: list.next, futex_offset and
 * list_op_pending.  Linux rejects every other length with EINVAL. */
#define ROBUST_LIST_HEAD_LEN 24
#define ROBUST_MAX_WALK 4096

static void robust_handle_futex_death(uint64_t futex_uaddr, uint32_t dying_tid) {
  if (!vmm_is_user_addr_range_writable(futex_uaddr, sizeof(uint32_t)))
    return;
  if (futex_uaddr & 3)
    return;
  uint32_t *uaddr = (uint32_t *)futex_uaddr;
  for (;;) {
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0)
      return;
    if ((cur & 0x3fffffff) != dying_tid)
      return; // Not owned by the dying thread.
    uint32_t newv = (cur & 0x80000000) | 0x40000000; // keep WAITERS, set OWNER_DIED
    uint32_t expected = cur;
    int exchanged = cmpxchg_user_u32(uaddr, &expected, newv);
    if (exchanged < 0)
      return;
    if (exchanged) {
      futex_wake_user(uaddr, 1);
      return;
    }
  }
}

static void robust_handle_death(struct thread *t) {
  if (!t)
    return;
  uint64_t head_addr = t->robust_list;
  if (!head_addr)
    return;
  t->robust_list = 0; // Prevent double handling on re-entry.
  // Verified musl ABI (pthread_impl.h: _m_lock=__vi[1] off 4,
  // _m_next=__p[4] off 32; pthread_create.c sets off=-28 and processes
  // private/detached lists in userspace, leaving pshared for the kernel).
  // Sanity range below still guards corrupted heads.
  if (!vmm_is_user_addr_range_valid(head_addr, ROBUST_LIST_HEAD_LEN))
    return;
  uint64_t head_next = 0;
  uint64_t futex_offset_u = 0;
  uint64_t pending = 0;
  if (copy_from_user(&head_next, (const void *)head_addr, 8) != 0)
    return;
  if (copy_from_user(&futex_offset_u, (const void *)(head_addr + 8), 8) != 0)
    return;
  if (copy_from_user(&pending, (const void *)(head_addr + 16), 8) != 0)
    return;
  int64_t futex_offset = (int64_t)futex_offset_u;
  // Corrupted heads (e.g. UAF) yield wild offsets; Linux offsets are tiny
  // (futex word inside the mutex). Refuse to walk instead of CASing heap.
  if (futex_offset < -32768 || futex_offset > 32768)
    return;
  uint32_t dying_tid = t->tid;
  // Lock operation in progress at death: handle it first, like Linux.
  if (pending && pending != head_addr &&
      vmm_is_user_addr_range_valid(pending, 8)) {
    uint64_t futex_uaddr = pending + (uint64_t)futex_offset;
    robust_handle_futex_death(futex_uaddr, dying_tid);
  }
  uint64_t entry = head_next;
  for (int i = 0; i < ROBUST_MAX_WALK && entry && entry != head_addr; i++) {
    if (!vmm_is_user_addr_range_valid(entry, 8))
      break;
    uint64_t next = 0;
    if (copy_from_user(&next, (const void *)entry, 8) != 0)
      break;
    uint64_t futex_uaddr = entry + (uint64_t)futex_offset;
    robust_handle_futex_death(futex_uaddr, dying_tid);
    entry = next;
  }
}

static uint64_t sys_set_robust_list(uint64_t head, uint64_t len, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-22; // EINVAL

  if (len != ROBUST_LIST_HEAD_LEN)
    return (uint64_t)-22; // EINVAL
  if (head && !vmm_is_user_addr_range_valid(head, len))
    return (uint64_t)-14; // EFAULT

  current->robust_list = head;
  return 0;
}

// get_robust_list(pid, &head, &len): read back the list registered with
// set_robust_list(2).  baloo_file, gdb and friends ask for it on startup;
// pid 0 refers to the calling thread.
static uint64_t sys_get_robust_list(uint64_t pid_val, uint64_t head_ptr,
                                    uint64_t len_ptr, uint64_t a3, uint64_t a4,
                                    uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-22; // EINVAL

  if (!head_ptr || !len_ptr ||
      !vmm_is_user_addr_range_writable(head_ptr, sizeof(uint64_t)) ||
      !vmm_is_user_addr_range_writable(len_ptr, sizeof(uint64_t)))
    return (uint64_t)-14; // EFAULT

  int32_t pid = (int32_t)pid_val;
  if (pid < 0)
    return (uint64_t)-22; // EINVAL

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  uint64_t head = 0;
  bool found = false;

  spinlock_acquire(&tid_lock);
  if (pid == 0) {
    head = current->robust_list;
    found = true;
  } else {
    // A tid, not a tgid: robust lists are per thread.
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (pid_namespace_tid(current, t) != (uint32_t)pid)
        continue;
      // Linux asks for ptrace_may_access(); same owner is the closest match.
      if (!capability_has(current, CAP_SYS_PTRACE) && t->uid != current->uid &&
          t->euid != current->euid)
        break;
      head = t->robust_list;
      found = true;
      break;
    }
  }
  spinlock_release(&tid_lock);

  if (!found)
    return (uint64_t)-3; // ESRCH

  uint64_t len = ROBUST_LIST_HEAD_LEN;
  copy_to_user((void *)head_ptr, &head, sizeof(head));
  copy_to_user((void *)len_ptr, &len, sizeof(len));
  return 0;
}

// ---------------------------------------------------------------------------
// sys_reboot — syscall 169, mirrors Linux reboot(2)
// ---------------------------------------------------------------------------

// Linux reboot(2) magic values
#define LINUX_REBOOT_MAGIC1       0xfee1dead
#define LINUX_REBOOT_MAGIC2       0x28121969
#define LINUX_REBOOT_MAGIC2A      0x05121996
#define LINUX_REBOOT_MAGIC2B      0x16041998
#define LINUX_REBOOT_MAGIC2C      0x20112000

// reboot commands
#define LINUX_REBOOT_CMD_RESTART       0x01234567
#define LINUX_REBOOT_CMD_HALT          0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF     0x4321fedc
#define LINUX_REBOOT_CMD_RESTART2      0xa1b2c3d4
#define LINUX_REBOOT_CMD_CAD_ON        0x89abcdef
#define LINUX_REBOOT_CMD_CAD_OFF       0x00000000
#define LINUX_REBOOT_CMD_KEXEC         0x45584543

static uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd,
                           uint64_t arg, uint64_t a4, uint64_t a5) {
  (void)arg;
  (void)a4;
  (void)a5;

  if (!capability_has(sched_get_current(), 22)) return (uint64_t)-1;

  // Both magic values must match (as on Linux)
  if (magic1 != LINUX_REBOOT_MAGIC1)
    return (uint64_t)-22; // EINVAL
  if (magic2 != LINUX_REBOOT_MAGIC2  && magic2 != LINUX_REBOOT_MAGIC2A &&
      magic2 != LINUX_REBOOT_MAGIC2B && magic2 != LINUX_REBOOT_MAGIC2C)
    return (uint64_t)-22; // EINVAL

  switch ((uint32_t)cmd) {
  case LINUX_REBOOT_CMD_RESTART:
  case LINUX_REBOOT_CMD_RESTART2:
    klog_puts("[REBOOT] System reboot requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown(); /* graceful CC.SHN before resetting the machine */
    acpi_reboot();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_POWER_OFF:
    klog_puts("[REBOOT] System power-off requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown(); /* graceful CC.SHN before power is removed */
    acpi_poweroff();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_HALT:
    klog_puts("[REBOOT] System halt requested via syscall.\n");
    vmm_defer_sync();
#ifdef LINUXKPI_LINUX_IMPORTED
    linuxkpi_pci_shutdown();
#endif
    e1000_shutdown();
    nvme_shutdown();
    hal_irq_disable();
    for (;;) hal_cpu_halt();
    break;

  case LINUX_REBOOT_CMD_CAD_ON:
  case LINUX_REBOOT_CMD_CAD_OFF:
    // Ctrl-Alt-Delete enable/disable -- no-op for now
    return 0;

  default:
    return (uint64_t)-22; // EINVAL
  }

  /* Should never reach here */
  return 0;
}

// ---------------------------------------------------------------------------
// init_module / delete_module / swapon: no-ops with honest errors.
// This kernel has no loadable-module support and no swap activation, so
// after validating arguments these fail without side effects:
//   init_module   -> EPERM  (loading is not implemented)
//   delete_module -> ENOENT (no modules are ever loaded)
//   swapon        -> EPERM  (swap areas cannot be activated)
// ---------------------------------------------------------------------------

static uint64_t sys_init_module(uint64_t image_ptr, uint64_t len,
                                uint64_t param_ptr, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!image_ptr)
    return (uint64_t)-14; // EFAULT
  if (!vmm_is_user_addr_range_valid(image_ptr, len ? 1 : 0))
    return (uint64_t)-14; // EFAULT
  if (param_ptr && !vmm_is_user_addr_range_valid(param_ptr, 1))
    return (uint64_t)-14; // EFAULT
  return (uint64_t)-1; // EPERM: module loading is not supported
}

static uint64_t sys_delete_module(uint64_t name_ptr, uint64_t flags,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!name_ptr || !vmm_is_user_addr_range_valid(name_ptr, 1))
    return (uint64_t)-14; // EFAULT
  // Linux delete_module flags: O_NONBLOCK | O_TRUNC.
  if (flags & ~(uint64_t)(0x800 | 0x200))
    return (uint64_t)-22; // EINVAL
  return (uint64_t)-2; // ENOENT: no modules are loaded, no name can match
}

static uint64_t sys_swapon(uint64_t path_ptr, uint64_t flags, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (!path_ptr || !vmm_is_user_addr_range_valid(path_ptr, 1))
    return (uint64_t)-14; // EFAULT
  return (uint64_t)-1; // EPERM: swap activation is not supported
}

// ---------------------------------------------------------------------------
// Capability operations use per-thread credentials and enforce irreversible drops.
static uint64_t sys_capget(uint64_t h, uint64_t d, uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
  (void)a; (void)b; (void)c; (void)e; return capability_get(h, d);
}
static uint64_t sys_capset(uint64_t h, uint64_t d, uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
  (void)a; (void)b; (void)c; (void)e; return capability_set(h, d);
}

// sys_seccomp (317)
//
// Verified classic-BPF filter installation and feature discovery.
// ---------------------------------------------------------------------------

#define SECCOMP_SET_MODE_STRICT    0
#define SECCOMP_SET_MODE_FILTER    1
#define SECCOMP_GET_ACTION_AVAIL   2

static uint64_t sys_seccomp(uint64_t op, uint64_t flags, uint64_t uargs,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a4; (void)a5; (void)a6;
  return (uint64_t)seccomp_control((uint32_t)op, (uint32_t)flags, uargs);
}

/* Explicit unsupported Landlock ABI, including version queries. Never
 * advertise an ABI version or return success without enforcing the rules. */
static uint64_t sys_landlock_unsupported(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  return (uint64_t)-38; /* ENOSYS */
}

// Namespace preparation is transactional: commit only after all allocations succeed.
static uint64_t sys_unshare(uint64_t flags, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  uint64_t known = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
                   CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
                   CLONE_NEWNET | CLONE_NEWTIME | CLONE_SYSVSEM |
                   CLONE_FILES | CLONE_FS;
  if (flags & ~known) return (uint64_t)-22;
  if (flags & (CLONE_FILES | CLONE_FS | CLONE_SYSVSEM)) return (uint64_t)-95;
  if (flags & (CLONE_NEWCGROUP | CLONE_NEWUSER | CLONE_NEWTIME))
    return (uint64_t)-1;
  if (!flags) return 0;
  struct thread *current = sched_get_current();
  if (!capability_has(current, CAP_SYS_ADMIN)) return (uint64_t)-1;
  struct uts_namespace *uts __attribute__((cleanup(uts_namespace_cleanup))) = NULL;
  struct ipc_namespace *ipc __attribute__((cleanup(ipc_namespace_cleanup))) = NULL;
  struct mount_namespace *mount __attribute__((cleanup(process_mount_cleanup))) = NULL;
  struct pid_namespace *pid __attribute__((cleanup(pid_namespace_cleanup))) = NULL;
  struct net_namespace *net __attribute__((cleanup(net_namespace_cleanup))) = NULL;
  if (flags & CLONE_NEWNET) { net = net_namespace_create(); if (!net) return (uint64_t)-12; }
  if (flags & CLONE_NEWUTS) { uts = uts_namespace_copy(current); if (!uts) return (uint64_t)-12; }
  if (flags & CLONE_NEWIPC) { ipc = ipc_namespace_create(); if (!ipc) return (uint64_t)-12; }
  if (flags & CLONE_NEWNS) { mount = vfs_namespace_prepare(current); if (!mount) return (uint64_t)-12; }
  if (flags & CLONE_NEWPID) { pid = pid_namespace_create(current); if (!pid) return (uint64_t)-12; }
  if (net) { net_namespace_install(current, net); net = NULL; }
  if (uts) { uts_namespace_install(current, uts); uts = NULL; }
  if (ipc) { ipc_namespace_install(current, ipc); ipc = NULL; }
  if (mount) { vfs_namespace_install(current, mount); mount = NULL; }
  if (pid) { pid_namespace_unshare(current, pid); pid = NULL; }
  return 0;
}

void syscall_register_process(void) {

  syscall_register(SYS_EXIT, sys_exit);
  syscall_register(SYS_EXIT_GROUP, sys_exit_group);
  syscall_register(SYS_SET_TID_ADDRESS, sys_set_tid_address);
  syscall_register(SYS_GETTID, sys_gettid);
  syscall_register(SYS_GETPID, sys_getpid);
  syscall_register(SYS_WAIT4, sys_wait4);
  syscall_register(SYS_WAITID, sys_waitid);
  syscall_register(SYS_UNAME, sys_uname);
  syscall_register(SYS_SETHOSTNAME, sys_sethostname);
  syscall_register(SYS_SETDOMAINNAME, sys_setdomainname);
  syscall_register(SYS_SYSINFO, sys_sysinfo);
  syscall_register(SYS_UPTIME, sys_uptime);
  syscall_register(SYS_GETCWD, sys_getcwd);
  syscall_register(SYS_CHDIR, sys_chdir);
  syscall_register(SYS_PRCTL, sys_prctl);
  syscall_register(SYS_SETITIMER, sys_setitimer);
  syscall_register(SYS_ALARM, sys_alarm);
  syscall_register_raw(SYS_FORK, sys_fork);
  syscall_register_raw(SYS_VFORK, sys_vfork);
  syscall_register_raw(SYS_CLONE, sys_clone);
  syscall_register_raw(SYS_CLONE3, sys_clone3);
  syscall_register_raw(SYS_EXECVE, sys_execve);
  syscall_register_raw(SYS_UMASK, sys_umask);
  syscall_register_raw(SYS_RSEQ, sys_rseq);
  syscall_register_raw(SYS_GETUID, sys_getuid);
  syscall_register_raw(SYS_GETGID, sys_getgid);
  syscall_register_raw(SYS_GETEUID, sys_geteuid);
  syscall_register_raw(SYS_GETEGID, sys_getegid);
  syscall_register_raw(SYS_GETRESUID, sys_getresuid);
  syscall_register_raw(SYS_GETRESGID, sys_getresgid);
  syscall_register_raw(SYS_SETRESUID, sys_setresuid);
  syscall_register_raw(SYS_SETRESGID, sys_setresgid);
  syscall_register_raw(SYS_GETPPID, sys_getppid);
  syscall_register_raw(SYS_SETPGID, sys_setpgid);
  syscall_register_raw(SYS_GETPGRP, sys_getpgrp);
  syscall_register(SYS_GETPGID, sys_getpgid);
  syscall_register(SYS_GETSID, sys_getsid);
  syscall_register_raw(SYS_SETSID, sys_setsid);
  syscall_register_raw(SYS_SETUID, sys_setuid);
  syscall_register_raw(SYS_SETGID, sys_setgid);
  syscall_register_raw(SYS_SETREUID, sys_setreuid);
  syscall_register_raw(SYS_SETREGID, sys_setregid);
  syscall_register_raw(SYS_GETGROUPS, sys_getgroups);
  syscall_register_raw(SYS_SETGROUPS, sys_setgroups);
  syscall_register_raw(SYS_SETFSUID, sys_setfsuid);
  syscall_register_raw(SYS_SETFSGID, sys_setfsgid);
  syscall_register(SYS_GETRLIMIT, sys_getrlimit);
  syscall_register(SYS_SETRLIMIT, sys_setrlimit);
  syscall_register(SYS_GETRUSAGE, sys_getrusage);
  syscall_register_raw(SYS_PRLIMIT64, sys_prlimit64);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
  syscall_register(SYS_SCHED_YIELD, sys_sched_yield);
  syscall_register(SYS_SCHED_GETAFFINITY, sys_sched_getaffinity);
  syscall_register(SYS_SCHED_SETAFFINITY, sys_sched_setaffinity);
  syscall_register(SYS_SCHED_GETPARAM, sys_sched_getparam);
  syscall_register(SYS_SCHED_SETPARAM, sys_sched_setparam);
  syscall_register(SYS_SCHED_SETATTR, sys_sched_setattr);
  syscall_register(SYS_SCHED_GETATTR, sys_sched_getattr);
  syscall_register(SYS_SCHED_GET_PRIORITY_MAX, sys_sched_get_priority_max);
  syscall_register(SYS_SCHED_GET_PRIORITY_MIN, sys_sched_get_priority_min);
  syscall_register(SYS_SCHED_RR_GET_INTERVAL, sys_sched_rr_get_interval);
  syscall_register(SYS_TIMES, sys_times);
  syscall_register(SYS_SCHED_GETSCHEDULER, sys_sched_getscheduler);
  syscall_register(SYS_SCHED_SETSCHEDULER, sys_sched_setscheduler);
  syscall_register(SYS_SETPRIORITY, sys_setpriority);
  syscall_register(SYS_GETPRIORITY, sys_getpriority);
  syscall_register(SYS_IOPRIO_SET, sys_ioprio_set);
  syscall_register(SYS_IOPRIO_GET, sys_ioprio_get);
  syscall_register(SYS_SET_ROBUST_LIST, sys_set_robust_list);
  syscall_register(SYS_GET_ROBUST_LIST, sys_get_robust_list);
  syscall_register(SYS_REBOOT, sys_reboot);
  syscall_register(SYS_INIT_MODULE, sys_init_module);
  syscall_register(SYS_DELETE_MODULE, sys_delete_module);
  syscall_register(SYS_SWAPON, sys_swapon);
  syscall_register(SYS_PIDFD_OPEN, sys_pidfd_open);
  syscall_register(SYS_PIDFD_SEND_SIGNAL, sys_pidfd_send_signal);
  syscall_register(SYS_PIDFD_GETFD, sys_pidfd_getfd);
  syscall_register(SYS_GETCPU, sys_getcpu);
  syscall_register(SYS_CAPGET, sys_capget);
  syscall_register(SYS_CAPSET, sys_capset);
  syscall_register(SYS_SECCOMP, sys_seccomp);
  syscall_register(SYS_LANDLOCK_CREATE_RULESET, sys_landlock_unsupported);
  syscall_register(SYS_LANDLOCK_ADD_RULE, sys_landlock_unsupported);
  syscall_register(SYS_LANDLOCK_RESTRICT_SELF, sys_landlock_unsupported);
  syscall_register(SYS_UNSHARE, sys_unshare);
}
