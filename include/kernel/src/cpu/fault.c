#include "fault.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"

#define FAULT_BUFFER_SIZE 32

static struct user_fault_record fault_buffer[FAULT_BUFFER_SIZE];
static uint32_t fault_head = 0;
static uint32_t fault_tail = 0;
static spinlock_t fault_lock = {0};
static void *fault_wait_queue = NULL; // wait_queue_t*

void fault_log_add(struct registers *regs, int sig, uint64_t cr2) {
  struct thread *t = sched_get_current();
  if (!t)
    return;

  spinlock_acquire(&fault_lock);

  struct user_fault_record *rec = &fault_buffer[fault_head];
  rec->tid = t->tid;
  rec->sig = sig;
  rec->rip = regs->rip;
  rec->rsp = regs->rsp;
  rec->cr2 = cr2;
  rec->err_code = regs->err_code;
  memcpy(rec->comm, t->comm, 16);
  if (t->last_subsystem) {
    strncpy(rec->subsystem, t->last_subsystem, sizeof(rec->subsystem) - 1);
    rec->subsystem[sizeof(rec->subsystem) - 1] = '\0';
  } else {
    rec->subsystem[0] = '\0';
  }
  if (t->last_kernel_file) {
    strncpy(rec->kernel_file, t->last_kernel_file, sizeof(rec->kernel_file) - 1);
    rec->kernel_file[sizeof(rec->kernel_file) - 1] = '\0';
  } else {
    rec->kernel_file[0] = '\0';
  }
  rec->kernel_line = t->last_kernel_line;
  if (t->last_kernel_func) {
    strncpy(rec->kernel_func, t->last_kernel_func, sizeof(rec->kernel_func) - 1);
    rec->kernel_func[sizeof(rec->kernel_func) - 1] = '\0';
  } else {
    rec->kernel_func[0] = '\0';
  }
  rec->error_code = t->last_error_code;
  rec->last_syscall_num = t->last_syscall_num;
  rec->last_syscall_ret = t->last_syscall_ret;
  memcpy(&rec->regs, regs, sizeof(struct registers));

  fault_head = (fault_head + 1) % FAULT_BUFFER_SIZE;
  if (fault_head == fault_tail) {
    fault_tail = (fault_tail + 1) % FAULT_BUFFER_SIZE; // Overwrite oldest
  }

  spinlock_release(&fault_lock);

  // Wake up any waiters on /dev/faults
  if (fault_wait_queue) {
    wait_queue_wake_all(fault_wait_queue);
  }
}

static uint32_t fault_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  (void)node;
  (void)offset;

  spinlock_acquire(&fault_lock);

  if (fault_head == fault_tail) {
    spinlock_release(&fault_lock);
    return 0; // EOF/No data
  }

  uint32_t count = 0;
  while (fault_tail != fault_head && size >= sizeof(struct user_fault_record)) {
    memcpy(buffer + count, &fault_buffer[fault_tail],
           sizeof(struct user_fault_record));
    count += sizeof(struct user_fault_record);
    size -= sizeof(struct user_fault_record);
    fault_tail = (fault_tail + 1) % FAULT_BUFFER_SIZE;
  }

  spinlock_release(&fault_lock);
  return count;
}

static int fault_dev_poll(vfs_node_t *node, int events) {
  (void)node;
  int revents = 0;

  spinlock_acquire(&fault_lock);
  if (fault_head != fault_tail) {
    revents |= POLLIN;
  }
  spinlock_release(&fault_lock);

  return revents & events;
}

void fault_init(void) {
  spinlock_init(&fault_lock);
  fault_wait_queue = kmalloc(sizeof(wait_queue_t));
  wait_queue_init(fault_wait_queue);

  vfs_node_t *dev = vfs_resolve_path("/dev");
  if (dev) {
    vfs_mknod(dev, "faults", 0666, FS_CHARDEV, NULL);
    vfs_node_t *node = vfs_resolve_path("/dev/faults");
    if (node) {
      node->read = fault_dev_read;
      node->poll = fault_dev_poll;
      node->wait_queue = fault_wait_queue;
      vfs_close(node);
    }
    vfs_close(dev);
  }
}
