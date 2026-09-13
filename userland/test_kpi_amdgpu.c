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
//   8. KMS (P6 C6): discovers the card's connector sysfs node, prefers an
//      already-connected sink, otherwise forces the connector on through the
//      DRM 6.6 `status` attribute (the no-monitor path: noedid modes), then
//      runs an atomic modetest-lite: property discovery, primary-plane enable
//      with PAGE_FLIP_EVENT, flip events from two framebuffers, WAIT_VBLANK,
//      a cursor-plane commit, and an atomic disable.  Unforces the connector
//      afterwards.
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
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <drm/amdgpu_drm.h>

#include <uapi/kpi_dmabuf.h>

#define PAGE_SIZE_ 4096UL
#define BO_LOOP 1000

/* Kernel-internal DRM plane types; the uapi exposes them as the "type"
 * property enum (0 overlay, 1 primary, 2 cursor).  libdrm's names are not
 * available here (raw ioctls only). */
#define KPI_PLANE_TYPE_PRIMARY 1
#define KPI_PLANE_TYPE_CURSOR 2

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
    uint64_t chunk_ptrs[1];
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

    /* 6.6 uAPI: cs.in.chunks points to an array of u64 pointers, and each
     * entry points at the chunk descriptor (drm_amdgpu_cs_parser_init()
     * copies the pointer array first, then each chunk). */
    chunk_ptrs[0] = (uint64_t)(uintptr_t)&chunk;

    memset(&args, 0, sizeof(args));
    args.in.ctx_id = ctx_id;
    args.in.bo_list_handle = bo_list_handle;
    args.in.num_chunks = 1;
    args.in.chunks = (uint64_t)(uintptr_t)chunk_ptrs;
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

/* ── 8. KMS: connector force + atomic modeset (P6 C6) ───────────────────── */

struct prop_set {
    uint32_t ids[64];
    uint64_t vals[64];
    uint32_t count;
};

struct atomic_req {
    uint32_t n_objs;
    uint32_t n_props;
    uint32_t objs[8];
    uint32_t counts[8];
    uint32_t props[48];
    uint64_t values[48];
};

static void req_add(struct atomic_req *r, uint32_t obj, const uint32_t *props,
                    const uint64_t *values, uint32_t n) {
    r->objs[r->n_objs] = obj;
    r->counts[r->n_objs] = n;
    r->n_objs++;
    for (uint32_t i = 0; i < n; i++) {
        r->props[r->n_props] = props[i];
        r->values[r->n_props] = values[i];
        r->n_props++;
    }
}

/* Discover one object's property IDs and current values. */
static int obj_props(int fd, uint32_t obj_id, uint32_t obj_type,
                     struct prop_set *out) {
    struct drm_mode_obj_get_properties p;
    uint32_t total;

    memset(&p, 0, sizeof(p));
    p.obj_id = obj_id;
    p.obj_type = obj_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p) || !p.count_props)
        return -1;
    total = p.count_props;
    if (total > 64)
        total = 64;

    memset(&p, 0, sizeof(p));
    p.obj_id = obj_id;
    p.obj_type = obj_type;
    p.count_props = total;
    p.props_ptr = (uint64_t)(uintptr_t)out->ids;
    p.prop_values_ptr = (uint64_t)(uintptr_t)out->vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p))
        return -1;
    out->count = p.count_props > 64 ? 64 : p.count_props;
    return 0;
}

static int prop_find(int fd, const struct prop_set *set, const char *name,
                     uint32_t *id_out, uint64_t *val_out) {
    struct drm_mode_get_property p;

    for (uint32_t i = 0; i < set->count; i++) {
        memset(&p, 0, sizeof(p));
        p.prop_id = set->ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p))
            return -1;
        if (strncmp(p.name, name, sizeof(p.name)) == 0) {
            if (id_out)
                *id_out = set->ids[i];
            if (val_out)
                *val_out = set->vals[i];
            return 0;
        }
    }
    return -1;
}

static int atomic_commit(int fd, struct atomic_req *r, uint32_t flags) {
    struct drm_mode_atomic at;

    memset(&at, 0, sizeof(at));
    at.flags = flags;
    at.count_objs = r->n_objs;
    at.objs_ptr = (uint64_t)(uintptr_t)r->objs;
    at.count_props_ptr = (uint64_t)(uintptr_t)r->counts;
    at.props_ptr = (uint64_t)(uintptr_t)r->props;
    at.prop_values_ptr = (uint64_t)(uintptr_t)r->values;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at);
}

