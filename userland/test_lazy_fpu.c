#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/* Raw syscalls keep libc (which may use SIMD) outside measured loops. */
static inline long raw6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

static int has_avx;
static int avx_enabled(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if ((c & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28))) return 0;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return (a & 6) == 6;
}

/* Keep sentinels live across a syscall in one asm block, so neither the
 * compiler nor the ABI's caller-saved XMM convention can spoil the test. */
static long probe(long syscall_nr, long arg, const uint64_t *seed, uint64_t *out) {
    long ret;
    if (has_avx) {
        __asm__ volatile("vmovdqu (%2), %%ymm15; fldl 32(%2); syscall; "
                         "fstpl 32(%3); vmovdqu %%ymm15, (%3); vzeroupper"
                         : "=a"(ret) : "a"(syscall_nr), "r"(seed), "r"(out), "D"(arg)
                         : "rcx", "r11", "ymm15", "st", "memory");
    } else {
        __asm__ volatile("movdqu (%2), %%xmm15; fldl 32(%2); syscall; "
                         "fstpl 32(%3); movdqu %%xmm15, (%3)"
                         : "=a"(ret) : "a"(syscall_nr), "r"(seed), "r"(out), "D"(arg)
                         : "rcx", "r11", "xmm15", "st", "memory");
    }
    return ret;
}
static int matches(const uint64_t *a, const uint64_t *b) {
    return !memcmp(a, b, has_avx ? 32 : 16) && a[4] == b[4];
}
static void *correctness_worker(void *arg) {
    uint64_t id = (uintptr_t)arg;
    uint64_t seed[5] = {id + 1, id + 2, id + 3, id + 4, 0x3ff0000000000000ULL};
    uint64_t out[5];
    for (int i = 0; i < 2000; i++) {
        probe(SYS_sched_yield, 0, seed, out);
        if (!matches(seed, out)) return (void *)1;
    }
    return NULL;
}
static void signal_clobber(int sig) {
    (void)sig;
    __asm__ volatile("pxor %%xmm15, %%xmm15; fldz; fstp %%st(0)" ::: "xmm15", "st");
}
static int correctness(void) {
    for (int round = 0; round < 3; round++) {
        pthread_t threads[4];
        for (int i = 0; i < 4; i++)
            if (pthread_create(&threads[i], NULL, correctness_worker, (void *)(uintptr_t)(i + round * 4))) return 0;
        for (int i = 0; i < 4; i++) {
            void *result;
            if (pthread_join(threads[i], &result) || result) return 0;
        }
    }
    uint64_t seed[5] = {11, 22, 33, 44, 0x4000000000000000ULL}, out[5];
    long pid = probe(SYS_fork, 0, seed, out);
    if (pid < 0) return 0;
    if (!pid) _exit(matches(seed, out) ? 0 : 1);
    int status;
    if (!matches(seed, out) || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 0;
    struct sigaction sa = {.sa_handler = signal_clobber};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL)) return 0;
    /* Queue while blocked, then unblock via raw syscall with state live. */
    sigset_t set;
    sigemptyset(&set); sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, NULL) || raise(SIGUSR1)) return 0;
    uint64_t mask = 1ULL << (SIGUSR1 - 1);
    long ret;
    register long r10 __asm__("r10") = 8;
    __asm__ volatile("movdqu (%2), %%xmm15; fldl 32(%2); syscall; "
                     "fstpl 32(%3); movdqu %%xmm15, (%3)"
                     : "=a"(ret) : "a"((long)SYS_rt_sigprocmask), "r"(seed), "r"(out),
                       "D"((long)SIG_UNBLOCK), "S"(&mask), "d"(0L), "r"(r10)
                     : "rcx", "r11", "xmm15", "st", "memory");
    return ret == 0 && !memcmp(seed, out, 16) && seed[4] == out[4];
}

#define ROUNDS 20000
static int turn;
static void wait_turn(int want) {
    int value;
    while ((value = __atomic_load_n(&turn, __ATOMIC_ACQUIRE)) != want)
        raw6(SYS_futex, (long)&turn, FUTEX_WAIT_PRIVATE, value, 0, 0, 0);
}
static void handoff(int next) {
    __atomic_store_n(&turn, next, __ATOMIC_RELEASE);
    raw6(SYS_futex, (long)&turn, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
}
static void touch_fpu(void) {
    __asm__ volatile("pxor %%xmm15, %%xmm15" ::: "xmm15");
}
static void *pong_integer(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) { wait_turn(1); handoff(0); }
    return NULL;
}
static void *pong_fpu(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) { wait_turn(1); touch_fpu(); handoff(0); }
    return NULL;
}
static uint64_t ns(const struct timespec *t) { return (uint64_t)t->tv_sec * 1000000000 + t->tv_nsec; }
static uint64_t benchmark(int use_fpu) {
    pthread_t thread;
    struct timespec start, end;
    turn = 0;
    if (pthread_create(&thread, NULL, use_fpu ? pong_fpu : pong_integer, NULL)) exit(1);
    raw6(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&start, 0, 0, 0, 0);
    for (int i = 0; i < ROUNDS; i++) {
        if (use_fpu) touch_fpu();
        handoff(1); wait_turn(0);
    }
    raw6(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&end, 0, 0, 0, 0);
    if (pthread_join(thread, NULL)) exit(1);
    return ns(&end) - ns(&start);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    has_avx = avx_enabled();
    puts("Lazy FPU: correctness across available CPUs");
    if (!correctness()) { puts("FAIL: SMP register preservation/fork/signal"); return 1; }
    cpu_set_t allowed, one;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) { perror("getaffinity"); return 1; }
    int cpu;
    for (cpu = 0; cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed); cpu++);
    if (cpu == CPU_SETSIZE) return 1;
    CPU_ZERO(&one); CPU_SET(cpu, &one);
    if (sched_setaffinity(0, sizeof(one), &one)) { perror("setaffinity"); return 1; }
    has_avx = avx_enabled();
    printf("Lazy FPU: CPU %d, x87/SSE%s correctness\n", cpu, has_avx ? "/AVX" : "");
    if (!correctness()) { puts("FAIL: register preservation/fork/signal"); return 1; }
    puts("PASS: yields, ownership collisions, thread teardown, fork, signal return");
    uint64_t integer = benchmark(0), fpu = benchmark(1);
    printf("%d rounds on one CPU: integer %llu ns/handoff, SIMD %llu ns/handoff\n",
           ROUNDS, (unsigned long long)(integer / (2 * ROUNDS)), (unsigned long long)(fpu / (2 * ROUNDS)));
    puts("Includes futex and scheduling overhead; compare identical eager/lazy builds.");
    return 0;
}
