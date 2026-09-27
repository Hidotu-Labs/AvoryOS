#ifndef CPU_FAULT_H
#define CPU_FAULT_H

#include <stdint.h>
#include "isr.h"

// Record of a userland fault
struct user_fault_record {
    uint32_t tid;
    uint32_t sig;
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr2;
    uint64_t err_code;
    char comm[16];
    char subsystem[32];
    char kernel_file[32];
    uint32_t kernel_line;
    char kernel_func[32];
    int64_t error_code;
    uint64_t last_syscall_num;
    int64_t last_syscall_ret;
    struct registers regs;
};

void fault_init(void);
void fault_log_add(struct registers *regs, int sig, uint64_t cr2);

#endif
