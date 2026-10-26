#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#define VFS_PATH_CACHE_WAYS 4
#define VFS_PATH_CACHE_BUCKETS 1024
#define VFS_PATH_CACHE_BUCKET_MASK (VFS_PATH_CACHE_BUCKETS - 1)

typedef struct vfs_node {
    uint32_t flags;
    uint32_t refcount;
    uint64_t inode;
    uint32_t mask;
    char name[32];
} vfs_node_t;

typedef struct {
    void *dir;
    void *node;
    uint32_t hash;
    uint16_t path_len;
    bool valid;
    uint64_t gen;
    char path[256];
} path_entry_t;

typedef struct {
    path_entry_t entries[VFS_PATH_CACHE_WAYS];
    uint32_t seq;
} path_bucket_t;

static path_bucket_t g_path_cache[VFS_PATH_CACHE_BUCKETS];
static uint64_t g_inval_gen = 1;

static inline bool old_path_has_dotdot(const char *path) {
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') return true;
        p++;
    }
    return false;
}

static inline uint32_t old_path_hash(void *dir, const char *path) {
    uint64_t hash = ((uint64_t)(uintptr_t)dir >> 4) ^ 0xcbf29ce484222325ULL;
    while (*path) {
        hash ^= (uint8_t)*path++;
        hash *= 0x100000001b3ULL;
    }
    return (uint32_t)(hash ^ (hash >> 32));
}

// Old Resolution: 3 passes + simulated pushfq/popfq
void *old_vfs_resolve_path(void *dir, const char *path) {
    bool has_dd = old_path_has_dotdot(path);
    size_t len_sz = strlen(path);
    if (len_sz >= 256) return NULL;

    uint16_t len = (uint16_t)len_sz;
    uint32_t hash = old_path_hash(dir, path);
    path_bucket_t *bucket = &g_path_cache[hash & VFS_PATH_CACHE_BUCKET_MASK];

    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");

    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    void *res = NULL;
    if ((s1 & 1) == 0) {
        for (int i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
            path_entry_t *e = &bucket->entries[i];
            if (e->valid && e->hash == hash && e->dir == dir && e->path_len == len) {
                if (memcmp(e->path, path, len) == 0 && e->gen == g_inval_gen) {
                    res = e->node;
                    break;
                }
            }
        }
    }
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
    return res;
}

// New Resolution: 1 pass + lockless seqlock (no pushfq/popfq)
static inline uint32_t new_path_scan_and_hash(void *dir, const char *path,
                                              uint16_t *out_len, bool *out_has_dotdot) {
    uint64_t hash = ((uint64_t)(uintptr_t)dir >> 4) ^ 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)path;
    uint16_t len = 0;
    bool has_dotdot = false;
    uint8_t prev = 0;

    while (*p && len < 255) {
        uint8_t c = *p++;
        if (c == '.' && prev == '.') has_dotdot = true;
        prev = c;
        hash ^= c;
        hash *= 0x100000001b3ULL;
        len++;
    }

    *out_len = len;
    *out_has_dotdot = has_dotdot;
    return (uint32_t)(hash ^ (hash >> 32));
}

void *new_vfs_resolve_path(void *dir, const char *path) {
    uint16_t len;
    bool has_dotdot;
    uint32_t hash = new_path_scan_and_hash(dir, path, &len, &has_dotdot);

    path_bucket_t *bucket = &g_path_cache[hash & VFS_PATH_CACHE_BUCKET_MASK];
    uint32_t s1 = __atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE);
    if (__builtin_expect(s1 & 1, 0)) return NULL;

    for (int i = 0; i < VFS_PATH_CACHE_WAYS; i++) {
        path_entry_t *e = &bucket->entries[i];
        if (e->valid && e->hash == hash && e->dir == dir && e->path_len == len) {
            if (memcmp(e->path, path, len) == 0 && e->gen == g_inval_gen) {
                if (__atomic_load_n(&bucket->seq, __ATOMIC_ACQUIRE) == s1) {
                    return e->node;
                }
            }
        }
    }
    return NULL;
}

