/*
 * test_blocking_liveness.c — AvoryOS Kernel Blocking & Spin-Detection Test Suite
 *
 * Verifies that blocking syscalls actually put threads into THREAD_BLOCKED
 * without burning CPU cycles (detecting false non-blocking, busy-spin loops,
 * and level-triggered polling storms).
 *
 * Scenarios tested:
 *   1. FIFO open() blocking (the supervise-daemon regression scenario)
 *   2. FIFO read() blocking until writer arrives
 *   3. Anonymous pipe read() blocking
 *   4. epoll_wait() timeout liveness and cycle burn
 *   5. Level-triggered epoll readiness drain (ensures no immediate return loop)
 *   6. futex(FUTEX_WAIT) parking
 *   7. AF_UNIX socket SO_RCVTIMEO blocking
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <linux/futex.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t get_thread_cpu_us(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) == 0) {
        return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000ULL +
               (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
    }
    return 0;
}

static int g_total_tests = 0;
static int g_passed_tests = 0;
static int g_failed_tests = 0;

typedef struct {
    const char *name;
    double expected_ms;
    double actual_ms;
    double cpu_ms;
    double cpu_pct;
    bool passed;
    const char *detail;
} test_result_t;

static void print_result(const test_result_t *res) {
    g_total_tests++;
    if (res->passed) g_passed_tests++;
    else g_failed_tests++;

    printf("  %-36s | %5.0f ms | %5.0f ms | %6.1f ms | %5.1f%% | ",
           res->name, res->expected_ms, res->actual_ms, res->cpu_ms, res->cpu_pct);

    if (res->passed) {
        printf(COLOR_GREEN "[PASS]" COLOR_RESET "\n");
    } else {
        printf(COLOR_RED "[FAIL: %s]" COLOR_RESET "\n", res->detail);
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// 1. FIFO open() Liveness Test
// ---------------------------------------------------------------------------
static void test_fifo_open(void) {
    const char *path = "/tmp/test_liveness_fifo";
    unlink(path);
    mkfifo(path, 0666);

    int res_pipe[2];
    pipe(res_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(res_pipe[0]);

        uint64_t c0 = get_thread_cpu_us();
        uint64_t t0 = get_time_ns();

        int fd = open(path, O_RDONLY);

        uint64_t t1 = get_time_ns();
        uint64_t c1 = get_thread_cpu_us();

        if (fd >= 0) close(fd);

        double actual_ms = (double)(t1 - t0) / 1e6;
        double cpu_ms = (double)(c1 - c0) / 1000.0;

        write(res_pipe[1], &actual_ms, sizeof(actual_ms));
        write(res_pipe[1], &cpu_ms, sizeof(cpu_ms));
        close(res_pipe[1]);
        _exit(0);
    }

    close(res_pipe[1]);

    // Hold off writer for 120ms
    usleep(120000);
    int wfd = open(path, O_WRONLY);

    double actual_ms = 0;
    double cpu_ms = 0;
    read(res_pipe[0], &actual_ms, sizeof(actual_ms));
    read(res_pipe[0], &cpu_ms, sizeof(cpu_ms));
    close(res_pipe[0]);

    waitpid(pid, NULL, 0);
    if (wfd >= 0) close(wfd);
    unlink(path);

    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "FIFO open(O_RDONLY) blocking",
        .expected_ms = 120.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (actual_ms >= 80.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 80.0) ? "Premature Return (Spinning)" : "Busy Spin CPU"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 2. FIFO read() Liveness Test
// ---------------------------------------------------------------------------
static void test_fifo_read(void) {
    const char *path = "/tmp/test_liveness_fifo2";
    unlink(path);
    mkfifo(path, 0666);

    int res_pipe[2];
    pipe(res_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(res_pipe[0]);
        int rfd = open(path, O_RDONLY);

        uint64_t c0 = get_thread_cpu_us();
        uint64_t t0 = get_time_ns();

        char b;
        int n = read(rfd, &b, 1);

        uint64_t t1 = get_time_ns();
        uint64_t c1 = get_thread_cpu_us();
        if (rfd >= 0) close(rfd);

        double actual_ms = (double)(t1 - t0) / 1e6;
        double cpu_ms = (double)(c1 - c0) / 1000.0;

        write(res_pipe[1], &actual_ms, sizeof(actual_ms));
        write(res_pipe[1], &cpu_ms, sizeof(cpu_ms));
        write(res_pipe[1], &n, sizeof(n));
        close(res_pipe[1]);
        _exit(0);
    }

    close(res_pipe[1]);

    int wfd = open(path, O_WRONLY);
    usleep(100000); // 100ms
    write(wfd, "A", 1);

    double actual_ms = 0;
    double cpu_ms = 0;
    int n = 0;
    read(res_pipe[0], &actual_ms, sizeof(actual_ms));
    read(res_pipe[0], &cpu_ms, sizeof(cpu_ms));
    read(res_pipe[0], &n, sizeof(n));
    close(res_pipe[0]);

    waitpid(pid, NULL, 0);
    if (wfd >= 0) close(wfd);
    unlink(path);

    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "FIFO read() blocking until data",
        .expected_ms = 100.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (n == 1 && actual_ms >= 70.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 70.0) ? "Immediate EOF / No Block" : "Busy Spin CPU"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 3. Pipe read() Liveness Test
// ---------------------------------------------------------------------------
static void test_pipe_read(void) {
    int p[2];
    pipe(p);

    int res_pipe[2];
    pipe(res_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(res_pipe[0]);
        close(p[1]);

        uint64_t c0 = get_thread_cpu_us();
        uint64_t t0 = get_time_ns();

        char b;
        int n = read(p[0], &b, 1);

        uint64_t t1 = get_time_ns();
        uint64_t c1 = get_thread_cpu_us();
        close(p[0]);

        double actual_ms = (double)(t1 - t0) / 1e6;
        double cpu_ms = (double)(c1 - c0) / 1000.0;

        write(res_pipe[1], &actual_ms, sizeof(actual_ms));
        write(res_pipe[1], &cpu_ms, sizeof(cpu_ms));
        write(res_pipe[1], &n, sizeof(n));
        close(res_pipe[1]);
        _exit(0);
    }

    close(res_pipe[1]);
    close(p[0]);

    usleep(100000);
    write(p[1], "Z", 1);

    double actual_ms = 0;
    double cpu_ms = 0;
    int n = 0;
    read(res_pipe[0], &actual_ms, sizeof(actual_ms));
    read(res_pipe[0], &cpu_ms, sizeof(cpu_ms));
    read(res_pipe[0], &n, sizeof(n));
    close(res_pipe[0]);

    waitpid(pid, NULL, 0);
    close(p[1]);

    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "Anonymous pipe read() blocking",
        .expected_ms = 100.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (n == 1 && actual_ms >= 70.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 70.0) ? "Premature EOF" : "Busy Spin CPU"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 4. epoll_wait() Timeout Liveness & CPU Burn
// ---------------------------------------------------------------------------
static void test_epoll_timeout(void) {
    int epfd = epoll_create1(0);
    int p[2];
    pipe(p);

    struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]};
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);

    uint64_t c0 = get_thread_cpu_us();
    uint64_t t0 = get_time_ns();

    struct epoll_event events[4];
    int nfds = epoll_wait(epfd, events, 4, 150); // 150ms timeout

    uint64_t t1 = get_time_ns();
    uint64_t c1 = get_thread_cpu_us();

    close(epfd);
    close(p[0]);
    close(p[1]);

    double actual_ms = (double)(t1 - t0) / 1e6;
    double cpu_ms = (double)(c1 - c0) / 1000.0;
    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "epoll_wait() timeout sleeping",
        .expected_ms = 150.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (nfds == 0 && actual_ms >= 120.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 120.0) ? "Premature Return" : "Kernel Polling Spin"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 5. Level-Triggered Epoll Drain Test (No Level-Trigger Storm)
// ---------------------------------------------------------------------------
static void test_epoll_drain(void) {
    int epfd = epoll_create1(0);
    int p[2];
    pipe(p);

    struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]};
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);

    // Make ready with 1 byte
    write(p[1], "X", 1);

    struct epoll_event events[1];
    int n1 = epoll_wait(epfd, events, 1, 0); // Non-blocking check

    // Drain the byte
    char b;
    read(p[0], &b, 1);

    // Second epoll call: buffer is empty now, must block for full 100ms!
    uint64_t c0 = get_thread_cpu_us();
    uint64_t t0 = get_time_ns();

    int n2 = epoll_wait(epfd, events, 1, 100);

    uint64_t t1 = get_time_ns();
    uint64_t c1 = get_thread_cpu_us();

    close(epfd);
    close(p[0]);
    close(p[1]);

    double actual_ms = (double)(t1 - t0) / 1e6;
    double cpu_ms = (double)(c1 - c0) / 1000.0;
    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "epoll level-trigger drain check",
        .expected_ms = 100.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (n1 == 1 && n2 == 0 && actual_ms >= 80.0 && cpu_pct < 10.0),
        .detail = (n2 > 0 || actual_ms < 80.0) ? "Spurious Level Re-trigger (100% CPU Bug)" : "High CPU"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 6. futex(FUTEX_WAIT) Parking Test
// ---------------------------------------------------------------------------
static int g_futex_val = 0;
static double g_futex_cpu_ms = 0;

static void *futex_waiter_thread(void *arg) {
    (void)arg;
    uint64_t c0 = get_thread_cpu_us();
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 120000000}; // 120ms timeout
    syscall(SYS_futex, &g_futex_val, FUTEX_WAIT, 0, &ts, NULL, 0);
    uint64_t c1 = get_thread_cpu_us();
    g_futex_cpu_ms = (double)(c1 - c0) / 1000.0;
    return NULL;
}

static void test_futex_wait(void) {
    g_futex_val = 0;
    g_futex_cpu_ms = 0;
    pthread_t th;

    uint64_t t0 = get_time_ns();
    pthread_create(&th, NULL, futex_waiter_thread, NULL);
    pthread_join(th, NULL);
    uint64_t t1 = get_time_ns();

    double actual_ms = (double)(t1 - t0) / 1e6;
    double cpu_ms = g_futex_cpu_ms;
    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "futex(FUTEX_WAIT) thread parking",
        .expected_ms = 120.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (actual_ms >= 90.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 90.0) ? "Premature Wakeup" : "Futex Spinlock Contention"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// 7. AF_UNIX SO_RCVTIMEO Blocking Test
// ---------------------------------------------------------------------------
static void test_socket_rcvtimeo(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    struct timeval tv = {.tv_sec = 0, .tv_usec = 120000}; // 120ms
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint64_t c0 = get_thread_cpu_us();
    uint64_t t0 = get_time_ns();

    char buf[16];
    ssize_t ret = recv(sv[0], buf, sizeof(buf), 0);

    uint64_t t1 = get_time_ns();
    uint64_t c1 = get_thread_cpu_us();

    close(sv[0]);
    close(sv[1]);

    double actual_ms = (double)(t1 - t0) / 1e6;
    double cpu_ms = (double)(c1 - c0) / 1000.0;
    double cpu_pct = actual_ms > 0 ? (cpu_ms / actual_ms) * 100.0 : 0.0;

    test_result_t res = {
        .name = "AF_UNIX SO_RCVTIMEO blocking",
        .expected_ms = 120.0,
        .actual_ms = actual_ms,
        .cpu_ms = cpu_ms,
        .cpu_pct = cpu_pct,
        .passed = (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && actual_ms >= 90.0 && cpu_pct < 10.0),
        .detail = (actual_ms < 90.0) ? "Immediate Timeout Error" : "Socket Spin Poll"
    };
    print_result(&res);
}

// ---------------------------------------------------------------------------
// Main Test Runner
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n" COLOR_BOLD "========================================================================================\n");
    printf("        AvoryOS Kernel Blocking Liveness & Spin-Detection Suite\n");
    printf("========================================================================================" COLOR_RESET "\n");

    printf("\n  %-36s | %8s | %8s | %10s | %6s | %s\n",
           "Test Name", "Expected", "Actual", "CPU Time", "CPU %", "Status");
    printf("  -------------------------------------+----------+----------+------------+--------+---------\n");
    fflush(stdout);

    test_fifo_open();
    test_fifo_read();
    test_pipe_read();
    test_epoll_timeout();
    test_epoll_drain();
    test_futex_wait();
    test_socket_rcvtimeo();

    printf("  -------------------------------------+----------+----------+------------+--------+---------\n");
    printf(COLOR_BOLD "Summary: %d / %d tests passed (" COLOR_RESET, g_passed_tests, g_total_tests);
    if (g_failed_tests == 0) {
        printf(COLOR_GREEN COLOR_BOLD "ALL TESTS PASSED - No High CPU Regressions Detected" COLOR_RESET ")\n\n");
    } else {
        printf(COLOR_RED COLOR_BOLD "%d TESTS FAILED - High CPU / Spin Defect Detected!" COLOR_RESET ")\n\n", g_failed_tests);
    }
    fflush(stdout);

    return g_failed_tests > 0 ? 1 : 0;
}
