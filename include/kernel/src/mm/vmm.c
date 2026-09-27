// vmm.c — Virtual Memory Manager: shared state and internal accessors
//
// This file owns the global kernel_pml4 pointer and provides the two
// internal helpers that tie the vmm_*.c modules together.
//
// Functional code lives in the following modules:
//   vmm_init.c  — boot-time page-table setup and protection
//   vmm_map.c   — page mapping, unmapping and address translation
//   vmm_clone.c — address-space cloning (fork / exec)
//   vmm_free.c  — user-space teardown
//   vmm_fault.c — demand paging and #PF handling

#include "vmm.h"
#include "../lock/spinlock.h"
#include <stdint.h>

// Physical address of the permanent kernel PML4, set once during vmm_init.
static uint64_t *kernel_pml4 = NULL;

uint64_t *vmm_get_active_pml4(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  return (uint64_t *)(cr3 & PAGE_MASK);
}

uint64_t *vmm_get_kernel_pml4(void) { return kernel_pml4; }

void vmm_set_kernel_pml4(uint64_t *pml4_phys) { kernel_pml4 = pml4_phys; }
