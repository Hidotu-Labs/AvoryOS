/* Filesystem/journal-heavy baseline; timings do not isolate journal CPU time.
 * Usage: test_journal_speed DIRECTORY [files_per_worker=500] [workers=1] [rounds=5]
 * Only files in a newly created private directory are modified or removed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

enum { CREATE, WRITE, FSYNC_WRITE, RENAME, UNLINK, FD_CHURN, PHASES };
static const char *names[] = {"create_empty", "write_4k", "overwrite_4k_fsync",
                              "rename", "unlink", "fd_churn"};
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    int go, abort;
} gate_t;
typedef struct {
    gate_t *gate;
    int dirfd, phase, error, drain_profile;
    unsigned id, files;
    uint64_t *samples, drain_ns;
    const char *error_op;
    unsigned error_file;
    int error_fd;
    uint64_t drain_total[3], drain_max[3];
    unsigned drain_calls[3];
} worker_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void filename(char *buf, size_t size, unsigned worker, unsigned file,
                     int renamed) {
    snprintf(buf, size, "w%u-f%u%s", worker, file, renamed ? ".renamed" : "");
}

static void pattern(unsigned char *buf, unsigned worker, unsigned file, int phase) {
    for (unsigned j = 0; j < 4096; j++)
        buf[j] = (unsigned char)(j * 17u + worker * 31u + file * 7u + phase * 13u);
}

static int write_all(int fd, const unsigned char *buf) {
    size_t done = 0;
    while (done < 4096) {
        ssize_t n = write(fd, buf + done, 4096 - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static void record_error(worker_t *w, const char *op, unsigned file, int fd) {
    w->error = errno ? errno : EIO;
    w->error_op = op;
    w->error_file = file;
    w->error_fd = fd;
}

static int sync_file(worker_t *w, const char *name, unsigned file) {
    int fd = openat(w->dirfd, name, O_RDWR);
    if (fd < 0) { record_error(w, "drain/openat", file, -1); return -1; }
    int ret = fsync(fd);
    if (ret) record_error(w, "drain/fsync", file, fd);
    if (close(fd) && !ret) {
        record_error(w, "drain/close", file, fd);
        ret = -1;
    }
    return ret;
}

static void drain_sample(worker_t *w, unsigned op, uint64_t start) {
    uint64_t ns = now_ns() - start;
    w->drain_total[op] += ns;
    if (ns > w->drain_max[op]) w->drain_max[op] = ns;
    w->drain_calls[op]++;
}

/* Separate path keeps the normal benchmark free of per-drain clock calls. */
static int sync_file_profiled(worker_t *w, const char *name, unsigned file) {
    uint64_t start = now_ns();
    int fd = openat(w->dirfd, name, O_RDWR);
    int saved_errno = errno;
    drain_sample(w, 0, start);
    if (fd < 0) {
        errno = saved_errno;
        record_error(w, "drain/openat", file, -1);
        return -1;
    }
    start = now_ns();
    int ret = fsync(fd);
    saved_errno = errno;
    drain_sample(w, 1, start);
    if (ret) {
        errno = saved_errno;
        record_error(w, "drain/fsync", file, fd);
    }
    start = now_ns();
    int closed = close(fd);
    saved_errno = errno;
    drain_sample(w, 2, start);
    if (closed && !ret) {
        errno = saved_errno;
        record_error(w, "drain/close", file, fd);
        ret = -1;
    }
    return ret;
}

