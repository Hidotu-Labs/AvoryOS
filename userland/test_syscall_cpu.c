/*
 * test_syscall_cpu.c - AscentOS / Linux Per-Syscall CPU Usage Benchmark Suite
 *
 * Companion to test_syscall_speed.c.  Same 8 categories + 1 extended category,
 * same "Valid / Invalid / payload-size / flag-variant" situations, but each
 * scenario reports CPU cost instead of only wall-clock cost:
 *
 *   wall avg ns  - CLOCK_MONOTONIC delta per op (same basis as speed test)
 *   cpu avg ns   - CLOCK_THREAD_CPUTIME_ID delta per op (user+sys CPU burned)
 *   cpu %        - cpu / wall * 100 (100% = fully CPU-bound, ~0% = sleeping)
 *   cycles/op    - RDTSC delta per op on x86_64 (finest-grained CPU signal)
 *   csw          - voluntary + involuntary context-switch delta (RUSAGE_THREAD)
 *
 * Portability notes:
 *   - On Linux all clocks are real: THREAD_CPUTIME is scheduler-accounted CPU.
 *   - On AvoryOS CLOCK_THREAD_CPUTIME_ID is currently aliased to monotonic
 *     (see kernel/src/syscalls/sys_arch.c) and getrusage() is 1ms-granular
 *     (see sys_process.c: runtime_total ticks).  There the cycles/op column
 *     is the most precise per-syscall CPU signal; cpu% will read ~100% for
 *     spin loops by construction.
 *   - Destructive syscalls (mount/chroot/swapon/...) are exercised only on
 *     guaranteed-failing error paths.  reboot/poweroff are deliberately NOT
 *     covered: reboot(2) with valid magics really reboots the machine.
 *
 * Usage: test_syscall_cpu [iterations_per_scenario]   (default 1000)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/sendfile.h>
#include <sys/ptrace.h>
#include <sched.h>
#include <dirent.h>
#include <linux/futex.h>
#include <linux/membarrier.h>

/* Some toolchains lag on newer __NR_ names; map them if present. */
#ifndef SYS_gettid
#ifdef __NR_gettid
#define SYS_gettid __NR_gettid
#endif
#endif
#ifndef SYS_statx
#ifdef __NR_statx
#define SYS_statx __NR_statx
#endif
#endif
#ifndef SYS_faccessat2
#ifdef __NR_faccessat2
#define SYS_faccessat2 __NR_faccessat2
#endif
#endif
#ifndef SYS_close_range
#ifdef __NR_close_range
#define SYS_close_range __NR_close_range
#endif
#endif
#ifndef SYS_memfd_create
#ifdef __NR_memfd_create
#define SYS_memfd_create __NR_memfd_create
#endif
#endif
#ifndef SYS_getrandom
#ifdef __NR_getrandom
#define SYS_getrandom __NR_getrandom
#endif
#endif
#ifndef SYS_pidfd_open
#ifdef __NR_pidfd_open
#define SYS_pidfd_open __NR_pidfd_open
#endif
#endif
#ifndef SYS_getcpu
#ifdef __NR_getcpu
#define SYS_getcpu __NR_getcpu
#endif
#endif
#ifndef SYS_kcmp
#ifdef __NR_kcmp
#define SYS_kcmp __NR_kcmp
#endif
#endif
#ifndef SYS_membarrier
#ifdef __NR_membarrier
#define SYS_membarrier __NR_membarrier
#endif
#endif
#ifndef SYS_sched_getattr
#ifdef __NR_sched_getattr
#define SYS_sched_getattr __NR_sched_getattr
#endif
#endif
#ifndef SYS_copy_file_range
#ifdef __NR_copy_file_range
#define SYS_copy_file_range __NR_copy_file_range
#endif
#endif
#ifndef SYS_newfstatat
#ifdef __NR_newfstatat
#define SYS_newfstatat __NR_newfstatat
#elif defined(SYS_fstatat)
#define SYS_newfstatat SYS_fstatat
#endif
#endif
#ifndef SYS_access
#ifdef __NR_access
#define SYS_access __NR_access
#endif
#endif
#ifndef SYS_getdents64
#ifdef __NR_getdents64
#define SYS_getdents64 __NR_getdents64
#endif
#endif
#ifndef SYS_prlimit64
#ifdef __NR_prlimit64
#define SYS_prlimit64 __NR_prlimit64
#endif
#endif
#ifndef SYS_set_robust_list
#ifdef __NR_set_robust_list
#define SYS_set_robust_list __NR_set_robust_list
#endif
#endif
#ifndef SYS_get_robust_list
#ifdef __NR_get_robust_list
#define SYS_get_robust_list __NR_get_robust_list
#endif
#endif
#ifndef SYS_set_tid_address
#ifdef __NR_set_tid_address
#define SYS_set_tid_address __NR_set_tid_address
#endif
#endif
#ifndef SYS_statfs
#ifdef __NR_statfs
#define SYS_statfs __NR_statfs
#endif
#endif
#ifndef SYS_fstatfs
#ifdef __NR_fstatfs
#define SYS_fstatfs __NR_fstatfs
#endif
#endif
#ifndef SYS_sync_file_range
#ifdef __NR_sync_file_range
#define SYS_sync_file_range __NR_sync_file_range
#endif
#endif
#ifndef SYS_preadv
#ifdef __NR_preadv
#define SYS_preadv __NR_preadv
#endif
#endif
#ifndef SYS_pwritev
#ifdef __NR_pwritev
#define SYS_pwritev __NR_pwritev
#endif
#endif
#ifndef SYS_recvmmsg
#ifdef __NR_recvmmsg
#define SYS_recvmmsg __NR_recvmmsg
#endif
#endif
#ifndef SYS_sendmmsg
#ifdef __NR_sendmmsg
#define SYS_sendmmsg __NR_sendmmsg
#endif
#endif
#ifndef SYS_times
#ifdef __NR_times
#define SYS_times __NR_times
#endif
#endif
#ifndef SYS_arch_prctl
#ifdef __NR_arch_prctl
#define SYS_arch_prctl __NR_arch_prctl
#endif
#endif

#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE 1
#endif
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef KCMP_FILE
#define KCMP_FILE 0
#endif
#ifndef MEMBARRIER_CMD_QUERY
#define MEMBARRIER_CMD_QUERY 0
#endif
#ifndef ARCH_GET_FS
#define ARCH_GET_FS 0x1003
#endif
#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x000007ffU
#endif

#define DEFAULT_ITERATIONS 1000

typedef struct {
    const char *category;
    const char *syscall_name;
    const char *situation;
    uint64_t wall_total_ns;
    uint64_t wall_min_ns;
    uint64_t wall_max_ns;
    double wall_avg_ns;
    uint64_t cpu_total_ns;
    double cpu_avg_ns;
    double cpu_pct;        /* cpu/wall*100 */
    uint64_t cycles_total;
    double cycles_per_op;
    double wall_ops_per_sec;
    double cpu_ops_per_sec;
    int success_count;
    int error_count;
    long nvcsw_delta;
    long nivcsw_delta;
} cpu_result_t;

static cpu_result_t results[768];
static size_t num_results = 0;

static inline uint64_t get_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Thread CPU time; falls back to monotonic if the clock is unavailable. */
static inline uint64_t get_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rdtsc_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

/* RUSAGE_THREAD when available gives per-thread counters; else SELF. */
static void sample_rusage(struct rusage *ru) {
#if defined(RUSAGE_THREAD)
    if (getrusage(RUSAGE_THREAD, ru) == 0) return;
#endif
    getrusage(RUSAGE_SELF, ru);
}

