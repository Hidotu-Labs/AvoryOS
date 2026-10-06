#ifndef NUMA_H
#define NUMA_H

#include "mm/pmm_internal.h"
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_NUMA_NODES 16
#define NUMA_NO_NODE   (-1)
#define NUMA_LOCAL_DISTANCE  10
#define NUMA_REMOTE_DISTANCE 20

#define MAX_NUMA_MEM_RANGES 64

struct numa_mem_range {
  uint64_t base;
  uint64_t length;
  int node_id;
  uint32_t flags;
};

struct numa_node {
  int node_id;
  bool online;
  uint64_t phys_start;
  uint64_t phys_end;
  uint64_t total_memory;
  uint64_t usable_memory;
  uint64_t cpu_mask; // Bitmask of CPUs belonging to this node (up to 64 CPUs)
  uint32_t cpu_count;
  struct buddy_zone zone; // Per-node buddy allocator with per-node lock!
  uint8_t distance[MAX_NUMA_NODES];
  int fallback_order[MAX_NUMA_NODES];
};

void numa_init(struct limine_rsdp_response *rsdp, uint64_t hhdm_offset);
uint32_t numa_node_count(void);
bool numa_is_available(void);
int numa_cpu_to_node(uint32_t cpu_id);
void numa_register_cpu(uint32_t cpu_id, uint32_t apic_id);
int numa_phys_to_node(uint64_t phys);
uint8_t numa_distance(int from_node, int to_node);
struct numa_node *numa_get_node(int node_id);
void numa_register_memory_range(uint64_t base, uint64_t length, int node_id);
void numa_dump(void);

// Node-directed allocation APIs
void *pmm_alloc_pages_nodemask(size_t count, int node_id, uint64_t nodemask);
void *pmm_alloc_pages_node(size_t count, int node_id);
void *pmm_alloc_page_node(int node_id);
void *pmm_alloc_huge_page_node(int node_id);

#endif
