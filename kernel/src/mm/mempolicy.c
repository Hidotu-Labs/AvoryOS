#include "mm/mempolicy.h"
#include "mm/numa.h"
#include "smp/cpu.h"
#include "lib/string.h"

void mempolicy_init(struct mempolicy *pol) {
  if (!pol)
    return;
  pol->mode = MPOL_DEFAULT;
  pol->nodemask = 1ULL; // Node 0 default
  pol->preferred_node = 0;
  pol->interleave_idx = 0;
}

int mempolicy_copy(struct mempolicy *dst, const struct mempolicy *src) {
  if (!dst || !src)
    return -1;
  dst->mode = src->mode;
  dst->nodemask = src->nodemask;
  dst->preferred_node = src->preferred_node;
  dst->interleave_idx = src->interleave_idx;
  return 0;
}

int mempolicy_select_node(const struct mempolicy *pol, uint64_t vaddr) {
  struct cpu_info *cpu = cpu_get_current();
  int local_node = (cpu && cpu->cpu_id < MAX_CPUS) ? numa_cpu_to_node(cpu->cpu_id) : 0;

  if (!pol || pol->mode == MPOL_DEFAULT || pol->mode == MPOL_LOCAL) {
    return local_node;
  }

  if (pol->mode == MPOL_PREFERRED) {
    int pref = pol->preferred_node;
    if (pref >= 0 && pref < MAX_NUMA_NODES) {
      struct numa_node *n = numa_get_node(pref);
      if (n && n->online)
        return pref;
    }
    return local_node;
  }

  if (pol->mode == MPOL_BIND) {
    // If local node is in nodemask, prefer it!
    if ((pol->nodemask & (1ULL << (local_node % 64)))) {
      return local_node;
    }
    // Otherwise pick the closest online node in the mask
    struct numa_node *ln = numa_get_node(local_node);
    for (int fb = 0; fb < MAX_NUMA_NODES; fb++) {
      int cand = ln->fallback_order[fb];
      if (cand != NUMA_NO_NODE && (pol->nodemask & (1ULL << (cand % 64)))) {
        struct numa_node *cn = numa_get_node(cand);
        if (cn && cn->online)
          return cand;
      }
    }
    // Fallback: any bit in mask
    for (int i = 0; i < MAX_NUMA_NODES; i++) {
      if ((pol->nodemask & (1ULL << i))) {
        struct numa_node *cn = numa_get_node(i);
        if (cn && cn->online)
          return i;
      }
    }
    return local_node;
  }

  if (pol->mode == MPOL_INTERLEAVE) {
    // Collect online nodes in mask
    int eligible[MAX_NUMA_NODES];
    int count = 0;
    for (int i = 0; i < MAX_NUMA_NODES; i++) {
      if ((pol->nodemask & (1ULL << i))) {
        struct numa_node *cn = numa_get_node(i);
        if (cn && cn->online) {
          eligible[count++] = i;
        }
      }
    }
    if (count == 0)
      return local_node;

    uint32_t idx;
    if (vaddr != 0) {
      // Base interleave on virtual page address
      idx = (uint32_t)((vaddr >> 12) % (uint64_t)count);
    } else {
      // Sequential round-robin
      struct mempolicy *mut_pol = (struct mempolicy *)pol;
      idx = __atomic_fetch_add(&mut_pol->interleave_idx, 1, __ATOMIC_RELAXED) % count;
    }
    return eligible[idx];
  }

  return local_node;
}
