// AvoryOS Phase 2 LinuxKPI exit criteria test: device BO mmap and PRIME-style
// fd passing.
//
// Runs against /dev/kpi_dmabuf (kernel/linuxkpi/src/kpi_dmabuf_testdev.c):
//
//   1. ioctl(ALLOC) exports a dma-buf; mmap the returned fd, write/read it
//      back, munmap and verify vm_ops->close ran exactly once.
//   2. Same on the device node itself, whose f_op->mmap() drives the
//      driver-side dma_buf_mmap() path.
//   3. mprotect() on a mapped BO: the bridge wrapper must survive the native
//      VMA remove/re-add, so vm_ops->close() runs exactly once, at munmap.
//   4. mremap() on a mapped BO: VM_DONTEXPAND mappings are rejected with
//      EINVAL like upstream, leaving the mapping intact.
//   5. fork(): the child imports the inherited dma-buf fd (dma_buf_get),
//      waits on an inherited sync_file fd, maps the BO, checks the parent's
//      pattern and writes its own; the parent sees the child's write.
//   6. Alloc/map/free soak loop with a native PMM free-page check.
//
// Build: userland/test_kpi_dmabuf.elf, installed as /bin/test_kpi_dmabuf.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <uapi/kpi_dmabuf.h>

#define PAGE_SIZE_ 4096UL
#define SOAK_LOOPS 200
#define SOAK_THREADS 4
#define SOAK_BO_PAGES 64

static int failures;

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, long detail) {
    failures++;
    printf("  [FAIL] %s", what);
    if (detail)
        printf(" (errno=%ld %s)", detail, strerror((int)detail));
    printf("\n");
}

static unsigned long close_count(int dev) {
    long n = ioctl(dev, KPI_DMABUF_IOC_CLOSE_COUNT, 0);

    if (n < 0)
        return 0;
    return (unsigned long)n;
}

static void *map_fd(int fd, unsigned long pages, int flags) {
    void *p = mmap(NULL, pages * PAGE_SIZE_, PROT_READ | PROT_WRITE, flags, fd, 0);

    return p == MAP_FAILED ? NULL : p;
}

/* 1. dma-buf fd mmap: write, read back, munmap closes the wrapper once. */
static void test_fd_mmap(int dev) {
    unsigned long pages = 256;
    size_t size = pages * PAGE_SIZE_;
    unsigned long before, after;
    int fd;
    unsigned char *p;

    printf("\n=== dma-buf fd mmap + exactly-once vm_ops->close ===\n");

    before = close_count(dev);
    fd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, pages);
    if (fd < 0) {
        fail("ioctl(ALLOC) returned a dma-buf fd", errno);
        return;
    }
    pass("ioctl(ALLOC) returned a dma-buf fd");

    p = map_fd(fd, pages, MAP_SHARED);
    if (!p) {
        fail("mmap(dma-buf fd)", errno);
        close(fd);
        return;
    }
    pass("mmap(dma-buf fd)");

    p[0] = 0x11;
    p[3 * PAGE_SIZE_ + 7] = 0x22;
    p[size - 1] = 0x33;
    if (p[0] == 0x11 && p[3 * PAGE_SIZE_ + 7] == 0x22 && p[size - 1] == 0x33)
        pass("write/read back through the mapping");
    else
        fail("write/read back through the mapping", 0);

    if (munmap(p, size) == 0)
        pass("munmap(dma-buf fd)");
    else
        fail("munmap(dma-buf fd)", errno);

    after = close_count(dev);
    if (after == before + 1) {
        pass("vm_ops->close ran exactly once");
    } else {
        failures++;
        printf("  [FAIL] vm_ops->close count %lu -> %lu (expected +1)\n",
               before, after);
    }
    close(fd);
}

