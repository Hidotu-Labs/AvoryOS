#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* Concurrent page faults, permission changes, discards and unmaps exercise
 * remote shootdowns in the same address space, as Firefox's workers do. */
#define WORKERS 8
#define ROUNDS 300
static void *worker(void *arg) {
    unsigned char stamp = (uintptr_t)arg + 1;
    for (int i = 0; i < ROUNDS; i++) {
        size_t size = (1 + i % 16) * 4096;
        volatile unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return (void *)1;
        for (size_t j = 0; j < size; j += 4096) p[j] = stamp;
        if (mprotect((void *)p, size, PROT_READ)) return (void *)1;
        for (size_t j = 0; j < size; j += 4096)
            if (p[j] != stamp) return (void *)1;
        if (mprotect((void *)p, size, PROT_READ | PROT_WRITE) ||
            madvise((void *)p, size, MADV_DONTNEED)) return (void *)1;
        for (size_t j = 0; j < size; j += 4096) {
            if (p[j] != 0) return (void *)1;
            p[j] = stamp;
        }
        if (munmap((void *)p, size)) return (void *)1;
    }
    return NULL;
}
static void report(const char *s) {
    puts(s);
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) { dprintf(fd, "%s\n", s); close(fd); }
}
int main(void) {
    pthread_t threads[WORKERS];
    report("TLBSTRESS: START");
    for (uintptr_t i = 0; i < WORKERS; i++)
        if (pthread_create(&threads[i], NULL, worker, (void *)i)) return 1;
    int failed = 0;
    for (int i = 0; i < WORKERS; i++) {
        void *result;
        if (pthread_join(threads[i], &result) || result) failed = 1;
    }
    report(failed ? "TLBSTRESS: FAIL" : "TLBSTRESS: PASS (8 workers, 2400 mapping cycles)");
    return failed;
}
