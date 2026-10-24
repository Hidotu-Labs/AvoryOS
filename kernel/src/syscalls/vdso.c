#include <stdint.h>
#include <stddef.h>

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct vdso_data {
    uint64_t boot_tsc;
    uint64_t khz;
    uint64_t boot_sec;
    uint64_t tsc_hz;
    uint64_t sec_mult;
    uint64_t sec_shift;
    uint64_t mult_rem_ns;
    uint64_t mult_rem_us;
    uint64_t getcpu_mode; /* 0 = syscall fallback, 1 = rdtscp, 2 = rdpid */
};

static inline const struct vdso_data *get_vdso_data(void) {
    uintptr_t rip;
    __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
    uintptr_t page_base = rip & ~0xFFFULL;
    return (const struct vdso_data *)(page_base + 0xE00);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t read_tsc_aux_rdtscp(void) {
    uint32_t aux;
    __asm__ volatile("rdtscp" : "=c"(aux) : : "rax", "rdx");
    return aux;
}

static inline uint32_t read_tsc_aux_rdpid(void) {
    uint64_t aux;
    /* rdpid %rax encoded as F3 0F C7 F8 so any assembler version accepts it */
    __asm__ volatile(".byte 0xf3, 0x0f, 0xc7, 0xf8" : "=a"(aux));
    return (uint32_t)aux;
}

static inline long vdso_syscall2(long nr, long arg1, long arg2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(arg1), "S"(arg2) : "rcx", "r11", "memory");
    return ret;
}

static inline long vdso_syscall3(long nr, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3) : "rcx", "r11", "memory");
    return ret;
}

int __vdso_clock_gettime(int clock_id, struct timespec *ts) {
    if (!ts)
        return -14; // -EFAULT

    const struct vdso_data *data = get_vdso_data();
    if (!data->tsc_hz || !data->sec_mult) {
        return (int)vdso_syscall2(228, clock_id, (long)ts);
    }

    if (clock_id == CLOCK_MONOTONIC || clock_id == CLOCK_MONOTONIC_RAW ||
        clock_id == CLOCK_BOOTTIME || clock_id == CLOCK_MONOTONIC_COARSE) {
        uint64_t tsc = rdtsc();
        uint64_t delta = (tsc > data->boot_tsc) ? (tsc - data->boot_tsc) : 0;
        
        uint64_t sec = (uint64_t)(((__uint128_t)delta * data->sec_mult) >> (64 + data->sec_shift));
        uint64_t sec_tsc = (sec * data->tsc_hz);
        uint64_t rem_tsc = (delta > sec_tsc) ? (delta - sec_tsc) : 0;
        uint64_t nsec = ((uint64_t)(((__uint128_t)rem_tsc * data->mult_rem_ns) >> 32));
        if (nsec >= 1000000000ULL) nsec = 999999999ULL;

        ts->tv_sec = (int64_t)sec;
        ts->tv_nsec = (int64_t)nsec;
        return 0;
    } else if (clock_id == CLOCK_REALTIME || clock_id == CLOCK_REALTIME_COARSE) {
        uint64_t tsc = rdtsc();
        uint64_t delta = (tsc > data->boot_tsc) ? (tsc - data->boot_tsc) : 0;
        
        uint64_t sec = (uint64_t)(((__uint128_t)delta * data->sec_mult) >> (64 + data->sec_shift));
        uint64_t sec_tsc = (sec * data->tsc_hz);
        uint64_t rem_tsc = (delta > sec_tsc) ? (delta - sec_tsc) : 0;
        uint64_t nsec = ((uint64_t)(((__uint128_t)rem_tsc * data->mult_rem_ns) >> 32));
        if (nsec >= 1000000000ULL) nsec = 999999999ULL;

        ts->tv_sec = (int64_t)(data->boot_sec + sec);
        ts->tv_nsec = (int64_t)nsec;
        return 0;
    }

    return (int)vdso_syscall2(228, clock_id, (long)ts);
}

int clock_gettime(int clock_id, struct timespec *ts) __attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (!tv)
        return 0;

    const struct vdso_data *data = get_vdso_data();
    if (!data->tsc_hz || !data->sec_mult) {
        return (int)vdso_syscall2(96, (long)tv, (long)tz);
    }

    uint64_t tsc = rdtsc();
    uint64_t delta = (tsc > data->boot_tsc) ? (tsc - data->boot_tsc) : 0;
    
    uint64_t sec = (uint64_t)(((__uint128_t)delta * data->sec_mult) >> (64 + data->sec_shift));
    uint64_t sec_tsc = (sec * data->tsc_hz);
    uint64_t rem_tsc = (delta > sec_tsc) ? (delta - sec_tsc) : 0;
    uint64_t usec = ((uint64_t)(((__uint128_t)rem_tsc * data->mult_rem_us) >> 32));
    if (usec >= 1000000ULL) usec = 999999ULL;

    tv->tv_sec = (int64_t)(data->boot_sec + sec);
    tv->tv_usec = (int64_t)usec;
    return 0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) __attribute__((weak, alias("__vdso_gettimeofday")));

int64_t __vdso_time(int64_t *tloc) {
    const struct vdso_data *data = get_vdso_data();
    if (!data->tsc_hz || !data->sec_mult) {
        return vdso_syscall2(201, (long)tloc, 0);
    }

    uint64_t tsc = rdtsc();
    uint64_t delta = (tsc > data->boot_tsc) ? (tsc - data->boot_tsc) : 0;
    uint64_t sec = (uint64_t)(((__uint128_t)delta * data->sec_mult) >> (64 + data->sec_shift));
    int64_t now = (int64_t)(data->boot_sec + sec);
    if (tloc)
        *tloc = now;
    return now;
}

int64_t time(int64_t *tloc) __attribute__((weak, alias("__vdso_time")));

__attribute__((section(".text.getcpu")))
long __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused) {
    (void)unused;
    const struct vdso_data *data = get_vdso_data();
    uint64_t mode = data->getcpu_mode;
    uint32_t aux;
    if (mode == 2) {
        aux = read_tsc_aux_rdpid();
    } else if (mode == 1) {
        aux = read_tsc_aux_rdtscp();
    } else {
        /* Fallback when neither RDPID nor RDTSCP is supported: extract the
         * per-CPU TSS selector from TR (GDT_TSS_BASE=6, 2 slots per CPU). */
        uint16_t sel;
        __asm__ volatile("str %0" : "=r"(sel));
        uint32_t idx = (uint32_t)(sel >> 3);
        aux = (idx >= 6U) ? ((idx - 6U) >> 1) : 0U;
    }
    if (cpu)
        *cpu = aux & 0xFFFU;
    if (node)
        *node = (aux >> 12) & 0xFFFU;
    return 0;
}

long getcpu(unsigned *cpu, unsigned *node, void *unused) __attribute__((weak, alias("__vdso_getcpu")));
