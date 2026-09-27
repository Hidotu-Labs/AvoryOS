#ifndef SCHED_H
#define SCHED_H

#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../lib/rbtree.h"
#include "../mm/vma.h"
#include "eevfd.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_SUPPLEMENTARY_GROUPS 32

// Forward declaration for embedded wait queue entry
struct wait_queue_entry;
typedef struct wait_queue_entry wait_queue_entry_t;

#include "../lock/spinlock.h"

// Shared memory management structure for 1:1 threads
struct mm_struct {
  struct vma_list vmas;    // Virtual memory areas
  uint64_t brk_base;       // Base of the heap
  uint64_t brk_current;    // Current end of the heap
  uint64_t mmap_next_addr; // Bump-pointer for anonymous mmap
  int ref_count;           // Reference count for sharing across threads
  uint16_t pcid;           // Hardware PCID tag (0 = kernel/unassigned)
  spinlock_t lock;         // Lock for thread-safe MM state updates
  uint64_t saved_auxv[64]; // Auxiliary vector (type, val pairs)
  uint32_t auxv_count;     // Number of uint64_t entries
  /*
   * argv snapshot for /proc/<pid>/cmdline, stored exactly as Linux stores it:
   * the argument strings back to back, each NUL terminated.  Userspace matches
   * command lines against this blob (busybox/procps `pgrep -f`, `pkill -f`,
   * `ps`, jps, and every "already running?" guard in the desktop scripts), so
   * substituting comm here silently breaks any pattern that contains a path -
   * e.g. Alpine's pipewire-launcher waits for `pgrep -f /usr/bin/pipewire`,
   * which can never match the 15-character name "pipewire" and therefore spins
   * exec'ing sleep+pgrep once per second forever.
   *
   * Bounded copy: a caller with a megabyte of argv must not be able to grow
   * per-process kernel memory, and no reader needs more than the invocation.
   * arg_start/arg_end are the user VA bounds of the real blob (what Linux
   * reports so setproctitle() can be supported later).
   */
  char     saved_cmdline[512];
  uint32_t cmdline_len; // Valid bytes in saved_cmdline (0 = unknown/kernel thread)
  uint64_t arg_start;   // User VA of argv[0] string
  uint64_t arg_end;     // User VA just past the last argv string
};


#define MAX_FDS 256

struct fd_path {
  uint32_t ref_count;
  char value[256];
};

/*
 * CLONE_FILES makes this object shared by all pthreads in a process. Keep
 * the arrays indirectly referenced from struct thread so existing syscall
 * code can continue to use t->fds[n] while observing one common table.
 */
struct fd_table {
  vfs_node_t *fds[MAX_FDS];
  uint64_t fd_offsets[MAX_FDS];
  uint64_t fd_flags[MAX_FDS];
  struct fd_path *fd_paths[MAX_FDS];
  /* Lowest descriptor which may be free. Protected by lock. */
  uint32_t next_fd;
  uint32_t ref_count;
  spinlock_t lock;
};

// Signal constants
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define SIG_DFL 0
#define SIG_IGN 1

// sigaltstack flags
#define SS_ONSTACK 1
#define SS_DISABLE 2
#define SS_AUTODISARM (1U << 31)

// sigaction flags
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTORER 0x04000000
#define SA_NODEFER 0x40000000

struct k_sigaction {
  void (*sa_handler)(int);
  uint64_t sa_flags;
  void (*sa_restorer)(void);
  uint64_t sa_mask;
};

typedef enum {
  THREAD_RUNNING,
  THREAD_READY,
  THREAD_BLOCKED,
  THREAD_SLEEPING,
  THREAD_DEAD,
  THREAD_ZOMBIE
} thread_state_t;

#define SCHED_PRIORITY_LEVELS 32
#define SCHED_PRIORITY_INTERACTIVE 0
#define SCHED_PRIORITY_DEFAULT 16
#define SCHED_PRIORITY_BACKGROUND 24
#define SCHED_PRIORITY_IDLE 31
#define SCHED_AGING_SCAN_INTERVAL_MS 50
#define SCHED_AGING_STEP_MS 250

