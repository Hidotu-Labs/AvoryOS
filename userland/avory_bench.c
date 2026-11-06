/* Userland benchmark harness and missing workloads. GPL-2.0. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char bindir[PATH_MAX], self[PATH_MAX];
static const char *suffix;

static void die(const char *s) { perror(s); exit(1); }
static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock_gettime");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static unsigned number(const char *s, unsigned max)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < 1 || n > max) {
        fprintf(stderr, "Expected integer in range 1..%u: %s\n", max, s);
        exit(2);
    }
    return (unsigned)n;
}
static int wait_ok(pid_t p)
{
    int status;
    pid_t r;
    do { r = waitpid(p, &status, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) die("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "Worker failed: wait status 0x%x\n", status);
        return 1;
    }
    return 0;
}
static void run(const char *name, char **args)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s%s", bindir, name, suffix) >= (int)sizeof(path)) {
        fprintf(stderr, "Executable path too long\n"); exit(2);
    }
    fflush(NULL);
    pid_t p = fork();
    if (p < 0) die("fork");
    if (!p) { execv(path, args); perror(path); _exit(127); }
    if (wait_ok(p)) exit(1);
}
static long proc_kib(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long value = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, strlen(key)) &&
            sscanf(line + strlen(key), "%ld", &value) == 1) break;
    }
    fclose(f);
    return value;
}
static void memory_snapshot(const char *stage)
{
    long rss = proc_kib("/proc/self/status", "VmRSS:");
    long virt = proc_kib("/proc/self/status", "VmSize:");
    long free_kib = proc_kib("/proc/meminfo", "MemFree:");
    printf("RAM stage=%s rss_KiB=%ld virtual_KiB=%ld system_free_KiB=%ld (-1=unavailable)\n",
           stage, rss, virt, free_kib);
}
static void memory(unsigned mib)
{
    size_t bytes = (size_t)mib * 1024 * 1024;
    memory_snapshot("before");
    uint64_t start = now();
    volatile uint64_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    printf("RAM mmap_us=%.3f requested_MiB=%u\n", (now()-start)/1e3, mib);
    memory_snapshot("mapped");
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < (long)sizeof(*p)) die("page size");
    start = now();
    for (size_t i = 0; i < bytes / sizeof(*p); i += (size_t)page_size / sizeof(*p)) p[i] = i;
    printf("RAM first_touch_us=%.3f\n", (now()-start)/1e3);
    memory_snapshot("touched");
    start = now();
    uint64_t checksum = 0;
    /* Volatile accesses and a verified checksum keep all eight passes real. */
    for (unsigned pass = 0; pass < 8; pass++)
        for (size_t i = 0; i < bytes / sizeof(*p); i++) {
            p[i] = (uint64_t)i + pass;
            checksum += p[i];
        }
    uint64_t elapsed = now()-start, n = bytes / sizeof(*p);
    uint64_t expected = 8 * (n * (n-1) / 2) + 28 * n;
    if (checksum != expected) { fprintf(stderr, "RAM checksum mismatch\n"); exit(1); }
    printf("RAM stream_read_write_MiB_s=%.3f passes=8 checksum=%" PRIu64 "\n",
           elapsed ? 16.0*mib*1e9/elapsed : 0, checksum);
    start = now();
    if (munmap((void *)p, bytes)) die("munmap");
    printf("RAM munmap_us=%.3f\n", (now()-start)/1e3);
    memory_snapshot("released");
}
static void startup(unsigned count)
{
    uint64_t ready_total = 0, total = 0, min = UINT64_MAX, max = 0;
    for (unsigned i = 0; i < count; i++) {
        int fds[2];
        if (pipe(fds)) die("pipe");
        char fdstr[24];
        snprintf(fdstr, sizeof(fdstr), "%d", fds[1]);
        fflush(NULL);
        uint64_t start = now();
        pid_t p = fork();
        if (p < 0) die("fork");
        if (!p) {
            close(fds[0]);
            execl(self, self, "--ready", fdstr, (char *)NULL);
            perror("startup exec"); _exit(127);
        }
        close(fds[1]);
        uint64_t ready;
        size_t got = 0;
        while (got < sizeof(ready)) {
            ssize_t r = read(fds[0], (char *)&ready + got, sizeof(ready)-got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fds[0]);
        int failed = wait_ok(p);
        if (failed || got != sizeof(ready) || ready < start) {
            fprintf(stderr, "Startup handshake failed\n"); exit(1);
        }
        uint64_t dt = ready-start;
        ready_total += dt;
        total += now()-start;
        if (dt < min) min = dt;
        if (dt > max) max = dt;
    }
    printf("STARTUP count=%u fork_exec_to_main_avg_us=%.3f min_us=%.3f max_us=%.3f fork_exec_wait_avg_us=%.3f\n",
           count, ready_total/(1e3*count), min/1e3, max/1e3, total/(1e3*count));
}
static void metadata(unsigned count, const char *base)
{
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/avory-meta-XXXXXX", base) >= (int)sizeof(dir)) {
        fprintf(stderr, "Work directory path too long\n"); exit(2);
    }
    if (!mkdtemp(dir)) die("mkdtemp");
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) die("open benchmark directory");
    uint64_t start = now();
    struct stat st;
    for (unsigned i = 0; i < count; i++) {
        int fd = openat(dfd, "entry", O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) goto fail;
        if (close(fd) || fstatat(dfd, "entry", &st, 0) ||
            renameat(dfd, "entry", dfd, "renamed") || unlinkat(dfd, "renamed", 0)) goto fail;
    }
    uint64_t dt = now()-start;
    printf("VFS metadata_cycles=%u create_close_stat_rename_unlink_ns_per_cycle=%.3f cycles_s=%.3f\n",
           count, (double)dt/count, dt ? (double)count*1e9/dt : 0);
    close(dfd);
    if (rmdir(dir)) die("rmdir");
    return;