static void *worker(void *arg) {
    worker_t *w = arg;
    pthread_mutex_lock(&w->gate->lock);
    w->gate->ready++;
    pthread_cond_broadcast(&w->gate->cond);
    while (!w->gate->go) pthread_cond_wait(&w->gate->cond, &w->gate->lock);
    int abort = w->gate->abort;
    pthread_mutex_unlock(&w->gate->lock);
    if (abort) return NULL;

    /* Check inherited directory access in each new pthread generation.
     * Keep this outside the per-operation samples. */
    struct stat st;
    if (fstat(w->dirfd, &st)) {
        record_error(w, "preflight/fstat-directory", 0, w->dirfd);
        return NULL;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        record_error(w, "preflight/directory-type", 0, w->dirfd);
        return NULL;
    }

    char src[64], dst[64];
    unsigned char buf[4096];
    for (unsigned i = 0; i < w->files; i++) {
        unsigned file = w->phase == FD_CHURN ? 0 : i;
        filename(src, sizeof(src), w->id, file, 0);
        filename(dst, sizeof(dst), w->id, i, 1);
        /* Pattern construction is outside the individual latency sample. */
        if (w->phase == WRITE || w->phase == FSYNC_WRITE)
            pattern(buf, w->id, i, w->phase);
        uint64_t start = now_ns();
        int ret = 0;
        if (w->phase <= FSYNC_WRITE || w->phase == FD_CHURN) {
            int flags = O_RDWR;
            if (w->phase == CREATE) flags |= O_CREAT | O_EXCL;
            int fd = openat(w->dirfd, src, flags, 0600);
            if (fd < 0) { record_error(w, "openat", file, -1); break; }
            if (w->phase == WRITE || w->phase == FSYNC_WRITE) {
                ret = write_all(fd, buf);
                if (ret) record_error(w, "write", i, fd);
            }
            if (w->phase == FD_CHURN) {
                ret = fstat(fd, &st);
                if (!ret && !S_ISREG(st.st_mode)) { errno = EIO; ret = -1; }
                if (ret) record_error(w, "fstat-file", file, fd);
            }
            if (!ret && w->phase == FSYNC_WRITE) {
                ret = fsync(fd);
                if (ret) record_error(w, "fsync", i, fd);
            }
            if (close(fd) && !ret) {
                record_error(w, "close", file, fd);
                ret = -1;
            }
        } else if (w->phase == RENAME) {
            ret = renameat(w->dirfd, src, w->dirfd, dst);
            if (ret) record_error(w, "renameat", i, w->dirfd);
        } else {
            ret = unlinkat(w->dirfd, dst, 0);
            if (ret) record_error(w, "unlinkat", i, w->dirfd);
        }
        if (ret) break;
        w->samples[i] = now_ns() - start;
    }
    if (w->error) return NULL;

    /* Include requested writeback in throughput, but report it separately
     * from per-operation latency. No global sync of unrelated filesystems. */
    uint64_t start = now_ns();
    if (w->phase == CREATE || w->phase == WRITE) {
        for (unsigned i = 0; i < w->files; i++) {
            filename(src, sizeof(src), w->id, i, 0);
            if (w->drain_profile ? sync_file_profiled(w, src, i) : sync_file(w, src, i)) break;
        }
    }
    w->drain_ns = now_ns() - start;
    return NULL;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile, including small sample counts. */
static uint64_t percentile(uint64_t *s, size_t n, unsigned pct) {
    return s[(n * pct + 99) / 100 - 1];
}

static int verify(int dirfd, unsigned files, unsigned workers, int phase) {
    unsigned char expected[4096], actual[4096];
    char name[64];
    for (unsigned w = 0; w < workers; w++) {
        for (unsigned i = 0; i < files; i++) {
            filename(name, sizeof(name), w, i, phase == RENAME);
            int fd = openat(dirfd, name, O_RDONLY);
            if (fd < 0) return -1;
            size_t done = 0;
            while (done < sizeof(actual)) {
                ssize_t n = read(fd, actual + done, sizeof(actual) - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { close(fd); errno = EIO; return -1; }
                done += (size_t)n;
            }
            unsigned char extra;
            ssize_t n;
            do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
            int closed = close(fd);
            pattern(expected, w, i, phase == WRITE ? WRITE : FSYNC_WRITE);
            if (n != 0 || closed || memcmp(expected, actual, sizeof(actual))) {
                errno = EIO;
                return -1;
            }
        }
    }
    return 0;
}

static int run_phase(int dirfd, int phase, unsigned files, unsigned workers,
                     unsigned round, int drain_profile, uint64_t *elapsed) {
    gate_t gate = {.lock = PTHREAD_MUTEX_INITIALIZER,
                   .cond = PTHREAD_COND_INITIALIZER};
    pthread_t threads[32];
    worker_t work[32] = {{0}};
    size_t count = (size_t)files * workers;
    uint64_t *samples = calloc(count, sizeof(*samples));
    if (!samples) return -1;
    unsigned launched = 0;
    int error = 0;
    for (unsigned w = 0; w < workers; w++) {
        work[w] = (worker_t){.gate = &gate, .dirfd = dirfd, .phase = phase,
                            .id = w, .files = files, .drain_profile = drain_profile,
                            .samples = samples + w * files};
        int ret = pthread_create(&threads[w], NULL, worker, &work[w]);
        if (ret) { error = ret; break; }
        launched++;
    }
    pthread_mutex_lock(&gate.lock);
    while (gate.ready < launched) pthread_cond_wait(&gate.cond, &gate.lock);
    uint64_t start = now_ns();
    gate.abort = error != 0;
    gate.go = 1;
    pthread_cond_broadcast(&gate.cond);
    pthread_mutex_unlock(&gate.lock);
    uint64_t max_drain = 0;
    for (unsigned w = 0; w < launched; w++) {
        int ret = pthread_join(threads[w], NULL);
        if (ret) { fprintf(stderr, "pthread_join: %s\n", strerror(ret)); exit(1); }
        if (work[w].error) {
            if (!error) error = work[w].error;
            fprintf(stderr, "FAIL worker=%u phase=%s op=%s file=w%u-f%u%s "
                    "fd=%d dirfd=%d errno=%d (%s)\n",
                    w, names[phase], work[w].error_op, w, work[w].error_file,
                    phase == UNLINK ? ".renamed" : "", work[w].error_fd,
                    dirfd, work[w].error, strerror(work[w].error));
        }
        if (work[w].drain_ns > max_drain) max_drain = work[w].drain_ns;
    }
    uint64_t dir_start = now_ns();
    if (!error && fsync(dirfd)) {
        error = errno;
        fprintf(stderr, "FAIL phase=%s op=directory-fsync dirfd=%d errno=%d (%s)\n",
                names[phase], dirfd, error, strerror(error));
    }
    uint64_t dir_ns = now_ns() - dir_start;
    *elapsed = now_ns() - start;
    pthread_cond_destroy(&gate.cond);
    pthread_mutex_destroy(&gate.lock);
    if (!error && !*elapsed) error = ERANGE;
    if (!error) {
        qsort(samples, count, sizeof(*samples), compare_u64);
        printf("%u,%s,%u,%u,%zu,%.3f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               round, names[phase], workers, files, count, *elapsed / 1e6,
               count * 1e9 / *elapsed, percentile(samples, count, 50) / 1e3,
               percentile(samples, count, 95) / 1e3,
               percentile(samples, count, 99) / 1e3,
               max_drain / 1e6, dir_ns / 1e6);
        fflush(stdout);
    }
    if (drain_profile && (phase == CREATE || phase == WRITE)) {
        static const char *ops[] = {"openat", "fsync", "close"};
        for (unsigned w = 0; w < launched; w++)
            for (unsigned op = 0; op < 3; op++)
                printf("# drain_profile,round=%u,phase=%s,worker=%u,op=%s,calls=%u,total_ms=%.3f,max_us=%.3f\n",
                       round, names[phase], w, ops[op], work[w].drain_calls[op],
                       work[w].drain_total[op] / 1e6, work[w].drain_max[op] / 1e3);
        fflush(stdout);
    }
    free(samples);
    if (error) { errno = error; return -1; }
    return 0;
}

static unsigned number(const char *s, unsigned max) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || !n || n > max || *s == '-') {
        fprintf(stderr, "Invalid argument '%s': expected 1..%u\n", s, max);
        exit(2);
    }
    return (unsigned)n;
}

