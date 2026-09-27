// Process Syscalls: fork, getpid, exit
#include "../acpi/acpi.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../drivers/storage/nvme.h"
#include "../drivers/timer/rtc.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"

#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../smp/cpu.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

// Wake waiters after CLONE_CHILD_CLEARTID/set_tid_address exit cleanup.
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count);

extern void mm_reset_mmap_state(struct thread *t);
void vma_list_init(struct vma_list *list);
void vma_list_destroy(struct vma_list *list);
static uint8_t nice_to_sched_priority(int nice);

#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define MMAP_REGION_BASE 0x7F0000000000ULL

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

/* A pidfd is recognised by its poll handler.  Without this check any descriptor
 * whose node happens to carry a ->device (a pty, a drm client, ...) would be
 * read as a struct pidfd_ctx by waitid(P_PIDFD), turning a stray file descriptor
 * into a wait on an arbitrary process. */
static struct pidfd_ctx *pidfd_ctx_of(vfs_node_t *node) {
  if (!node || node->poll != pidfd_poll || !node->device)
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
  bool exited = false;
  if (!pidfd_task_state((uint32_t)pid, &exited)) return (uint64_t)-3;
  return pidfd_install(sched_get_current(), (uint32_t)pid, flags);
}

// exit / exit_group (shared)
void process_do_exit(uint64_t status) __attribute__((noreturn));
void process_do_exit(uint64_t status) {
  struct thread *current = sched_get_current();
  if (current && current->is_forked_child) {
    klog_proc_exit(current->tid, current->tgid,
                   (current->clone_flags & CLONE_THREAD) != 0,
                   current->comm, status);
  }
  if (current && current->tid_address) {
    uint32_t *tidptr = (uint32_t *)current->tid_address;
    if (vmm_is_user_addr_range_writable((uint64_t)tidptr, sizeof(*tidptr))) {
      __atomic_store_n(tidptr, 0, __ATOMIC_RELEASE);
      uint64_t woken = futex_wake_user(tidptr, 1);
      klog_puts("[PROC] clear_child_tid tid=");
      klog_uint64(current->tid);
      klog_puts(" addr=");
      klog_hex64((uint64_t)tidptr);
      klog_puts(" woken=");
      klog_uint64(woken);
      klog_puts("\n");
    } else {
      klog_puts("[PROC] clear_child_tid tid=");
      klog_uint64(current->tid);
      klog_puts(" addr=");
      klog_hex64((uint64_t)tidptr);
      klog_puts(" NOT WRITABLE\n");
    }
    current->tid_address = NULL;
  }

  if (current)
    sched_reparent_children(current);

  if (current && !current->is_main_session) {
    klog_debug_puts("\n[SYSCALL] Process exited with status: ");
    klog_debug_uint64(status);
    klog_debug_puts("\n");
  }

  klog_debug_puts("[EXITDBG] begin cleanup\n");

  // Common cleanup. CLONE_FILES tables remain alive until the final thread
  // drops its reference; closing every fd on each pthread exit would break
  // descriptors still in use by its siblings.
  if (current) {
    sched_release_files(current);
    klog_debug_puts("[EXITDBG] files released\n");

    if (current->cwd_node) {
      vfs_close(current->cwd_node);
      current->cwd_node = NULL;
    }
    klog_debug_puts("[EXITDBG] cwd released\n");
  }

  if (current && current->is_forked_child) {
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
      current->state = THREAD_ZOMBIE;
      if (current->parent) {
        uint32_t parent_tgid = current->parent->tgid;
        for (struct thread *waiter = global_thread_list; waiter;
             waiter = waiter->global_next) {
          if (waiter->tgid == parent_tgid && waiter->waiting_for_child &&
              waiter->state == THREAD_BLOCKED) {
            parent_to_wake = waiter;
            break;
          }
        }
      }
      spinlock_release(&tid_lock);
      klog_debug_puts("[EXITDBG] zombie published\n");
      /* Anyone waiting on a pidfd for this process (poll or waitid(P_PIDFD))
       * is woken here; they re-scan and observe the zombie. */
      pidfd_wake_waiters();
      /* ... and the parent's SIGCHLD handler runs here, the way Linux sends
       * do_notify_parent() right after publishing the zombie. */
      signal_notify_parent_exit(current);
    }

    /* A vfork parent is blocked inside sys_clone_internal(), before it can
     * enter wait4().  Publish the zombie before releasing it so an immediate
     * waitpid() can observe and reap this child. */
    if (current->clone_flags & CLONE_VFORK) {
      if (current->parent && current->parent->state == THREAD_BLOCKED)
        sched_wakeup(current->parent);
      current->clone_flags &= ~CLONE_VFORK;
    }

    if (parent_to_wake) {
      klog_debug_puts("[EXITDBG] waking parent\n");
      sched_wakeup(parent_to_wake);
      klog_debug_puts("[EXITDBG] parent wake returned\n");
    }

    // Reaping handles the address-space reference after this task is off-CPU.
    if (current->cr3) {
      struct cpu_info *cpu = cpu_get_current();
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
  struct thread *cur = sched_get_current();
  if (cur) {
    klog_puts("[PROC] sys_exit: tid=");
    klog_uint64(cur->tid);
    klog_puts(" tgid=");
    klog_uint64(cur->tgid);
    klog_puts(" comm=");
    klog_puts(cur->comm[0] ? cur->comm : "?");
    klog_puts(" status=");
    klog_uint64(status);
    klog_puts("\n");
  }
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
  struct thread *cur = sched_get_current();
  if (cur) {
    klog_puts("[PROC] sys_exit_group: tid=");
    klog_uint64(cur->tid);
    klog_puts(" tgid=");
    klog_uint64(cur->tgid);
    klog_puts(" comm=");
    klog_puts(cur->comm[0] ? cur->comm : "?");
    klog_puts(" status=");
    klog_uint64(status);
    klog_puts("\n");
  }
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
  return current->tid;
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
  return current ? current->tid : 0;
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
    return current->tgid;
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
  vfs_node_t *node = vfs_resolve_path_at(fs_root, new_path);
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
          if (t->tid == (uint32_t)target_pid || t->tgid == (uint32_t)target_pid)
            matches = true;
        } else if (target_pid == 0) {
          if (t->pgid == current->pgid)
            matches = true;
        } else { // target_pid < -1
          if (t->pgid == (uint32_t)(-target_pid))
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
        *wstatus = (zombie->exit_status & 0xFF) << 8;
      }
      uint32_t reaped_pid = zombie->tid;

      // Destruction must wait until the exiting child has switched off its
      // kernel stack.  On SMP the awakened parent can run concurrently with
      // process_do_exit(), so direct reaping here would free a live stack.
      sched_queue_reap_and_wait(zombie);

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
        (id < MAX_FDS && current->fds[id]) ? pidfd_ctx_of(current->fds[id]) : NULL;
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
        case P_PID:   matches = (t->tid == id || t->tgid == id); break;
        case P_PGID:  matches = (t->pgid == id); break;
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
        si->si_code   = CLD_EXITED;
        si->si_pid    = (uint32_t)zombie->tid;
        si->si_uid    = (uint32_t)zombie->uid;
        si->si_status = zombie->exit_status & 0xFF;
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
      bool on_pidfd_wq = false;

      if (idtype == P_PIDFD) {
        wait_queue_add(&pidfd_event_wait, &pidfd_entry);
        on_pidfd_wq = true;
        current->wakeup_ticks = lapic_timer_get_ticks() + 10;
      }

      sched_yield();

      if (on_pidfd_wq) {
        wait_queue_remove(&pidfd_event_wait, &pidfd_entry);
        current->wakeup_ticks = 0;
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

  for (int fd = 0; fd < MAX_FDS; fd++) {
    spinlock_acquire(&t->files->lock);
    vfs_node_t *node = t->fds[fd];
    if (!node || node == FD_RESERVED ||
        !(t->fd_flags[fd] & FD_FLAGS_CLOEXEC_BIT)) {
      spinlock_release(&t->files->lock);
      continue;
    }

    t->fds[fd] = NULL;
    t->fd_offsets[fd] = 0;
    t->fd_flags[fd] = 0;
    fd_path_clear(t, fd);
    if ((uint32_t)fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    vfs_close(node);
  }
}

// sys_execve
#define TSC_PROBES_ENABLE
#include "../lib/tsc.h"

static uint64_t sys_execve(struct syscall_regs *regs) {
  const char **user_argv = (const char **)regs->rsi;
  const char **user_envp = (const char **)regs->rdx;
  const char *user_path = (const char *)regs->rdi;

  if (!user_path)
    return (uint64_t)-14; // EFAULT

  // 1. Copy everything to kernel memory BEFORE switching CR3
  // We need to copy the path, the argv array, and the envp array
  size_t path_len = strlen(user_path);
  char *path = kmalloc(path_len + 1);
  if (!path)
    return (uint64_t)-12;
  memcpy(path, user_path, path_len + 1);

  TSC_BEGIN(execve_arg_copy);
  int argc = 0;
  if (user_argv)
    while (user_argv[argc])
      argc++;
  char **k_argv = kmalloc((argc + 1) * sizeof(char *));
  if (!k_argv) {
    kfree(path);
    return (uint64_t)-12;
  }
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(user_argv[i]);
    k_argv[i] = kmalloc(len + 1);
    memcpy(k_argv[i], user_argv[i], len + 1);
  }
  k_argv[argc] = NULL;

  int envc = 0;
  if (user_envp)
    while (user_envp[envc])
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
    size_t len = strlen(user_envp[i]);
    k_envp[i] = kmalloc(len + 1);
    memcpy(k_envp[i], user_envp[i], len + 1);
  }
  k_envp[envc] = NULL;
  TSC_END(execve_arg_copy);

  // Shebang (#!) script interpretation loop (up to 4 levels of recursion)
  struct thread *exec_thread = sched_get_current();
  int shebang_depth = 0;
  uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;

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

  if (exec_mode & 04000)
    current->euid = current->suid = current->fsuid = exec_uid;
  if (exec_mode & 02000)
    current->egid = current->sgid = current->fsgid = exec_gid;

  current->fs_base = 0;
  current->gs_base = 0;

  // Reset custom signal handlers to SIG_DFL after exec; preserve SIG_IGN (POSIX requirement).
  for (int i = 0; i < 64; i++) {
    if ((uint64_t)current->signal_handlers[i].sa_handler != (uint64_t)SIG_IGN) {
      memset(&current->signal_handlers[i], 0, sizeof(struct k_sigaction));
    }
  }
  current->pending_signals = 0;
  memset(current->signal_sender_pid, 0, sizeof(current->signal_sender_pid));

  // The old address space is gone, so any robust list registered in it is too.
  current->robust_list = 0;

  // We are now safely loaded into the new address space!
  TSC_BEGIN(execve_stack);
  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, path, (const char **)k_argv,
      (const char **)k_envp, &elf_info);
  TSC_END(execve_stack);

  /* Probes are cumulative (fork stats must survive to be averaged), so print
   * the first few launches; fork_probe_maybe_dump() covers later activity. */
  {
    static uint32_t exec_probe_dumps;
    if (exec_probe_dumps < 4) {
      exec_probe_dumps++;
      tsc_probe_dump();
    }
  }

  // Do not release a vfork parent until exec has actually succeeded. The
  // child needs the shared address space intact to handle ENOEXEC fallbacks.
  if (current->clone_flags & CLONE_VFORK) {
    if (current->parent && current->parent->state == THREAD_BLOCKED)
      sched_wakeup(current->parent);
    current->clone_flags &= ~CLONE_VFORK;
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
  }

  // Store the full executable path for /proc/self/exe
  strncpy(current->exe_path, path, sizeof(current->exe_path) - 1);
  current->exe_path[sizeof(current->exe_path) - 1] = '\0';

  // Map the vsyscall page into the new address space
  vmm_map_vsyscall_page((uint64_t *)current->cr3);

  // Free the old address space (from fork) now that the new one is loaded.
  // We've already switched CR3, so this is safe.
  // We only free it if it was NOT shared (i.e. not a vfork/thread exec).
  if (old_cr3 != 0 && !shared_mm) {
    TSC_BEGIN(exec_free_old);
    vmm_free_user_pages_vma(old_cr3, &old_vmas);
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

  // Switch to the child's cloned address space
  __asm__ volatile("mov %0, %%cr3" ::"r"(self->cr3) : "memory");

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Restore user TLS bases — child inherits parent's FS_BASE (musl needs TLS)
  wrmsr(IA32_KERNEL_GS_BASE, self->gs_base);
  wrmsr(IA32_FS_BASE, self->fs_base);

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
}