static int wait_flip_event(int fd, struct drm_event_vblank *ev) {
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLIN))
        return -1;
    if (read(fd, ev, sizeof(*ev)) != (ssize_t)sizeof(*ev))
        return -1;
    if (ev->base.type != DRM_EVENT_FLIP_COMPLETE) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/* ── sysfs connector helpers ──
 * DRM 6.6's connector `status` attribute is RW: writing "on" forces the
 * connector on (and triggers a probe), "detect" restores auto-detection. */

static int sysfs_read_str(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return 0;
}

static int sysfs_write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

static long sysfs_read_long(const char *path) {
    char buf[32];
    char *end;
    long v;

    if (sysfs_read_str(path, buf, sizeof(buf)))
        return -1;
    errno = 0;
    v = strtol(buf, &end, 10);
    if (errno || end == buf)
        return -1;
    return v;
}

struct conn_pick {
    char sysfs[320];
    char name[32];
    char status[24];
    uint32_t conn_id;
};

static int conn_is_hdmi(const char *name) {
    return strncmp(name, "HDMI-A-", 7) == 0 || strncmp(name, "HDMI-B-", 7) == 0;
}

static int conn_is_dp(const char *name) {
    return strncmp(name, "DP-", 3) == 0;
}

/* Pick a connector: already-connected HDMI first, then any connected, then
 * HDMI, then DP.  Only HDMI/DP are considered (the iGPU has no legacy VGA);
 * writeback connectors are skipped. */
static int pick_connector(const char *card, struct conn_pick *pick) {
    DIR *d;
    struct dirent *de;
    char prefix[32];
    int best_rank = 99;
    int found = 0;

    snprintf(prefix, sizeof(prefix), "%s-", card);
    d = opendir("/sys/class/drm");
    if (!d)
        return -1;

    while ((de = readdir(d)) != NULL) {
        char path[320];
        char status[24];
        const char *name;
        long cid;
        int rank;

        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
            continue;
        name = de->d_name + strlen(prefix);
        if ((!conn_is_hdmi(name) && !conn_is_dp(name)) ||
            strstr(name, "Writeback"))
            continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
        if (sysfs_read_str(path, status, sizeof(status)))
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/connector_id", de->d_name);
        cid = sysfs_read_long(path);
        if (cid < 0)
            continue;

        if (strcmp(status, "connected") == 0)
            rank = conn_is_hdmi(name) ? 0 : 1;
        else
            rank = conn_is_hdmi(name) ? 2 : 3;

        if (rank < best_rank) {
            best_rank = rank;
            snprintf(pick->sysfs, sizeof(pick->sysfs),
                     "/sys/class/drm/%s", de->d_name);
            snprintf(pick->name, sizeof(pick->name), "%s", name);
            snprintf(pick->status, sizeof(pick->status), "%s", status);
            pick->conn_id = (uint32_t)cid;
            found = 1;
        }
    }
    closedir(d);
    return found ? 0 : -1;
}

/* Encoder -> possible CRTCs -> the CRTC id for this connector. */
static uint32_t connector_crtc(int fd, uint32_t conn_id) {
    struct drm_mode_get_connector con;
    struct drm_mode_get_encoder enc;
    struct drm_mode_card_res res;
    uint32_t crtcs[16] = {0};

    memset(&con, 0, sizeof(con));
    con.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) || !con.encoder_id)
        return 0;

    memset(&enc, 0, sizeof(enc));
    enc.encoder_id = con.encoder_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc))
        return 0;

    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs ||
        res.count_crtcs > 16)
        return 0;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
        return 0;

    for (uint32_t i = 0; i < res.count_crtcs; i++)
        if (enc.possible_crtcs & (1u << i))
            return crtcs[i];
    return crtcs[0];
}

/* Fetch all modes (the first GETCONNECTOR probes the connector) and pick the
 * largest by pixel area.  Returns a calloc'd array the caller frees. */
