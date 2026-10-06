#include "mm/numa.h"
#include "acpi/acpi.h"
#include "console/klog.h"
#include "lib/string.h"
#include "smp/cpu.h"

static struct numa_node numa_nodes[MAX_NUMA_NODES];
static uint32_t numa_active_nodes = 0;
static bool numa_available = false;

static struct numa_mem_range numa_ranges[MAX_NUMA_MEM_RANGES];
static uint32_t numa_range_count = 0;

static int numa_cpu_nodes[MAX_CPUS];
static int numa_apic_nodes[256];

uint32_t numa_node_count(void) {
  return numa_active_nodes > 0 ? numa_active_nodes : 1;
}

bool numa_is_available(void) {
  return numa_available;
}

struct numa_node *numa_get_node(int node_id) {
  if (node_id < 0 || node_id >= MAX_NUMA_NODES || !numa_nodes[node_id].online)
    return &numa_nodes[0];
  return &numa_nodes[node_id];
}

int numa_cpu_to_node(uint32_t cpu_id) {
  if (cpu_id >= MAX_CPUS)
    return 0;
  return numa_cpu_nodes[cpu_id];
}

uint8_t numa_distance(int from_node, int to_node) {
  if (from_node < 0 || from_node >= MAX_NUMA_NODES ||
      to_node < 0 || to_node >= MAX_NUMA_NODES)
    return NUMA_REMOTE_DISTANCE;
  return numa_nodes[from_node].distance[to_node];
}

void numa_register_memory_range(uint64_t base, uint64_t length, int node_id) {
  if (node_id < 0 || node_id >= MAX_NUMA_NODES || length == 0)
    return;

  if (numa_range_count < MAX_NUMA_MEM_RANGES) {
    numa_ranges[numa_range_count].base = base;
    numa_ranges[numa_range_count].length = length;
    numa_ranges[numa_range_count].node_id = node_id;
    numa_ranges[numa_range_count].flags = 1;
    numa_range_count++;
  }

  struct numa_node *n = &numa_nodes[node_id];
  n->online = true;
  if (n->phys_start == 0 || base < n->phys_start)
    n->phys_start = base;
  if (base + length > n->phys_end)
    n->phys_end = base + length;
  n->total_memory += length;
}

int numa_phys_to_node(uint64_t phys) {
  for (uint32_t i = 0; i < numa_range_count; i++) {
    if (phys >= numa_ranges[i].base &&
        phys < numa_ranges[i].base + numa_ranges[i].length) {
      return numa_ranges[i].node_id;
    }
  }
  return 0; // Default to Node 0
}

void numa_register_cpu(uint32_t cpu_id, uint32_t apic_id) {
  if (cpu_id >= MAX_CPUS)
    return;

  int node = (apic_id < 256) ? numa_apic_nodes[apic_id] : 0;
  if (node < 0 || node >= MAX_NUMA_NODES || !numa_nodes[node].online)
    node = 0;

  numa_cpu_nodes[cpu_id] = node;
  numa_nodes[node].cpu_mask |= (1ULL << (cpu_id % 64));
  numa_nodes[node].cpu_count++;
}

static void numa_sort_fallback_order(int node_id) {
  struct numa_node *node = &numa_nodes[node_id];
  int count = 0;

  node->fallback_order[count++] = node_id;

  for (int pass = 0; pass < MAX_NUMA_NODES; pass++) {
    int best_node = -1;
    uint8_t best_dist = 255;

    for (int other = 0; other < MAX_NUMA_NODES; other++) {
      if (!numa_nodes[other].online || other == node_id)
        continue;

      // Check if already in fallback list
      bool already = false;
      for (int k = 0; k < count; k++) {
        if (node->fallback_order[k] == other) {
          already = true;
          break;
        }
      }
      if (already)
        continue;

      if (node->distance[other] < best_dist) {
        best_dist = node->distance[other];
        best_node = other;
      }
    }

    if (best_node >= 0) {
      node->fallback_order[count++] = best_node;
    } else {
      break;
    }
  }

  while (count < MAX_NUMA_NODES) {
    node->fallback_order[count++] = NUMA_NO_NODE;
  }
}