fail:
    perror("VFS metadata operation");
    unlinkat(dfd, "entry", 0); unlinkat(dfd, "renamed", 0);
    close(dfd); rmdir(dir); exit(1);
}
static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s all|core [seconds=3] [RAM_MiB=32] [workdir=/tmp]\n"
            "       %s cpu|iterations|fork [seconds=3]\n"
            "       %s startup [count=50]\n"
            "       %s memory [MiB=32]\n"
            "       %s vfs [seconds=3] [workdir=/tmp]\n"
            "       %s desktop|scheduling|storage|apps|graphics [samples=120] [workdir=/tmp] [workers=2]\n"
            "       %s browser [runs=3] [workdir=/tmp]\n"
            "       %s command RUNS -- PROGRAM [ARGS...]\n",
            name, name, name, name, name, name, name, name);
}
int main(int argc, char **argv)
{
    /* Take the timestamp before path resolution or stdio initialization. */
    uint64_t reached_main = now();
    if (argc == 3 && !strcmp(argv[1], "--ready")) {
        int fd = (int)number(argv[2], INT_MAX);
        ssize_t n;
        do { n = write(fd, &reached_main, sizeof(reached_main)); } while (n < 0 && errno == EINTR);
        _exit(n == sizeof(reached_main) ? 0 : 1);
    }
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (len < 0 || len >= (ssize_t)sizeof(self)-1) die("/proc/self/exe");
    self[len] = 0;
    strcpy(bindir, self);
    char *slash = strrchr(bindir, '/');
    if (!slash) { fprintf(stderr, "Expected absolute executable path\n"); return 1; }
    *slash = 0;
    suffix = len >= 4 && !strcmp(self+len-4, ".elf") ? ".elf" : "";
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!strcmp(mode,"desktop") || !strcmp(mode,"scheduling") || !strcmp(mode,"storage") ||
        !strcmp(mode,"apps") || !strcmp(mode,"graphics") || !strcmp(mode,"browser") || !strcmp(mode,"command")) {
        char **args=calloc((size_t)argc+1,sizeof(*args));if(!args)die("desktop arguments");
        args[0]="bench_desktop";for(int i=1;i<argc;i++)args[i]=argv[i];
        if(!strcmp(mode,"desktop"))args[1]="all";
        run("bench_desktop",args);free(args);return 0;
    }
    int core = !strcmp(mode,"core");
    int all = !strcmp(mode, "all") || core, vfs = !strcmp(mode, "vfs");
    if (!all && !vfs && strcmp(mode, "cpu") && strcmp(mode, "iterations") &&
        strcmp(mode, "fork") && strcmp(mode, "startup") && strcmp(mode, "memory")) {
        usage(argv[0]); return !strcmp(mode, "--help") ? 0 : 2;
    }
    if (argc > (all ? 5 : vfs ? 4 : 3)) { usage(argv[0]); return 2; }
    unsigned limit = !strcmp(mode, "memory") ? 1024 : !strcmp(mode, "startup") ? 100000 : 3600;
    unsigned value = argc > 2 ? number(argv[2], limit) : !strcmp(mode, "memory") ? 32 : !strcmp(mode, "startup") ? 50 : 3;
    unsigned mib = all && argc > 3 ? number(argv[3], 1024) : 32;
    const char *base = all && argc > 4 ? argv[4] : vfs && argc > 3 ? argv[3] : "/tmp";
    char seconds[16]; snprintf(seconds, sizeof(seconds), "%u", value);
    if (all || !strcmp(mode, "cpu")) {
        puts("CPU: UnixBench Dhrystone 2 (COUNT is iterations over the requested seconds)");
        run("bench_cpu", (char *[]){"bench_cpu", seconds, NULL});
    }
    if (all || !strcmp(mode, "iterations")) {
        puts("ITERATIONS: UnixBench Hanoi, 16 disks (65535 recursive moves per iteration)");
        run("bench_iterations", (char *[]){"bench_iterations", seconds, "16", NULL});
    }
    if (all || !strcmp(mode, "fork")) {
        puts("PROCESS: UnixBench fork/exit/wait throughput");
        run("bench_fork", (char *[]){"bench_fork", seconds, NULL});
    }
    if (all || !strcmp(mode, "startup")) startup(all ? 50 : value);
    if (all || !strcmp(mode, "memory")) memory(all ? mib : value);
    if (all || vfs) {
        metadata(1000, base);
        char dir[PATH_MAX];
        if (snprintf(dir, sizeof(dir), "%s/avory-io-XXXXXX", base) >= (int)sizeof(dir)) {
            fprintf(stderr, "Work directory path too long\n"); return 2;
        }
        if (!mkdtemp(dir)) die("mkdtemp");
        const char *tests[] = {"-w", "-r", "-c"};
        for (unsigned i = 0; i < 3; i++) {
            printf("VFS: UnixBench fstime %s (8 KiB buffer, 32 MiB files, cached I/O)\n", tests[i]);
            run("bench_vfs", (char *[]){"bench_vfs", (char *)tests[i], "-b", "8192", "-m", "32768", "-t", seconds, "-f", "-d", dir, NULL});
        }
        if (rmdir(dir)) die("rmdir benchmark directory");
    }
    if(all&&!core)run("bench_desktop",(char *[]){"bench_desktop","all","120",(char *)base,NULL});
    return 0;
}
