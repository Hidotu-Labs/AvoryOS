// AvoryOS Phase 6 C5 userland test: amdgpu queues, VM, BOs, command
// submission.
//
// Raw ioctls against the amdgpu DRM node (no libdrm): discovers the node by
// DRM_IOCTL_VERSION name, then exercises
//
//   1. AMDGPU_INFO (accel working, VRAM/GTT heap sizes, HW IP counts)
//   2. GEM create/mmap/write/read/close in VRAM and GTT
//   3. PRIME handle -> fd -> handle roundtrip
//   4. AMDGPU_CTX alloc/query/free and AMDGPU_GEM_VA map/unmap
//   5. AMDGPU_BO_LIST create/update/destroy
//   6. SDMA copy through AMDGPU_CS: GTT src/dst/IB BOs VM-mapped, a BO list,
//      a 7-dword SDMA COPY_LINEAR packet (same layout as
//      sdma_v5_2_emit_copy_buffer()), fence via AMDGPU_WAIT_CS, then
//      byte-for-byte verification of the destination
//   7. 1000x BO create/map/free loop with a native PMM free-page invariant
//
// The intentionally-bad CS / fence-timeout recovery path stays in C8 (it
// needs the reset paths); this suite proves the success path and clean ioctl
// error handling.
//
// Build: userland/test_kpi_amdgpu.elf, installed as /bin/test_kpi_amdgpu.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/amdgpu_drm.h>

#include <uapi/kpi_dmabuf.h>

#define PAGE_SIZE_ 4096UL
#define BO_LOOP 1000

/* GPU VA slots for the CS test: far above anything the driver uses at boot. */
#define VA_SRC 0x0000008000000000ULL
#define VA_DST 0x0000008000200000ULL
#define VA_IB  0x0000008000400000ULL

#define COPY_BYTES (64 * 1024)
#define IB_DWORDS 7

static int failures;

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, long detail) {
    failures++;
    printf("  [FAIL] %s", what);
    if (detail)
        printf(" (errno=%ld %s)", detail, strerror((int)detail));
    printf("\n");
}

static void info(const char *what, long v) { printf("  [INFO] %s (%ld)\n", what, v); }

/* ── node discovery ─────────────────────────────────────────────────────── */

static int drm_name_is(const char *path, const char *want) {
    char name[32] = {0};
    struct drm_version ver;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    memset(&ver, 0, sizeof(ver));
    ver.name = name;
    ver.name_len = sizeof(name) - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0 && strcmp(name, want) == 0)
        return fd;
    close(fd);
    return -1;
}

static int find_amdgpu_node(char *path_out, size_t path_len) {
    for (int i = 0; i < 8; i++) {
        char path[64];
        int fd;

        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        fd = drm_name_is(path, "amdgpu");
        if (fd >= 0) {
            snprintf(path_out, path_len, "%s", path);
            return fd;
        }
    }
    return -1;
}

static int find_amdgpu_render_node(char *path_out, size_t path_len) {
    for (int i = 0; i < 8; i++) {
        char path[64];
        int fd;

        snprintf(path, sizeof(path), "/dev/dri/renderD%d", 128 + i);
        fd = drm_name_is(path, "amdgpu");
        if (fd >= 0) {
            snprintf(path_out, path_len, "%s", path);
            return fd;
        }
    }
    return -1;
}

/* ── small raw-ioctl helpers ────────────────────────────────────────────── */

static int gem_create(int fd, uint64_t size, uint64_t domains, uint64_t flags,
                      uint32_t *handle) {
    union drm_amdgpu_gem_create args;

    memset(&args, 0, sizeof(args));
    args.in.bo_size = size;
    args.in.alignment = PAGE_SIZE_;
    args.in.domains = domains;
    args.in.domain_flags = flags;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &args))
        return -1;
    *handle = args.out.handle;
    return 0;
}

static int gem_close(int fd, uint32_t handle) {
    struct drm_gem_close args;

    memset(&args, 0, sizeof(args));
    args.handle = handle;
    return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

static void *gem_mmap(int fd, uint32_t handle, uint64_t size) {
    union drm_amdgpu_gem_mmap args;
    void *p;

    memset(&args, 0, sizeof(args));
    args.in.handle = handle;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &args))
        return NULL;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
             (off_t)args.out.addr_ptr);
    return p == MAP_FAILED ? NULL : p;
}

