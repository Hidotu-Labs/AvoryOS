// kcmp(2) — compare the kernel resources of two tasks.
//
// Mesa's os_same_file_description() (radeonsi, virgl, the Wayland WSI) calls
// KCMP_FILE to find out whether two DRM descriptors are one open file
// description, and CRIU uses the remaining types to deduplicate checkpoint
// state.  Linux returns 0 when the resources are equal, 1/2 when the first is
// smaller/larger (an obfuscated but stable total order), and a negative errno
// on failure.  Callers treat every negative value as "cannot determine", so
// distinct resources must come back as 1/2 and never as -1.
//
// AvoryOS has no struct file/fs_struct/sighand_struct objects; the resources
// map onto the kernel's own structures:
//
//   KCMP_FILE       vfs_node_t        per-open node (dup() shares it)
//   KCMP_VM         struct mm_struct  shared by CLONE_VM threads
//   KCMP_FILES      struct fd_table   shared by CLONE_FILES threads
//   KCMP_FS         struct thread     cwd state is per task, never shared
//   KCMP_SIGHAND    struct thread     handlers are copied per task
//   KCMP_IO         NULL              no io_context exists
//   KCMP_SYSVSEM    NULL              no Sem-UNDO lists are tracked
//   KCMP_EPOLL_TFD  eventpoll_t       watched node of another task's epoll
//
// KCMP_FS/KCMP_SIGHAND report "different" for distinct tasks because that is
// what this kernel actually does: fork and clone copy the working directory
// and the signal-handler table instead of sharing a reference-counted object.

#include "../fs/vfs.h"
#include "../sched/sched.h"
#include "../socket/epoll.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

// Linux uapi/linux/kcmp.h
#define KCMP_FILE 0
#define KCMP_VM 1
#define KCMP_FILES 2
#define KCMP_FS 3
#define KCMP_SIGHAND 4
#define KCMP_IO 5
#define KCMP_SYSVSEM 6
#define KCMP_EPOLL_TFD 7

// Linux struct kcmp_epoll_slot
struct kcmp_epoll_slot {
  uint32_t efd;
  uint32_t tfd;
  uint32_t toff;
};

#define KCMP_EPERM 1
#define KCMP_ENOENT 2
#define KCMP_ESRCH 3
#define KCMP_EBADF 9
#define KCMP_EFAULT 14
#define KCMP_EINVAL 22

extern spinlock_t tid_lock;
extern struct thread *find_thread_by_tid_locked(uint32_t tid);

// 0 when both tokens name the same object, otherwise 1 (a < b) or 2 (a > b).
static uint64_t kcmp_ident(const void *a, const void *b) {
  if (a == b)
    return 0;
  return (uintptr_t)a < (uintptr_t)b ? 1 : 2;
}

// No ptrace permission model yet; mirror sys_get_robust_list(): the caller may
// inspect its own task, root may inspect anything, otherwise the owner must
// match.  The pid namespace is flat, so there is nothing else to check.
static bool kcmp_may_access(struct thread *current, struct thread *target) {
  if (target == current)
    return true;
  if (current->euid == 0)
    return true;
  return target->uid == current->uid || target->euid == current->euid;
}

// Live node behind fd `idx` of task `t`.  The caller holds tid_lock, which
// keeps `t` from being reaped, and the table lock protects the slot while it
// is sampled.  The result is only ever used as an identity token: if another
// thread closes the descriptor afterwards the node may be freed, but its
// pointer value is still a unique token for the comparison.  Linux truncates
// the fd to unsigned int before the lookup; do the same.
static vfs_node_t *kcmp_fd_node(struct thread *t, uint64_t idx) {
  uint32_t fd = (uint32_t)idx;
  if (!t || !t->files || fd >= MAX_FDS)
    return NULL;
  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  spinlock_release(&t->files->lock);
  if (!node || node == FD_RESERVED)
    return NULL;
  return node;
}

