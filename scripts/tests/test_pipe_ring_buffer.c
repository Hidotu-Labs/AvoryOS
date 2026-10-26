#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <x86intrin.h>

#define PIPE_BUF_SIZE 65536
#define PIPE_BUF_MASK (PIPE_BUF_SIZE - 1)

/* Ring buffer pipe implementation */
typedef struct {
    uint8_t *buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} ring_pipe_t;

static void ring_pipe_init(ring_pipe_t *p) {
    p->buffer = malloc(PIPE_BUF_SIZE);
    p->head = 0;
    p->tail = 0;
    p->count = 0;
}

static void ring_pipe_free(ring_pipe_t *p) {
    free(p->buffer);
    p->buffer = NULL;
}

static uint32_t ring_pipe_write(ring_pipe_t *p, const uint8_t *src, uint32_t size) {
    uint32_t avail = PIPE_BUF_SIZE - p->count;
    uint32_t to_write = size < avail ? size : avail;
    if (to_write == 0) return 0;

    uint32_t first = PIPE_BUF_SIZE - p->head;
    if (first > to_write) first = to_write;
    memcpy(p->buffer + p->head, src, first);

    uint32_t second = to_write - first;
    if (second > 0) {
        memcpy(p->buffer, src + first, second);
    }

    p->head = (p->head + to_write) & PIPE_BUF_MASK;
    p->count += to_write;
    return to_write;
}

static uint32_t ring_pipe_read(ring_pipe_t *p, uint8_t *dst, uint32_t size) {
    uint32_t to_read = size < p->count ? size : p->count;
    if (to_read == 0) return 0;

    uint32_t first = PIPE_BUF_SIZE - p->tail;
    if (first > to_read) first = to_read;
    memcpy(dst, p->buffer + p->tail, first);

    uint32_t second = to_read - first;
    if (second > 0) {
        memcpy(dst + first, p->buffer, second);
    }

    p->tail = (p->tail + to_read) & PIPE_BUF_MASK;
    p->count -= to_read;
    if (p->count == 0) {
        p->head = 0;
        p->tail = 0;
    }
    return to_read;
}

static uint32_t ring_pipe_tee(ring_pipe_t *in, ring_pipe_t *out, uint32_t len) {
    uint32_t avail_in = in->count;
    uint32_t avail_out = PIPE_BUF_SIZE - out->count;
    uint32_t to_tee = len;
    if (to_tee > avail_in) to_tee = avail_in;
    if (to_tee > avail_out) to_tee = avail_out;
    if (to_tee == 0) return 0;

    uint32_t done = 0;
    while (done < to_tee) {
        uint32_t in_pos = (in->tail + done) & PIPE_BUF_MASK;
        uint32_t out_pos = (out->head + done) & PIPE_BUF_MASK;
        uint32_t in_contig = PIPE_BUF_SIZE - in_pos;
        uint32_t out_contig = PIPE_BUF_SIZE - out_pos;
        uint32_t rem = to_tee - done;
        uint32_t step = rem < in_contig ? rem : in_contig;
        if (step > out_contig) step = out_contig;
        memcpy(out->buffer + out_pos, in->buffer + in_pos, step);
        done += step;
    }

    out->head = (out->head + done) & PIPE_BUF_MASK;
    out->count += done;
    return done;
}

/* Old ramfs + memmove compaction implementation */
typedef struct {
    uint8_t *data;
    uint32_t length;
    uint32_t read_offset;
} old_pipe_t;

static void old_pipe_init(old_pipe_t *p) {
    p->data = malloc(PIPE_BUF_SIZE);
    p->length = 0;
    p->read_offset = 0;
}

static void old_pipe_free(old_pipe_t *p) {
    free(p->data);
    p->data = NULL;
}

static uint32_t old_pipe_write(old_pipe_t *p, const uint8_t *src, uint32_t size) {
    // Compact unread data to offset 0
    if (p->read_offset > 0) {
        uint32_t unread = p->length - p->read_offset;
        if (unread > 0 && p->data) {
            memmove(p->data, p->data + p->read_offset, unread);
        }
        p->length = unread;
        p->read_offset = 0;
    }

    uint32_t avail = PIPE_BUF_SIZE - p->length;
    uint32_t to_write = size < avail ? size : avail;
    if (to_write == 0) return 0;

    memcpy(p->data + p->length, src, to_write);
    p->length += to_write;
    return to_write;
}

static uint32_t old_pipe_read(old_pipe_t *p, uint8_t *dst, uint32_t size) {
    uint32_t unread = p->length - p->read_offset;
    uint32_t to_read = size < unread ? size : unread;
    if (to_read == 0) return 0;

    memcpy(dst, p->data + p->read_offset, to_read);
    p->read_offset += to_read;
    if (p->read_offset >= p->length) {
        p->read_offset = 0;
        p->length = 0;
    }
    return to_read;
}

