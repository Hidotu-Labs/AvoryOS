#ifndef __AVORY_LINUXKPI_ASM_CURRENT_H
#define __AVORY_LINUXKPI_ASM_CURRENT_H

/* AvoryOS overlay for <asm/current.h>.
 *
 * The upstream header reads a per-CPU `pcpu_hot` block that does not exist
 * here.  `current` must be the per-thread Linux `task_struct` shadow: imported
 * code dereferences fields on it (`current->group_leader`, `signal_pending`
 * needs the shadow so it can find the native thread), and dma-fence stores it
 * in wait callbacks for wake_up_state().  Returning the native `struct thread`
 * here corrupts every one of those users.  The pcpu_hot declaration only
 * exists so <asm/processor.h>'s current_top_of_stack() compiles; its fields
 * are not maintained. */

#include <linux/types.h>

struct task_struct;

/* Per-thread task_struct shadow for the calling thread (linuxkpi/src/task.c).
 * Declared directly to keep this header independent of the LinuxKPI headers. */
void *linuxkpi_current_task(void);

static inline struct task_struct *get_current(void) {
  return (struct task_struct *)linuxkpi_current_task();
}

#define current get_current()

/* Layout mirrors upstream (the union pad is 64 bytes); only the type matters
 * for headers that name the fields. */
struct pcpu_hot {
  union {
    struct {
      struct task_struct *current_task;
      int preempt_count;
      int cpu_number;
      unsigned long top_of_stack;
      void *hardirq_stack_ptr;
      unsigned short softirq_pending;
      _Bool hardirq_stack_inuse;
    };
    unsigned char pad[64];
  };
};

extern struct pcpu_hot pcpu_hot;

#endif /* __AVORY_LINUXKPI_ASM_CURRENT_H */