static void add_cpu_result(const char *category, const char *syscall_name,
                           const char *situation,
                           uint64_t wall_total, uint64_t wall_min, uint64_t wall_max,
                           uint64_t cpu_total, uint64_t cycles_total,
                           unsigned iterations, int successes, int errors,
                           long nvcsw, long nivcsw) {
    if (num_results >= sizeof(results) / sizeof(results[0])) return;
    cpu_result_t *r = &results[num_results++];
    r->category = category;
    r->syscall_name = syscall_name;
    r->situation = situation;
    r->wall_total_ns = wall_total;
    r->wall_min_ns = wall_min;
    r->wall_max_ns = wall_max;
    r->wall_avg_ns = (double)wall_total / (double)iterations;
    r->cpu_total_ns = cpu_total;
    r->cpu_avg_ns = (double)cpu_total / (double)iterations;
    r->cpu_pct = (wall_total > 0) ? (100.0 * (double)cpu_total / (double)wall_total) : 0.0;
    r->cycles_total = cycles_total;
    r->cycles_per_op = (double)cycles_total / (double)iterations;
    r->wall_ops_per_sec = (wall_total > 0) ? ((double)iterations * 1e9 / (double)wall_total) : 0.0;
    r->cpu_ops_per_sec = (cpu_total > 0) ? ((double)iterations * 1e9 / (double)cpu_total) : 0.0;
    r->success_count = successes;
    r->error_count = errors;
    r->nvcsw_delta = nvcsw;
    r->nivcsw_delta = nivcsw;
}

/*
 * Per-scenario harness.  Wall min/max are tracked per iteration (cheap
 * CLOCK_MONOTONIC pair); CPU and cycle costs are taken as batch deltas
 * divided by the iteration count so the clock reads don't perturb them.
 */
#define CPU_BENCH_START() \
    struct rusage _ru0, _ru1; sample_rusage(&_ru0); \
    uint64_t _w_start = get_wall_ns(); \
    uint64_t _c_start = get_cpu_ns(); \
    uint64_t _t_start = rdtsc_cycles(); \
    uint64_t _w_min = (uint64_t)-1; \
    uint64_t _w_max = 0; \
    int _succ = 0, _err = 0

#define CPU_BENCH_ITER_START() \
    uint64_t _t0 = get_wall_ns()

#define CPU_BENCH_ITER_END(ret_cond) \
    do { uint64_t _t1 = get_wall_ns(); uint64_t _d = _t1 - _t0; \
        if (_d < _w_min) _w_min = _d; \
        if (_d > _w_max) _w_max = _d; \
        if (ret_cond) _succ++; else _err++; } while (0)

#define CPU_BENCH_FINISH(category, sysname, situation, iterations) \
    do { uint64_t _w_end = get_wall_ns(); uint64_t _c_end = get_cpu_ns(); \
        uint64_t _t_end = rdtsc_cycles(); \
        sample_rusage(&_ru1); \
        if (_w_min == (uint64_t)-1) _w_min = 0; \
        add_cpu_result(category, sysname, situation, _w_end - _w_start, _w_min, _w_max, \
            (_c_end >= _c_start ? _c_end - _c_start : 0), \
            (_t_end >= _t_start ? _t_end - _t_start : 0), \
            iterations, _succ, _err, \
            (long)(_ru1.ru_nvcsw - _ru0.ru_nvcsw), (long)(_ru1.ru_nivcsw - _ru0.ru_nivcsw)); } while (0)

/* =========================================================================
 * Category 1: Process, Thread & ID syscalls
 * ========================================================================= */
static void bench_process_thread(unsigned iter) {
    const char *cat = "Process/Thread";

    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t p = syscall(SYS_getpid); CPU_BENCH_ITER_END(p > 0); }
        CPU_BENCH_FINISH(cat, "getpid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t p = syscall(SYS_getppid); CPU_BENCH_ITER_END(p >= 0); }
        CPU_BENCH_FINISH(cat, "getppid", "Valid / Success Path", iter); }
#ifdef SYS_gettid
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t t = syscall(SYS_gettid); CPU_BENCH_ITER_END(t > 0); }
        CPU_BENCH_FINISH(cat, "gettid", "Valid / Success Path", iter); }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            uid_t u = syscall(SYS_getuid); CPU_BENCH_ITER_END(u != (uid_t)-1); }
        CPU_BENCH_FINISH(cat, "getuid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            uid_t u = syscall(SYS_geteuid); CPU_BENCH_ITER_END(u != (uid_t)-1); }
        CPU_BENCH_FINISH(cat, "geteuid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            gid_t g = syscall(SYS_getgid); CPU_BENCH_ITER_END(g != (gid_t)-1); }
        CPU_BENCH_FINISH(cat, "getgid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            gid_t g = syscall(SYS_getegid); CPU_BENCH_ITER_END(g != (gid_t)-1); }
        CPU_BENCH_FINISH(cat, "getegid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t pg = syscall(SYS_getpgid, 0); CPU_BENCH_ITER_END(pg > 0); }
        CPU_BENCH_FINISH(cat, "getpgid", "Valid (Self PID=0)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t pg = syscall(SYS_getpgid, 9999999);
            CPU_BENCH_ITER_END(pg < 0 && errno == ESRCH); }
        CPU_BENCH_FINISH(cat, "getpgid", "Invalid (ESRCH Non-existent PID)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t s = syscall(SYS_getsid, 0); CPU_BENCH_ITER_END(s > 0); }
        CPU_BENCH_FINISH(cat, "getsid", "Valid (Self PID=0)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t p = syscall(SYS_getpgrp); CPU_BENCH_ITER_END(p > 0); }
        CPU_BENCH_FINISH(cat, "getpgrp", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_setpgid, 0, 0); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "setpgid", "Valid (Join Own Group)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int n = syscall(SYS_getgroups, 0, NULL); CPU_BENCH_ITER_END(n >= 0); }
        CPU_BENCH_FINISH(cat, "getgroups", "Valid (Size Query)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            uid_t r, e, s; int r0 = syscall(SYS_getresuid, &r, &e, &s);
            CPU_BENCH_ITER_END(r0 == 0); }
        CPU_BENCH_FINISH(cat, "getresuid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            gid_t r, e, s; int r0 = syscall(SYS_getresgid, &r, &e, &s);
            CPU_BENCH_ITER_END(r0 == 0); }
        CPU_BENCH_FINISH(cat, "getresgid", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_yield); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sched_yield", "Valid / Success Path", iter); }
    { struct sched_param sp; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_getparam, 0, &sp); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sched_getparam", "Valid (Self PID=0)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_getscheduler, 0); CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "sched_getscheduler", "Valid (Self PID=0)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_get_priority_max, SCHED_OTHER);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "sched_get_prio_max", "Valid (SCHED_OTHER)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_get_priority_min, SCHED_OTHER);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "sched_get_prio_min", "Valid (SCHED_OTHER)", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_rr_get_interval, 0, &ts);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sched_rr_get_interval", "Valid (Self PID=0)", iter); }
    { cpu_set_t set; CPU_ZERO(&set); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sched_getaffinity, 0, sizeof(set), &set);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "sched_getaffinity", "Valid (Self Mask Query)", iter); }
#ifdef SYS_getcpu
    { unsigned cpu = 0, node = 0; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_getcpu, &cpu, &node, NULL);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "getcpu", "Valid / Success Path", iter); }
#endif
#ifdef SYS_times
    { struct tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; } buf;
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            long r = syscall(SYS_times, &buf); CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "times", "Valid / Success Path", iter); }
#endif
    { struct rlimit rl; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_getrlimit, RLIMIT_NOFILE, &rl);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "getrlimit", "Valid (RLIMIT_NOFILE)", iter); }
#ifdef SYS_prlimit64
    { struct rlimit old; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, &old);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "prlimit64", "Valid (Query NOFILE)", iter); }
#endif
    { struct rusage ru; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_getrusage, RUSAGE_SELF, &ru);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "getrusage", "Valid (RUSAGE_SELF)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_prctl, PR_GET_DUMPABLE, 0, 0, 0, 0);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "prctl", "Valid (PR_GET_DUMPABLE)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_prctl, -1, 0, 0, 0, 0);
            CPU_BENCH_ITER_END(r < 0 && errno == EINVAL); }
        CPU_BENCH_FINISH(cat, "prctl", "Invalid (EINVAL option=-1)", iter); }
#ifdef SYS_arch_prctl
    { unsigned long fs = 0; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_arch_prctl, ARCH_GET_FS, &fs);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "arch_prctl", "Valid (ARCH_GET_FS)", iter); }