void numa_init(struct limine_rsdp_response *rsdp, uint64_t hhdm_offset) {
  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    numa_nodes[i].node_id = i;
    numa_nodes[i].online = false;
    numa_nodes[i].phys_start = 0;
    numa_nodes[i].phys_end = 0;
    numa_nodes[i].total_memory = 0;
    numa_nodes[i].usable_memory = 0;
    numa_nodes[i].cpu_mask = 0;
    numa_nodes[i].cpu_count = 0;
    buddy_zone_init(&numa_nodes[i].zone);

    for (int j = 0; j < MAX_NUMA_NODES; j++) {
      numa_nodes[i].distance[j] = (i == j) ? NUMA_LOCAL_DISTANCE : NUMA_REMOTE_DISTANCE;
      numa_nodes[i].fallback_order[j] = NUMA_NO_NODE;
    }
  }

  for (int i = 0; i < MAX_CPUS; i++)
    numa_cpu_nodes[i] = 0;
  for (int i = 0; i < 256; i++)
    numa_apic_nodes[i] = 0;

  numa_range_count = 0;

  struct acpi_srat *srat = NULL;
  if (rsdp && rsdp->address) {
    srat = (struct acpi_srat *)acpi_early_find_table(rsdp, hhdm_offset, "SRAT");
  }

  if (srat && srat->header.length > sizeof(struct acpi_srat)) {
    uint8_t *entry_ptr = srat->entries;
    uint8_t *end = (uint8_t *)srat + srat->header.length;

    while (entry_ptr < end) {
      struct acpi_srat_entry_header *hdr = (struct acpi_srat_entry_header *)entry_ptr;
      if (hdr->length == 0)
        break; // Malformed entry guard

      if (hdr->type == 0 && hdr->length >= sizeof(struct acpi_srat_cpu_affinity)) {
        struct acpi_srat_cpu_affinity *cpu = (struct acpi_srat_cpu_affinity *)entry_ptr;
        if (cpu->flags & 1) {
          uint32_t domain = cpu->proximity_domain_lo |
                            ((uint32_t)cpu->proximity_domain_hi[0] << 8) |
                            ((uint32_t)cpu->proximity_domain_hi[1] << 16) |
                            ((uint32_t)cpu->proximity_domain_hi[2] << 24);
          if (domain < MAX_NUMA_NODES) {
            numa_apic_nodes[cpu->apic_id] = (int)domain;
            numa_nodes[domain].online = true;
          }
        }
      } else if (hdr->type == 1 && hdr->length >= sizeof(struct acpi_srat_mem_affinity)) {
        struct acpi_srat_mem_affinity *mem = (struct acpi_srat_mem_affinity *)entry_ptr;
        if (mem->flags & 1) {
          uint64_t base = (uint64_t)mem->base_address_lo | ((uint64_t)mem->base_address_hi << 32);
          uint64_t length = (uint64_t)mem->length_lo | ((uint64_t)mem->length_hi << 32);
          uint32_t domain = mem->proximity_domain;
          if (domain < MAX_NUMA_NODES && length > 0) {
            numa_register_memory_range(base, length, (int)domain);
          }
        }
      } else if (hdr->type == 2 && hdr->length >= sizeof(struct acpi_srat_x2apic_affinity)) {
        struct acpi_srat_x2apic_affinity *x2 = (struct acpi_srat_x2apic_affinity *)entry_ptr;
        if (x2->flags & 1) {
          uint32_t domain = x2->proximity_domain;
          if (domain < MAX_NUMA_NODES && x2->x2apic_id < 256) {
            numa_apic_nodes[x2->x2apic_id] = (int)domain;
            numa_nodes[domain].online = true;
          }
        }
      }

      entry_ptr += hdr->length;
    }

    // Parse SLIT if present
    struct acpi_slit *slit = (struct acpi_slit *)acpi_early_find_table(rsdp, hhdm_offset, "SLIT");
    if (slit && slit->localities_count > 0) {
      uint64_t count = slit->localities_count;
      for (uint64_t i = 0; i < count && i < MAX_NUMA_NODES; i++) {
        for (uint64_t j = 0; j < count && j < MAX_NUMA_NODES; j++) {
          numa_nodes[i].distance[j] = slit->matrix[i * count + j];
        }
      }
    }
  }

  // Count active nodes
  numa_active_nodes = 0;
  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    if (numa_nodes[i].online)
      numa_active_nodes++;
  }

  if (numa_active_nodes == 0) {
    // Non-NUMA / UMA fallback: 1 node (Node 0)
    numa_nodes[0].online = true;
    numa_active_nodes = 1;
    numa_available = false;
  } else {
    numa_available = (numa_active_nodes > 1);
  }

  // Build fallback orders for each active node
  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    if (numa_nodes[i].online) {
      numa_sort_fallback_order(i);
    }
  }

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " NUMA Subsystem Initialized: ");
  klog_uint64(numa_active_nodes);
  if (numa_available) {
    klog_puts(" active nodes discovered (ACPI SRAT/SLIT).\n");
  } else {
    klog_puts(" node (UMA fallback mode).\n");
  }
}

