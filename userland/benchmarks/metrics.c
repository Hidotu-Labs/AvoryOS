/* AvoryOS additions, GPL-2.0. Linked into the unmodified UnixBench tests. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static struct timespec started;
static struct rusage initial;
static pid_t owner;

static double cpu_seconds(const struct rusage *r)
{
    return r->ru_utime.tv_sec + r->ru_stime.tv_sec +
           (r->ru_utime.tv_usec + r->ru_stime.tv_usec) / 1e6;
}

static void metrics(void)
{
    struct timespec end;
    struct rusage r;
    /* spawn's children inherit atexit handlers; only report the worker. */
    if (getpid() != owner) return;
    if (clock_gettime(CLOCK_MONOTONIC, &end) || getrusage(RUSAGE_SELF, &r)) {
        perror("benchmark accounting");
        return;
    }
    double wall = end.tv_sec - started.tv_sec +
                  (end.tv_nsec - started.tv_nsec) / 1e9;
    double cpu = cpu_seconds(&r) - cpu_seconds(&initial);
    double user = r.ru_utime.tv_sec - initial.ru_utime.tv_sec +
                  (r.ru_utime.tv_usec - initial.ru_utime.tv_usec) / 1e6;
    double system = r.ru_stime.tv_sec - initial.ru_stime.tv_sec +
                    (r.ru_stime.tv_usec - initial.ru_stime.tv_usec) / 1e6;
    fprintf(stderr, "METRICS wall_s=%.6f cpu_s=%.6f user_s=%.6f system_s=%.6f cpu_percent=%.2f (worker only)\n",
            wall, cpu, user, system, wall > 0 ? 100 * cpu / wall : 0);
}

__attribute__((constructor)) static void begin(void)
{
    owner = getpid();
    if (clock_gettime(CLOCK_MONOTONIC, &started) ||
        getrusage(RUSAGE_SELF, &initial) || atexit(metrics)) {
        perror("benchmark accounting init");
        exit(1);
    }
}