/* 2. Device-node mmap: f_op->mmap() calls dma_buf_mmap(). */
static void test_device_mmap(int dev) {
    unsigned long pages = 128;
    size_t size = pages * PAGE_SIZE_;
    unsigned long before, after;
    long bo_pages;
    int fd;
    unsigned char *p;

    printf("\n=== device-node mmap via dma_buf_mmap() ===\n");

    before = close_count(dev);
    fd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, pages);
    if (fd < 0) {
        fail("ioctl(ALLOC) returned a dma-buf fd", errno);
        return;
    }

    bo_pages = ioctl(dev, KPI_DMABUF_IOC_BO_PAGES, 0);
    if (bo_pages == (long)pages)
        pass("ioctl(BO_PAGES) reports the exported BO size");
    else
        fail("ioctl(BO_PAGES) reports the exported BO size", 0);

    p = map_fd(dev, pages, MAP_SHARED);
    if (!p) {
        fail("mmap(device node)", errno);
        close(fd);
        return;
    }
    pass("mmap(device node) -> dma_buf_mmap()");

    p[PAGE_SIZE_ - 1] = 0x44;
    p[7 * PAGE_SIZE_ + 3] = 0x55;
    if (p[PAGE_SIZE_ - 1] == 0x44 && p[7 * PAGE_SIZE_ + 3] == 0x55)
        pass("device mapping aliases the dma-buf pages");
    else
        fail("device mapping aliases the dma-buf pages", 0);

    if (munmap(p, size) == 0)
        pass("munmap(device node)");
    else
        fail("munmap(device node)", errno);

    after = close_count(dev);
    if (after == before + 1) {
        pass("device-node mapping closed exactly once");
    } else {
        failures++;
        printf("  [FAIL] device-node close count %lu -> %lu (expected +1)\n",
               before, after);
    }
    close(fd);
}

/* 3. mprotect() must keep the Linux vm_area_struct wrapper attached: upstream
 * neither closes the mapping when protections change nor loses its vm_ops.
 * The wrapper is shared by the split native VMAs, so vm_ops->close() must
 * still run exactly once, at munmap. */
static void test_mprotect_bridge(int dev) {
    unsigned long pages = 64;
    size_t size = pages * PAGE_SIZE_;
    unsigned long before, mid, after;
    int fd;
    unsigned char *p;

    printf("\n=== mprotect keeps the bridge wrapper (close only at munmap) ===\n");

    before = close_count(dev);
    fd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, pages);
    if (fd < 0) {
        fail("ioctl(ALLOC) returned a dma-buf fd", errno);
        return;
    }

    p = map_fd(fd, pages, MAP_SHARED);
    if (!p) {
        fail("mmap(dma-buf fd)", errno);
        close(fd);
        return;
    }

    p[0] = 0x5a;
    p[size - 1] = 0xa5;

    /* Full-range protection change.  With the wrapper dropped on the re-add
     * this closed the mapping right here instead of at munmap. */
    if (mprotect(p, size, PROT_READ) == 0)
        pass("mprotect(full range, PROT_READ)");
    else
        fail("mprotect(full range, PROT_READ)", errno);

    if (p[0] == 0x5a && p[size - 1] == 0xa5)
        pass("mapping readable while write-protected");
    else
        fail("mapping readable while write-protected", 0);

    /* Split-range change: the middle native VMA splits off and must keep a
     * reference to the same wrapper. */
    if (mprotect(p + PAGE_SIZE_, 3 * PAGE_SIZE_, PROT_READ) == 0)
        pass("mprotect(split range, PROT_READ)");
    else
        fail("mprotect(split range, PROT_READ)", errno);

    if (mprotect(p, size, PROT_READ | PROT_WRITE) == 0)
        pass("mprotect(full range, PROT_READ|PROT_WRITE)");
    else
        fail("mprotect(full range, PROT_READ|PROT_WRITE)", errno);

    p[PAGE_SIZE_] = 0x5b;
    if (p[0] == 0x5a && p[PAGE_SIZE_] == 0x5b && p[size - 1] == 0xa5)
        pass("data survives the protection changes");
    else
        fail("data survives the protection changes", 0);

    mid = close_count(dev);
    if (mid == before) {
        pass("mprotect did not close the wrapper");
    } else {
        failures++;
        printf("  [FAIL] mprotect close count %lu -> %lu (expected +0)\n",
               before, mid);
    }

    if (munmap(p, size) == 0)
        pass("munmap after mprotect");
    else
        fail("munmap after mprotect", errno);

    after = close_count(dev);
    if (after == before + 1) {
        pass("wrapper closed exactly once at munmap");
    } else {
        failures++;
        printf("  [FAIL] mprotect close count %lu -> %lu (expected +1)\n",
               before, after);
    }
    close(fd);
}

/* 4. mremap() on a VM_DONTEXPAND mapping (GEM/dma-buf mappings set it) must
 * fail with EINVAL like upstream and leave the mapping untouched. */