static int vm_map(int fd, uint32_t handle, uint64_t va, uint64_t size,
                  uint32_t flags) {
    struct drm_amdgpu_gem_va args;

    memset(&args, 0, sizeof(args));
    args.handle = handle;
    args.operation = AMDGPU_VA_OP_MAP;
    args.flags = flags;
    args.va_address = va;
    args.offset_in_bo = 0;
    args.map_size = size;
    return ioctl(fd, DRM_IOCTL_AMDGPU_GEM_VA, &args);
}

static int vm_unmap(int fd, uint32_t handle, uint64_t va, uint64_t size) {
    struct drm_amdgpu_gem_va args;

    memset(&args, 0, sizeof(args));
    args.handle = handle;
    args.operation = AMDGPU_VA_OP_UNMAP;
    args.flags = 0;
    args.va_address = va;
    args.offset_in_bo = 0;
    args.map_size = size;
    return ioctl(fd, DRM_IOCTL_AMDGPU_GEM_VA, &args);
}

static int ctx_alloc(int fd, uint32_t *ctx_id) {
    union drm_amdgpu_ctx args;

    memset(&args, 0, sizeof(args));
    args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    args.in.priority = AMDGPU_CTX_PRIORITY_NORMAL;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &args))
        return -1;
    *ctx_id = args.out.alloc.ctx_id;
    return 0;
}

static int ctx_query(int fd, uint32_t ctx_id, uint32_t *hangs,
                     uint32_t *reset_status) {
    union drm_amdgpu_ctx args;

    memset(&args, 0, sizeof(args));
    args.in.op = AMDGPU_CTX_OP_QUERY_STATE;
    args.in.ctx_id = ctx_id;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &args))
        return -1;
    *hangs = args.out.state.hangs;
    *reset_status = args.out.state.reset_status;
    return 0;
}

static int ctx_free(int fd, uint32_t ctx_id) {
    union drm_amdgpu_ctx args;

    memset(&args, 0, sizeof(args));
    args.in.op = AMDGPU_CTX_OP_FREE_CTX;
    args.in.ctx_id = ctx_id;
    return ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &args);
}

static int bo_list_create(int fd, const uint32_t *handles, int n,
                          uint32_t *out_handle) {
    struct drm_amdgpu_bo_list_entry entries[8];
    union drm_amdgpu_bo_list args;

    if (n > 8)
        return -1;
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < n; i++)
        entries[i].bo_handle = handles[i];
    memset(&args, 0, sizeof(args));
    args.in.operation = AMDGPU_BO_LIST_OP_CREATE;
    args.in.list_handle = 0;
    args.in.bo_number = (uint32_t)n;
    args.in.bo_info_size = sizeof(entries[0]);
    args.in.bo_info_ptr = (uint64_t)(uintptr_t)entries;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &args))
        return -1;
    *out_handle = args.out.list_handle;
    return 0;
}

static int bo_list_update(int fd, uint32_t list_handle, const uint32_t *handles,
                          int n) {
    struct drm_amdgpu_bo_list_entry entries[8];
    union drm_amdgpu_bo_list args;

    if (n > 8)
        return -1;
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < n; i++)
        entries[i].bo_handle = handles[i];
    memset(&args, 0, sizeof(args));
    args.in.operation = AMDGPU_BO_LIST_OP_UPDATE;
    args.in.list_handle = list_handle;
    args.in.bo_number = (uint32_t)n;
    args.in.bo_info_size = sizeof(entries[0]);
    args.in.bo_info_ptr = (uint64_t)(uintptr_t)entries;
    return ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &args);
}

static int bo_list_destroy(int fd, uint32_t list_handle) {
    union drm_amdgpu_bo_list args;

    memset(&args, 0, sizeof(args));
    args.in.operation = AMDGPU_BO_LIST_OP_DESTROY;
    args.in.list_handle = list_handle;
    return ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &args);
}

static int cs_submit(int fd, uint32_t ctx_id, uint32_t bo_list_handle,
                     uint32_t ip_type, uint32_t ring, uint64_t ib_va,
                     uint32_t ib_bytes, uint64_t *out_fence) {
    struct drm_amdgpu_cs_chunk_ib ib;
    struct drm_amdgpu_cs_chunk chunk;
    union drm_amdgpu_cs args;

    memset(&ib, 0, sizeof(ib));
    ib.flags = 0;
    ib.va_start = ib_va;
    ib.ib_bytes = ib_bytes;
    ib.ip_type = ip_type;
    ib.ip_instance = 0;
    ib.ring = ring;

    memset(&chunk, 0, sizeof(chunk));
    chunk.chunk_id = AMDGPU_CHUNK_ID_IB;
    chunk.length_dw = sizeof(ib) / 4;
    chunk.chunk_data = (uint64_t)(uintptr_t)&ib;

    memset(&args, 0, sizeof(args));
    args.in.ctx_id = ctx_id;
    args.in.bo_list_handle = bo_list_handle;
    args.in.num_chunks = 1;
    args.in.chunks = (uint64_t)(uintptr_t)&chunk;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_CS, &args))
        return -1;
    *out_fence = args.out.handle;
    return 0;
}