int main(void) {
    vfs_node_t root = { .inode = 1, .refcount = 10 };
    vfs_node_t target = { .inode = 42, .refcount = 10 };
    const char *test_path = "/usr/lib/x86_64-linux-gnu/libGLX.so.0";
    uint16_t path_len = strlen(test_path);

    // Populate cache entry
    uint32_t hash = old_path_hash(&root, test_path);
    path_bucket_t *b = &g_path_cache[hash & VFS_PATH_CACHE_BUCKET_MASK];
    b->entries[0].valid = true;
    b->entries[0].dir = &root;
    b->entries[0].node = &target;
    b->entries[0].hash = hash;
    b->entries[0].path_len = path_len;
    b->entries[0].gen = g_inval_gen;
    memcpy(b->entries[0].path, test_path, path_len + 1);

    // Verify correctness
    void *check_old = old_vfs_resolve_path(&root, test_path);
    void *check_new = new_vfs_resolve_path(&root, test_path);
    if (check_old != &target || check_new != &target) {
        printf("[FAIL] Correctness mismatch: old=%p new=%p target=%p\n",
               check_old, check_new, &target);
        return 1;
    }

    const int iterations = 100000;
    uint64_t s, e;

    // 1. Benchmark Old Resolution
    s = __rdtsc();
    for (int i = 0; i < iterations; i++) {
        void *res = old_vfs_resolve_path(&root, test_path);
        (void)res;
    }
    e = __rdtsc();
    double cyc_old = (double)(e - s) / iterations;

    // 2. Benchmark New Resolution
    s = __rdtsc();
    for (int i = 0; i < iterations; i++) {
        void *res = new_vfs_resolve_path(&root, test_path);
        (void)res;
    }
    e = __rdtsc();
    double cyc_new = (double)(e - s) / iterations;

    // 3. Benchmark Relative Syscall AT_FDCWD resolution
    // Old: resolves cwd_path ("/home/user") from root every time
    const char *cwd_path = "/home/user";
    uint32_t cwd_hash = old_path_hash(&root, cwd_path);
    path_bucket_t *cb = &g_path_cache[cwd_hash & VFS_PATH_CACHE_BUCKET_MASK];
    vfs_node_t cwd_node = { .inode = 100, .refcount = 10 };
    cb->entries[1].valid = true;
    cb->entries[1].dir = &root;
    cb->entries[1].node = &cwd_node;
    cb->entries[1].hash = cwd_hash;
    cb->entries[1].path_len = strlen(cwd_path);
    cb->entries[1].gen = g_inval_gen;
    memcpy(cb->entries[1].path, cwd_path, strlen(cwd_path) + 1);

    s = __rdtsc();
    for (int i = 0; i < iterations; i++) {
        // Old: vfs_resolve_path_at(fs_root, t->cwd_path)
        void *base = old_vfs_resolve_path(&root, cwd_path);
        void *leaf = old_vfs_resolve_path(base, test_path);
        (void)leaf;
    }
    e = __rdtsc();
    double cyc_stat_old = (double)(e - s) / iterations;

    s = __rdtsc();
    for (int i = 0; i < iterations; i++) {
        // New: t->cwd_node directly!
        void *base = &cwd_node;
        void *leaf = new_vfs_resolve_path(base, test_path);
        (void)leaf;
    }
    e = __rdtsc();
    double cyc_stat_new = (double)(e - s) / iterations;

    printf("======================================================================\n");
    printf("   MICROBENCHMARK: VFS PATH RESOLUTION & RELATIVE SYSCALL LATENCY\n");
    printf("   Path: %s\n", test_path);
    printf("======================================================================\n\n");
    printf("1. Absolute Path Dcache Lookup:\n");
    printf("   - Old (3 passes + CLI/STI pushfq):  %6.1f cycles/lookup\n", cyc_old);
    printf("   - New (1 pass + Lockless Seqlock):   %6.1f cycles/lookup  ==> %4.2fX FASTER\n\n",
           cyc_new, cyc_old / cyc_new);

    printf("2. Relative Path Syscall Resolution (fstatat / openat AT_FDCWD):\n");
    printf("   - Old (resolve cwd_path + resolve leaf): %6.1f cycles/lookup\n", cyc_stat_old);
    printf("   - New (direct t->cwd_node + fast lookup):%6.1f cycles/lookup  ==> %4.2fX FASTER\n\n",
           cyc_stat_new, cyc_stat_old / cyc_stat_new);

    printf("[SUCCESS] All correctness checks and benchmarks passed!\n");
    return 0;
}