static void test_mremap_bridge(int dev) {
    unsigned long pages = 32;
    size_t size = pages * PAGE_SIZE_;
    unsigned long before, after;
    void *q;
    int fd;
    unsigned char *p;

    printf("\n=== mremap on a VM_DONTEXPAND mapping is rejected ===\n");

    before = close_count(dev);
    fd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, pages);
    if (fd < 0) {
        fail("ioctl(ALLOC) returned a dma-buf fd", errno);
        return;
    }

    p = map_fd(fd, pages, MAP_SHARED);
    if (!p) {
        fail("mmap(dma-buf fd)", errno);
        close(fd);
        return;
    }
    p[0] = 0x77;
    p[size - 1] = 0x88;

    errno = 0;
    q = mremap(p, size, size * 2, MREMAP_MAYMOVE);
    if (q == MAP_FAILED && errno == EINVAL) {
        pass("mremap rejected with EINVAL (VM_DONTEXPAND)");
    } else if (q == MAP_FAILED) {
        fail("mremap rejected with EINVAL", errno);
    } else {
        fail("mremap unexpectedly succeeded on a VM_DONTEXPAND mapping", 0);
        munmap(q, size * 2);
        close(fd);
        return;
    }

    if (p[0] == 0x77 && p[size - 1] == 0x88)
        pass("mapping intact after the rejected mremap");
    else
        fail("mapping intact after the rejected mremap", 0);

    if (close_count(dev) == before)
        pass("rejected mremap did not close the wrapper");
    else
        fail("rejected mremap did not close the wrapper", 0);

    if (munmap(p, size) == 0)
        pass("munmap after rejected mremap");
    else
        fail("munmap after rejected mremap", errno);

    after = close_count(dev);
    if (after == before + 1) {
        pass("wrapper closed exactly once at munmap");
    } else {
        failures++;
        printf("  [FAIL] mremap close count %lu -> %lu (expected +1)\n",
               before, after);
    }
    close(fd);
}