void numa_dump(void) {
  klog_puts("--- NUMA Topology ---\n");
  klog_puts("Active nodes: ");
  klog_uint64(numa_active_nodes);
  klog_puts("\n");

  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    if (!numa_nodes[i].online)
      continue;
    klog_puts("  Node ");
    klog_uint64(i);
    klog_puts(": CPUs=");
    klog_uint64(numa_nodes[i].cpu_count);
    klog_puts(" TotalRAM=");
    klog_uint64(numa_nodes[i].total_memory / (1024 * 1024));
    klog_puts("MB Usable=");
    klog_uint64(numa_nodes[i].usable_memory / (1024 * 1024));
    klog_puts("MB [0x");
    klog_hex64(numa_nodes[i].phys_start);
    klog_puts(" - 0x");
    klog_hex64(numa_nodes[i].phys_end);
    klog_puts("]\n");
  }
}

void *pmm_alloc_pages_nodemask(size_t count, int node_id, uint64_t nodemask) {
  if (count == 0)
    return NULL;

  if (node_id < 0 || node_id >= MAX_NUMA_NODES || !numa_nodes[node_id].online)
    node_id = 0;

  size_t order = pmm_get_order(count);

  // 1. Try local node first if permitted by nodemask
  if (nodemask == 0 || (nodemask & (1ULL << (node_id % 64)))) {
    void *ptr = buddy_alloc_internal(&numa_nodes[node_id].zone, order);
    if (ptr)
      return ptr;
  }

  // 2. Fall back to nearest available nodes according to SLIT distance
  for (int i = 0; i < MAX_NUMA_NODES; i++) {
    int target = numa_nodes[node_id].fallback_order[i];
    if (target == NUMA_NO_NODE || target == node_id)
      continue;
    if (!numa_nodes[target].online)
      continue;
    if (nodemask != 0 && !(nodemask & (1ULL << (target % 64))))
      continue;

    void *ptr = buddy_alloc_internal(&numa_nodes[target].zone, order);
    if (ptr)
      return ptr;
  }

  return NULL;
}

void *pmm_alloc_pages_node(size_t count, int node_id) {
  return pmm_alloc_pages_nodemask(count, node_id, 0);
}

void *pmm_alloc_page_node(int node_id) {
  return pmm_alloc_pages_node(1, node_id);
}

void *pmm_alloc_huge_page_node(int node_id) {
  void *phys = pmm_alloc_pages_node(512, node_id);
  if (!phys)
    return NULL;
  if ((uint64_t)phys & 0x1FFFFFULL) {
    pmm_free_pages(phys, 512);
    return NULL;
  }
  return phys;
}
