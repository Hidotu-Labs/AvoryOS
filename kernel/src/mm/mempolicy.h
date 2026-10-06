#ifndef MM_MEMPOLICY_H
#define MM_MEMPOLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NUMA Memory Policies (Linux ABI compatible) */
#define MPOL_DEFAULT    0
#define MPOL_PREFERRED  1
#define MPOL_BIND       2
#define MPOL_INTERLEAVE 3
#define MPOL_LOCAL      4
#define MPOL_MAX        5

/* Flags for sys_set_mempolicy / sys_mbind / sys_get_mempolicy */
#define MPOL_F_STATIC_NODES   (1 << 15)
#define MPOL_F_RELATIVE_NODES (1 << 14)
#define MPOL_MODE_FLAGS       (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES)

#define MPOL_F_NODE         (1 << 0)
#define MPOL_F_ADDR         (1 << 1)
#define MPOL_F_MEMS_ALLOWED (1 << 2)

/* Flags for mbind / migrate */
#define MPOL_MF_STRICT      (1 << 0)
#define MPOL_MF_MOVE        (1 << 1)
#define MPOL_MF_MOVE_ALL    (1 << 2)

struct mempolicy {
  int mode;
  uint64_t nodemask;       // Bitmask of allowed NUMA nodes (0..63)
  int preferred_node;      // For MPOL_PREFERRED
  uint32_t interleave_idx; // State for MPOL_INTERLEAVE round-robin
};

void mempolicy_init(struct mempolicy *pol);
int mempolicy_select_node(const struct mempolicy *pol, uint64_t vaddr);
int mempolicy_copy(struct mempolicy *dst, const struct mempolicy *src);

#endif