static struct drm_mode_modeinfo *connector_modes(int fd, uint32_t conn_id,
                                                 uint32_t *count_out,
                                                 uint32_t *best_out) {
    struct drm_mode_get_connector con;
    struct drm_mode_modeinfo *modes;
    uint32_t cap, best = 0;
    uint64_t best_area = 0;

    memset(&con, 0, sizeof(con));
    con.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) || !con.count_modes)
        return NULL;

    cap = con.count_modes;
    modes = calloc(cap, sizeof(*modes));
    if (!modes)
        return NULL;

    memset(&con, 0, sizeof(con));
    con.connector_id = conn_id;
    con.count_modes = cap;
    con.modes_ptr = (uint64_t)(uintptr_t)modes;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con)) {
        free(modes);
        return NULL;
    }

    for (uint32_t i = 0; i < cap; i++) {
        uint64_t area = (uint64_t)modes[i].hdisplay * modes[i].vdisplay;

        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    *count_out = cap;
    *best_out = best;
    return modes;
}

static int dumb_create_and_map(int fd, uint32_t w, uint32_t h, uint32_t *handle,
                               uint32_t *pitch, uint64_t *size, void **map) {
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;

    memset(&cd, 0, sizeof(cd));
    cd.width = w;
    cd.height = h;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) || !cd.handle)
        return -1;

    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        struct drm_mode_destroy_dumb dd = {.handle = cd.handle};

        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        return -1;
    }

    *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (*map == MAP_FAILED) {
        struct drm_mode_destroy_dumb dd = {.handle = cd.handle};

        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        *map = NULL;
        return -1;
    }
    *handle = cd.handle;
    *pitch = cd.pitch;
    *size = cd.size;
    return 0;
}

static int add_fb2(int fd, uint32_t handle, uint32_t w, uint32_t h,
                   uint32_t pitch, uint32_t format, uint32_t *fb_id) {
    struct drm_mode_fb_cmd2 fb;

    memset(&fb, 0, sizeof(fb));
    fb.width = w;
    fb.height = h;
    fb.pixel_format = format;
    fb.handles[0] = handle;
    fb.pitches[0] = pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) || !fb.fb_id)
        return -1;
    *fb_id = fb.fb_id;
    return 0;
}

static uint32_t find_plane(int fd, uint64_t want_type, struct prop_set *props) {
    struct drm_mode_get_plane_res pres;
    uint32_t ids[32];
    struct prop_set ps;

    memset(&pres, 0, sizeof(pres));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) || !pres.count_planes)
        return 0;
    if (pres.count_planes > 32)
        pres.count_planes = 32;
    pres.plane_id_ptr = (uint64_t)(uintptr_t)ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres))
        return 0;

    for (uint32_t i = 0; i < pres.count_planes; i++) {
        uint64_t type;

        if (obj_props(fd, ids[i], DRM_MODE_OBJECT_PLANE, &ps))
            continue;
        if (prop_find(fd, &ps, "type", 0, &type) == 0 && type == want_type) {
            if (props)
                *props = ps;
            return ids[i];
        }
    }
    return 0;
}

static int cursor_plane_commit(int fd, uint32_t plane_id,
                               const struct prop_set *pp, uint32_t crtc_id,
                               uint32_t fb_id, int x, int y, uint32_t w,
                               uint32_t h) {
    struct atomic_req req;
    uint32_t p_crtc, p_fb, p_cx, p_cy, p_cw, p_ch, p_sx, p_sy, p_sw, p_sh;
    uint32_t props[10];
    uint64_t values[10];
    uint32_t n = 0;

    if (prop_find(fd, pp, "CRTC_ID", &p_crtc, 0) ||
        prop_find(fd, pp, "FB_ID", &p_fb, 0) ||
        prop_find(fd, pp, "CRTC_X", &p_cx, 0) ||
        prop_find(fd, pp, "CRTC_Y", &p_cy, 0) ||
        prop_find(fd, pp, "CRTC_W", &p_cw, 0) ||
        prop_find(fd, pp, "CRTC_H", &p_ch, 0) ||
        prop_find(fd, pp, "SRC_X", &p_sx, 0) ||
        prop_find(fd, pp, "SRC_Y", &p_sy, 0) ||
        prop_find(fd, pp, "SRC_W", &p_sw, 0) ||
        prop_find(fd, pp, "SRC_H", &p_sh, 0))
        return -1;

    memset(&req, 0, sizeof(req));
    props[n] = p_crtc;
    values[n++] = crtc_id;
    props[n] = p_fb;
    values[n++] = fb_id;
    props[n] = p_cx;
    values[n++] = (uint64_t)x;
    props[n] = p_cy;
    values[n++] = (uint64_t)y;
    props[n] = p_cw;
    values[n++] = w;
    props[n] = p_ch;
    values[n++] = h;
    props[n] = p_sx;
    values[n++] = 0;
    props[n] = p_sy;
    values[n++] = 0;
    props[n] = p_sw;
    values[n++] = (uint64_t)w << 16;
    props[n] = p_sh;
    values[n++] = (uint64_t)h << 16;
    req_add(&req, plane_id, props, values, n);
    return atomic_commit(fd, &req, 0);
}