#endif
    { pid_t self = getpid(); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_kill, self, 0); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "kill", "Valid (Sig 0 Self Check)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_kill, 9999999, 0);
            CPU_BENCH_ITER_END(r < 0 && errno == ESRCH); }
        CPU_BENCH_FINISH(cat, "kill", "Invalid (ESRCH Non-existent PID)", iter); }
    {
        pid_t self_p = getpid();
#ifdef SYS_gettid
        pid_t self_t = syscall(SYS_gettid);
#else
        pid_t self_t = self_p;
#endif
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_tgkill, self_p, self_t, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "tgkill", "Valid (Sig 0 Self Thread Check)", iter); }
#ifdef SYS_pidfd_open
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int fd = syscall(SYS_pidfd_open, getpid(), 0);
            if (fd >= 0) { close(fd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "pidfd_open", "Valid (Self PID)", iter); }
#endif
#ifdef SYS_set_tid_address
    { int tidptr = 0; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            long r = syscall(SYS_set_tid_address, &tidptr);
            CPU_BENCH_ITER_END(r > 0); }
        CPU_BENCH_FINISH(cat, "set_tid_address", "Valid / Success Path", iter); }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t child = fork();
            if (child == 0) { _exit(0); }
            else if (child > 0) {
                int status = 0;
                syscall(SYS_wait4, child, &status, 0, NULL);
                CPU_BENCH_ITER_END(WIFEXITED(status));
            } else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "fork+wait4", "Valid (Subprocess Lifecycle)", iter); }
}

/* =========================================================================
 * Category 2: Memory Management syscalls
 * ========================================================================= */
static void bench_memory(unsigned iter) {
    const char *cat = "Memory";

    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            uintptr_t b = (uintptr_t)syscall(SYS_brk, 0);
            CPU_BENCH_ITER_END(b != 0); }
        CPU_BENCH_FINISH(cat, "brk", "Valid (Query Break Address)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 4096, PROT_READ|PROT_WRITE,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) { syscall(SYS_munmap, p, 4096);
                CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "mmap+munmap", "Valid (4KB Anon Private)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 65536, PROT_READ|PROT_WRITE,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) { syscall(SYS_munmap, p, 65536);
                CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "mmap+munmap", "Valid (64KB Anon Private)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 0, PROT_READ,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            CPU_BENCH_ITER_END(p == MAP_FAILED && errno == EINVAL); }
        CPU_BENCH_FINISH(cat, "mmap", "Invalid (EINVAL Length 0)", iter); }
    { void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_mprotect, p, 4096, PROT_READ|PROT_WRITE);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "mprotect", "Valid (PROT_READ -> PROT_READ|WRITE)", iter);
            munmap(p, 4096); } }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_mprotect, NULL, 4096, PROT_READ);
            CPU_BENCH_ITER_END(r < 0 && (errno == ENOMEM || errno == EINVAL || errno == EFAULT)); }
        CPU_BENCH_FINISH(cat, "mprotect", "Invalid (EFAULT/ENOMEM Null Address)", iter); }
    { void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { memset(p, 0xAA, 4096); CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_madvise, p, 4096, MADV_DONTNEED);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "madvise", "Valid (MADV_DONTNEED 4KB)", iter);
            munmap(p, 4096); } }
    { void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_madvise, p, 4096, MADV_WILLNEED);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "madvise", "Valid (MADV_WILLNEED 4KB)", iter);
            munmap(p, 4096); } }
    { void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r1 = syscall(SYS_mlock, p, 4096);
                int r2 = syscall(SYS_munlock, p, 4096);
                CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
            CPU_BENCH_FINISH(cat, "mlock+munlock", "Valid (4KB Lock/Unlock)", iter);
            munmap(p, 4096); } }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r1 = syscall(SYS_mlockall, MCL_CURRENT);
            int r2 = syscall(SYS_munlockall);
            CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
        CPU_BENCH_FINISH(cat, "mlockall+munlockall", "Valid (MCL_CURRENT)", iter); }
    { void *p = mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_msync, p, 4096, MS_ASYNC);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "msync", "Valid (MS_ASYNC 4KB Anon)", iter);
            munmap(p, 8192); } }
    { void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { unsigned char vec[1]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_mincore, p, 4096, vec);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "mincore", "Valid (4KB Anon Query)", iter);
            munmap(p, 4096); } }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) { CPU_BENCH_ITER_END(0); continue; }
            void *q = (void*)syscall(SYS_mremap, p, 4096, 8192, MREMAP_MAYMOVE);
            if (q != MAP_FAILED) { syscall(SYS_munmap, q, 8192);
                CPU_BENCH_ITER_END(1); }
            else { syscall(SYS_munmap, p, 4096); CPU_BENCH_ITER_END(0); } }
        CPU_BENCH_FINISH(cat, "mremap", "Valid (4KB -> 8KB MAYMOVE)", iter); }
#ifdef SYS_memfd_create
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int fd = syscall(SYS_memfd_create, "bench_memfd", 0);
            if (fd >= 0) { close(fd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "memfd_create", "Valid / Success Path", iter); }
#endif
#ifdef SYS_membarrier
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "membarrier", "Valid (CMD_QUERY)", iter); }
#endif
}

/* =========================================================================
 * Category 3: File & File Descriptor I/O syscalls
 * ========================================================================= */
static void bench_file_io(unsigned iter) {
    const char *cat = "File I/O";

    char tmp_file[] = "/tmp/bench_cpu_file_XXXXXX";
    int tmp_fd = mkstemp(tmp_file);
    if (tmp_fd < 0) { perror("mkstemp"); return; }
    char dummy_buf[65536];
    memset(dummy_buf, 'A', sizeof(dummy_buf));
    if (write(tmp_fd, dummy_buf, sizeof(dummy_buf)) != (ssize_t)sizeof(dummy_buf)) {
        perror("write tmp"); close(tmp_fd); unlink(tmp_file); return;
    }

    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int fd = syscall(SYS_openat, AT_FDCWD, tmp_file, O_RDONLY, 0);
            if (fd >= 0) { close(fd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "openat", "Valid (O_RDONLY Existing File)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int fd = syscall(SYS_openat, AT_FDCWD, "/non_existent_path_xyz123", O_RDONLY, 0);
            CPU_BENCH_ITER_END(fd < 0 && errno == ENOENT); }
        CPU_BENCH_FINISH(cat, "openat", "Invalid (ENOENT Non-existent File)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_close, -1);
            CPU_BENCH_ITER_END(r < 0 && errno == EBADF); }
        CPU_BENCH_FINISH(cat, "close", "Invalid (EBADF FD=-1)", iter); }
#ifdef SYS_close_range
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int d = open("/dev/null", O_WRONLY);
            if (d < 0) { CPU_BENCH_ITER_END(0); continue; }
            unsigned hi = (unsigned)d;
            int r = syscall(SYS_close_range, d, hi, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "close_range", "Valid (Single FD Range)", iter); }
