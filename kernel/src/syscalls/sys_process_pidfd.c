#include "../sched/capability.h"
// Process syscall implementation.
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
#include "../mm/numa.h"
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

#include "sys_process_internal.h"

extern spinlock_t tid_lock;
extern struct thread *global_thread_list;
extern void signal_notify_thread(struct thread *t, int sig);

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

int process_pidfd_target(struct thread *t, uint32_t fd, uint32_t *pid_out) {
  if (!pid_out || !FD_IN_RANGE(t, fd))
    return -9;
  struct pidfd_ctx *ctx = pidfd_ctx_of(t->fds[fd]);
  if (!ctx)
    return -9;
  *pid_out = ctx->pid;
  return 0;
}

wait_queue_t *process_pidfd_wait_queue(void) {
  return &pidfd_event_wait;
}

void process_pidfd_abort(struct thread *t, int fd, vfs_node_t *node);

/* Reserves a descriptor number and builds its pidfd node, without publishing
 * either: clone(CLONE_PIDFD) has to reserve before the child exists (no
 * allocation may fail after the child was created) and publish only after the
 * child copied the parent's descriptor table (the pidfd must not leak into the
 * child).  While reserved the descriptor is unreachable from user space. */
int64_t process_pidfd_reserve(struct thread *t, int *out_fd, vfs_node_t **out_node) {
  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE
  struct pidfd_ctx *ctx = kmalloc(sizeof(*ctx));
  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!ctx || !node) {
    if (ctx) kfree(ctx);
    if (node) kfree(node);
    process_pidfd_abort(t, fd, NULL);
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
void process_pidfd_publish(struct thread *t, int fd, vfs_node_t *node,
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
void process_pidfd_abort(struct thread *t, int fd, vfs_node_t *node) {
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
  int64_t r = process_pidfd_reserve(t, &fd, &node);
  if (r < 0)
    return r;
  process_pidfd_publish(t, fd, node, pid, flags);
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


void syscall_register_pidfd(void) {
  syscall_register(SYS_PIDFD_OPEN, sys_pidfd_open);
  syscall_register(SYS_PIDFD_SEND_SIGNAL, sys_pidfd_send_signal);
  syscall_register(SYS_PIDFD_GETFD, sys_pidfd_getfd);
}