// Information saved on context switch.
// We push callee-saved registers manually in switch.asm.
struct context {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t rbp;
  uint64_t rbx;
  uint64_t rflags;   // RFLAGS (carries the coarse SMAP AC window)
  uint64_t ret_addr; // RIP (pushed automatically by call)
} __attribute__((packed));

struct thread {
  uint64_t rsp; // Must be first field (offset 0) for optimal assembly
  uint32_t tid;
  uint32_t tgid; // Thread group ID (== tid for group leader)
  uint8_t fpu_state[4096] __attribute__((aligned(64))); // Saved XSAVE/FPU state (at offset 64)
  uint64_t stack_base;
  uint64_t stack_size;
  thread_state_t state;
  bool waiting_for_child;       // Blocked specifically inside wait4().
  uint64_t wakeup_ticks;
  struct thread *deadline_next; // Per-CPU ordered timeout queue link
  bool deadline_queued;
  struct fd_table *files;
  vfs_node_t **fds;
  uint64_t *fd_offsets;  // Track seek offset per file descriptor
  uint64_t *fd_flags;    // Track flags (O_NONBLOCK, etc.) for each FD
  struct fd_path **fd_paths; // Shared, reference-counted descriptor paths
  uint64_t cr3;                 // Per-process page table (0 = inherited/kernel)
  bool is_forked_child;         // True for forked children (affects sys_exit)
  bool is_idle;                 // True for idle thread (cannot be terminated)
  void *fork_ctx;               // Saved register state for child entry
  bool is_main_session;    // True if this is the primary user session (Bash)
  uint64_t clone_flags;    // Flags passed to clone()
  struct thread *parent;   // Pointer to parent thread (for wait4)
  struct thread *children; // Head of children list
  struct thread *sibling_next; // Link to next sibling in parent's children list
  uint32_t pgid;               // Process group ID
  uint32_t sid;                // Session ID
  int exit_status;             // Status code when exiting (for wait4)
  uint64_t *tid_address;       // Pointer to user-space TID for set_tid_address
  uint64_t robust_list;        // Futex robust-list head from set_robust_list(2)
  struct thread *global_next;  // Used to link all threads together
  struct thread *rq_next;      // Next thread in circular run queue
  struct thread *rq_prev;      // Previous thread in circular run queue
  bool on_runqueue;            // Protected by owning CPU queue_lock
  /* Deferred preemption request.  Set by sched_wakeup() when a thread more
   * eligible than whatever is running becomes runnable on *this* CPU, and
   * consumed by sched_check_resched() at interrupt/syscall exit.  Without it
   * a same-CPU wake had to wait for the next LAPIC tick (~1 ms) before the
   * woken thread ran, which is the dominant cost of every X11 request/reply
   * round trip and every input event. */
  volatile bool need_resched;
  uint8_t queued_priority;     // Queue containing this thread
  uint64_t ready_since_ms;     // Start of current runnable wait
  struct thread *reap_next;    // Used for automatic reaping of detached threads
  bool reap_remove_runqueue;   // Remote exit_group victim still needs unlink
  char cwd_path[256];          // Current working directory
  vfs_node_t *cwd_node;        // Current working directory VFS node
  struct mm_struct *mm;        // Shared memory management state
  uint64_t fs_base;            // User FS_BASE (TLS) — inherited across fork
  uint64_t gs_base;            // User GS_BASE (TLS) — inherited across fork
  uint32_t uid;                // User ID
  uint32_t gid;                // Group ID
  uint32_t euid;               // Effective User ID
  uint32_t egid;               // Effective Group ID
  uint32_t suid;               // Saved set-user-ID
  uint32_t sgid;               // Saved set-group-ID
  uint32_t fsuid;              // File system User ID
  uint32_t fsgid;              // File system Group ID
  uint32_t supplementary_groups[MAX_SUPPLEMENTARY_GROUPS];
  uint32_t supplementary_group_count;
  uint32_t umask;              // File creation mask