static void test_kms(int fd, const char *card) {
    struct conn_pick pick;
    struct drm_set_client_cap client_cap;
    struct prop_set conn_props, crtc_props, plane_props, cursor_props;
    struct drm_mode_modeinfo *modes = NULL, *mode;
    struct drm_mode_create_blob blob;
    struct drm_mode_destroy_blob dblob;
    struct drm_mode_destroy_dumb ddumb;
    struct atomic_req req;
    union drm_wait_vblank vbl;
    struct drm_event_vblank ev;
    char status_path[384];
    uint32_t plane_id = 0, cursor_id = 0, crtc_id = 0;
    uint32_t conn_crtc_id, crtc_mode_id, crtc_active;
    uint32_t plane_fb_id, plane_crtc_id;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h, p_crtc_x, p_crtc_y, p_crtc_w,
        p_crtc_h;
    uint32_t mode_count = 0, best = 0;
    uint32_t dumb1 = 0, pitch1 = 0, fb1 = 0;
    uint32_t dumb2 = 0, pitch2 = 0, fb2 = 0;
    uint32_t cdumb = 0, cpitch = 0, cfb = 0;
    uint64_t dsize1 = 0, dsize2 = 0, csize = 0;
    void *map1 = NULL, *map2 = NULL, *cmap = NULL;
    uint32_t props[16];
    uint64_t values[16];
    uint32_t n;
    int forced = 0;

    memset(&blob, 0, sizeof(blob));
    printf("\n=== KMS: connector + atomic modeset (DCN) ===\n");

    if (pick_connector(card, &pick)) {
        printf("  [SKIP] no HDMI/DP connector sysfs node for %s\n", card);
        return;
    }
    printf("  [INFO] connector %s status=%s id=%u\n", pick.name, pick.status,
           pick.conn_id);

    snprintf(status_path, sizeof(status_path), "%s/status", pick.sysfs);
    if (strcmp(pick.status, "connected") != 0) {
        if (sysfs_write_str(status_path, "on")) {
            printf("  [SKIP] cannot force %s on (errno=%d %s)\n", pick.name,
                   errno, strerror(errno));
            return;
        }
        forced = 1;
        sysfs_read_str(status_path, pick.status, sizeof(pick.status));
        if (strcmp(pick.status, "connected") != 0) {
            printf("  [SKIP] %s still %s after force\n", pick.name, pick.status);
            goto out_unforce;
        }
        pass("connector forced on (no sink: noedid modes)");
    } else {
        pass("connector reports connected");
    }

    modes = connector_modes(fd, pick.conn_id, &mode_count, &best);
    if (!modes) {
        fail("connector has no modes", errno);
        goto out_unforce;
    }
    mode = &modes[best];
    printf("  [PASS] connector probe: %u mode(s), using %ux%u@%u\n", mode_count,
           mode->hdisplay, mode->vdisplay, mode->vrefresh);

    memset(&client_cap, 0, sizeof(client_cap));
    client_cap.capability = DRM_CLIENT_CAP_ATOMIC;
    client_cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &client_cap)) {
        fail("SET_CLIENT_CAP(ATOMIC)", errno);
        goto out_unforce;
    }

    crtc_id = connector_crtc(fd, pick.conn_id);
    if (!crtc_id) {
        fail("no CRTC for the connector", 0);
        goto out_unforce;
    }

    if (obj_props(fd, pick.conn_id, DRM_MODE_OBJECT_CONNECTOR, &conn_props) ||
        prop_find(fd, &conn_props, "CRTC_ID", &conn_crtc_id, 0) ||
        obj_props(fd, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props) ||
        prop_find(fd, &crtc_props, "MODE_ID", &crtc_mode_id, 0) ||
        prop_find(fd, &crtc_props, "ACTIVE", &crtc_active, 0)) {
        fail("connector/CRTC property discovery", 0);
        goto out_unforce;
    }

    plane_id = find_plane(fd, KPI_PLANE_TYPE_PRIMARY, &plane_props);
    cursor_id = find_plane(fd, KPI_PLANE_TYPE_CURSOR, &cursor_props);
    if (!plane_id || prop_find(fd, &plane_props, "FB_ID", &plane_fb_id, 0) ||
        prop_find(fd, &plane_props, "CRTC_ID", &plane_crtc_id, 0) ||
        prop_find(fd, &plane_props, "SRC_X", &p_src_x, 0) ||
        prop_find(fd, &plane_props, "SRC_Y", &p_src_y, 0) ||
        prop_find(fd, &plane_props, "SRC_W", &p_src_w, 0) ||
        prop_find(fd, &plane_props, "SRC_H", &p_src_h, 0) ||
        prop_find(fd, &plane_props, "CRTC_X", &p_crtc_x, 0) ||
        prop_find(fd, &plane_props, "CRTC_Y", &p_crtc_y, 0) ||
        prop_find(fd, &plane_props, "CRTC_W", &p_crtc_w, 0) ||
        prop_find(fd, &plane_props, "CRTC_H", &p_crtc_h, 0)) {
        fail("primary plane property discovery", 0);
        goto out_unforce;
    }
    pass("atomic property discovery (connector/CRTC/primary plane)");
    if (cursor_id)
        printf("  [INFO] cursor plane %u found\n", cursor_id);
    else
        printf("  [WARN] no cursor plane found\n");

    /* Two full-screen buffers so the second commit is a real page flip. */
    if (dumb_create_and_map(fd, mode->hdisplay, mode->vdisplay, &dumb1, &pitch1,
                            &dsize1, &map1) ||
        dumb_create_and_map(fd, mode->hdisplay, mode->vdisplay, &dumb2, &pitch2,
                            &dsize2, &map2)) {
        fail("CREATE_DUMB/MAP_DUMB", errno);
        goto out_all;
    }
    memset(map1, 0x22, dsize1);
    memset(map2, 0x44, dsize2);
    if (add_fb2(fd, dumb1, mode->hdisplay, mode->vdisplay, pitch1,
                DRM_FORMAT_XRGB8888, &fb1) ||
        add_fb2(fd, dumb2, mode->hdisplay, mode->vdisplay, pitch2,
                DRM_FORMAT_XRGB8888, &fb2)) {
        fail("ADDFB2", errno);
        goto out_all;
    }

    blob.data = (uint64_t)(uintptr_t)mode;
    blob.length = sizeof(*mode);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) || !blob.blob_id) {
        fail("MODE_CREATEPROPBLOB", errno);
        goto out_all;
    }

    memset(&req, 0, sizeof(req));
    n = 0;
    props[n] = conn_crtc_id;
    values[n++] = crtc_id;
    req_add(&req, pick.conn_id, props, values, n);
    n = 0;
    props[n] = crtc_mode_id;
    values[n++] = blob.blob_id;
    props[n] = crtc_active;
    values[n++] = 1;
    req_add(&req, crtc_id, props, values, n);
    n = 0;
    props[n] = plane_fb_id;
    values[n++] = fb1;
    props[n] = plane_crtc_id;
    values[n++] = crtc_id;
    props[n] = p_src_x;
    values[n++] = 0;
    props[n] = p_src_y;
    values[n++] = 0;
    props[n] = p_src_w;
    values[n++] = (uint64_t)mode->hdisplay << 16;
    props[n] = p_src_h;
    values[n++] = (uint64_t)mode->vdisplay << 16;
    props[n] = p_crtc_x;
    values[n++] = 0;
    props[n] = p_crtc_y;
    values[n++] = 0;
    props[n] = p_crtc_w;
    values[n++] = mode->hdisplay;
    props[n] = p_crtc_h;
    values[n++] = mode->vdisplay;
    req_add(&req, plane_id, props, values, n);

    if (atomic_commit(fd, &req,
                      DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT)) {
        fail("atomic enable commit (forced connector)", errno);
        goto out_all;
    }
    printf("  [PASS] atomic enable commit at %ux%u (ALLOW_MODESET)\n",
           mode->hdisplay, mode->vdisplay);

    if (wait_flip_event(fd, &ev) == 0)
        printf("  [PASS] page-flip event 1 via poll/read (seq=%u)\n",
               ev.sequence);
    else
        fail("page-flip event 1", errno);

    /* True flip: the next commit changes only the plane's FB_ID. */
    memset(&req, 0, sizeof(req));
    n = 0;
    props[n] = plane_fb_id;
    values[n++] = fb2;
    req_add(&req, plane_id, props, values, n);
    if (atomic_commit(fd, &req, DRM_MODE_PAGE_FLIP_EVENT)) {
        fail("atomic page flip (fb2)", errno);
    } else {
        pass("atomic page flip (plane FB_ID change)");
        if (wait_flip_event(fd, &ev) == 0)
            printf("  [PASS] page-flip event 2 via poll/read (seq=%u)\n",
                   ev.sequence);
        else
            fail("page-flip event 2", errno);
    }

    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = _DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl) == 0)
        pass("WAIT_VBLANK relative 1 returned");
    else
        fail("WAIT_VBLANK", errno);

    if (cursor_id) {
        if (dumb_create_and_map(fd, 64, 64, &cdumb, &cpitch, &csize, &cmap)) {
            fail("cursor CREATE_DUMB/MAP_DUMB", errno);
        } else {
            memset(cmap, 0x80, csize);
            if (add_fb2(fd, cdumb, 64, 64, cpitch, DRM_FORMAT_ARGB8888, &cfb)) {
                fail("cursor ADDFB2", errno);
            } else if (cursor_plane_commit(fd, cursor_id, &cursor_props,
                                           crtc_id, cfb, 100, 100, 64, 64)) {
                fail("cursor plane commit", errno);
            } else {
                pass("cursor plane commit");
                if (cursor_plane_commit(fd, cursor_id, &cursor_props, 0, 0, 0,
                                        0, 64, 64) == 0)
                    pass("cursor plane off");
                else
                    fail("cursor plane off", errno);
            }
        }
    }

    /* Disable: connector detached, plane off, CRTC MODE_ID/ACTIVE cleared. */
    memset(&req, 0, sizeof(req));
    n = 0;
    props[n] = conn_crtc_id;
    values[n++] = 0;
    req_add(&req, pick.conn_id, props, values, n);
    n = 0;
    props[n] = crtc_mode_id;
    values[n++] = 0;
    props[n] = crtc_active;
    values[n++] = 0;
    req_add(&req, crtc_id, props, values, n);
    n = 0;
    props[n] = plane_fb_id;
    values[n++] = 0;
    props[n] = plane_crtc_id;
    values[n++] = 0;
    req_add(&req, plane_id, props, values, n);
    if (atomic_commit(fd, &req, DRM_MODE_ATOMIC_ALLOW_MODESET) == 0)
        pass("atomic disable commit");
    else
        fail("atomic disable commit", errno);

