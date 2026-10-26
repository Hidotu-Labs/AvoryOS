#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <x86intrin.h>

#define EXT4_EXT_MAGIC 0xF30A
#define EXT4_MAX_PHYS_BLOCK ((1ULL << 48) - 1)
#define EXT4_EXTENT_STACK_BUF_MAX 4096

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header_t;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
} ext2_inode_t;

typedef struct {
    uint32_t block_size;
    uint64_t blocks_count;
} ext2_mount_t;

static uint64_t g_alloc_count = 0;
static uint64_t g_free_count = 0;

static void *test_kmalloc(size_t sz) {
    g_alloc_count++;
    return malloc(sz);
}

static void test_kfree(void *ptr) {
    if (ptr) {
        g_free_count++;
        free(ptr);
    }
}

static uint8_t g_disk_blocks[16][4096];

static int mock_read_block(ext2_mount_t *mnt, uint64_t block_nr, void *buf) {
    (void)mnt;
    if (block_nr >= 16) return -1;
    memcpy(buf, g_disk_blocks[block_nr], 4096);
    return 0;
}

static inline uint64_t ext4_extent_get_pblock(ext4_extent_t *ex) {
    return ((uint64_t)ex->ee_start_hi << 32) | (uint64_t)ex->ee_start_lo;
}

/* Original baseline implementation with dynamic bufs[5] allocation per depth */
static uint64_t baseline_ext4_get_extent_run(ext2_mount_t *mnt, ext2_inode_t *inode,
                                             uint32_t logical_block, uint32_t *out_run_len) {
    if (out_run_len)
        *out_run_len = 1;
    uint8_t *bufs[5] = {NULL, NULL, NULL, NULL, NULL};
    int depth_idx = 0;
    uint64_t result = 0;
    uint32_t hole_limit = UINT32_MAX - logical_block;
    if (!hole_limit) hole_limit = 1;

    ext4_extent_header_t *hdr = (ext4_extent_header_t *)inode->i_block;
    void *entries = (uint8_t *)inode->i_block + sizeof(ext4_extent_header_t);

    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        goto done;

    while (1) {
        if (hdr->eh_depth == 0) {
            ext4_extent_t *exts = (ext4_extent_t *)entries;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                ext4_extent_t *ex = &exts[i];
                uint16_t len = ex->ee_len & 0x7FFF;
                if (logical_block < ex->ee_block) {
                    if (out_run_len)
                        *out_run_len = ex->ee_block - logical_block;
                    result = 0;
                    goto done;
                }
                if (logical_block >= ex->ee_block && logical_block < ex->ee_block + len) {
                    uint32_t offset_in_extent = logical_block - ex->ee_block;
                    if (out_run_len)
                        *out_run_len = len - offset_in_extent;
                    if (ex->ee_len > 0x8000) {
                        result = 0;
                        goto done;
                    }
                    uint64_t pblock = ext4_extent_get_pblock(ex);
                    if (!pblock || pblock > EXT4_MAX_PHYS_BLOCK ||
                        pblock >= mnt->blocks_count ||
                        offset_in_extent >= mnt->blocks_count - pblock) {
                        result = 0;
                        goto done;
                    }
                    result = pblock + offset_in_extent;
                    goto done;
                }
            }
            if (out_run_len)
                *out_run_len = hole_limit;
            goto done;
        } else {
            ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)entries;
            ext4_extent_idx_t *best = NULL;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                if (idxs[i].ei_block <= logical_block)
                    best = &idxs[i];
                else if (idxs[i].ei_block - logical_block < hole_limit)
                    hole_limit = idxs[i].ei_block - logical_block;
            }
            if (!best && hdr->eh_entries)
                best = &idxs[0];
            if (!best || depth_idx >= 4)
                goto done;

            uint64_t child_phys = ((uint64_t)best->ei_leaf_hi << 32) | best->ei_leaf_lo;
            uint8_t *buf = test_kmalloc(mnt->block_size);
            if (!buf)
                goto done;
            bufs[depth_idx++] = buf;
            if (mock_read_block(mnt, child_phys, buf) != 0)
                goto done;

            hdr = (ext4_extent_header_t *)buf;
            entries = buf + sizeof(ext4_extent_header_t);
            if (hdr->eh_magic != EXT4_EXT_MAGIC)
                goto done;
        }
    }

done:
    for (int i = 0; i < 5; i++)
        if (bufs[i]) test_kfree(bufs[i]);
    return result;
}

