#ifndef CPU_MSR_H
#define CPU_MSR_H

#include <stdint.h>

#define IA32_SPEC_CTRL        0x00000048U
#define SPEC_CTRL_IBRS        (1ULL << 0)
#define SPEC_CTRL_STIBP       (1ULL << 1)
#define SPEC_CTRL_SSBD        (1ULL << 2)
#define IA32_PRED_CMD         0x00000049U
#define PRED_CMD_IBPB         (1ULL << 0)
#define IA32_ARCH_CAPABILITIES 0x0000010AU
#define ARCH_CAP_MDS_NO       (1ULL << 5)
#define ARCH_CAP_TAA_NO       (1ULL << 8)

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#endif