// sys_fork (raw handler — receives full register frame)
uint64_t sys_fork(struct syscall_regs *regs) {
  // 1. Get current parent state
  uint64_t *parent_pml4_phys = vmm_get_active_pml4();
  struct thread *parent = sched_get_current();

  // 2. Clone the user address space with VMA awareness
  //    Shared mappings share physical pages, private mappings get copied
  TSC_BEGIN(fork_clone);
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(
      parent_pml4_phys, (parent && parent->mm) ? &parent->mm->vmas : NULL);
  TSC_END(fork_clone);
  fork_probe_maybe_dump();

  // 3. Allocate and populate the child's saved register state.
  //    RAX = 0 so the child sees fork() returning 0.
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    if (child_cr3) {
      vmm_free_user_pages_vma(
          child_cr3, (parent && parent->mm) ? &parent->mm->vmas : NULL);
    }
    return (uint64_t)(-12);
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  // 4. Create a kernel thread for the child process.
  //    sched_create_kernel_thread enqueues it on a CPU's run queue.
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, cpu_get_current(), false);
  if (!child) {
    kfree(child_regs);
    if (child_cr3) {
      vmm_free_user_pages_vma(
          child_cr3, (parent && parent->mm) ? &parent->mm->vmas : NULL);
    }
    return (uint64_t)(-12);
  }

  // Clear or free the default MM allocated by sched_create_kernel_thread
  // as we're about to replace it with a cloned one.
  if (child->mm) {
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
    child->mm = NULL;
  }

  // 5. Configure child thread
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->cpu_affinity = parent->cpu_affinity;
  child->priority = parent->static_priority;
  child->static_priority = parent->static_priority;
  child->nice_value = parent->nice_value;
  eevfd_set_nice(&child->se, (int)child->nice_value);
  child->tgid = child->tid; // Fork creates a new process (new thread group)
  /* Linux implements fork() as a clone whose exit signal is SIGCHLD, and the
   * parent is notified through it.  Keep that visible here so the notification
   * does not depend on which entry point created the child. */
  child->clone_flags = SIGCHLD;

  // 6. Copy file descriptors from parent to child (with reference counting)
  if (parent) {
    for (int i = 0; i < MAX_FDS; i++) {
      if (parent->fds[i] && parent->fds[i] != FD_RESERVED) {
        child->fds[i] = parent->fds[i];
        child->fd_offsets[i] = parent->fd_offsets[i];
        child->fd_flags[i] = parent->fd_flags[i];
        if (parent->fd_paths[i]) {
          child->fd_paths[i] = parent->fd_paths[i];
          __atomic_add_fetch(&child->fd_paths[i]->ref_count, 1,
                             __ATOMIC_RELAXED);
        }
        // Increment reference count for each inherited FD
        vfs_open(child->fds[i]);
      }
    }
    // 7. Clone memory state
    child->mm = kmalloc(sizeof(struct mm_struct));
    if (child->mm) {
      vma_list_init(&child->mm->vmas);
      vma_list_clone(&child->mm->vmas, &parent->mm->vmas);
      child->mm->brk_base = parent->mm->brk_base;
      child->mm->brk_current = parent->mm->brk_current;
      child->mm->mmap_next_addr = parent->mm->mmap_next_addr;
      child->mm->ref_count = 1;
      child->mm->pcid = pcid_alloc();
      spinlock_init(&child->mm->lock);
      memcpy(child->mm->saved_auxv, parent->mm->saved_auxv,
             sizeof(child->mm->saved_auxv));
      child->mm->auxv_count = parent->mm->auxv_count;
      memcpy(child->mm->saved_cmdline, parent->mm->saved_cmdline,
             sizeof(child->mm->saved_cmdline));
      child->mm->cmdline_len = parent->mm->cmdline_len;
      child->mm->arg_start   = parent->mm->arg_start;
      child->mm->arg_end     = parent->mm->arg_end;
    }

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

    // Inherit alternate signal stack
    child->ss_sp = parent->ss_sp;
    child->ss_size = parent->ss_size;
    child->ss_flags = parent->ss_flags;
  }

  // 8. Enqueue child thread now that it is fully configured
  sched_enqueue_thread(child, cpu_get_current());

  // 9. Return child PID to parent
  return child->tid;
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
    vma_list_clone(&child_mm->vmas, &parent->mm->vmas);
    child_mm->brk_base = parent->mm->brk_base;
    child_mm->brk_current = parent->mm->brk_current;
    child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
    child_mm->ref_count = 1;
    child_mm->pcid = pcid_alloc();
    spinlock_init(&child_mm->lock);
  }

  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, cpu_get_current(), false);
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
  child->priority = parent->static_priority;
  child->static_priority = parent->static_priority;
  child->nice_value = parent->nice_value;
  eevfd_set_nice(&child->se, (int)child->nice_value);
  child->clone_flags = flags;
  child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->tid;
  child->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;
  child->gs_base = parent->gs_base;
  child->tid_address =
      (flags & CLONE_CHILD_CLEARTID) ? (uint64_t *)ctid : NULL;
  // A new task starts from the parent's robust list, exactly like Linux; a
  // fresh thread installs its own through set_robust_list(2).
  child->robust_list = parent->robust_list;

  if (flags & CLONE_FILES) {
    sched_share_files(child, parent);
  } else {
    for (int i = 0; i < MAX_FDS; i++) {
      /* FD_RESERVED is a descriptor another thread of this process is in the
       * middle of allocating (including the pidfd reserved for CLONE_PIDFD
       * below); it is not a file, and it must not reach the child. */
      if (!parent->fds[i] || parent->fds[i] == FD_RESERVED)
        continue;
      child->fds[i] = parent->fds[i];
      child->fd_offsets[i] = parent->fd_offsets[i];
      child->fd_flags[i] = parent->fd_flags[i];
      if (parent->fd_paths[i]) {
        child->fd_paths[i] = parent->fd_paths[i];
        __atomic_add_fetch(&child->fd_paths[i]->ref_count, 1,
                           __ATOMIC_RELAXED);
      }
      vfs_open(child->fds[i]);
    }
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

  if (flags & CLONE_PARENT_SETTID)
    __atomic_store_n((uint32_t *)ptid, child->tid, __ATOMIC_RELEASE);
  if ((flags & CLONE_CHILD_SETTID) && (flags & CLONE_VM))
    __atomic_store_n((uint32_t *)ctid, child->tid, __ATOMIC_RELEASE);
  if (pidfd_fd >= 0) {
    /* The child has copied the descriptor table by now, so the pidfd is only
     * the parent's.  Publish it before the child runs: the whole point of a
     * pidfd is to be able to watch a process that may already be gone. */
    pidfd_publish(parent, pidfd_fd, pidfd_node, child->tid, 0);
    int pidfd_user = pidfd_fd;
    copy_to_user((void *)pidfd_slot, &pidfd_user, sizeof(int));
  }
  bool vfork = (flags & CLONE_VFORK) != 0;
  if (vfork)
    parent->state = THREAD_BLOCKED;

  /* Publication is the final creation step. */
  sched_enqueue_thread(child, cpu_get_current());

  if (vfork) {
    while (parent->state == THREAD_BLOCKED)
      sched_yield();
  }

  return child->tid;
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

// sys_vfork (syscall 58)
uint64_t sys_vfork(struct syscall_regs *regs) {
  // vfork is essentially clone with shared VM and parent blocking.
  // Standard flags: CLONE_VM | CLONE_VFORK | SIGCHLD
  return sys_clone_internal(regs, CLONE_VM | CLONE_VFORK | 17, 0, 0, 0, 0, 0);
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
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
  char _f[8]; // padding to 112 bytes (Linux ABI)
} __attribute__((packed));

static uint64_t sys_sysinfo(uint64_t info_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct sysinfo *info = (struct sysinfo *)info_ptr;
  if (!info)
    return (uint64_t)-14; // EFAULT

  info->uptime = (int64_t)(lapic_timer_get_ms() / 1000);
  info->loads[0] = 0;
  info->loads[1] = 0;
  info->loads[2] = 0;
  info->totalram = pmm_get_total_memory();
  info->freeram = (uint64_t)pmm_get_free_pages() * PAGE_SIZE;
  info->sharedram = 0;
  info->bufferram = 0;
  info->totalswap = 0;
  info->freeswap = 0;
  info->procs = sched_get_thread_count();
  info->totalhigh = 0;
  info->freehigh = 0;
  info->mem_unit = 1;
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

static uint64_t sys_uname(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct utsname *buf = (struct utsname *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  strcpy(buf->sysname, "Ascension");
  strcpy(buf->nodename, "AvoryOS");
  strcpy(buf->release, "2.5.0 Beta");

  // Dynamic date/time from RTC
  char datetime[32];
  rtc_format_datetime(rtc_get_timestamp(), datetime, sizeof(datetime));

  char version[80];
  strcpy(version, "2.5.0 Beta ");
  strcat(version, datetime);
  strcpy(buf->version, version);

  strcpy(buf->machine, "x86_64");
  strcpy(buf->domainname, "localhost");

  return 0;
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
  case 1: // PR_SET_PDEATHSIG
    return 0;
  case 2: // PR_GET_PDEATHSIG
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 0;
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
  case 36: // PR_SET_CHILD_SUBREAPER
    return 0;
  case 37: // PR_GET_CHILD_SUBREAPER
    if (arg2 && vmm_is_user_addr_range_writable(arg2, sizeof(int))) {
      *(int *)arg2 = 0;
      return 0;
    }
    return (uint64_t)-14;
  case 38: // PR_SET_NO_NEW_PRIVS
    return 0;
  case 39: // PR_GET_NO_NEW_PRIVS
    return 0;
  case 0x41555856: { // PR_GET_AUXV (Linux 6.4+)
    if (!arg2 || !arg3)
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
  default:
    return (uint64_t)-22; // EINVAL
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
  if (t->euid != 0) {
    if (ruid != UINT32_MAX && ruid != t->uid && ruid != t->euid && ruid != t->suid) return (uint64_t)-1;
    if (euid != UINT32_MAX && euid != t->uid && euid != t->euid && euid != t->suid) return (uint64_t)-1;
    if (suid != UINT32_MAX && suid != t->uid && suid != t->euid && suid != t->suid) return (uint64_t)-1;
  }
  if (ruid != UINT32_MAX) t->uid = ruid;
  if (euid != UINT32_MAX) t->euid = euid;
  if (suid != UINT32_MAX) t->suid = suid;
  t->fsuid = t->euid;
  return 0;
}

// sys_setresgid
static uint64_t sys_setresgid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi, egid = (uint32_t)regs->rsi;
  uint32_t sgid = (uint32_t)regs->rdx;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;
  if (t->euid != 0) {
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
  if (t->euid == 0) {
    t->uid = t->euid = t->suid = t->fsuid = uid;
  } else if (uid == t->uid || uid == t->suid) {
    t->euid = t->fsuid = uid;
  } else {
    return (uint64_t)-1;
  }
  return 0;
}

// sys_setgid (syscall 106)
static uint64_t sys_setgid(struct syscall_regs *regs) {
  uint32_t gid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  if (t->euid == 0) {
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

  if (t->euid != 0) {
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
  return 0;
}

// sys_setregid (syscall 114)
static uint64_t sys_setregid(struct syscall_regs *regs) {
  uint32_t rgid = (uint32_t)regs->rdi;
  uint32_t egid = (uint32_t)regs->rsi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (t->euid != 0) {
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
  if (t->euid == 0 || fsuid == t->uid || fsuid == t->euid ||
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
  if (t->euid == 0 || fsgid == t->gid || fsgid == t->egid ||
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
  if (!t || t->euid != 0) return (uint64_t)-1;
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
  if (!t)
    return 0;
  if (t->parent)
    return t->parent->tgid;
  return 0;
}

// sys_setpgid
static uint64_t sys_setpgid(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t pgid = (uint32_t)regs->rsi;
  struct thread *current = sched_get_current();
  struct thread *target = NULL;

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
    return current->pgid;
  struct thread *target = sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH
  return target->pgid;
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
    return (uint64_t)(current->sid ? current->sid : (current->tgid ? current->tgid : 1));
  struct thread *target = sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH
  return (uint64_t)(target->sid ? target->sid : (target->tgid ? target->tgid : 1));
}

// sys_getpgrp
static uint64_t sys_getpgrp(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->pgid;
}

// sys_rseq implementation (stub)
uint64_t sys_rseq(struct syscall_regs *regs) {
  (void)regs;
  // AvoryOS does not maintain kernel restartable sequences.
  // Returning 0 causes glibc 2.35+ / 2.42 to believe rseq is registered,
  // but rseq_area.cpu_id remains -1 (RSEQ_CPU_ID_UNINITIALIZED), causing
  // glibc tcache/malloc or locks to hang/spin indefinitely.
  // Returning -ENOSYS allows glibc to cleanly disable rseq and fallback.
  return (uint64_t)-38; // -ENOSYS
}

// sys_setsid
static uint64_t sys_setsid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  // POSIX: A process group leader cannot create a new session
  if (current->tid == current->pgid) {
    return (uint64_t)-1; // EPERM
  }

  current->sid = current->tid;
  current->pgid = current->tid;
  current->ctty = NULL; // Clear controlling terminal

  klog_debugf("[SETSID] New session created: %llu\n", (unsigned long long)current->sid);

  return (uint64_t)current->sid;
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
  t->it_real_next = value_ms ? now + value_ms : 0;

  if (old_value_ptr)
    memcpy((void *)old_value_ptr, &old_value, sizeof(old_value));
  if (t->it_real_next)
    lapic_timer_rearm_if_earlier(t->it_real_next);
  return 0;
}

static uint64_t sys_alarm(uint64_t seconds, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t) return 0;
  uint64_t now = lapic_timer_get_ticks();
  uint64_t remaining = t->it_real_next > now ? (t->it_real_next - now + 999) / 1000 : 0;
  t->it_real_value = seconds * 1000;
  t->it_real_interval = 0;
  t->it_real_next = seconds ? now + seconds * 1000 : 0;
  if (t->it_real_next)
    lapic_timer_rearm_if_earlier(t->it_real_next);
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

static uint64_t sys_getrlimit(uint64_t resource, uint64_t rlim_ptr, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct rlimit *r = (struct rlimit *)rlim_ptr;
  if (!r)
    return (uint64_t)-14; // EFAULT

  // Simple stubs: everything is infinite for now
  r->rlim_cur = RLIM_INFINITY;
  r->rlim_max = RLIM_INFINITY;

  // Some apps specifically check RLIMIT_NOFILE
  if (resource == 7) { // RLIMIT_NOFILE
    r->rlim_cur = MAX_FDS;
    r->rlim_max = MAX_FDS;
  } else if (resource == 4) { // RLIMIT_CORE (default 0 on Linux)
    r->rlim_cur = 0;
    r->rlim_max = RLIM_INFINITY;
  }

  return 0;
}

static uint64_t sys_prlimit64(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t resource = (uint32_t)regs->rsi;
  struct rlimit *new_limit = (struct rlimit *)regs->rdx;
  struct rlimit *old_limit = (struct rlimit *)regs->r10;

  if (pid != 0) {
    struct thread *t = sched_get_thread_by_tid(pid);
    if (!t)
      return (uint64_t)-3; // ESRCH
    if (t != sched_get_current())
      return (uint64_t)-1; // EPERM (only self for now)
  }

  if (old_limit) {
    old_limit->rlim_cur = RLIM_INFINITY;
    old_limit->rlim_max = RLIM_INFINITY;
    if (resource == 7) { // RLIMIT_NOFILE
      old_limit->rlim_cur = MAX_FDS;
      old_limit->rlim_max = MAX_FDS;
    } else if (resource == 4) { // RLIMIT_CORE
      old_limit->rlim_cur = 0;
      old_limit->rlim_max = RLIM_INFINITY;
    }
  }

  if (new_limit) {
    // Stub: we don't actually enforce many limits yet,
    // so we just ignore the new limits for now.
  }

  return 0;
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
      pid == 0 ? current : sched_get_thread_by_tid((uint32_t)pid);
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
      pid == 0 ? current : sched_get_thread_by_tid((uint32_t)pid);
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

static uint64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH
  if (current->euid != 0 && current->uid != target->uid &&
      current->euid != target->uid) return (uint64_t)-1;

  if (!param_ptr || !vmm_is_user_addr_range_valid(param_ptr, sizeof(int)))
    return (uint64_t)-14; // EFAULT

  int sched_priority = *(int *)param_ptr;
  if (sched_priority != 0)
    return (uint64_t)-22; // SCHED_OTHER requires priority zero
  return 0;
}

static uint64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr,
                                   uint64_t a2, uint64_t a3, uint64_t a4,
                                   uint64_t a5) {
  (void)pid;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!param_ptr || !vmm_is_user_addr_range_valid(param_ptr, sizeof(int))) {
    return (uint64_t)-14; // EFAULT
  }

  // struct sched_param { int sched_priority; };
  *(int *)param_ptr = 0;
  return 0;
}

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

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

// sys_sched_getscheduler (syscall 145)
// Returns the scheduling policy of the given process.
static uint64_t sys_sched_getscheduler(uint64_t pid, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH

  if (target->nice_value <= -10)
    return SCHED_RR;
  if (target->nice_value >= 10)
    return SCHED_BATCH;
  return SCHED_OTHER;
}

// sys_sched_setscheduler (syscall 144)
// Sets the scheduling policy and parameters for a process.
// Maps real-time (SCHED_FIFO/SCHED_RR) to highest interactive priority,
// and SCHED_BATCH to longer throughput slices.
static uint64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
                                       uint64_t param_ptr, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  struct thread *target =
      pid == 0 ? current : sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH

  if (current->euid != 0 && current->uid != target->uid &&
      current->euid != target->uid)
    return (uint64_t)-1; // EPERM

  if (!param_ptr || !vmm_is_user_addr_range_valid(param_ptr, sizeof(int)))
    return (uint64_t)-14; // EFAULT

  int sched_priority = *(int *)param_ptr;
  uint64_t clean_policy = policy & ~0x40000000ULL; // Mask SCHED_RESET_ON_FORK

  if (clean_policy == SCHED_FIFO || clean_policy == SCHED_RR) {
    if (sched_priority < 1 || sched_priority > 99)
      return (uint64_t)-22; // EINVAL
    // Map RT priority (1..99) to highest interactive priority (nice -20..-10)
    int nice = -20 + (99 - sched_priority) / 10;
    if (nice < -20) nice = -20;
    if (nice > -10) nice = -10;
    sched_set_priority(target, nice_to_sched_priority(nice), (int8_t)nice);
    eevfd_set_slice(&target->se, EEVFD_LATENCY_SLICE_NS);
    return 0;
  } else if (clean_policy == SCHED_BATCH) {
    if (sched_priority != 0) return (uint64_t)-22;
    int nice = 5;
    sched_set_priority(target, nice_to_sched_priority(nice), (int8_t)nice);
    eevfd_set_slice(&target->se, EEVFD_BATCH_SLICE_NS);
    return 0;
  } else if (clean_policy == SCHED_IDLE) {
    if (sched_priority != 0) return (uint64_t)-22;
    int nice = 19;
    sched_set_priority(target, nice_to_sched_priority(nice), (int8_t)nice);
    eevfd_set_slice(&target->se, EEVFD_MAX_SLICE_NS);
    return 0;
  } else if (clean_policy == SCHED_OTHER) {
    if (sched_priority != 0)
      return (uint64_t)-22; // EINVAL
    sched_set_priority(target, nice_to_sched_priority(0), 0);
    return 0;
  }

  return (uint64_t)-22; // EINVAL
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
      sched_get_thread_by_tid((uint32_t)who);
  if (!target) return (uint64_t)-3;
  if (current->euid != 0 && current->uid != target->uid &&
      current->euid != target->uid) return (uint64_t)-1;
  if (nice < target->nice_value && current->euid != 0)
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
      sched_get_thread_by_tid((uint32_t)who);
  if (!target) return (uint64_t)-3;
  return (uint64_t)(20 - target->nice_value);
}

/* sizeof(struct robust_list_head) on x86-64: list.next, futex_offset and
 * list_op_pending.  Linux rejects every other length with EINVAL. */
#define ROBUST_LIST_HEAD_LEN 24

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
      if (t->tid != (uint32_t)pid)
        continue;
      // Linux asks for ptrace_may_access(); same owner is the closest match.
      if (current->euid != 0 && t->uid != current->uid &&
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
    nvme_shutdown(); /* graceful CC.SHN before resetting the machine */
    acpi_reboot();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_POWER_OFF:
    klog_puts("[REBOOT] System power-off requested via syscall.\n");
    nvme_shutdown(); /* graceful CC.SHN before power is removed */
    acpi_poweroff();
    /* noreturn */
    break;

  case LINUX_REBOOT_CMD_HALT:
    klog_puts("[REBOOT] System halt requested via syscall.\n");
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
// sys_capget (125) / sys_capset (126)
//
// AvoryOS runs as all-capable root and has no capability enforcement layer.
// capget reports a full capability set so that tools (e.g. bwrap) which probe
// capabilities before deciding whether to proceed can see a sane response.
// capset is a silent no-op — we never take capabilities away.
//
// Structures mirror the Linux uapi/linux/capability.h layout (version 3 = V3).
// ---------------------------------------------------------------------------

#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define _LINUX_CAPABILITY_U32S_3    2

struct __user_cap_header {
  uint32_t version;
  int pid;
};

struct __user_cap_data {
  uint32_t effective;
  uint32_t permitted;
  uint32_t inheritable;
};

static uint64_t sys_capget(uint64_t hdrp_arg, uint64_t datap_arg,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a3; (void)a4; (void)a5; (void)a6;

  struct __user_cap_header *hdrp = (struct __user_cap_header *)hdrp_arg;
  struct __user_cap_data  *datap = (struct __user_cap_data  *)datap_arg;

  if (!hdrp)
    return (uint64_t)-14; // EFAULT

  if (!vmm_is_user_addr_range_valid((uint64_t)hdrp, sizeof(*hdrp)))
    return (uint64_t)-14; // EFAULT

  // Normalise version: write back V3 so the caller knows what we support.
  uint32_t version = hdrp->version;
  if (version != _LINUX_CAPABILITY_VERSION_3) {
    hdrp->version = _LINUX_CAPABILITY_VERSION_3;
    if (!datap)
      return (uint64_t)-22; // EINVAL — caller must re-issue with correct version
  }

  if (!datap)
    return 0;

  if (!vmm_is_user_addr_range_valid((uint64_t)datap,
                                     sizeof(*datap) * _LINUX_CAPABILITY_U32S_3))
    return (uint64_t)-14; // EFAULT

  // Grant all capabilities in both 32-bit words (caps 0-63).
  for (int i = 0; i < _LINUX_CAPABILITY_U32S_3; i++) {
    datap[i].effective   = 0xFFFFFFFFu;
    datap[i].permitted   = 0xFFFFFFFFu;
    datap[i].inheritable = 0xFFFFFFFFu;
  }

  return 0;
}

static uint64_t sys_capset(uint64_t hdrp_arg, uint64_t datap_arg,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)hdrp_arg; (void)datap_arg;
  (void)a3; (void)a4; (void)a5; (void)a6;
  // No capability enforcement — accept unconditionally.
  return 0;
}

// ---------------------------------------------------------------------------
// sys_seccomp (317)
//
// AvoryOS has no BPF interpreter or seccomp infrastructure.  We implement the
// minimum needed for bwrap and WebKitGTK to proceed:
//
//   SECCOMP_SET_MODE_STRICT (op=0) — genuinely can't be supported without
//     signal delivery changes; return EINVAL so callers fall back gracefully.
//
//   SECCOMP_SET_MODE_FILTER (op=1) — silently accept and return success.
//     The supplied BPF program is ignored; all syscalls remain permitted.
//     bwrap passes SECCOMP_SET_MODE_FILTER before exec-ing the sandboxed
//     binary.  Returning 0 lets it continue; the "sandbox" is a no-op on
//     AvoryOS, which is acceptable given that we already disabled bwrap's
//     user-namespace isolation via WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS.
//
//   SECCOMP_GET_ACTION_AVAIL (op=2) — return EINVAL; not supported.
//   Everything else                 — return EINVAL.
// ---------------------------------------------------------------------------

#define SECCOMP_SET_MODE_STRICT    0
#define SECCOMP_SET_MODE_FILTER    1
#define SECCOMP_GET_ACTION_AVAIL   2

static uint64_t sys_seccomp(uint64_t op, uint64_t flags, uint64_t uargs,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)flags; (void)uargs; (void)a4; (void)a5; (void)a6;

  switch (op) {
  case SECCOMP_SET_MODE_FILTER:
    // Accept the filter program without actually installing it.
    return 0;

  case SECCOMP_SET_MODE_STRICT:
  case SECCOMP_GET_ACTION_AVAIL:
  default:
    return (uint64_t)-22; // EINVAL
  }
}

// sys_unshare (272)
// Namespaces are unsupported in AvoryOS. Returning -ENOSYS tells bwrap,
// container runtimes, and WebKit that unprivileged user/mount namespaces
// are unavailable so they can fail gracefully or fallback to direct execution.
static uint64_t sys_unshare(uint64_t flags, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  klog_puts("[SYSCALL] unshare: flags=0x");
  klog_hex64(flags);
  klog_puts(" -> ENOSYS\n");
  return (uint64_t)-38; // -ENOSYS
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
  syscall_register(SYS_GETRUSAGE, sys_getrusage);
  syscall_register_raw(SYS_PRLIMIT64, sys_prlimit64);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
  syscall_register(SYS_SCHED_YIELD, sys_sched_yield);
  syscall_register(SYS_SCHED_GETAFFINITY, sys_sched_getaffinity);
  syscall_register(SYS_SCHED_SETAFFINITY, sys_sched_setaffinity);
  syscall_register(SYS_SCHED_GETPARAM, sys_sched_getparam);
  syscall_register(SYS_SCHED_SETPARAM, sys_sched_setparam);
  syscall_register(SYS_SCHED_GET_PRIORITY_MAX, sys_sched_get_priority_max);
  syscall_register(SYS_SCHED_GET_PRIORITY_MIN, sys_sched_get_priority_min);
  syscall_register(SYS_SCHED_GETSCHEDULER, sys_sched_getscheduler);
  syscall_register(SYS_SCHED_SETSCHEDULER, sys_sched_setscheduler);
  syscall_register(SYS_SETPRIORITY, sys_setpriority);
  syscall_register(SYS_GETPRIORITY, sys_getpriority);
  syscall_register(SYS_SET_ROBUST_LIST, sys_set_robust_list);
  syscall_register(SYS_GET_ROBUST_LIST, sys_get_robust_list);
  syscall_register(SYS_REBOOT, sys_reboot);
  syscall_register(SYS_PIDFD_OPEN, sys_pidfd_open);
  syscall_register(SYS_GETCPU, sys_getcpu);
  syscall_register(SYS_CAPGET, sys_capget);
  syscall_register(SYS_CAPSET, sys_capset);
  syscall_register(SYS_SECCOMP, sys_seccomp);
  syscall_register(SYS_UNSHARE, sys_unshare);
}