/* Optimized implementation using recycled stack scratch buffer */
static uint64_t optimized_ext4_get_extent_run(ext2_mount_t *mnt, ext2_inode_t *inode,
                                              uint32_t logical_block, uint32_t *out_run_len) {
    if (out_run_len)
        *out_run_len = 1;
    uint8_t stack_buf[EXT4_EXTENT_STACK_BUF_MAX] __attribute__((aligned(8)));
    uint8_t *dyn_buf = NULL;
    uint8_t *buf = NULL;
    int depth_idx = 0;
    uint64_t result = 0;
    uint32_t hole_limit = UINT32_MAX - logical_block;
    if (!hole_limit) hole_limit = 1;

    ext4_extent_header_t *hdr = (ext4_extent_header_t *)inode->i_block;
    void *entries = (uint8_t *)inode->i_block + sizeof(ext4_extent_header_t);

    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        goto done;

    while (1) {
        if (hdr->eh_depth == 0) {
            ext4_extent_t *exts = (ext4_extent_t *)entries;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                ext4_extent_t *ex = &exts[i];
                uint16_t len = ex->ee_len & 0x7FFF;
                if (logical_block < ex->ee_block) {
                    if (out_run_len)
                        *out_run_len = ex->ee_block - logical_block;
                    result = 0;
                    goto done;
                }
                if (logical_block >= ex->ee_block && logical_block < ex->ee_block + len) {
                    uint32_t offset_in_extent = logical_block - ex->ee_block;
                    if (out_run_len)
                        *out_run_len = len - offset_in_extent;
                    if (ex->ee_len > 0x8000) {
                        result = 0;
                        goto done;
                    }
                    uint64_t pblock = ext4_extent_get_pblock(ex);
                    if (!pblock || pblock > EXT4_MAX_PHYS_BLOCK ||
                        pblock >= mnt->blocks_count ||
                        offset_in_extent >= mnt->blocks_count - pblock) {
                        result = 0;
                        goto done;
                    }
                    result = pblock + offset_in_extent;
                    goto done;
                }
            }
            if (out_run_len)
                *out_run_len = hole_limit;
            goto done;
        } else {
            ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)entries;
            ext4_extent_idx_t *best = NULL;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                if (idxs[i].ei_block <= logical_block)
                    best = &idxs[i];
                else if (idxs[i].ei_block - logical_block < hole_limit)
                    hole_limit = idxs[i].ei_block - logical_block;
            }
            if (!best && hdr->eh_entries)
                best = &idxs[0];
            if (!best || depth_idx >= 4)
                goto done;

            uint64_t child_phys = ((uint64_t)best->ei_leaf_hi << 32) | best->ei_leaf_lo;
            if (!buf) {
                if (mnt->block_size <= sizeof(stack_buf)) {
                    buf = stack_buf;
                } else {
                    dyn_buf = test_kmalloc(mnt->block_size);
                    if (!dyn_buf)
                        goto done;
                    buf = dyn_buf;
                }
            }
            depth_idx++;
            if (mock_read_block(mnt, child_phys, buf) != 0)
                goto done;

            hdr = (ext4_extent_header_t *)buf;
            entries = buf + sizeof(ext4_extent_header_t);
            if (hdr->eh_magic != EXT4_EXT_MAGIC)
                goto done;
        }
    }

done:
    if (dyn_buf)
        test_kfree(dyn_buf);
    return result;
}

