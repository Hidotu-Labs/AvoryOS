#ifndef CGROUPFS_H
#define CGROUPFS_H

#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

struct thread;
struct cgroupfs_group;

/* Which live view a cgroup control file presents. */
enum cgroupfs_file {
  CGROUPFS_PROCS = 0,
  CGROUPFS_THREADS,
  CGROUPFS_EVENTS,
  CGROUPFS_KILL,
  CGROUPFS_FREEZE,
  CGROUPFS_CONTROLLERS,
  CGROUPFS_SUBTREE_CONTROL,
  CGROUPFS_CPU_WEIGHT,
  CGROUPFS_CPU_MAX,
  CGROUPFS_CPU_STAT,
  CGROUPFS_CPU_PRESSURE,
  CGROUPFS_MEM_CURRENT,
  CGROUPFS_MEM_MIN,
  CGROUPFS_MEM_LOW,
  CGROUPFS_MEM_HIGH,
  CGROUPFS_MEM_MAX,
  CGROUPFS_MEM_OOM_GROUP,
  CGROUPFS_MEM_EVENTS,
  CGROUPFS_MEM_STAT,
  CGROUPFS_MEM_PRESSURE,
  CGROUPFS_PIDS_CURRENT,
  CGROUPFS_PIDS_MAX,
  CGROUPFS_PIDS_EVENTS,
  CGROUPFS_IO_PRESSURE,
};

enum psi_resource {
  PSI_CPU = 0,
  PSI_MEMORY = 1,
  PSI_IO = 2,
};

struct cgroupfs_group *cgroupfs_group_create(struct cgroupfs_group *parent);
/* Fails (returns false) while the group still has live member processes. */
bool cgroupfs_group_remove(struct cgroupfs_group *group);
void cgroupfs_bind_file(vfs_node_t *file, struct cgroupfs_group *group,
                        enum cgroupfs_file kind);

/* A new process starts in its parent's cgroup (Linux fork semantics). */
void cgroupfs_fork(struct thread *parent, struct thread *child);
/* clone3(CLONE_INTO_CGROUP): place the child in the cgroup dirfd names. */
int cgroupfs_clone_into(struct thread *child, vfs_node_t *dir);

/* pids controller: returns true if a fork into parent's (or cgroup_dir's)
 * cgroup is allowed under pids.max. Increments pids.events max on denial. */
bool cgroupfs_can_fork(struct thread *parent, vfs_node_t *cgroup_dir);

/* cpu controller: scale base EEVDF weight by the thread's cgroup cpu.weight. */
uint32_t cgroupfs_effective_cpu_weight(struct thread *t, uint32_t base_weight);

/* cpu controller: account dt_ns of CPU execution and enforce cpu.max quota. */
void cgroupfs_account_cpu_tick(struct thread *t, uint64_t dt_ns,
                               uint64_t now_ns, bool in_user);

/* Returns true if t's cgroup (or any ancestor) is frozen or cpu-throttled. */
bool cgroupfs_is_throttled_or_frozen(struct thread *t, uint64_t now_ns);

/* memory controller: check memory.high / memory.max before mapping `bytes`
 * of user memory. May reclaim, throttle briefly, or invoke cgroup OOM kill.
 * Returns 0 on success, -12 (-ENOMEM) if denied/killed. */
int cgroupfs_mem_try_charge(struct thread *t, uint64_t bytes);

/* PSI (Pressure Stall Information) tracking & formatting. */
void psi_record_stall(enum psi_resource res, struct cgroupfs_group *g,
                      bool full, uint64_t stall_ns);
void psi_tick(uint64_t now_ns);
uint32_t psi_format(enum psi_resource res, struct cgroupfs_group *g,
                    char *buf, uint32_t cap);

#endif
