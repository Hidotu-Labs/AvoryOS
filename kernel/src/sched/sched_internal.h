#ifndef SCHED_INTERNAL_H
#define SCHED_INTERNAL_H

#include "sched.h"
#include "../lock/spinlock.h"
#include "../smp/cpu.h"
#include <stdbool.h>
#include <stdint.h>

#define THREAD_STACK_SIZE 16384

/* Global thread registry state */
extern spinlock_t tid_lock;
extern struct thread *global_thread_list;
extern uint32_t next_tid;

/* Stack allocation & lookaside caching */
void *thread_stack_alloc(void);
void thread_stack_release(uint64_t stack_base);
size_t thread_stack_cache_reclaim(size_t max_reclaim);
size_t kstack_cache_total_cached(void);
void kstack_cache_init(void);

/* Thread search under tid_lock */
struct thread *find_thread_by_tid_locked(uint32_t tid);

/* Fast PID / TID bitmap allocator under tid_lock */
uint32_t sched_alloc_tid_locked(void);
void sched_free_tid_locked(uint32_t tid);

/* Thread reaper operations */
bool sched_thread_off_cpu(struct thread *t);
void sched_process_reap_queue(struct thread *prev);

/* Runqueue / Deadline helpers */
void remove_from_runqueue(struct thread *t);
void sched_deadline_remove_locked(struct cpu_info *cpu, struct thread *t);
void sched_deadline_insert_locked(struct cpu_info *cpu, struct thread *t);
void sched_deadline_expire_locked(struct cpu_info *cpu, uint64_t now);
void sched_arm_next_deadline(struct cpu_info *cpu, struct thread *next_t);

#endif /* SCHED_INTERNAL_H */