/* Correctness Test */
static void run_correctness_tests(void) {
    printf("[*] Running Ring-Buffer Pipe Correctness Tests...\n");
    ring_pipe_t p;
    ring_pipe_init(&p);

    uint8_t *wbuf = malloc(65536);
    uint8_t *rbuf = malloc(65536);
    for (int i = 0; i < 65536; i++) {
        wbuf[i] = (uint8_t)(i * 37 + 13);
    }

    // 1. Alternating small writes & reads causing wrap around
    uint32_t total_transferred = 0;
    uint32_t stream_seed = 0;
    for (int iter = 0; iter < 50000; iter++) {
        uint32_t sz = 1 + (iter % 2048);
        // If pipe doesn't have room for sz, read first
        while (PIPE_BUF_SIZE - p.count < sz) {
            uint32_t read_bytes = ring_pipe_read(&p, rbuf, 4096);
            for (uint32_t i = 0; i < read_bytes; i++) {
                assert(rbuf[i] == (uint8_t)(total_transferred + i));
            }
            total_transferred += read_bytes;
        }

        for (uint32_t i = 0; i < sz; i++) {
            wbuf[i] = (uint8_t)(stream_seed + i);
        }
        uint32_t written = ring_pipe_write(&p, wbuf, sz);
        assert(written == sz);
        stream_seed += sz;

        uint32_t rsz = 1 + ((iter * 7) % 2048);
        if (rsz > p.count) rsz = p.count;
        if (rsz > 0) {
            uint32_t read_bytes = ring_pipe_read(&p, rbuf, rsz);
            assert(read_bytes == rsz);
            for (uint32_t i = 0; i < read_bytes; i++) {
                assert(rbuf[i] == (uint8_t)(total_transferred + i));
            }
            total_transferred += read_bytes;
        }
    }

    // Drain remainder
    while (p.count > 0) {
        uint32_t read_bytes = ring_pipe_read(&p, rbuf, 65536);
        for (uint32_t i = 0; i < read_bytes; i++) {
            assert(rbuf[i] == (uint8_t)(total_transferred + i));
        }
        total_transferred += read_bytes;
    }
    assert(total_transferred == stream_seed);
    assert(p.head == 0 && p.tail == 0 && p.count == 0);
    printf("    -> Wrapped across 64KB boundary over %u bytes correctly!\n", total_transferred);

    // 2. Test tee duplication
    ring_pipe_t p_out;
    ring_pipe_init(&p_out);
    // Offset head/tail to mid buffer so tee wraps around
    p.head = 60000;
    p.tail = 60000;
    p.count = 0;
    for (int i = 0; i < 10000; i++) {
        wbuf[i] = (uint8_t)i;
    }
    ring_pipe_write(&p, wbuf, 10000); // wraps 60000 -> 4464
    assert(p.count == 10000);

    p_out.head = 62000;
    p_out.tail = 62000;
    p_out.count = 0;
    uint32_t teed = ring_pipe_tee(&p, &p_out, 10000);
    assert(teed == 10000);
    assert(p.count == 10000); // non-destructive!
    assert(p_out.count == 10000);

    ring_pipe_read(&p_out, rbuf, 10000);
    for (int i = 0; i < 10000; i++) {
        assert(rbuf[i] == (uint8_t)i);
    }
    printf("    -> sys_tee() non-destructive circular copy across boundary: PASS\n");

    ring_pipe_free(&p);
    ring_pipe_free(&p_out);
    free(wbuf);
    free(rbuf);
}

/* Benchmark */
static void run_benchmarks(void) {
    printf("\n[*] Benchmarking Streaming Pipe IPC (64KB Ring vs Old Memmove Compaction)...\n");

    const int iterations = 100000;
    const uint32_t chunk_sizes[] = {512, 4096, 16384, 32768};
    const int num_sizes = sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);

    uint8_t *wbuf = malloc(65536);
    uint8_t *rbuf = malloc(65536);
    memset(wbuf, 0xAB, 65536);

    for (int s = 0; s < num_sizes; s++) {
        uint32_t chunk = chunk_sizes[s];
        printf("\n  [+] Chunk size: %u bytes (%d round-trips):\n", chunk, iterations);

        // Benchmark Old memmove compaction
        old_pipe_t old_p;
        old_pipe_init(&old_p);

        // Pre-fill partially to simulate streaming pipeline with unread data
        old_pipe_write(&old_p, wbuf, chunk);

        uint64_t t0 = __rdtsc();
        for (int i = 0; i < iterations; i++) {
            old_pipe_write(&old_p, wbuf, chunk);
            old_pipe_read(&old_p, rbuf, chunk);
        }
        uint64_t t1 = __rdtsc();
        uint64_t old_cycles = t1 - t0;
        old_pipe_free(&old_p);

        // Benchmark New Ring-Buffer
        ring_pipe_t ring_p;
        ring_pipe_init(&ring_p);

        // Pre-fill partially
        ring_pipe_write(&ring_p, wbuf, chunk);

        uint64_t t2 = __rdtsc();
        for (int i = 0; i < iterations; i++) {
            ring_pipe_write(&ring_p, wbuf, chunk);
            ring_pipe_read(&ring_p, rbuf, chunk);
        }
        uint64_t t3 = __rdtsc();
        uint64_t ring_cycles = t3 - t2;
        ring_pipe_free(&ring_p);

        double old_per_op = (double)old_cycles / (iterations * 2);
        double ring_per_op = (double)ring_cycles / (iterations * 2);
        double speedup = (double)old_cycles / (double)ring_cycles;

        printf("      Old Memmove Compaction: %8.2f cycles/op\n", old_per_op);
        printf("      New Circular Ring-Buf:  %8.2f cycles/op\n", ring_per_op);
        printf("      >>> SPEEDUP: %.2fX FASTER <<<\n", speedup);
    }

    free(wbuf);
    free(rbuf);
}

int main(void) {
    run_correctness_tests();
    run_benchmarks();
    return 0;
}
