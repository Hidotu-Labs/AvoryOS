#ifndef SYSCALLS_SYSDIAG_H
#define SYSCALLS_SYSDIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;
struct syscall_regs;
struct vfs_node;

#define SYSDIAG_MAX_SYSCALL_MISSES 64
#define SYSDIAG_MAX_IOCTL_MISSES   128
#define SYSDIAG_MAX_FEATURE_MISSES 64

typedef struct {
  uint64_t syscall_num;
  char name[48];
  char comm[32];
  uint32_t last_tgid;
  uint32_t last_tid;
  uint64_t last_rip;
  uint64_t last_args[6];
  uint64_t total_calls;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  bool used;
} sysdiag_syscall_entry_t;

typedef struct {
  uint32_t request;
  char name[48];
  char comm[32];
  char node_name[48];
  uint32_t node_type;
  uint64_t fd;
  uint32_t last_tgid;
  uint32_t last_tid;
  uint64_t last_arg;
  uint64_t total_calls;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  bool handler_called;
  bool used;
} sysdiag_ioctl_entry_t;

typedef struct {
  char subsystem[32];
  char feature[48];
  char comm[32];
  uint32_t last_tgid;
  uint32_t last_tid;
  int64_t return_code;
  uint64_t total_calls;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  bool used;
} sysdiag_feature_entry_t;

#define SYSDIAG_MAX_FS_MISSES      128
#define SYSDIAG_MAX_SYSCALL_ERRORS 128

typedef struct {
  char path[96];
  char comm[32];
  char op[16];
  uint32_t last_tgid;
  uint32_t last_tid;
  int64_t error_code;
  uint64_t total_calls;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  bool used;
} sysdiag_fs_entry_t;

typedef struct {
  uint64_t syscall_num;
  int64_t error_code;
  char name[48];
  char comm[32];
  char path[96]; // first-seen pathname for path-taking syscalls ("" if none)
  /* Latest descriptor state for a failing read/write-family call.  For an
   * EAGAIN this records the poll() view of the same fd, so a poller that
   * keeps reporting readiness while the read never makes progress is
   * distinguishable from ordinary nonblocking traffic. */
  char detail[160];
  uint32_t last_tgid;
  uint32_t last_tid;
  uint64_t last_rip;
  uint64_t last_args[6];
  uint64_t total_calls;
  uint64_t first_seen_ms;
  uint64_t last_seen_ms;
  uint64_t last_detail_ms;
  bool is_severe;
  bool used;
  /* At least one recorded call on this key was answered by a seccomp
   * RET_ERRNO decision before dispatch; the handler never ran for it. */
  bool via_seccomp;
} sysdiag_syscall_error_t;

void sysdiag_init(void);

/* Record an unhandled / unimplemented syscall invocation */
void sysdiag_record_missing_syscall(uint64_t syscall_num, struct thread *t,
                                    const struct syscall_regs *regs);

/* Record an unhandled / unimplemented ioctl command on a file descriptor/node */
void sysdiag_record_missing_ioctl(uint64_t fd, uint32_t request, uint64_t arg,
                                  struct thread *t, struct vfs_node *node,
                                  bool handler_called);

/* Record an unhandled sub-command, flag, or option inside a supported syscall */
void sysdiag_record_unimplemented_feature(const char *subsystem,
                                          const char *feature, struct thread *t,
                                          int64_t error_code);

/* Record missing or failed access to /proc or /sys pseudo-fs paths */
void sysdiag_record_missing_fs_node(const char *path, struct thread *t,
                                    int64_t error_code, const char *op);

/* Record a failing syscall invocation with its error return */
void sysdiag_record_syscall_error(uint64_t syscall_num, int64_t error_code,
                                  struct thread *t,
                                  const struct syscall_regs *regs);

/* Dump full diagnostic report to serial / klog */
void sysdiag_dump_report(void);

/* Format report into a buffer (for /proc/sysdiag or procfs read) */
size_t sysdiag_format_report(char *buf, size_t max_size);

#endif /* SYSCALLS_SYSDIAG_H */