#endif

    int dev_zero = open("/dev/zero", O_RDONLY);
    int dev_null = open("/dev/null", O_WRONLY);

    if (dev_zero >= 0) {
        { char buf[16]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 0);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "read", "Valid (0-byte payload)", iter); }
        { char buf[16]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 16);
                CPU_BENCH_ITER_END(r == 16); }
            CPU_BENCH_FINISH(cat, "read", "Valid (16-byte payload)", iter); }
        { char buf[4096]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 4096);
                CPU_BENCH_ITER_END(r == 4096); }
            CPU_BENCH_FINISH(cat, "read", "Valid (4096-byte payload)", iter); }
        { char *buf = malloc(65536); if (buf) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 65536);
                CPU_BENCH_ITER_END(r == 65536); }
            CPU_BENCH_FINISH(cat, "read", "Valid (65536-byte payload)", iter);
            free(buf); } }
    }
    { char buf[16]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_read, -1, buf, 16);
            CPU_BENCH_ITER_END(r < 0 && errno == EBADF); }
        CPU_BENCH_FINISH(cat, "read", "Invalid (EBADF FD=-1)", iter); }

    if (dev_null >= 0) {
        { char buf[16]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 0);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "write", "Valid (0-byte payload)", iter); }
        { char buf[16]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 16);
                CPU_BENCH_ITER_END(r == 16); }
            CPU_BENCH_FINISH(cat, "write", "Valid (16-byte payload)", iter); }
        { char buf[4096]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 4096);
                CPU_BENCH_ITER_END(r == 4096); }
            CPU_BENCH_FINISH(cat, "write", "Valid (4096-byte payload)", iter); }
        { char *buf = malloc(65536); if (buf) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 65536);
                CPU_BENCH_ITER_END(r == 65536); }
            CPU_BENCH_FINISH(cat, "write", "Valid (65536-byte payload)", iter);
            free(buf); } }
    }
    { char buf[1024]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_pread64, tmp_fd, buf, 1024, 0);
            CPU_BENCH_ITER_END(r == 1024); }
        CPU_BENCH_FINISH(cat, "pread64", "Valid (1024 bytes offset 0)", iter); }
    { char buf[1024]; memset(buf, 'B', sizeof(buf)); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_pwrite64, tmp_fd, buf, 1024, 0);
            CPU_BENCH_ITER_END(r == 1024); }
        CPU_BENCH_FINISH(cat, "pwrite64", "Valid (1024 bytes offset 0)", iter); }
    { char b1[512], b2[512]; struct iovec iov[2] = { {b1, 512}, {b2, 512} };
        syscall(SYS_lseek, tmp_fd, 0, SEEK_SET); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_readv, tmp_fd, iov, 2);
            CPU_BENCH_ITER_END(r == 1024);
            syscall(SYS_lseek, tmp_fd, 0, SEEK_SET); }
        CPU_BENCH_FINISH(cat, "readv", "Valid (2-chunk iovec 1024B)", iter); }
    { char b1[512], b2[512]; struct iovec iov[2] = { {b1, 512}, {b2, 512} };
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_writev, dev_null >= 0 ? dev_null : tmp_fd, iov, 2);
            CPU_BENCH_ITER_END(r == 1024); }
        CPU_BENCH_FINISH(cat, "writev", "Valid (2-chunk iovec 1024B)", iter); }
#ifdef SYS_preadv
    { char b1[512], b2[512]; struct iovec iov[2] = { {b1, 512}, {b2, 512} };
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_preadv, tmp_fd, iov, 2, 0);
            CPU_BENCH_ITER_END(r == 1024); }
        CPU_BENCH_FINISH(cat, "preadv", "Valid (2-chunk iovec off 0)", iter); }
#endif
#ifdef SYS_pwritev
    { char b1[512], b2[512]; struct iovec iov[2] = { {b1, 512}, {b2, 512} };
        memset(b1, 'C', sizeof(b1)); memset(b2, 'C', sizeof(b2));
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_pwritev, tmp_fd, iov, 2, 0);
            CPU_BENCH_ITER_END(r == 1024); }
        CPU_BENCH_FINISH(cat, "pwritev", "Valid (2-chunk iovec off 0)", iter); }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            off_t o = syscall(SYS_lseek, tmp_fd, 0, SEEK_SET);
            CPU_BENCH_ITER_END(o == 0); }
        CPU_BENCH_FINISH(cat, "lseek", "Valid (SEEK_SET Offset 0)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int newfd = syscall(SYS_dup, tmp_fd);
            if (newfd >= 0) { close(newfd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "dup", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int spare = open("/dev/null", O_WRONLY);
            if (spare < 0) { CPU_BENCH_ITER_END(0); continue; }
            int r = syscall(SYS_dup2, tmp_fd, spare);
            if (r >= 0) { close(r == spare ? spare : r);
                if (r != spare) close(spare);
                CPU_BENCH_ITER_END(1); }
            else { close(spare); CPU_BENCH_ITER_END(0); } }
        CPU_BENCH_FINISH(cat, "dup2", "Valid (Target Reuse)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int spare = open("/dev/null", O_WRONLY);
            if (spare < 0) { CPU_BENCH_ITER_END(0); continue; }
            int r = syscall(SYS_dup3, tmp_fd, spare, O_CLOEXEC);
            if (r >= 0) { close(r); CPU_BENCH_ITER_END(1); }
            else { close(spare); CPU_BENCH_ITER_END(0); } }
        CPU_BENCH_FINISH(cat, "dup3", "Valid (O_CLOEXEC)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int fl = syscall(SYS_fcntl, tmp_fd, F_GETFL, 0);
            CPU_BENCH_ITER_END(fl >= 0); }
        CPU_BENCH_FINISH(cat, "fcntl", "Valid (F_GETFL Flags Query)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r1 = syscall(SYS_flock, tmp_fd, LOCK_SH);
            int r2 = syscall(SYS_flock, tmp_fd, LOCK_UN);
            CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
        CPU_BENCH_FINISH(cat, "flock", "Valid (LOCK_SH + LOCK_UN)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fsync, tmp_fd); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fsync", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fdatasync, tmp_fd); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fdatasync", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_ftruncate, tmp_fd, 65536);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "ftruncate", "Valid (Size 64KB)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fallocate, tmp_fd, 0, 0, 4096);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fallocate", "Valid (0/0/4KB Keep)", iter); }
#ifdef SYS_sync_file_range
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sync_file_range, tmp_fd, 0, 4096,
                            SYNC_FILE_RANGE_WRITE);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sync_file_range", "Valid (WRITE 4KB)", iter); }
#endif
#ifdef SYS_copy_file_range
    { char tmp2[] = "/tmp/bench_cpu_copy_XXXXXX";
        int fd2 = mkstemp(tmp2);
        if (fd2 >= 0) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                loff_t off_in = 0, off_out = 0;
                ssize_t r = syscall(SYS_copy_file_range, tmp_fd, &off_in, fd2, &off_out,
                                    4096, 0);
                CPU_BENCH_ITER_END(r > 0); }
            CPU_BENCH_FINISH(cat, "copy_file_range", "Valid (4KB tmp->tmp off 0)", iter);
            close(fd2); unlink(tmp2); } }
#endif
    { struct stat st; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fstat, tmp_fd, &st); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fstat", "Valid / Success Path", iter); }
    { struct stat st; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_newfstatat, AT_FDCWD, tmp_file, &st, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "newfstatat", "Valid (Path Stat Query)", iter); }
#ifdef SYS_statx
    { struct { uint32_t m0, m1, m2, m3, m4, m5, m6; uint64_t a[16]; } sx;
        memset(&sx, 0, sizeof(sx)); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_statx, AT_FDCWD, tmp_file, 0, STATX_BASIC_STATS, &sx);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "statx", "Valid (BASIC_STATS)", iter); }
#endif
#ifdef SYS_statfs
    { struct statfs sfs; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_statfs, "/tmp", &sfs);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "statfs", "Valid (/tmp Query)", iter); }
#endif
#ifdef SYS_fstatfs
    { struct statfs sfs; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fstatfs, tmp_fd, &sfs);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fstatfs", "Valid (FD Query)", iter); }
#endif
#ifdef SYS_getdents64
    { int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { char dent[4096]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                syscall(SYS_lseek, dfd, 0, SEEK_SET);
                long r = syscall(SYS_getdents64, dfd, dent, sizeof(dent));
                CPU_BENCH_ITER_END(r > 0); }
            CPU_BENCH_FINISH(cat, "getdents64", "Valid (/tmp Rewind+Read)", iter);
            close(dfd); } }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_access, tmp_file, F_OK);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "access", "Valid (F_OK Existing File)", iter); }
#ifdef SYS_faccessat2
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_faccessat2, AT_FDCWD, tmp_file, F_OK, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "faccessat2", "Valid (F_OK Existing File)", iter); }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fchmod, tmp_fd, 0644);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fchmod", "Valid (Mode 0644)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            mode_t old_m = syscall(SYS_umask, 0022);
            CPU_BENCH_ITER_END(old_m != (mode_t)-1); }
        CPU_BENCH_FINISH(cat, "umask", "Valid / Success Path", iter); }
    { char cwd[4096]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            long r = syscall(SYS_getcwd, cwd, sizeof(cwd));
            CPU_BENCH_ITER_END(r > 0); }
        CPU_BENCH_FINISH(cat, "getcwd", "Valid (4096-byte Buffer)", iter); }
    { char cwd[4096]; CPU_BENCH_START();
        if (syscall(SYS_getcwd, cwd, sizeof(cwd)) <= 0) strncpy(cwd, "/tmp", sizeof(cwd));
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_chdir, "/tmp");
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "chdir", "Valid (/tmp)", iter);
        syscall(SYS_chdir, cwd); }

    if (dev_zero >= 0) close(dev_zero);
    if (dev_null >= 0) close(dev_null);
    close(tmp_fd);
    unlink(tmp_file);
}