/* timeout_ns: 0 means "wait forever" in the amdgpu ABI. */
static int wait_cs(int fd, uint64_t fence, uint32_t ctx_id, uint32_t ip_type,
                   uint32_t ring, uint64_t timeout_ns, uint64_t *status) {
    union drm_amdgpu_wait_cs args;

    memset(&args, 0, sizeof(args));
    args.in.handle = fence;
    args.in.timeout = timeout_ns;
    args.in.ip_type = ip_type;
    args.in.ip_instance = 0;
    args.in.ring = ring;
    args.in.ctx_id = ctx_id;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &args))
        return -1;
    *status = args.out.status;
    return 0;
}

static long pmm_free_pages(void) {
    int dev = open("/dev/kpi_dmabuf", O_RDWR | O_CLOEXEC);
    long n;

    if (dev < 0)
        return -1;
    n = ioctl(dev, KPI_DMABUF_IOC_PMM_FREE, 0);
    close(dev);
    return n;
}

/* ── 1. info ────────────────────────────────────────────────────────────── */

static void test_info(int fd) {
    struct drm_amdgpu_memory_info mem;
    struct drm_amdgpu_info args;
    uint32_t accel = 0, ip_count = 0;

    printf("\n=== AMDGPU_INFO ===\n");

    memset(&args, 0, sizeof(args));
    args.return_pointer = (uint64_t)(uintptr_t)&accel;
    args.return_size = sizeof(accel);
    args.query = AMDGPU_INFO_ACCEL_WORKING;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &args) == 0 && accel == 1)
        pass("AMDGPU_INFO_ACCEL_WORKING = 1");
    else
        fail("AMDGPU_INFO_ACCEL_WORKING", errno);

    memset(&mem, 0, sizeof(mem));
    memset(&args, 0, sizeof(args));
    args.return_pointer = (uint64_t)(uintptr_t)&mem;
    args.return_size = sizeof(mem);
    args.query = AMDGPU_INFO_MEMORY;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &args) == 0 && mem.vram.total_heap_size > 0 &&
        mem.gtt.total_heap_size > 0) {
        pass("AMDGPU_INFO_MEMORY reports VRAM and GTT heaps");
        info("VRAM heap bytes", (long)mem.vram.total_heap_size);
        info("GTT heap bytes", (long)mem.gtt.total_heap_size);
    } else {
        fail("AMDGPU_INFO_MEMORY", errno);
    }

    memset(&args, 0, sizeof(args));
    args.return_pointer = (uint64_t)(uintptr_t)&ip_count;
    args.return_size = sizeof(ip_count);
    args.query = AMDGPU_INFO_HW_IP_COUNT;
    args.query_hw_ip.type = AMDGPU_HW_IP_DMA;
    args.query_hw_ip.ip_instance = 0;
    if (ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &args) == 0 && ip_count > 0) {
        pass("AMDGPU_INFO_HW_IP_COUNT sees an SDMA instance");
        info("SDMA instances", ip_count);
    } else {
        fail("AMDGPU_INFO_HW_IP_COUNT(DMA)", errno);
    }
}

/* ── 2. GEM create/mmap in both memory domains ─────────────────────────── */

static void test_gem_domain(int fd, const char *name, uint64_t domains,
                            uint64_t flags) {
    uint32_t handle = 0;
    unsigned char *p;
    char what[96];

    snprintf(what, sizeof(what), "GEM create(%s)", name);
    if (gem_create(fd, PAGE_SIZE_ * 16, domains, flags, &handle)) {
        fail(what, errno);
        return;
    }
    pass(what);

    p = gem_mmap(fd, handle, PAGE_SIZE_ * 16);
    snprintf(what, sizeof(what), "GEM mmap(%s) + write/read pattern", name);
    if (!p) {
        fail(what, errno);
    } else {
        p[0] = 0x5a;
        p[PAGE_SIZE_ * 8 + 3] = 0xa5;
        p[PAGE_SIZE_ * 16 - 1] = 0xff;
        if (p[0] == 0x5a && p[PAGE_SIZE_ * 8 + 3] == 0xa5 &&
            p[PAGE_SIZE_ * 16 - 1] == 0xff)
            pass(what);
        else
            fail(what, 0);
        munmap(p, PAGE_SIZE_ * 16);
    }

    snprintf(what, sizeof(what), "GEM close(%s)", name);
    if (gem_close(fd, handle) == 0)
        pass(what);
    else
        fail(what, errno);
}

