#ifndef MM_PCID_H
#define MM_PCID_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define PCID_KERNEL        0
#define PCID_MIN           1
#define PCID_MAX           4095
#define PCID_COUNT         4096

#define CR3_NOFLUSH        (1ULL << 63)
#define CR3_PCID_MASK      0x0000000000000FFFULL
#define CR3_ADDR_MASK      0x000FFFFFFFFFF000ULL

#define INVPCID_TYPE_INDIV_ADDR       0
#define INVPCID_TYPE_SINGLE_CTXT      1
#define INVPCID_TYPE_ALL_NON_GLOBAL   2
#define INVPCID_TYPE_ALL_INCL_GLOBAL  3

struct invpcid_desc {
  uint64_t pcid : 12;
  uint64_t rsvd : 52;
  uint64_t addr;
} __attribute__((packed));

struct cpu_info;

// PCID Manager Initialization
void pcid_init(void);

// PCID Allocation & Lifetime
uint16_t pcid_alloc(void);
void pcid_free(uint16_t pcid);

// Per-CPU TLB Caching State
bool cpu_pcid_is_cached(struct cpu_info *cpu, uint16_t pcid);
void cpu_pcid_mark_cached(struct cpu_info *cpu, uint16_t pcid);
void cpu_pcid_invalidate(struct cpu_info *cpu, uint16_t pcid);
void cpu_pcid_invalidate_all(struct cpu_info *cpu);

// Hardware Invalidation
static inline void invpcid(uint64_t type, struct invpcid_desc *desc) {
  __asm__ volatile("invpcid (%1), %0" : : "r"(type), "r"(desc) : "memory");
}

void pcid_flush_context(uint16_t pcid);
void pcid_flush_all(void);

#endif
