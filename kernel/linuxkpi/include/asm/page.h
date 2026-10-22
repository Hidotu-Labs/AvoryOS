#ifndef __AVORY_LINUXKPI_ASM_PAGE_H
#define __AVORY_LINUXKPI_ASM_PAGE_H
#include_next <asm/page.h>
#ifndef __pfn_to_phys
#define __pfn_to_phys(pfn) ((phys_addr_t)(pfn) << PAGE_SHIFT)
#endif
/* Match the imported x86-64 four-level TASK_SIZE_MAX contract, including
 * Linux's top guard page. GSP receives this value as its maximum user VA;
 * passing 1<<47 would put that limit in the noncanonical address hole. */
#undef TASK_SIZE
#define TASK_SIZE ((1UL << 47) - PAGE_SIZE)
#endif