int main(void) {
    printf("=== Ext4 Extent Path Buffer Recycling Verification & Benchmark ===\n\n");

    ext2_mount_t mnt = {
        .block_size = 4096,
        .blocks_count = 1000000ULL,
    };

    memset(g_disk_blocks, 0, sizeof(g_disk_blocks));

    /* --- Set up Depth 0 Inode --- */
    ext2_inode_t inode_d0;
    memset(&inode_d0, 0, sizeof(inode_d0));
    ext4_extent_header_t *h0 = (ext4_extent_header_t *)inode_d0.i_block;
    h0->eh_magic = EXT4_EXT_MAGIC;
    h0->eh_entries = 2;
    h0->eh_max = 4;
    h0->eh_depth = 0;
    ext4_extent_t *ex0 = (ext4_extent_t *)(h0 + 1);
    ex0[0].ee_block = 0;
    ex0[0].ee_len = 10;
    ex0[0].ee_start_hi = 0;
    ex0[0].ee_start_lo = 100;
    ex0[1].ee_block = 20;
    ex0[1].ee_len = 5;
    ex0[1].ee_start_hi = 0;
    ex0[1].ee_start_lo = 200;

    /* --- Set up Depth 1 Inode & Leaf Block --- */
    /* Root index in inode -> leaf at physical block 1 */
    ext2_inode_t inode_d1;
    memset(&inode_d1, 0, sizeof(inode_d1));
    ext4_extent_header_t *h1 = (ext4_extent_header_t *)inode_d1.i_block;
    h1->eh_magic = EXT4_EXT_MAGIC;
    h1->eh_entries = 1;
    h1->eh_max = 4;
    h1->eh_depth = 1;
    ext4_extent_idx_t *idx1 = (ext4_extent_idx_t *)(h1 + 1);
    idx1[0].ei_block = 0;
    idx1[0].ei_leaf_lo = 1; /* leaf at block 1 */
    idx1[0].ei_leaf_hi = 0;

    /* Block 1 (leaf) */
    ext4_extent_header_t *leaf1 = (ext4_extent_header_t *)g_disk_blocks[1];
    leaf1->eh_magic = EXT4_EXT_MAGIC;
    leaf1->eh_entries = 2;
    leaf1->eh_max = 340;
    leaf1->eh_depth = 0;
    ext4_extent_t *lex1 = (ext4_extent_t *)(leaf1 + 1);
    lex1[0].ee_block = 0;
    lex1[0].ee_len = 50;
    lex1[0].ee_start_hi = 0;
    lex1[0].ee_start_lo = 1000;
    lex1[1].ee_block = 100;
    lex1[1].ee_len = 25;
    lex1[1].ee_start_hi = 0;
    lex1[1].ee_start_lo = 2000;

    /* --- Set up Depth 2 Inode & Intermediate Index & Leaf Block --- */
    /* Root index in inode -> intermediate index at block 2 -> leaf at block 3 */
    ext2_inode_t inode_d2;
    memset(&inode_d2, 0, sizeof(inode_d2));
    ext4_extent_header_t *h2 = (ext4_extent_header_t *)inode_d2.i_block;
    h2->eh_magic = EXT4_EXT_MAGIC;
    h2->eh_entries = 1;
    h2->eh_max = 4;
    h2->eh_depth = 2;
    ext4_extent_idx_t *idx2 = (ext4_extent_idx_t *)(h2 + 1);
    idx2[0].ei_block = 0;
    idx2[0].ei_leaf_lo = 2; /* intermediate index at block 2 */
    idx2[0].ei_leaf_hi = 0;

    /* Block 2 (intermediate index, depth 1) */
    ext4_extent_header_t *mid2 = (ext4_extent_header_t *)g_disk_blocks[2];
    mid2->eh_magic = EXT4_EXT_MAGIC;
    mid2->eh_entries = 1;
    mid2->eh_max = 340;
    mid2->eh_depth = 1;
    ext4_extent_idx_t *mid_idx = (ext4_extent_idx_t *)(mid2 + 1);
    mid_idx[0].ei_block = 0;
    mid_idx[0].ei_leaf_lo = 3; /* leaf at block 3 */
    mid_idx[0].ei_leaf_hi = 0;

    /* Block 3 (leaf, depth 0) */
    ext4_extent_header_t *leaf2 = (ext4_extent_header_t *)g_disk_blocks[3];
    leaf2->eh_magic = EXT4_EXT_MAGIC;
    leaf2->eh_entries = 2;
    leaf2->eh_max = 340;
    leaf2->eh_depth = 0;
    ext4_extent_t *lex2 = (ext4_extent_t *)(leaf2 + 1);
    lex2[0].ee_block = 0;
    lex2[0].ee_len = 100;
    lex2[0].ee_start_hi = 0;
    lex2[0].ee_start_lo = 5000;
    lex2[1].ee_block = 200;
    lex2[1].ee_len = 80;
    lex2[1].ee_start_hi = 0;
    lex2[1].ee_start_lo = 8000;

    /* Correctness validation across test cases */
    printf("[1] Correctness Validation (Depth 0, Depth 1, Depth 2):\n");
    struct {
        ext2_inode_t *inode;
        uint32_t logical;
        const char *desc;
    } tests[] = {
        {&inode_d0, 0, "Depth 0 (block 0)"},
        {&inode_d0, 5, "Depth 0 (block 5 inside extent 0)"},
        {&inode_d0, 15, "Depth 0 (block 15 in hole)"},
        {&inode_d0, 22, "Depth 0 (block 22 inside extent 1)"},
        {&inode_d1, 0, "Depth 1 (block 0)"},
        {&inode_d1, 25, "Depth 1 (block 25 inside extent 0)"},
        {&inode_d1, 75, "Depth 1 (block 75 in hole)"},
        {&inode_d1, 110, "Depth 1 (block 110 inside extent 1)"},
        {&inode_d2, 0, "Depth 2 (block 0)"},
        {&inode_d2, 50, "Depth 2 (block 50 inside extent 0)"},
        {&inode_d2, 150, "Depth 2 (block 150 in hole)"},
        {&inode_d2, 210, "Depth 2 (block 210 inside extent 1)"},
    };

    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        uint32_t base_run = 0, opt_run = 0;
        uint64_t base_p = baseline_ext4_get_extent_run(&mnt, tests[i].inode, tests[i].logical, &base_run);
        uint64_t opt_p  = optimized_ext4_get_extent_run(&mnt, tests[i].inode, tests[i].logical, &opt_run);
        if (base_p != opt_p || base_run != opt_run) {
            printf("  [FAIL] %s: base=(pblock=%lu, run=%u), opt=(pblock=%lu, run=%u)\n",
                   tests[i].desc, base_p, base_run, opt_p, opt_run);
            return 1;
        }
        printf("  [PASS] %-40s -> pblock=%lu, run_len=%u\n", tests[i].desc, opt_p, opt_run);
    }

    /* Heap allocation tracking validation */
    printf("\n[2] Heap Allocation & Churn Verification:\n");
    g_alloc_count = 0;
    g_free_count = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t run;
        baseline_ext4_get_extent_run(&mnt, &inode_d1, 10, &run);
    }
    printf("  Baseline (Depth 1, 100 lookups):  %lu kmallocs, %lu kfrees\n", g_alloc_count, g_free_count);

    g_alloc_count = 0;
    g_free_count = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t run;
        baseline_ext4_get_extent_run(&mnt, &inode_d2, 10, &run);
    }
    printf("  Baseline (Depth 2, 100 lookups):  %lu kmallocs, %lu kfrees\n", g_alloc_count, g_free_count);

    g_alloc_count = 0;
    g_free_count = 0;
    for (int i = 0; i < 100; i++) {
        uint32_t run;
        optimized_ext4_get_extent_run(&mnt, &inode_d1, 10, &run);
        optimized_ext4_get_extent_run(&mnt, &inode_d2, 10, &run);
    }
    printf("  Optimized (Depth 1+2, 200 lookups): %lu kmallocs, %lu kfrees\n", g_alloc_count, g_free_count);
    if (g_alloc_count != 0 || g_free_count != 0) {
        printf("  [FAIL] Optimized implementation incurred heap allocations!\n");
        return 1;
    }
    printf("  -> VERIFIED ZERO HEAP ALLOCATIONS (100%% heap churn eliminated)!\n\n");

    /* Performance Benchmarking */
    const int ITERS = 1000000;
    printf("[3] Performance Microbenchmark (%d lookups):\n", ITERS);

    /* Benchmark Depth 1 */
    uint64_t start, end;
    volatile uint64_t dummy = 0;

    start = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        uint32_t run;
        dummy += baseline_ext4_get_extent_run(&mnt, &inode_d1, 10 + (i & 0x1F), &run);
    }
    end = __rdtsc();
    uint64_t base_d1_cycles = end - start;

    start = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        uint32_t run;
        dummy += optimized_ext4_get_extent_run(&mnt, &inode_d1, 10 + (i & 0x1F), &run);
    }
    end = __rdtsc();
    uint64_t opt_d1_cycles = end - start;

    printf("  -- Depth 1 Extent Tree Lookup --\n");
    printf("  Baseline (kmalloc/kfree per descent) : %lu cycles total (%lu cycles/lookup)\n",
           base_d1_cycles, base_d1_cycles / ITERS);
    printf("  Optimized (Recycled Stack Buffer)    : %lu cycles total (%lu cycles/lookup)\n",
           opt_d1_cycles, opt_d1_cycles / ITERS);
    printf("  -> Speedup: %.2fX FASTER\n\n", (double)base_d1_cycles / (double)opt_d1_cycles);

    /* Benchmark Depth 2 */
    start = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        uint32_t run;
        dummy += baseline_ext4_get_extent_run(&mnt, &inode_d2, 20 + (i & 0x1F), &run);
    }
    end = __rdtsc();
    uint64_t base_d2_cycles = end - start;

    start = __rdtsc();
    for (int i = 0; i < ITERS; i++) {
        uint32_t run;
        dummy += optimized_ext4_get_extent_run(&mnt, &inode_d2, 20 + (i & 0x1F), &run);
    }
    end = __rdtsc();
    uint64_t opt_d2_cycles = end - start;

    printf("  -- Depth 2 Extent Tree Lookup --\n");
    printf("  Baseline (2x kmalloc/kfree per descent): %lu cycles total (%lu cycles/lookup)\n",
           base_d2_cycles, base_d2_cycles / ITERS);
    printf("  Optimized (Recycled Stack Buffer)      : %lu cycles total (%lu cycles/lookup)\n",
           opt_d2_cycles, opt_d2_cycles / ITERS);
    printf("  -> Speedup: %.2fX FASTER\n\n", (double)base_d2_cycles / (double)opt_d2_cycles);

    (void)dummy;
    return 0;
}