/* ── 3. PRIME roundtrip ─────────────────────────────────────────────────── */

static void test_prime(int fd) {
    uint32_t handle = 0, imported = 0;
    struct drm_prime_handle ph;

    printf("\n=== PRIME handle <-> fd roundtrip ===\n");

    if (gem_create(fd, PAGE_SIZE_ * 32, AMDGPU_GEM_DOMAIN_GTT,
                   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &handle)) {
        fail("GEM create for PRIME", errno);
        return;
    }

    memset(&ph, 0, sizeof(ph));
    ph.handle = handle;
    ph.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0)
        pass("PRIME_HANDLE_TO_FD exported a dma-buf fd");
    else {
        fail("PRIME_HANDLE_TO_FD", errno);
        gem_close(fd, handle);
        return;
    }

    {
        struct drm_prime_handle ph2;

        memset(&ph2, 0, sizeof(ph2));
        ph2.fd = ph.fd;
        ph2.flags = DRM_CLOEXEC | DRM_RDWR;
        if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph2) == 0 &&
            ph2.handle != 0) {
            pass("PRIME_FD_TO_HANDLE imported it back");
            gem_close(fd, ph2.handle);
        } else {
            fail("PRIME_FD_TO_HANDLE", errno);
        }
    }

    close(ph.fd);
    gem_close(fd, handle);
    (void)imported;
}

/* ── 4. context + VM map/unmap ──────────────────────────────────────────── */

static void test_ctx_and_vm(int fd) {
    uint32_t handle = 0, ctx = 0, hangs = 0, reset_status = 0;
    uint64_t va = 0x0000009000000000ULL;

    printf("\n=== AMDGPU_CTX + AMDGPU_GEM_VA ===\n");

    if (ctx_alloc(fd, &ctx) == 0 && ctx != 0)
        pass("AMDGPU_CTX alloc");
    else {
        fail("AMDGPU_CTX alloc", errno);
        return;
    }

    if (ctx_query(fd, ctx, &hangs, &reset_status) == 0)
        pass("AMDGPU_CTX query state");
    else
        fail("AMDGPU_CTX query state", errno);

    if (gem_create(fd, PAGE_SIZE_ * 16, AMDGPU_GEM_DOMAIN_GTT,
                   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &handle)) {
        fail("GEM create for VM map", errno);
        ctx_free(fd, ctx);
        return;
    }

    if (vm_map(fd, handle, va, PAGE_SIZE_ * 16,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE) == 0)
        pass("AMDGPU_GEM_VA map");
    else
        fail("AMDGPU_GEM_VA map", errno);

    if (vm_unmap(fd, handle, va, PAGE_SIZE_ * 16) == 0)
        pass("AMDGPU_GEM_VA unmap");
    else
        fail("AMDGPU_GEM_VA unmap", errno);

    gem_close(fd, handle);
    ctx_free(fd, ctx);
    if (ctx_free(fd, ctx) != 0)
        pass("AMDGPU_CTX double-free rejected");
    else
        fail("AMDGPU_CTX double-free rejected", 0);
}

/* ── 5. BO list ─────────────────────────────────────────────────────────── */

static void test_bo_list(int fd) {
    uint32_t handles[2], list = 0;

    printf("\n=== AMDGPU_BO_LIST ===\n");

    for (int i = 0; i < 2; i++) {
        if (gem_create(fd, PAGE_SIZE_ * 8, AMDGPU_GEM_DOMAIN_GTT,
                       AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &handles[i])) {
            fail("GEM create for BO list", errno);
            for (int j = 0; j < i; j++)
                gem_close(fd, handles[j]);
            return;
        }
    }

    if (bo_list_create(fd, handles, 2, &list) == 0)
        pass("BO_LIST create (2 BOs)");
    else
        fail("BO_LIST create", errno);

    if (list && bo_list_update(fd, list, handles, 2) == 0)
        pass("BO_LIST update");
    else
        fail("BO_LIST update", errno);

    if (list && bo_list_destroy(fd, list) == 0)
        pass("BO_LIST destroy");
    else
        fail("BO_LIST destroy", errno);

    for (int i = 0; i < 2; i++)
        gem_close(fd, handles[i]);
}