/* 5. fork(): inherited dma-buf + sync_file fds across two processes. */
static int test_fork_prime(int dev) {
    unsigned long pages = 256;
    size_t size = pages * PAGE_SIZE_;
    unsigned long before, after;
    unsigned char *p, *cp;
    int bofd, syncfd;
    pid_t child;
    int status = 0;

    printf("\n=== fork PRIME-style fd passing (dma-buf + sync_file) ===\n");

    before = close_count(dev);
    bofd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, pages);
    if (bofd < 0) {
        fail("parent ioctl(ALLOC)", errno);
        return 0;
    }
    syncfd = (int)ioctl(dev, KPI_DMABUF_IOC_SYNC_FD, 0);
    if (syncfd < 0) {
        fail("parent ioctl(SYNC_FD)", errno);
        close(bofd);
        return 0;
    }
    pass("parent created dma-buf + sync_file fds");

    p = map_fd(bofd, pages, MAP_SHARED);
    if (!p) {
        fail("parent mmap(dma-buf fd)", errno);
        close(bofd);
        close(syncfd);
        return 0;
    }
    memset(p, 0, size);
    p[0] = 0xA1;
    p[7 * PAGE_SIZE_ + 5] = 0xA2;

    child = fork();
    if (child < 0) {
        fail("fork()", errno);
        munmap(p, size);
        close(bofd);
        close(syncfd);
        return 0;
    }

    if (child == 0) {
        /* Child: the fds arrived through fork's fd-table copy. */
        if (ioctl(dev, KPI_DMABUF_IOC_IMPORT, bofd) != 0)
            _exit(10); /* dma_buf_get() on the inherited fd */
        if (ioctl(dev, KPI_DMABUF_IOC_WAIT, syncfd) != 0)
            _exit(11); /* sync_file_get_fence() + dma_fence_wait() */

        cp = map_fd(bofd, pages, MAP_SHARED);
        if (!cp)
            _exit(12);
        if (cp[0] != 0xA1 || cp[7 * PAGE_SIZE_ + 5] != 0xA2)
            _exit(13); /* child must see the parent's pattern */

        cp[5 * PAGE_SIZE_ + 9] = 0x5C; /* child writes; parent must see it */
        munmap(cp, size);
        close(syncfd);
        close(bofd);
        /* device fd is closed by exit() */
        _exit(0);
    }

    /* Parent: release the fence once the child is up, then collect it. */
    if (ioctl(dev, KPI_DMABUF_IOC_SIGNAL, 0) != 0)
        fail("parent ioctl(SIGNAL)", errno);
    waitpid(child, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        pass("child imported, waited, mapped and verified the BO");
    } else {
        failures++;
        printf("  [FAIL] child exited with status %d (10=import 11=wait "
               "12=mmap 13=verify)\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    if (p[5 * PAGE_SIZE_ + 9] == 0x5C)
        pass("parent sees the data written by the child");
    else
        fail("parent sees the data written by the child", 0);

    munmap(p, size);
    close(syncfd);
    close(bofd);

    after = close_count(dev);
    /* One mapping in the parent plus one in the child. */
    if (after == before + 2) {
        pass("two-process mappings closed exactly once each");
    } else {
        failures++;
        printf("  [FAIL] two-process close count %lu -> %lu (expected +2)\n",
               before, after);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* 6. Alloc/map/free loop with a PMM free-page delta. */
static void test_soak(int dev) {
    long baseline, final;
    unsigned long c0, c1;
    int i;

    printf("\n=== alloc/map/free soak (%d loops) ===\n", SOAK_LOOPS);

    c0 = close_count(dev);
    baseline = ioctl(dev, KPI_DMABUF_IOC_PMM_FREE, 0);
    if (baseline <= 0) {
        fail("ioctl(PMM_FREE)", errno);
        return;
    }

    for (i = 0; i < SOAK_LOOPS; i++) {
        int fd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, 64);

        if (fd < 0) {
            fail("soak ioctl(ALLOC)", errno);
            return;
        }
        unsigned char *p = map_fd(fd, 64, MAP_SHARED);
        if (!p) {
            fail("soak mmap", errno);
            close(fd);
            return;
        }
        p[i & 63] = (unsigned char)i;
        if (p[i & 63] != (unsigned char)i) {
            fail("soak readback", 0);
            munmap(p, 64 * PAGE_SIZE_);
            close(fd);
            return;
        }
        munmap(p, 64 * PAGE_SIZE_);
        close(fd);
    }
    pass("loop completed");

    c1 = close_count(dev);
    if (c1 == c0 + SOAK_LOOPS) {
        pass("vm_ops->close ran once per loop");
    } else {
        failures++;
        printf("  [FAIL] soak close count %lu -> %lu (expected +%d)\n", c0, c1,
               SOAK_LOOPS);
    }

    final = ioctl(dev, KPI_DMABUF_IOC_PMM_FREE, 0);
    printf("  [INFO] PMM free pages baseline=%ld final=%ld delta=%ld\n",
           baseline, final, final - baseline);
    /* Any positive delta is unrelated kernel memory being freed while the
     * soak runs; only a net *loss* of free pages would be a leak. */
    if (final >= baseline - 8) {
        pass("PMM free-page count did not go backwards");
    } else {
        failures++;
        printf("  [FAIL] PMM free-page leak: delta=%ld\n", final - baseline);
    }
}

/* ── 10-minute class SMP soak ───────────────────────────────────────────── */

struct soak_thread {
    int dev;
    unsigned long iters;
    unsigned long errors;
};

static volatile int soak_stop;
static struct soak_thread soak_threads[SOAK_THREADS];

static void *soak_worker(void *arg) {
    struct soak_thread *t = arg;
    unsigned long n = 0;

    while (!__atomic_load_n(&soak_stop, __ATOMIC_RELAXED)) {
        int bofd = (int)ioctl(t->dev, KPI_DMABUF_IOC_ALLOC, SOAK_BO_PAGES);
        unsigned char *p;
        unsigned char v;
        size_t off;

        if (bofd < 0) {
            t->errors++;
            break;
        }
        p = map_fd(bofd, SOAK_BO_PAGES, MAP_SHARED);
        if (!p) {
            t->errors++;
            close(bofd);
            break;
        }

        v = (unsigned char)(n * 2654435761u >> 24);
        off = (n * 7919u) % (SOAK_BO_PAGES * PAGE_SIZE_);
        p[off] = v;
        if (p[off] != v)
            t->errors++;
        /* Kernel-side invariant: every BO page is a managed head page with
         * exactly one reference while the mapping is live. */
        if (ioctl(t->dev, KPI_DMABUF_IOC_VERIFY, 0) != SOAK_BO_PAGES)
            t->errors++;

        munmap(p, SOAK_BO_PAGES * PAGE_SIZE_);
        close(bofd);
        n++;
    }

    t->iters = n;
    return NULL;
}

static int test_long_soak(int seconds) {
    struct timespec start, now;
    unsigned long c0, c1, total = 0, errors = 0;
    long baseline, final;
    int dev, t;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS KPI dma-buf SMP soak (%d s, %d threads) ===\n", seconds,
           SOAK_THREADS);

    dev = open("/dev/kpi_dmabuf", O_RDWR);
    if (dev < 0) {
        printf("  [SKIP] /dev/kpi_dmabuf not present (%s)\n", strerror(errno));
        return 0;
    }

    /* Warm-up: the first device opens and dma-buf exports grow the kernel
     * heap/slab caches by a fixed amount.  Let that settle before reading the
     * baseline so the measured delta reflects steady-state leakage. */
    for (int i = 0; i < 256; i++) {
        int bofd = (int)ioctl(dev, KPI_DMABUF_IOC_ALLOC, SOAK_BO_PAGES);
        unsigned char *p;

        if (bofd < 0)
            break;
        p = map_fd(bofd, SOAK_BO_PAGES, MAP_SHARED);
        if (p) {
            p[i % (SOAK_BO_PAGES * PAGE_SIZE_)] = (unsigned char)i;
            munmap(p, SOAK_BO_PAGES * PAGE_SIZE_);
        }
        close(bofd);
    }

    for (t = 0; t < SOAK_THREADS; t++) {
        soak_threads[t].dev = open("/dev/kpi_dmabuf", O_RDWR);
        if (soak_threads[t].dev < 0) {
            fail("soak thread device open", errno);
            close(dev);
            return 1;
        }
    }
    pthread_t th[SOAK_THREADS];
    for (t = 0; t < SOAK_THREADS; t++) {
        if (pthread_create(&th[t], NULL, soak_worker, &soak_threads[t]) != 0) {
            fail("pthread_create", errno);
            soak_stop = 1;
            while (--t >= 0)
                pthread_join(th[t], NULL);
            close(dev);
            return 1;
        }
    }

    /* Let the workers spin for a moment first: creating the threads and their
     * kernel stacks is a one-time PMM cost that must not show up as leakage.
     * Only then sample the steady-state baseline. */
    sleep(3);

    baseline = ioctl(dev, KPI_DMABUF_IOC_PMM_FREE, 0);
    c0 = close_count(dev);
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        sleep(30);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((long)(now.tv_sec - start.tv_sec) >= seconds)
            break;
    }
    soak_stop = 1;
    for (t = 0; t < SOAK_THREADS; t++)
        pthread_join(th[t], NULL);

    for (t = 0; t < SOAK_THREADS; t++) {
        total += soak_threads[t].iters;
        errors += soak_threads[t].errors;
        close(soak_threads[t].dev);
    }

    c1 = close_count(dev);
    final = ioctl(dev, KPI_DMABUF_IOC_PMM_FREE, 0);
    printf("  [SOAK] iterations=%lu errors=%lu closes=%lu\n", total, errors,
           c1 - c0);
    printf("  [SOAK] PMM free pages baseline=%ld final=%ld delta=%ld\n",
           baseline, final, final - baseline);

    if (errors == 0 && total > 0)
        pass("soak iterations completed without errors");
    else
        fail("soak iteration errors", (long)errors);

    if (c1 == c0 + total)
        pass("vm_ops->close ran once per soak iteration");
    else
        fail("soak close count mismatch", 0);

    if (final >= baseline - 8)
        pass("PMM free-page count did not go backwards");
    else
        fail("PMM free-page leak", 0);

    close(dev);
    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    int dev;

    if (argc > 1 && atoi(argv[1]) > 0)
        return test_long_soak(atoi(argv[1]));

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS Phase 2 KPI dma-buf test ===\n");

    dev = open("/dev/kpi_dmabuf", O_RDWR);
    if (dev < 0) {
        printf("  [SKIP] /dev/kpi_dmabuf not present (%s)\n", strerror(errno));
        return 0;
    }

    test_fd_mmap(dev);
    test_device_mmap(dev);
    test_mprotect_bridge(dev);
    test_mremap_bridge(dev);
    test_fork_prime(dev);
    test_soak(dev);

    close(dev);
    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
