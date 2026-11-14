#ifndef CGROUPFS_H
#define CGROUPFS_H

#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

struct thread;
struct cgroupfs_group;

/* Which live view a cgroup control file presents. */
enum cgroupfs_file {
  CGROUPFS_PROCS,
  CGROUPFS_THREADS,
  CGROUPFS_EVENTS,
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

#endif