out_all:
    if (cfb) {
        uint32_t id = cfb;

        ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (fb2) {
        uint32_t id = fb2;

        ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (fb1) {
        uint32_t id = fb1;

        ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (blob.blob_id) {
        memset(&dblob, 0, sizeof(dblob));
        dblob.blob_id = blob.blob_id;
        ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &dblob);
    }
    if (cmap)
        munmap(cmap, csize);
    if (map2)
        munmap(map2, dsize2);
    if (map1)
        munmap(map1, dsize1);
    if (cdumb) {
        memset(&ddumb, 0, sizeof(ddumb));
        ddumb.handle = cdumb;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ddumb);
    }
    if (dumb2) {
        memset(&ddumb, 0, sizeof(ddumb));
        ddumb.handle = dumb2;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ddumb);
    }
    if (dumb1) {
        memset(&ddumb, 0, sizeof(ddumb));
        ddumb.handle = dumb1;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ddumb);
    }
    free(modes);

out_unforce:
    if (forced)
        sysfs_write_str(status_path, "detect");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    char node[64] = {0}, rnode[64] = {0};
    const char *card;
    int fd, rfd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS Phase 6 C5/C6 KPI amdgpu test ===\n");

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

    card = strrchr(node, '/');
    test_kms(fd, card ? card + 1 : node);

    close(fd);
    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