static int cleanup(int dirfd, const char *path, unsigned files, unsigned workers) {
    int failed = 0;
    char name[64];
    for (unsigned w = 0; w < workers; w++) {
        for (unsigned i = 0; i < files; i++) {
            for (int renamed = 0; renamed <= 1; renamed++) {
                filename(name, sizeof(name), w, i, renamed);
                if (unlinkat(dirfd, name, 0) && errno != ENOENT) failed = 1;
            }
        }
    }
    if (close(dirfd)) failed = 1;
    if (rmdir(path)) failed = 1;
    if (failed) fprintf(stderr, "Cleanup incomplete: %s\n", path);
    return failed;
}

int main(int argc, char **argv) {
    int fd_check = argc > 1 && !strcmp(argv[1], "--fd-check");
    int drain_profile = argc > 1 && !strcmp(argv[1], "--drain-profile");
    if (fd_check || drain_profile) { argc--; argv++; }
    if (argc < 2 || argc > 5 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: %s DIRECTORY [files_per_worker=500] [workers=1] [rounds=5]\n",
                "test_journal_speed");
        fprintf(stderr, "       test_journal_speed --fd-check DIRECTORY [iterations=5000] [workers=4] [rounds=1]\n");
        fprintf(stderr, "       test_journal_speed --drain-profile DIRECTORY [files_per_worker=500] [workers=1] [rounds=5]\n");
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }
    unsigned files = argc > 2 ? number(argv[2], 100000) : (fd_check ? 5000 : 500);
    unsigned workers = argc > 3 ? number(argv[3], 32) : (fd_check ? 4 : 1);
    unsigned rounds = argc > 4 ? number(argv[4], 100) : (fd_check ? 1 : 5);
    struct statfs fs;
    if (statfs(argv[1], &fs)) { perror("statfs parent"); return 1; }
    printf("# test_journal_speed v2; parent=%s fs_magic=0x%lx files=%u workers=%u rounds=%u\n",
           argv[1], (unsigned long)fs.f_type, files, workers, rounds);
    printf("# Filesystem workload timings; fsync success does not prove crash durability.\n");
    if (drain_profile)
        printf("# Drain profiling adds clock reads; compare speed with profiling disabled.\n");
    if ((unsigned long)fs.f_type != 0xef53)
        printf("# Parent is not reported as ext2/ext3/ext4; use a journaled mount for AvoryOS baselines.\n");
    printf("round,phase,workers,files_per_worker,operations,total_ms,ops_per_sec,p50_us,p95_us,p99_us,max_worker_drain_ms,dir_fsync_ms\n");
    uint64_t durations[PHASES][100] = {{0}};
    int first_phase = fd_check ? FD_CHURN : CREATE;
    int end_phase = fd_check ? PHASES : FD_CHURN;
    for (unsigned r = 0; r < rounds; r++) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/journal-bench-XXXXXX", argv[1]) >= (int)sizeof(path)) {
            fprintf(stderr, "Parent path too long\n"); return 2;
        }
        if (!mkdtemp(path)) { perror("mkdtemp"); return 1; }
        fprintf(stderr, "Round %u/%u: %s\n", r + 1, rounds, path);
        int dirfd = open(path, O_RDONLY | O_DIRECTORY);
        if (dirfd < 0) { perror("open test directory"); rmdir(path); return 1; }
        int failed = 0;
        if (fd_check) {
            char name[64];
            for (unsigned w = 0; w < workers; w++) {
                filename(name, sizeof(name), w, 0, 0);
                int fd = openat(dirfd, name, O_RDWR | O_CREAT | O_EXCL, 0600);
                if (fd < 0 || close(fd)) { perror("fd-check setup"); failed = 1; break; }
            }
        }
        for (int phase = first_phase; !failed && phase < end_phase; phase++) {
            if (run_phase(dirfd, phase, files, workers, r + 1, drain_profile, &durations[phase][r])) {
                fprintf(stderr, "FAIL %s: %s\n", names[phase], strerror(errno));
                failed = 1; break;
            }
            /* Verification is outside all timed regions. */
            if (phase >= WRITE && phase <= RENAME && verify(dirfd, files, workers, phase)) {
                fprintf(stderr, "FAIL verify after %s: %s\n", names[phase], strerror(errno));
                failed = 1; break;
            }
        }
        failed |= cleanup(dirfd, path, fd_check ? 1 : files, workers);
        if (failed) return 1;
    }
    for (int phase = first_phase; phase < end_phase; phase++) {
        qsort(durations[phase], rounds, sizeof(uint64_t), compare_u64);
        double median = (double)durations[phase][rounds / 2];
        if (!(rounds & 1)) median = (median + durations[phase][rounds / 2 - 1]) / 2;
        printf("# median,%s,total_ms=%.3f,ops_per_sec=%.2f\n",
               names[phase], median / 1e6, (double)files * workers * 1e9 / median);
    }
    printf("# PASS: all operations and content checks completed; private directories removed.\n");
    return 0;
}
