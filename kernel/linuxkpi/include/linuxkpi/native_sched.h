#ifndef LINUXKPI_NATIVE_SCHED_H
#define LINUXKPI_NATIVE_SCHED_H

/* Native scheduler / time bridge for LinuxKPI implementation files.
 * Implementations: kernel/src/linuxkpi/native_sched.c (native headers only).
 *
 * No stdint/stddef typedefs are used here so the header cannot conflict with
 * the Linux headers the implementation files also include. */

/* The calling thread's native `struct thread *`, as an opaque handle. */
void *linuxkpi_current_thread(void);

/* Create and enqueue a kernel thread with an argument.  Returns the opaque
 * thread handle, or 0 on failure.  `name` is truncated to 15 characters. */
void *linuxkpi_kthread_create(void (*fn)(void *), void *arg, const char *name);

/* Block the calling thread with no timeout; woken by linuxkpi_wake_thread(). */
void linuxkpi_thread_block(void);

/* Block the calling thread for up to `ms` milliseconds.
 * Returns 1 if woken early and 0 if the timeout expired. */
int linuxkpi_schedule_timeout_ms(unsigned long ms);

/* Wake a thread previously blocked by the helpers above. */
void linuxkpi_wake_thread(void *thread);

/* Yield the calling thread once (native sched_yield()). */
void linuxkpi_yield(void);

/* Wake every thread parked on a native wait queue (an opaque `wait_queue_t *`)
 * that the KPI poll bridge attached to a struct wait_queue_head. */
void linuxkpi_wake_poll_queue(void *native_wq);

/* True when the native scheduler has asked the calling thread to reschedule
 * (sched_wakeup() on this CPU or a reschedule IPI). */
_Bool linuxkpi_need_resched(void);

/* Hardirq nesting: the native ISR wraps hardware-IRQ dispatch with
 * linuxkpi_irq_enter()/linuxkpi_irq_exit(); the depth accessors back
 * in_interrupt()/in_softirq().  Depth is per-thread (see struct thread). */
void linuxkpi_irq_enter(void);
void linuxkpi_irq_exit(void);
int linuxkpi_irq_depth(void);
int linuxkpi_softirq_depth(void);

/* True if the thread has a pending signal. */
_Bool linuxkpi_thread_has_pending_signal(void *thread);

/* The calling thread's thread-group id (== pid for the group leader).  Used
 * to back Linux's task_tgid() for the DRM master checks. */
unsigned long linuxkpi_current_tgid(void);

/* Per-thread Linux task_struct shadow.  `linuxkpi_current_task()` lazily
 * allocates one through the weak linuxkpi_task_shadow_new() hook (defined on
 * the Linux side, which knows sizeof(struct task_struct)); the kernel always
 * links that definition, the weak form only exists so native_sched.c stays
 * native-header-only.
 *
 *   struct task_struct *tsk = linuxkpi_current_task();  // current
 *   void *thread = linuxkpi_task_thread(tsk);           // back to native
 */
void *linuxkpi_current_task(void);
void *linuxkpi_task_for_thread(void *thread);
void *linuxkpi_task_thread(void *task);
unsigned long linuxkpi_thread_tgid(void *thread);
unsigned long linuxkpi_thread_pid(void *thread);
void linuxkpi_thread_comm(void *thread, char *buf, unsigned long size);

/* Monotonic time since boot. */
unsigned long long linuxkpi_monotonic_ms(void);
unsigned long long linuxkpi_monotonic_ns(void);

/* Number of online CPUs. */
int linuxkpi_cpu_count(void);

/* Busy-wait for `ns` nanoseconds (TSC based). */
void linuxkpi_udelay_ns(unsigned long long ns);

/* Per-thread LinuxKPI control block attached to a native thread (see
 * kernel/src/sched/sched.h kpi_data).  `linuxkpi_thread_self_data` reads the
 * calling thread's block. */
void *linuxkpi_thread_data(void *thread);
void *linuxkpi_thread_self_data(void);

/* mmap bridge: the Linux vm_area_struct produced by the last device mmap in
 * this thread, consumed by sys_mmap() right after node->mmap() returns. */
void linuxkpi_vma_set_pending(void *linux_vma);
void *linuxkpi_vma_take_pending(void);

#endif /* LINUXKPI_NATIVE_SCHED_H */