  // Real-time interval timer (ITIMER_REAL)
  uint64_t it_real_value;    // Remaining ticks until signal (0 = disabled)
  uint64_t it_real_interval; // Reload value in ticks
  uint64_t it_real_next;     // Absolute tick count when timer expires

  // Controlling terminal (set when opening PTY slave as session leader)
  vfs_node_t *ctty; // Controlling terminal (PTY slave or console)

  // Signal state
  struct k_sigaction signal_handlers[64];
  uint64_t pending_signals;
  uint32_t signal_sender_pid[64];
  uint64_t signal_mask;
  uint64_t saved_signal_mask;
  bool has_saved_signal_mask;
  uint64_t fault_addr;
  uint32_t fault_code;
  uint64_t last_report_rip;
  uint64_t last_report_addr;
  int last_report_sig;
  struct registers sigreturn_regs; // Safe kernel-side register store for IRETQ restoration

  // Alternate signal stack (sigaltstack)
  uint64_t ss_sp;   // Base of alternate signal stack
  uint64_t ss_size; // Size of alternate signal stack
  int ss_flags;     // SS_DISABLE, SS_ONSTACK, etc.

  // Active wait queue entries registered by this thread
  spinlock_t wait_entries_lock;
  struct wait_queue_entry *wait_entries_head;

  // Embedded wait queue entry for safe blocking across context switches
  // Using stack-allocated entries is unsafe because the stack frame becomes
  // invalid when the thread is descheduled, leading to corrupted wait queues
  struct wait_queue_entry *wq_entry_next; // For multi-wait (epoll)

  // Scheduling/EEVFD priority state
  struct sched_entity se;  // EEVFD scheduling entity
  uint32_t cpu_index;      // Index of CPU this thread is enqueued on
  uint8_t priority;        // Current dynamic priority (0-31)
  uint8_t static_priority; // Base priority
  int8_t nice_value;       // Userspace nice value (-20..19)
  uint64_t time_slice;     // Remaining ticks in current quantum
  uint64_t
      runtime_total; // Total CPU time consumed (in LAPIC ticks, 1 tick = 1ms)
  uint64_t runtime_burst; // CPU time used in current quantum (for MLFQ)
  char comm[16];          // Executable name (basename, max 15 chars + NUL)
  char exe_path[256];     // Full path of the current executable (for /proc/self/exe)
  uint64_t cpu_affinity;  // Bitmask of allowed CPUs

  // Subsystem & Site Diagnostics
  const char *last_subsystem;
  const char *last_kernel_file;
  uint32_t last_kernel_line;
  const char *last_kernel_func;
  int64_t last_error_code;
  uint64_t last_syscall_num;
  uint64_t last_syscall_args[6];
  int64_t last_syscall_ret;

  // Futex parking diagnostics (lock/lockdiag.c).  A thread blocked on a futex
  // with no timeout armed is the shape of a lost wakeup, and these three are
  // what let the hang report name it.  Appended at the end: assembly pins
  // offsets earlier in this struct, not these.
  uint64_t blocked_since_ms; // 0 when not parked in futex_wait()
  bool in_futex_wait;
  bool blocked_reported; // already named once by the stuck-waiter warning
  uint32_t futex_bucket; // futex_hash bucket, 0xFFFFFFFF when not waiting
};

/*
 * Stable copy used by procfs. Returning a raw thread pointer after dropping
 * tid_lock is unsafe because the reaper may free it immediately.
 */
struct sched_thread_snapshot {
  uint32_t tid;
  uint32_t tgid;
  uint32_t parent_tid;
  uint32_t pgid;
  thread_state_t state;
  uint64_t runtime_total;
  uint64_t virt_bytes;
  uint64_t resident_bytes;
  uint32_t uid, gid, euid, egid, suid, sgid;
  char comm[16];
};

bool fd_path_set(struct thread *t, int fd, const char *path);
void fd_path_dup(struct thread *t, int dst, int src);
void fd_path_clear(struct thread *t, int fd);
const char *fd_path_value(struct thread *t, int fd);

void sched_init(void);
void sched_run_phase1_test(void);
void sched_wakeup(struct thread *t);
void sched_yield(void);

