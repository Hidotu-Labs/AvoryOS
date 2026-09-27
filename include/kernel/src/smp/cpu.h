#ifndef SMP_CPU_H
#define SMP_CPU_H

#include "../lock/spinlock.h"
#include "../sched/eevfd.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration
struct thread;

// Limits
#define MAX_CPUS 64
#define CPU_STACK_SIZE (16384) // 16 KiB kernel stack per CPU

// CPU Status
#define CPU_STATUS_OFFLINE 0
#define CPU_STATUS_BSP 1
#define CPU_STATUS_ONLINE 2

// Per-CPU Data
// Every CPU gets one of these.  The BSP's is initialized at boot; each AP
// populates its own copy when it wakes up in the trampoline.
//
// At runtime the current CPU's struct is accessible via the GS segment base.
struct cpu_info {
  struct cpu_info *self; // self-pointer (GS:0 → this)
  uint32_t cpu_id;       // logical index (0 = BSP, 1..N = APs)
  uint32_t apic_id;      // the LAPIC hardware ID from the MADT
  uint8_t status;        // CPU_STATUS_*
  uint64_t stack_top;    // top of this CPU's kernel stack
  uint64_t kernel_cr3;   // page table root (shared early on)
  uint64_t ticks;        // per-CPU tick counter (for future scheduler)

  // -- Task Scheduling --
  struct thread *current_thread;
  struct thread *idle_thread;   // Permanent pointer to the idle task
  struct thread *runqueues[32]; // Heads of priority queues
  uint32_t runqueue_bitmap;     // Bit set if runqueues[i] is NOT empty
  spinlock_t queue_lock;
  uint32_t runnable_count;      // Number of READY or RUNNING threads
  uint64_t timer_deadline_ms;   // Absolute deadline currently armed in LAPIC
  uint64_t quantum_deadline_ms; // End of the current scheduler quantum
  uint64_t scratch_rsp;
  // The old task whose kernel stack switch_context is still using.  This is
  // distinct from current_thread, which is published before the assembly
  // stack handoff so interrupts observe the arriving task.
  struct thread *switching_from;
  uint64_t sigreturn_frame;
#define MAX_TIMER_HEAP_SIZE 1024

  // Timed sleepers ordered by wakeup_ticks. Protected by queue_lock.
  struct thread *deadline_head;
  struct thread *runqueue_tails[32]; // Tails; placed after assembly ABI fields
  uint64_t next_aging_scan_ms;      // Next per-thread aging pass
  struct thread *timer_heap[MAX_TIMER_HEAP_SIZE];
  uint32_t timer_heap_count;
  struct thread *fpu_owner; // Currently loaded FPU/SSE state owner on this CPU
  struct eevfd_rq eevfd;    // EEVFD Runqueue structure
  uint64_t active_pcids_bmp[64]; // PCID caching tracking (4096 bits)
} __attribute__((aligned(64)));



_Static_assert(offsetof(struct cpu_info, scratch_rsp) == 368,
               "update syscall_entry.asm scratch_rsp offset");
_Static_assert(offsetof(struct cpu_info, switching_from) == 376,
               "update switch.asm switching_from offset");
_Static_assert(offsetof(struct cpu_info, sigreturn_frame) == 384,
               "update syscall_entry.asm sigreturn offset");

// Public API

// Initialize the per-CPU subsystem.  Must be called after pmm_init() and
// acpi_init() so that physical memory can be allocated and APIC IDs are known.
void cpu_init(void);

// Wakes up all Application Processors (APs) discovered during cpu_init.
// Must be called after lapic_timer_init() so that APs can calibrate their
// timers.
void cpu_init_aps(void);

// Returns a pointer to the calling CPU's cpu_info (read from GS base).
struct cpu_info *cpu_get_current(void);

// Returns the total number of CPUs discovered in the MADT.
uint32_t cpu_get_count(void);

// Returns the cpu_info for a given logical CPU index.
struct cpu_info *cpu_get_info(uint32_t cpu_id);

// Returns the BSP's cpu_info.
struct cpu_info *cpu_get_bsp(void);

#endif
