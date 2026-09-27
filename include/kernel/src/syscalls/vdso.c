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

long __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused) {
    return vdso_syscall3(309, (long)cpu, (long)node, (long)unused);
}

long getcpu(unsigned *cpu, unsigned *node, void *unused) __attribute__((weak, alias("__vdso_getcpu")));
