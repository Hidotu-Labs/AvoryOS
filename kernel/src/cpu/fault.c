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

static void fault_sanitize(char *dst, const char *src, uint32_t cap) {
  if (cap == 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  uint32_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; i++) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
  }
  dst[i] = '\0';
}

static const char *fault_sig_name(int sig) {
  switch (sig) {
  case 4:
    return "SIGILL";
  case 6:
    return "SIGABRT";
  case 7:
    return "SIGBUS";
  case 8:
    return "SIGFPE";
  case 11:
    return "SIGSEGV";
  default:
    return "SIG?";
  }
}

extern const char *syscall_get_name(uint64_t num);

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
  rec->vec = regs->int_no;
  fault_sanitize(rec->comm, t->comm, sizeof(rec->comm));
  fault_sanitize(rec->exe, t->exe_path[0] ? t->exe_path : "?",
                 sizeof(rec->exe));
  fault_sanitize(rec->subsystem,
                 t->last_subsystem ? t->last_subsystem : "",
                 sizeof(rec->subsystem));
  fault_sanitize(rec->kernel_file,
                 t->last_kernel_file ? t->last_kernel_file : "",
                 sizeof(rec->kernel_file));
  rec->kernel_line = t->last_kernel_line;
  fault_sanitize(rec->kernel_func,
                 t->last_kernel_func ? t->last_kernel_func : "",
                 sizeof(rec->kernel_func));
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

  // Text interface: one line per fault so `cat /dev/faults` is readable.
  // Previously this copied raw `struct user_fault_record` bytes, which is why
  // `cat` showed binary garbage with embedded comm/syscall strings.
  // Destructive read (consumes records); returns 0 at EOF like before.
  spinlock_acquire(&fault_lock);

  if (fault_head == fault_tail) {
    spinlock_release(&fault_lock);
    return 0; // EOF/No data
  }

  uint32_t count = 0;
  while (fault_tail != fault_head) {
    struct user_fault_record *r = &fault_buffer[fault_tail];
    const char *signame = fault_sig_name((int)r->sig);
    const char *scname = syscall_get_name(r->last_syscall_num);
    // Stack buffer: snprintf never sleeps, safe under this spinlock.
    char line[384];
    int len = snprintf(
        line, sizeof(line),
        "tid=%u comm='%s' exe='%s' sig=%u(%s) vec=%llu err=0x%llx "
        "rip=0x%llx addr=0x%llx rsp=0x%llx syscall=%s(%llu)->%lld\n",
        r->tid, r->comm[0] ? r->comm : "?", r->exe[0] ? r->exe : "?",
        r->sig, signame, (unsigned long long)r->vec,
        (unsigned long long)r->err_code, (unsigned long long)r->rip,
        (unsigned long long)r->cr2, (unsigned long long)r->rsp,
        scname ? scname : "?",
        (unsigned long long)r->last_syscall_num, (long long)r->last_syscall_ret);
    if (len <= 0)
      break;
    if ((uint32_t)len >= sizeof(line))
      len = (int)sizeof(line) - 1;
    if ((uint32_t)len > size - count)
      break; // keep record for next read; never emit a truncated line
    memcpy(buffer + count, line, (uint32_t)len);
    count += (uint32_t)len;
    size -= (uint32_t)len;
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