/* ── 6. SDMA copy through AMDGPU_CS ─────────────────────────────────────── */

/* Same 7-dword COPY_LINEAR packet sdma_v5_2_emit_copy_buffer() builds
 * (SDMA_PKT_HEADER_OP(1) | SUB_OP(0), count-1, endian swap, src lo/hi,
 * dst lo/hi). */
static void sdma_copy_packet(uint32_t *ib, uint64_t src, uint64_t dst,
                             uint32_t bytes) {
    ib[0] = 1u | (0u << 8); /* SDMA_OP_COPY | SDMA_SUBOP_COPY_LINEAR */
    ib[1] = bytes - 1;
    ib[2] = 0;
    ib[3] = (uint32_t)src;
    ib[4] = (uint32_t)(src >> 32);
    ib[5] = (uint32_t)dst;
    ib[6] = (uint32_t)(dst >> 32);
}

static void test_sdma_copy(int fd) {
    uint32_t src_bo = 0, dst_bo = 0, ib_bo = 0, ctx = 0, list = 0;
    uint32_t handles[3];
    unsigned char *src_map = NULL, *dst_map = NULL, *ib_map = NULL;
    uint64_t fence = 0, status = 0;

    printf("\n=== SDMA copy through AMDGPU_CS ===\n");

    if (gem_create(fd, COPY_BYTES, AMDGPU_GEM_DOMAIN_GTT,
                   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &src_bo) ||
        gem_create(fd, COPY_BYTES, AMDGPU_GEM_DOMAIN_GTT,
                   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &dst_bo) ||
        gem_create(fd, PAGE_SIZE_, AMDGPU_GEM_DOMAIN_GTT,
                   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &ib_bo)) {
        fail("GEM create src/dst/ib", errno);
        goto out;
    }
    pass("GEM create src/dst/ib (GTT)");

    src_map = gem_mmap(fd, src_bo, COPY_BYTES);
    dst_map = gem_mmap(fd, dst_bo, COPY_BYTES);
    ib_map = gem_mmap(fd, ib_bo, PAGE_SIZE_);
    if (!src_map || !dst_map || !ib_map) {
        fail("GEM mmap src/dst/ib", errno);
        goto out;
    }
    pass("GEM mmap src/dst/ib");

    for (int i = 0; i < COPY_BYTES; i++)
        src_map[i] = (unsigned char)(i * 7 + 3);
    memset(dst_map, 0, COPY_BYTES);

    if (vm_map(fd, src_bo, VA_SRC, COPY_BYTES,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE) ||
        vm_map(fd, dst_bo, VA_DST, COPY_BYTES,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE) ||
        vm_map(fd, ib_bo, VA_IB, PAGE_SIZE_,
               AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE)) {
        fail("GEM_VA map src/dst/ib", errno);
        goto out;
    }
    pass("GEM_VA map src/dst/ib");

    sdma_copy_packet((uint32_t *)ib_map, VA_SRC, VA_DST, COPY_BYTES);

    handles[0] = ib_bo;
    handles[1] = src_bo;
    handles[2] = dst_bo;
    if (bo_list_create(fd, handles, 3, &list)) {
        fail("BO_LIST create for CS", errno);
        goto out;
    }
    if (ctx_alloc(fd, &ctx)) {
        fail("CTX alloc for CS", errno);
        goto out;
    }
    pass("BO list + context ready");

    if (cs_submit(fd, ctx, list, AMDGPU_HW_IP_DMA, 0, VA_IB,
                  IB_DWORDS * 4, &fence) == 0 && fence != 0)
        pass("AMDGPU_CS submitted to SDMA ring 0");
    else {
        fail("AMDGPU_CS submit", errno);
        goto out;
    }

    /* 5 s in nanoseconds; 0 would wait forever. */
    if (wait_cs(fd, fence, ctx, AMDGPU_HW_IP_DMA, 0, 5000000000ULL, &status) == 0 &&
        status == 0)
        pass("AMDGPU_WAIT_CS completed (fence signaled)");
    else
        fail("AMDGPU_WAIT_CS", errno);

    {
        int mismatch = -1;

        for (int i = 0; i < COPY_BYTES; i++) {
            if (dst_map[i] != (unsigned char)(i * 7 + 3)) {
                mismatch = i;
                break;
            }
        }
        if (mismatch < 0)
            pass("SDMA copy verified byte-for-byte (64 KiB)");
        else {
            failures++;
            printf("  [FAIL] SDMA copy mismatch at byte %d (got 0x%02x want 0x%02x)\n",
                   mismatch, dst_map[mismatch], (unsigned char)(mismatch * 7 + 3));
        }
    }

out:
    if (ctx)
        ctx_free(fd, ctx);
    if (list)
        bo_list_destroy(fd, list);
    if (src_map)
        munmap(src_map, COPY_BYTES);
    if (dst_map)
        munmap(dst_map, COPY_BYTES);
    if (ib_map)
        munmap(ib_map, PAGE_SIZE_);
    if (ib_bo) {
        vm_unmap(fd, ib_bo, VA_IB, PAGE_SIZE_);
        gem_close(fd, ib_bo);
    }
    if (src_bo) {
        vm_unmap(fd, src_bo, VA_SRC, COPY_BYTES);
        gem_close(fd, src_bo);
    }
    if (dst_bo) {
        vm_unmap(fd, dst_bo, VA_DST, COPY_BYTES);
        gem_close(fd, dst_bo);
    }
}

