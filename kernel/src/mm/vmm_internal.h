#ifndef VMM_INTERNAL_H
#define VMM_INTERNAL_H

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

void vmm_dbg_out(const char *s);
void vmm_dbg_hex(uint64_t v);
uint64_t *vmm_get_pte_ptr(uint64_t *pml4_phys, uint64_t virtual_addr);

struct thread;
struct registers;

int pf_reject_record(uint64_t cr2, uint64_t error_code,
                     const struct registers *regs,
                     const struct thread *current, const char *reason,
                     const char *file, uint32_t line, uint64_t detail,
                     uint64_t detail2);

#define PF_REJECT(reason, detail, detail2)                                     \
  pf_reject_record(cr2, error_code, regs, current, (reason), __FILE__,         \
                   __LINE__, (uint64_t)(detail), (uint64_t)(detail2))

#endif