/* =========================================================================
 * Category 4: Time & Timer syscalls
 * ========================================================================= */
static void bench_time_timers(unsigned iter) {
    const char *cat = "Time/Timers";

    { struct timeval tv; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_gettimeofday, &tv, NULL);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "gettimeofday", "Valid / Success Path", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "clock_gettime", "Valid (CLOCK_MONOTONIC)", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "clock_gettime", "Valid (CLOCK_REALTIME)", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &ts);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "clock_gettime", "Valid (CLOCK_THREAD_CPUTIME_ID)", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_gettime, -1, &ts);
            CPU_BENCH_ITER_END(r < 0 && errno == EINVAL); }
        CPU_BENCH_FINISH(cat, "clock_gettime", "Invalid (EINVAL Clock ID -1)", iter); }
    { struct timespec ts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_getres, CLOCK_MONOTONIC, &ts);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "clock_getres", "Valid (CLOCK_MONOTONIC Resolution)", iter); }
    { struct timespec req = {0, 0}, rem; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_nanosleep, &req, &rem);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "nanosleep", "Valid (0ns Duration)", iter); }
    { struct timespec req = {0, 0}, rem; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, &rem);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "clock_nanosleep", "Valid (0ns Relative)", iter); }
    { time_t t; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            time_t r = syscall(SYS_time, &t);
            CPU_BENCH_ITER_END(r > 0); }
        CPU_BENCH_FINISH(cat, "time", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            unsigned rem = syscall(SYS_alarm, 0); (void)rem;
            CPU_BENCH_ITER_END(1); }
        CPU_BENCH_FINISH(cat, "alarm", "Valid (Cancel Alarm = 0)", iter); }
    { struct itimerval itv = {{0, 0}, {0, 0}}, old_itv; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_setitimer, ITIMER_REAL, &itv, &old_itv);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "setitimer", "Valid (Query/Disable ITIMER_REAL)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int tfd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (tfd >= 0) { close(tfd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "timerfd_create", "Valid (CLOCK_MONOTONIC TFD_NONBLOCK)", iter); }
    { int tfd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (tfd >= 0) {
            struct itimerspec newv = {{0, 0}, {0, 0}}, oldv;
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_timerfd_settime, tfd, 0, &newv, &oldv);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "timerfd_settime", "Valid (Disarm Query)", iter);
            { struct itimerspec cur; CPU_BENCH_START();
                for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                    int r = syscall(SYS_timerfd_gettime, tfd, &cur);
                    CPU_BENCH_ITER_END(r == 0); }
                CPU_BENCH_FINISH(cat, "timerfd_gettime", "Valid / Success Path", iter); }
            close(tfd); } }
}

/* =========================================================================
 * Category 5: Signal Handling syscalls
 * ========================================================================= */
static void bench_signals(unsigned iter) {
    const char *cat = "Signals";

    { struct sigaction sa; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_rt_sigaction, SIGUSR1, NULL, &sa, 8);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "rt_sigaction", "Valid (Query SIGUSR1 Handler)", iter); }
    { struct sigaction sa; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_rt_sigaction, 0, NULL, &sa, 8);
            CPU_BENCH_ITER_END(r < 0 && errno == EINVAL); }
        CPU_BENCH_FINISH(cat, "rt_sigaction", "Invalid (EINVAL Signal 0)", iter); }
    { sigset_t set, oldset; sigemptyset(&set); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_rt_sigprocmask, SIG_BLOCK, &set, &oldset, 8);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "rt_sigprocmask", "Valid (SIG_BLOCK Empty Set)", iter); }
    { sigset_t pend; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_rt_sigpending, &pend, 8);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "rt_sigpending", "Valid / Success Path", iter); }
    { stack_t old_ss; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sigaltstack, NULL, &old_ss);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sigaltstack", "Valid (Query Current Alt Stack)", iter); }
#ifdef SYS_set_robust_list
#ifdef SYS_get_robust_list
    { void *head = NULL; size_t len = 0; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_get_robust_list, 0, &head, &len);
            CPU_BENCH_ITER_END(r == 0 || (r < 0 && errno == ESRCH)); }
        CPU_BENCH_FINISH(cat, "get_robust_list", "Valid (Self Query)", iter); }
#endif
#endif
}

/* =========================================================================
 * Category 6: I/O Multiplexing & Event syscalls
 * ========================================================================= */
static void bench_events_epoll(unsigned iter) {
    const char *cat = "I/O Events";

    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_poll, NULL, 0, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "poll", "Valid (0 FDs, Timeout 0ms)", iter); }
    { int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { struct pollfd pfd = { fd, POLLOUT, 0 }; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_poll, &pfd, 1, 0);
                CPU_BENCH_ITER_END(r == 1); }
            CPU_BENCH_FINISH(cat, "poll", "Valid (1 FD POLLOUT, Timeout 0ms)", iter);
            close(fd); } }
#ifdef SYS_ppoll
    { struct timespec ts = {0, 0}; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_ppoll, NULL, 0, &ts, NULL, 8);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "ppoll", "Valid (0 FDs, Timeout 0ns)", iter); }
#endif
    { struct timeval tv = {0, 0}; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            tv.tv_sec = 0; tv.tv_usec = 0;
            int r = syscall(SYS_select, 0, NULL, NULL, NULL, &tv);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "select", "Valid (0 FDs, Timeout 0ms)", iter); }
#ifdef SYS_pselect6
    { struct timespec ts = {0, 0}; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &ts, NULL);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "pselect6", "Valid (0 FDs, Timeout 0ns)", iter); }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int epfd = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
            if (epfd >= 0) { close(epfd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "epoll_create1", "Valid (EPOLL_CLOEXEC)", iter); }
    { int epfd = epoll_create1(0);
        int dummy_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (epfd >= 0 && dummy_fd >= 0) {
            struct epoll_event ev = { EPOLLOUT, {.fd = dummy_fd} };
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r1 = syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, dummy_fd, &ev);
                int r2 = syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_DEL, dummy_fd, NULL);
                CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
            CPU_BENCH_FINISH(cat, "epoll_ctl", "Valid (EPOLL_CTL_ADD + DEL)", iter);
            close(epfd); close(dummy_fd); } }
    { int epfd = epoll_create1(0);
        if (epfd >= 0) { struct epoll_event events[4]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_epoll_wait, epfd, events, 4, 0);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "epoll_wait", "Valid (Empty Set, Timeout 0ms)", iter);
            close(epfd); } }
#ifdef SYS_epoll_pwait
    { int epfd = epoll_create1(0);
        if (epfd >= 0) { struct epoll_event events[4]; CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_epoll_pwait, epfd, events, 4, 0, NULL, 8);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "epoll_pwait", "Valid (Empty Set, Timeout 0ms)", iter);
            close(epfd); } }
#endif
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int efd = syscall(SYS_eventfd2, 0, EFD_NONBLOCK);
            if (efd >= 0) {
                uint64_t val = 1;
                syscall(SYS_write, efd, &val, sizeof(val));
                syscall(SYS_read, efd, &val, sizeof(val));
                close(efd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "eventfd2", "Valid (Create+Write+Read+Close)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int ifd = syscall(SYS_inotify_init1, IN_NONBLOCK | IN_CLOEXEC);
            if (ifd >= 0) { close(ifd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "inotify_init1", "Valid (NONBLOCK+CLOEXEC)", iter); }
    { int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ifd >= 0) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int wd = syscall(SYS_inotify_add_watch, ifd, "/tmp", IN_MODIFY);
                if (wd >= 0) {
                    int r = syscall(SYS_inotify_rm_watch, ifd, wd);
                    CPU_BENCH_ITER_END(r == 0); }
                else CPU_BENCH_ITER_END(0); }
            CPU_BENCH_FINISH(cat, "inotify_add+rm_watch", "Valid (/tmp IN_MODIFY)", iter);
            close(ifd); } }