/* ── 7. BO churn invariant ──────────────────────────────────────────────── */

static void test_bo_loop(int fd) {
    long baseline, final;
    int errors = 0;

    printf("\n=== %dx BO create/map/free loop ===\n", BO_LOOP);

    /* Warm up allocator caches before sampling. */
    for (int i = 0; i < 64; i++) {
        uint32_t handle;

        if (gem_create(fd, PAGE_SIZE_ * 16, AMDGPU_GEM_DOMAIN_GTT,
                       AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &handle))
            continue;
        gem_close(fd, handle);
    }

    baseline = pmm_free_pages();

    for (int i = 0; i < BO_LOOP; i++) {
        uint32_t handle;
        unsigned char *p;

        if (gem_create(fd, PAGE_SIZE_ * 16, AMDGPU_GEM_DOMAIN_GTT,
                       AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &handle)) {
            errors++;
            continue;
        }
        p = gem_mmap(fd, handle, PAGE_SIZE_ * 16);
        if (!p) {
            errors++;
            gem_close(fd, handle);
            continue;
        }
        p[0] = (unsigned char)i;
        p[PAGE_SIZE_ * 16 - 1] = (unsigned char)~i;
        if (p[0] != (unsigned char)i || p[PAGE_SIZE_ * 16 - 1] != (unsigned char)~i)
            errors++;
        munmap(p, PAGE_SIZE_ * 16);
        gem_close(fd, handle);
    }

    if (errors == 0)
        pass("1000 loops completed without errors");
    else {
        failures++;
        printf("  [FAIL] %d loop error(s)\n", errors);
    }

    final = pmm_free_pages();
    if (baseline < 0 || final < 0) {
        info("PMM free-page count unavailable", final);
    } else {
        long delta = baseline - final;

        info("PMM free pages baseline", baseline);
        info("PMM free pages final", final);
        if (delta <= 0)
            pass("PMM free-page count did not go backwards");
        else {
            failures++;
            printf("  [FAIL] PMM free pages dropped by %ld\n", delta);
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    char node[64] = {0}, rnode[64] = {0};
    int fd, rfd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS Phase 6 C5 KPI amdgpu test ===\n");

    fd = find_amdgpu_node(node, sizeof(node));
    if (fd < 0) {
        printf("  [SKIP] no amdgpu DRM node (kpi_amdgpu=0 or not initialized)\n");
        return 0;
    }
    printf("  [INFO] amdgpu DRM node: %s\n", node);

    rfd = find_amdgpu_render_node(rnode, sizeof(rnode));
    if (rfd >= 0) {
        printf("  [INFO] amdgpu render node: %s\n", rnode);
        close(rfd);
    } else {
        printf("  [WARN] no amdgpu render node found\n");
    }

    test_info(fd);
    test_gem_domain(fd, "VRAM", AMDGPU_GEM_DOMAIN_VRAM,
                    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED);
    test_gem_domain(fd, "GTT", AMDGPU_GEM_DOMAIN_GTT,
                    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED);
    test_prime(fd);
    test_ctx_and_vm(fd);
    test_bo_list(fd);
    test_sdma_copy(fd);
    test_bo_loop(fd);

    close(fd);
    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