struct cpu_info;
struct thread *sched_create_kernel_thread(void (*entry_point)(void),
                                          struct cpu_info *explicit_cpu,
                                          bool enqueue);

void sched_tick(struct registers *regs);
void sched_yield_user(void);

/* Deferred preemption point for the interrupt and syscall return paths.
 * to_user is true when the caller is about to return to ring 3. */
void sched_check_resched(bool to_user);

// Returns the current thread *for the CPU currently executing this code*
struct thread *sched_get_current(void);

// Load balancing / dispatching
void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu);
bool sched_set_priority(struct thread *t, uint8_t priority, int8_t nice_value);
bool sched_validate_runqueues(struct cpu_info *cpu);

// Task management for shell
void sched_print_tasks(void);
bool sched_terminate_thread(uint32_t tid);
struct thread *sched_get_thread_by_tid(uint32_t tid);
bool sched_get_thread_snapshot(uint32_t tid,
                               struct sched_thread_snapshot *snapshot);
size_t sched_read_thread_auxv(uint32_t tid, uint32_t offset, uint32_t size,
                              uint8_t *buffer);
/* /proc/<pid>/cmdline backing store: copies out of the argv snapshot taken at
 * exec.  Returns bytes copied (0 once past the end / no argv recorded). */
size_t sched_read_thread_cmdline(uint32_t tid, uint32_t offset, uint32_t size,
                                 uint8_t *buffer);
/* Total readable length of that snapshot, or 0 if there is none. */
size_t sched_thread_cmdline_size(uint32_t tid);
bool sched_get_nth_thread_tid(uint32_t index, uint32_t *tid);
bool sched_get_nth_open_fd(uint32_t tid, uint32_t index, uint32_t *fd);
bool sched_get_fd_path_snapshot(uint32_t tid, uint32_t fd, char *path,
                                size_t path_size);

// Reap a zombie thread (remove from runqueue, free resources)
void sched_reap_thread(struct thread *t);
void sched_queue_reap(struct thread *t);
void sched_queue_reap_and_wait(struct thread *t);
void sched_terminate_thread_group(struct thread *current);
void sched_share_files(struct thread *child, struct thread *parent);
void sched_release_files(struct thread *t);
bool sched_ensure_files(struct thread *t);

// Returns the total number of threads in the global thread list
uint16_t sched_get_thread_count(void);
uint16_t sched_get_runnable_thread_count(void);

// Aggregates CPU time across all threads.
// *out_user_ms = sum of runtime_total for non-idle threads (ms)
// *out_idle_ms = total elapsed CPU-ms minus user_ms (ms)
void sched_get_total_cpu_ms(uint64_t *out_user_ms, uint64_t *out_idle_ms);

// Returns the head of the global thread list (caller must hold no locks;
// used by procfs for read-only enumeration under tid_lock)
struct thread *sched_get_thread_list_head(void);

// Reparent children to init
void sched_reparent_children(struct thread *parent);

int alloc_fd(struct thread *t);

/*
 * Returns true if the calling thread has at least one unmasked pending signal.
 * Used by blocking syscalls to break out of their wait loops and return
 * -EINTR, allowing signal delivery before re-entering the syscall.
 */
static inline bool thread_has_pending_signal(struct thread *t) {
  if (!t) return false;
  uint64_t unmaskable = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));
  return (t->pending_signals & (~t->signal_mask | unmaskable)) != 0;
}

// Userspace Management
#include "elf.h"
#include <stdbool.h>
bool elf_load(const char *path, uint64_t *pml4, elf_info_t *out_info);
// Linux-style stack: path string near top, then auxv/envp/argv/argc; RSP points
// at argc.
uint64_t process_build_initial_stack(uint64_t stack_top, const char *path,
                                     const char **argv, const char **envp,
                                     const elf_info_t *elf_info);
bool process_exec_argv(const char **argv);
void process_do_exit(uint64_t status);

#define ASCENTOS_USER_STACK_TOP 0x00007FFFF0000000ULL

#endif