#ifdef SYS_signalfd4
    { sigset_t m; sigemptyset(&m); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int sfd = syscall(SYS_signalfd4, -1, &m, 8, SFD_NONBLOCK | SFD_CLOEXEC);
            if (sfd >= 0) { close(sfd); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "signalfd4", "Valid (Empty Mask Create+Close)", iter); }
#endif
}

/* =========================================================================
 * Category 7: Sockets & IPC syscalls
 * ========================================================================= */
static void bench_sockets_ipc(unsigned iter) {
    const char *cat = "Sockets/IPC";

    { int fds[2]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_pipe2, fds, O_CLOEXEC);
            if (r == 0) { close(fds[0]); close(fds[1]);
                CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "pipe2", "Valid (Create & Close Pipe)", iter); }
    { int fds[2];
        if (pipe(fds) == 0) {
            char buf[64] = "Benchmarking pipe IPC speed throughput test data payload!";
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t w = syscall(SYS_write, fds[1], buf, 64);
                ssize_t r = syscall(SYS_read, fds[0], buf, 64);
                CPU_BENCH_ITER_END(w == 64 && r == 64); }
            CPU_BENCH_FINISH(cat, "pipe_write+read", "Valid (64-byte IPC Ping-Pong)", iter);
            close(fds[0]); close(fds[1]); } }
    { int fds[2];
        if (pipe(fds) == 0) {
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int fl = syscall(SYS_fcntl, fds[0], F_GETFL, 0);
                CPU_BENCH_ITER_END(fl >= 0); }
            CPU_BENCH_FINISH(cat, "fcntl(pipe)", "Valid (F_GETFL Pipe FD)", iter);
            close(fds[0]); close(fds[1]); } }
    { int pa[2], pb[2];
        if (pipe(pa) == 0 && pipe(pb) == 0) {
            { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t w = syscall(SYS_write, pa[1], "0123456789abcdef", 16);
                ssize_t t = syscall(SYS_tee, pa[0], pb[1], 16, 0);
                /* original stays in pa, duplicate lands in pb; drain both */
                char tmp[16]; ssize_t r1 = syscall(SYS_read, pa[0], tmp, 16);
                ssize_t r2 = syscall(SYS_read, pb[0], tmp, 16);
                CPU_BENCH_ITER_END(w == 16 && t == 16 && r1 == 16 && r2 == 16); }
            CPU_BENCH_FINISH(cat, "tee", "Valid (16B PipeA->PipeB Dup)", iter); }
            close(pa[0]); close(pa[1]); close(pb[0]); close(pb[1]); } }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int s = syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
            if (s >= 0) { close(s); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "socket", "Valid (AF_UNIX SOCK_STREAM)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int s = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
            if (s >= 0) { close(s); CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "socket", "Valid (AF_INET SOCK_DGRAM)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int s = syscall(SYS_socket, -1, SOCK_STREAM, 0);
            CPU_BENCH_ITER_END(s < 0 && (errno == EAFNOSUPPORT || errno == EINVAL)); }
        CPU_BENCH_FINISH(cat, "socket", "Invalid (EAFNOSUPPORT Domain -1)", iter); }
    { int sv[2]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv);
            if (r == 0) { close(sv[0]); close(sv[1]);
                CPU_BENCH_ITER_END(1); }
            else CPU_BENCH_ITER_END(0); }
        CPU_BENCH_FINISH(cat, "socketpair", "Valid (AF_UNIX Connected Pair)", iter); }
    { int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            char send_buf[64] = "Datagram socketpair speed benchmark payload data!";
            char recv_buf[64];
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                ssize_t s = syscall(SYS_sendto, sv[0], send_buf, 64, 0, NULL, 0);
                ssize_t r = syscall(SYS_recvfrom, sv[1], recv_buf, 64, 0, NULL, NULL);
                CPU_BENCH_ITER_END(s == 64 && r == 64); }
            CPU_BENCH_FINISH(cat, "sendto+recvfrom", "Valid (64-byte Datagram Transfer)", iter);
            close(sv[0]); close(sv[1]); } }
    { int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            char sbuf[64] = "stream socket msghdr benchmark payload padding data!!";
            char rbuf[64];
            struct iovec siov = { sbuf, 64 }, riov = { rbuf, 64 };
            struct msghdr smsg, rmsg;
            memset(&smsg, 0, sizeof(smsg)); memset(&rmsg, 0, sizeof(rmsg));
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                smsg.msg_iov = &siov; smsg.msg_iovlen = 1;
                rmsg.msg_iov = &riov; rmsg.msg_iovlen = 1;
                ssize_t s = syscall(SYS_sendmsg, sv[0], &smsg, 0);
                ssize_t r = syscall(SYS_recvmsg, sv[1], &rmsg, 0);
                CPU_BENCH_ITER_END(s == 64 && r == 64); }
            CPU_BENCH_FINISH(cat, "sendmsg+recvmsg", "Valid (64B Stream Msg Pair)", iter);
            close(sv[0]); close(sv[1]); } }
    { int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            struct sockaddr_un addr; socklen_t len = sizeof(addr);
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                len = sizeof(addr);
                int r1 = syscall(SYS_getsockname, sv[0], (struct sockaddr*)&addr, &len);
                len = sizeof(addr);
                int r2 = syscall(SYS_getpeername, sv[0], (struct sockaddr*)&addr, &len);
                CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
            CPU_BENCH_FINISH(cat, "getsockname+peername", "Valid (Socket Address Query)", iter);
            close(sv[0]); close(sv[1]); } }
    { int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            int v = 0; socklen_t l = sizeof(v);
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                l = sizeof(v);
                int r = syscall(SYS_getsockopt, sv[0], SOL_SOCKET, SO_TYPE, &v, &l);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "getsockopt", "Valid (SOL_SOCKET SO_TYPE)", iter);
            { int sndbuf = 65536; CPU_BENCH_START();
                for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                    int r = syscall(SYS_setsockopt, sv[0], SOL_SOCKET, SO_SNDBUF,
                                    &sndbuf, sizeof(sndbuf));
                    CPU_BENCH_ITER_END(r == 0); }
                CPU_BENCH_FINISH(cat, "setsockopt", "Valid (SO_SNDBUF 64KB)", iter); }
            close(sv[0]); close(sv[1]); } }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int sv2[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) != 0)
                { CPU_BENCH_ITER_END(0); continue; }
            int r = syscall(SYS_shutdown, sv2[0], SHUT_WR);
            close(sv2[0]); close(sv2[1]);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "shutdown", "Valid (Fresh Pair SHUT_WR)", iter); }
    { char path[64]; snprintf(path, sizeof(path), "/tmp/bench_cpu_sock_%d", getpid());
        unlink(path); CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0) { CPU_BENCH_ITER_END(0); continue; }
            struct sockaddr_un a; memset(&a, 0, sizeof(a));
            a.sun_family = AF_UNIX;
            strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
            unlink(path);
            int r1 = syscall(SYS_bind, s, (struct sockaddr*)&a, sizeof(a));
            int r2 = syscall(SYS_listen, s, 4);
            close(s); unlink(path);
            CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
        CPU_BENCH_FINISH(cat, "bind+listen", "Valid (AF_UNIX Tmp Path)", iter); }
    { int futex_val = 0; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_futex, &futex_val, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
            CPU_BENCH_ITER_END(r >= 0); }
        CPU_BENCH_FINISH(cat, "futex", "Valid (FUTEX_WAKE_PRIVATE 0 Waiters)", iter); }
#ifdef SYS_kcmp
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            pid_t self = getpid();
            int r = syscall(SYS_kcmp, self, self, KCMP_FILE, 0, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "kcmp", "Valid (KCMP_FILE Self FD 0)", iter); }
