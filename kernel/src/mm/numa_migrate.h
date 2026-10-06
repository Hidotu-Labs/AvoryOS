#ifndef NUMA_MIGRATE_H
#define NUMA_MIGRATE_H

#include "sched/sched.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int numa_migrate_page(struct thread *t, uint64_t vaddr, int target_node);

uint64_t sys_move_pages(uint64_t pid, uint64_t count, uint64_t pages_uptr,
                        uint64_t nodes_uptr, uint64_t status_uptr,
                        uint64_t flags);

uint64_t sys_migrate_pages(uint64_t pid, uint64_t maxnode,
                          uint64_t old_nodes_uptr, uint64_t new_nodes_uptr,
                          uint64_t a4, uint64_t a5);

#endif