// KCMP_EPOLL_TFD: is the file behind task1's fd `idx1` the file watched by
// task2's epoll descriptor slot->efd at target slot->tfd/slot->toff?  AvoryOS
// keeps at most one item per fd number, so toff must be 0.
static uint64_t kcmp_epoll_target(struct thread *t1, struct thread *t2,
                                  uint64_t idx1,
                                  const struct kcmp_epoll_slot *slot) {
  vfs_node_t *filp = kcmp_fd_node(t1, idx1);
  if (!filp)
    return (uint64_t)-KCMP_EBADF;

  if (!t2->files || slot->efd >= MAX_FDS)
    return (uint64_t)-KCMP_EBADF;

  /* The table lock keeps the epoll descriptor from being closed, and with it
   * the eventpoll instance from being destroyed, while its watched table is
   * inspected.  Lock order tid_lock -> files->lock -> ep->lock; nothing takes
   * files->lock while holding ep->lock. */
  spinlock_acquire(&t2->files->lock);
  vfs_node_t *ep_node = t2->fds[slot->efd];
  if (!ep_node || ep_node == FD_RESERVED) {
    spinlock_release(&t2->files->lock);
    return (uint64_t)-KCMP_EBADF;
  }
  if ((ep_node->flags & FS_TYPE_MASK) != FS_EPOLL || !ep_node->device) {
    spinlock_release(&t2->files->lock);
    return (uint64_t)-KCMP_EINVAL;
  }

  uint64_t ret = (uint64_t)-KCMP_ENOENT;
  if (slot->toff == 0 && slot->tfd < EPOLL_MAX_WATCHED) {
    eventpoll_t *ep = (eventpoll_t *)ep_node->device;
    spinlock_acquire(&ep->lock);
    epitem_t *item = ep->items[slot->tfd];
    vfs_node_t *target = item ? item->node : NULL;
    spinlock_release(&ep->lock);
    if (target)
      ret = kcmp_ident(filp, target);
  }
  spinlock_release(&t2->files->lock);
  return ret;
}

static uint64_t sys_kcmp(uint64_t pid1_arg, uint64_t pid2_arg, uint64_t type_arg,
                         uint64_t idx1, uint64_t idx2, uint64_t a5) {
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-KCMP_EINVAL;

  /* The arguments are pid_t; truncating matches what Linux's syscall wrapper
   * sees (and makes a negative pid fall out as "no such task"). */
  uint32_t pid1 = (uint32_t)(int32_t)pid1_arg;
  uint32_t pid2 = (uint32_t)(int32_t)pid2_arg;
  int type = (int32_t)type_arg;

  /* User memory is read before any kernel lock is taken: copy_from_user() may
   * take a page fault and must not run with tid_lock held.  The task lookup
   * still happens first, so a bad task wins with ESRCH, exactly like Linux. */
  struct kcmp_epoll_slot slot = {0};
  bool slot_ok = false;
  if (type == KCMP_EPOLL_TFD && idx2 &&
      vmm_is_user_addr_range_valid(idx2, sizeof(slot)) &&
      copy_from_user(&slot, (const void *)idx2, sizeof(slot)) == 0)
    slot_ok = true;

  uint64_t ret = (uint64_t)-KCMP_EINVAL;

  spinlock_acquire(&tid_lock);

  struct thread *t1 = find_thread_by_tid_locked(pid1);
  struct thread *t2 = find_thread_by_tid_locked(pid2);

  if (!t1 || !t2) {
    ret = (uint64_t)-KCMP_ESRCH;
  } else if (!kcmp_may_access(current, t1) || !kcmp_may_access(current, t2)) {
    ret = (uint64_t)-KCMP_EPERM;
  } else {
    switch (type) {
    case KCMP_FILE: {
      vfs_node_t *filp1 = kcmp_fd_node(t1, idx1);
      vfs_node_t *filp2 = kcmp_fd_node(t2, idx2);
      ret = (filp1 && filp2) ? kcmp_ident(filp1, filp2)
                             : (uint64_t)-KCMP_EBADF;
      break;
    }
    case KCMP_VM:
      ret = kcmp_ident(t1->mm, t2->mm);
      break;
    case KCMP_FILES:
      ret = kcmp_ident(t1->files, t2->files);
      break;
    case KCMP_FS:
    case KCMP_SIGHAND:
      /* No shared fs_struct/sighand_struct exists: distinct tasks have their
       * own cwd and signal-handler copies, so only a task shares with
       * itself. */
      ret = kcmp_ident(t1, t2);
      break;
    case KCMP_IO:
    case KCMP_SYSVSEM:
      /* Every task has no io_context and no Sem-UNDO list, and two tasks
       * without the object compare equal (Linux's kcmp_ptr(NULL, NULL)). */
      ret = 0;
      break;
    case KCMP_EPOLL_TFD:
      ret = slot_ok ? kcmp_epoll_target(t1, t2, idx1, &slot)
                    : (uint64_t)-KCMP_EFAULT;
      break;
    default:
      ret = (uint64_t)-KCMP_EINVAL;
      break;
    }
  }

  spinlock_release(&tid_lock);
  return ret;
}

void syscall_register_kcmp(void) {
  syscall_register(SYS_KCMP, sys_kcmp);
}