#endif
    { int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        if (shmid >= 0) {
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                void *p = (void*)syscall(SYS_shmat, shmid, NULL, 0);
                if (p != (void*)-1) {
                    int r = syscall(SYS_shmdt, p);
                    CPU_BENCH_ITER_END(r == 0); }
                else CPU_BENCH_ITER_END(0); }
            CPU_BENCH_FINISH(cat, "shmat+shmdt", "Valid (4KB Private Seg)", iter);
            { struct shmid_ds ds; CPU_BENCH_START();
                for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                    int r = syscall(SYS_shmctl, shmid, IPC_STAT, &ds);
                    CPU_BENCH_ITER_END(r == 0); }
                CPU_BENCH_FINISH(cat, "shmctl", "Valid (IPC_STAT Query)", iter); }
            shmctl(shmid, IPC_RMID, NULL); } }
    { int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
        if (semid >= 0) {
            union semun { int val; struct semid_ds *buf; unsigned short *array; } arg;
            arg.val = 1; semctl(semid, 0, SETVAL, arg);
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_semctl, semid, 0, GETVAL);
                CPU_BENCH_ITER_END(r >= 0); }
            CPU_BENCH_FINISH(cat, "semctl", "Valid (GETVAL Query)", iter);
            semctl(semid, 0, IPC_RMID); } }
}

/* =========================================================================
 * Category 8: System Info & Random syscalls
 * ========================================================================= */
static void bench_system_info(unsigned iter) {
    const char *cat = "System/Info";

    { struct utsname uts; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_uname, &uts); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "uname", "Valid / Success Path", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_uname, NULL);
            CPU_BENCH_ITER_END(r < 0 && errno == EFAULT); }
        CPU_BENCH_FINISH(cat, "uname", "Invalid (EFAULT Null Address)", iter); }
    { struct sysinfo si; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sysinfo, &si); CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "sysinfo", "Valid / Success Path", iter); }
    { char host[65]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = gethostname(host, sizeof(host));
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "gethostname", "Valid / Success Path", iter); }
    { char longname[80]; memset(longname, 'h', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = '\0';
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_sethostname, longname, 65);
            CPU_BENCH_ITER_END(r < 0 && (errno == EINVAL || errno == EPERM)); }
        CPU_BENCH_FINISH(cat, "sethostname", "Invalid (EINVAL 65-char Name)", iter); }
#ifdef SYS_getrandom
    { uint64_t rand_val; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_getrandom, &rand_val, sizeof(rand_val), GRND_NONBLOCK);
            CPU_BENCH_ITER_END(r == (ssize_t)sizeof(rand_val)); }
        CPU_BENCH_FINISH(cat, "getrandom", "Valid (8-byte Non-blocking)", iter); }
    { char buf[256]; CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            ssize_t r = syscall(SYS_getrandom, buf, sizeof(buf), GRND_NONBLOCK);
            CPU_BENCH_ITER_END(r == (ssize_t)sizeof(buf)); }
        CPU_BENCH_FINISH(cat, "getrandom", "Valid (256-byte Non-blocking)", iter); }
#endif
}

/* =========================================================================
 * Category 9: Extended coverage - every other cheap/safe syscall
 * ========================================================================= */
static void bench_extended(unsigned iter) {
    const char *cat = "Extended";

    /* readlinkat / symlinkat / unlinkat lifecycle on a tmp path */
    { char path[64], link[80]; snprintf(path, sizeof(path), "/tmp/bench_cpu_x_%d", getpid());
        snprintf(link, sizeof(link), "%s_link", path);
        unlink(link); unlink(path);
        int fd = open(path, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            unlink(link);
            int r1 = syscall(SYS_symlinkat, path, AT_FDCWD, link);
            char buf[256]; ssize_t r2 = syscall(SYS_readlinkat, AT_FDCWD, link, buf, sizeof(buf));
            int r3 = syscall(SYS_unlinkat, AT_FDCWD, link, 0);
            CPU_BENCH_ITER_END(r1 == 0 && r2 > 0 && r3 == 0); }
        CPU_BENCH_FINISH(cat, "symlinkat+readlinkat+unlinkat", "Valid (Tmp Link Lifecycle)", iter);
        unlink(link); unlink(path); }

    /* mkdirat + rmdirat lifecycle */
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            char dir[64]; snprintf(dir, sizeof(dir), "/tmp/bench_cpu_d_%d_%u", getpid(), i);
            int r1 = syscall(SYS_mkdirat, AT_FDCWD, dir, 0700);
            int r2 = syscall(SYS_unlinkat, AT_FDCWD, dir, AT_REMOVEDIR);
            CPU_BENCH_ITER_END(r1 == 0 && r2 == 0); }
        CPU_BENCH_FINISH(cat, "mkdirat+unlinkat", "Valid (Tmp Dir Lifecycle)", iter); }

    /* renameat lifecycle */
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            char a[64], b[64];
            snprintf(a, sizeof(a), "/tmp/bench_cpu_r_%d_a", getpid());
            snprintf(b, sizeof(b), "/tmp/bench_cpu_r_%d_b", getpid());
            int fd = open(a, O_CREAT | O_WRONLY, 0600);
            if (fd >= 0) close(fd); else { CPU_BENCH_ITER_END(0); continue; }
            unlink(b);
            int r1 = syscall(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b);
            unlink(b);
            CPU_BENCH_ITER_END(r1 == 0); }
        CPU_BENCH_FINISH(cat, "renameat", "Valid (Tmp File Rename)", iter); }

    /* fchmodat + utimensat on tmp file */
    { char path[64]; snprintf(path, sizeof(path), "/tmp/bench_cpu_m_%d", getpid());
        int fd = open(path, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_fchmodat, AT_FDCWD, path, 0644, 0);
            CPU_BENCH_ITER_END(r == 0); }
        CPU_BENCH_FINISH(cat, "fchmodat", "Valid (Mode 0644 Tmp File)", iter);
        { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_utimensat, AT_FDCWD, path, NULL, 0);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "utimensat", "Valid (UTIME_NOW Tmp File)", iter); }
        unlink(path); }

    /* xattr round-trip on tmp file (tolerate ENOTSUP as valid measurement) */
    { char path[64]; snprintf(path, sizeof(path), "/tmp/bench_cpu_xa_%d", getpid());
        int fd = open(path, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r1 = syscall(SYS_setxattr, path, "user.bench", "v", 1, 0);
            char v[8], list[256];
            ssize_t r2 = syscall(SYS_getxattr, path, "user.bench", v, sizeof(v));
            ssize_t r3 = syscall(SYS_listxattr, path, list, sizeof(list));
            int r4 = syscall(SYS_removexattr, path, "user.bench");
            CPU_BENCH_ITER_END((r1 == 0 && r2 == 1 && r3 >= 0 && r4 == 0) ||
                               (r1 < 0)); }
        CPU_BENCH_FINISH(cat, "setxattr+getxattr+list+rm", "Valid (user.bench Tmp File)", iter);
        unlink(path); }

    /* sendfile tmp -> /dev/null */
    { char path[64]; snprintf(path, sizeof(path), "/tmp/bench_cpu_sf_%d", getpid());
        int fd = open(path, O_CREAT | O_RDWR, 0600);
        if (fd >= 0) {
            char z[4096]; memset(z, 'S', sizeof(z));
            if (write(fd, z, sizeof(z)) != (ssize_t)sizeof(z)) { close(fd); unlink(path); fd = -1; }
        }
        if (fd >= 0) {
            int out = open("/dev/null", O_WRONLY);
            if (out >= 0) { CPU_BENCH_START();
                for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                    off_t off = 0;
                    ssize_t r = syscall(SYS_sendfile, out, fd, &off, 4096);
                    CPU_BENCH_ITER_END(r == 4096); }
                CPU_BENCH_FINISH(cat, "sendfile", "Valid (4KB tmp->/dev/null)", iter);
                close(out); }
            close(fd); unlink(path); } }

    /* readahead on tmp file (advisory; always succeeds or EINVAL) */
    { char path[64]; snprintf(path, sizeof(path), "/tmp/bench_cpu_ra_%d", getpid());
        int fd = open(path, O_CREAT | O_RDWR, 0600);
        if (fd >= 0) {
            char z[16384]; memset(z, 0, sizeof(z));
            ssize_t w = write(fd, z, sizeof(z));
            (void)w;
            CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                long r = syscall(SYS_readahead, fd, 0, 16384);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "readahead", "Valid (16KB Advisory)", iter);
            close(fd); unlink(path); } }

    /* fallocate punch + zero keep-size variants are covered in File I/O;
     * here: madvise RANDOM + SEQUENTIAL advice rotation on anon memory */
    { void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) { CPU_BENCH_START();
            for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
                int r = syscall(SYS_madvise, p, 4096,
                                (i & 1) ? MADV_SEQUENTIAL : MADV_RANDOM);
                CPU_BENCH_ITER_END(r == 0); }
            CPU_BENCH_FINISH(cat, "madvise", "Valid (RANDOM/SEQUENTIAL Rotate)", iter);
            munmap(p, 4096); } }

    /* error-path-only coverage for privileged/destructive numbers:
     * each still enters the kernel, validates, and returns.  NOTE: no
     * reboot/poweroff coverage by design - reboot(2) with valid magics
     * would really reboot the machine, so it is deliberately absent. */
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_mount, "none", "/tmp", "no_such_fstype_xyz", 0, NULL);
            CPU_BENCH_ITER_END(r < 0 && (errno == ENODEV || errno == EPERM || errno == EBUSY || errno == EINVAL || errno == EFAULT)); }
        CPU_BENCH_FINISH(cat, "mount", "Invalid (ENODEV Bogus Fstype, No Effect)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_chroot, "/non_existent_path_xyz123");
            CPU_BENCH_ITER_END(r < 0 && (errno == ENOENT || errno == EPERM)); }
        CPU_BENCH_FINISH(cat, "chroot", "Invalid (ENOENT/EPERM)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_settimeofday, NULL, NULL);
            CPU_BENCH_ITER_END(r == 0 || (r < 0 && errno == EPERM)); }
        CPU_BENCH_FINISH(cat, "settimeofday", "Error-path (NULL Zone Query)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_swapon, "/non_existent_path_xyz123", 0);
            CPU_BENCH_ITER_END(r < 0 && (errno == ENOENT || errno == EPERM || errno == EINVAL)); }
        CPU_BENCH_FINISH(cat, "swapon", "Invalid (ENOENT/EPERM)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_init_module, NULL, 0, "");
            CPU_BENCH_ITER_END(r < 0); }
        CPU_BENCH_FINISH(cat, "init_module", "Invalid (EFAULT Null, No Load)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_delete_module, "no_such_module_xyz", 0);
            CPU_BENCH_ITER_END(r < 0); }
        CPU_BENCH_FINISH(cat, "delete_module", "Invalid (ENOENT)", iter); }
    { CPU_BENCH_START();
        for (unsigned i = 0; i < iter; i++) { CPU_BENCH_ITER_START();
            int r = syscall(SYS_ptrace, PTRACE_TRACEME, 0, NULL, NULL);
            /* first call may succeed (self-trace), rest fail EPERM; both are measured */
            CPU_BENCH_ITER_END(r == 0 || (r < 0 && (errno == EPERM || errno == EINVAL))); }
        CPU_BENCH_FINISH(cat, "ptrace", "Error-path (TRACEME Self)", iter); }
}

/* =========================================================================
 * Formatted Report Printer
 * ========================================================================= */
static void print_report(unsigned iterations) {
    printf("\n========================================================================================================================\n");
    printf("                 ASCENTOS / LINUX SYSCALL CPU BENCHMARK REPORT (%u ITERATIONS EACH)\n", iterations);
    printf("                 wall=CLOCK_MONOTONIC  cpu=CLOCK_THREAD_CPUTIME_ID  cycles=RDTSC(x86)  csw=RUSAGE_THREAD delta\n");
    printf("========================================================================================================================\n");
    printf("%-14s | %-26s | %-30s | %-9s | %-9s | %-6s | %-10s | %-10s | %-7s\n",
           "Category", "Syscall", "Situation / Scenario", "WallAvg", "CpuAvg", "Cpu%", "Cycles/op", "WallOps/s", "Pass/Err");
    printf("---------------+----------------------------+--------------------------------+-----------+-----------+--------+------------+------------+---------\n");

    double grand_wall_ns = 0.0, grand_cpu_ns = 0.0;
    double grand_cycles = 0.0;
    unsigned total_tests = num_results;

    for (size_t i = 0; i < num_results; i++) {
        cpu_result_t *r = &results[i];
        grand_wall_ns += (double)r->wall_total_ns;
        grand_cpu_ns += (double)r->cpu_total_ns;
        grand_cycles += (double)r->cycles_total;

        char wall_str[24], cpu_str[24], cyc_str[24], ops_str[24], pct_str[16], pass_str[16];
        if (r->wall_avg_ns >= 1e6)
            snprintf(wall_str, sizeof(wall_str), "%.2f ms", r->wall_avg_ns / 1e6);
        else if (r->wall_avg_ns >= 1e3)
            snprintf(wall_str, sizeof(wall_str), "%.1f us", r->wall_avg_ns / 1e3);
        else
            snprintf(wall_str, sizeof(wall_str), "%.1f ns", r->wall_avg_ns);
        if (r->cpu_avg_ns >= 1e6)
            snprintf(cpu_str, sizeof(cpu_str), "%.2f ms", r->cpu_avg_ns / 1e6);
        else if (r->cpu_avg_ns >= 1e3)
            snprintf(cpu_str, sizeof(cpu_str), "%.1f us", r->cpu_avg_ns / 1e3);
        else
            snprintf(cpu_str, sizeof(cpu_str), "%.1f ns", r->cpu_avg_ns);
        snprintf(pct_str, sizeof(pct_str), "%.1f%%", r->cpu_pct);
        if (r->cycles_per_op >= 1e6)
            snprintf(cyc_str, sizeof(cyc_str), "%.2f M", r->cycles_per_op / 1e6);
        else if (r->cycles_per_op >= 1e3)
            snprintf(cyc_str, sizeof(cyc_str), "%.1f K", r->cycles_per_op / 1e3);
        else
            snprintf(cyc_str, sizeof(cyc_str), "%.0f", r->cycles_per_op);
        if (r->wall_ops_per_sec >= 1e6)
            snprintf(ops_str, sizeof(ops_str), "%.2f M/s", r->wall_ops_per_sec / 1e6);
        else
            snprintf(ops_str, sizeof(ops_str), "%.2f K/s", r->wall_ops_per_sec / 1e3);
        snprintf(pass_str, sizeof(pass_str), "%d/%d", r->success_count, r->error_count);

        printf("%-14s | %-26s | %-30s | %9s | %9s | %6s | %10s | %10s | %-7s\n",
               r->category, r->syscall_name, r->situation,
               wall_str, cpu_str, pct_str, cyc_str, ops_str, pass_str);
    }

    printf("========================================================================================================================\n");
    printf("SUMMARY: %u scenarios x %u iterations (total invocations: %lu)\n",
           total_tests, iterations, (unsigned long)total_tests * iterations);
    printf("Total wall time: %.2f ms | Total CPU time: %.2f ms (%.1f%%) | Total cycles: %.3f G\n",
           grand_wall_ns / 1e6, grand_cpu_ns / 1e6,
           grand_wall_ns > 0 ? 100.0 * grand_cpu_ns / grand_wall_ns : 0.0,
           grand_cycles / 1e9);
    printf("NOTE: on AvoryOS THREAD_CPUTIME currently aliases monotonic and rusage is 1ms-granular;\n");
    printf("      cycles/op is the finest per-syscall CPU-cost signal there. On Linux all columns are exact.\n");
    printf("========================================================================================================================\n\n");
}

int main(int argc, char **argv) {
    unsigned iterations = DEFAULT_ITERATIONS;
    if (argc > 1) {
        int user_iter = atoi(argv[1]);
        if (user_iter > 0) iterations = (unsigned)user_iter;
    }

    printf("Starting Syscall CPU Benchmark Suite (%u iterations per scenario)...\n", iterations);
    fflush(stdout);

    bench_process_thread(iterations);
    bench_memory(iterations);
    bench_file_io(iterations);
    bench_time_timers(iterations);
    bench_signals(iterations);
    bench_events_epoll(iterations);
    bench_sockets_ipc(iterations);
    bench_system_info(iterations);
    bench_extended(iterations);
    /* TEMPORARY profiler hook: ask the kernel for its TSC breakdown now that
     * every stage ran (prctl 0x50544344 plots to serial; on Linux the option
     * is unknown -> EINVAL, harmless). */
    syscall(SYS_prctl, 0x50544344, 0, 0, 0, 0);

    print_report(iterations);

    return 0;
}
