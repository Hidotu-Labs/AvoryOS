#define main nouveau_memory_oracle_main
#include "test_nouveau_memory.c"
#undef main
#include "dma-buf.h"

static int prime_failed(const char *stage) {
    fprintf(stderr, "NOUVEAU_C6_PRIME: FAIL %s errno=%d (%s)\n", stage, errno, strerror(errno));
    return 1;
}

static int find_sink(void) {
    for (unsigned int minor = 0; minor < 16; minor++) {
        char path[64], name[64] = {0}, busid[64] = {0};
        snprintf(path, sizeof(path), "/dev/dri/card%u", minor);
        int descriptor = open(path, O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
        struct drm_unique unique = {.unique_len = sizeof(busid) - 1, .unique = busid};
        struct drm_get_cap capability = {.capability = DRM_CAP_PRIME};
        if (!ioctl(descriptor, DRM_IOCTL_VERSION, &version) && !strcmp(name, "ascentdrm") &&
            !ioctl(descriptor, DRM_IOCTL_GET_UNIQUE, &unique) && !strcmp(busid, "pci:0000:00:06.0") &&
            !ioctl(descriptor, DRM_IOCTL_GET_CAP, &capability) &&
            (capability.value & DRM_PRIME_CAP_IMPORT)) {
            printf("NOUVEAU_C6_PRIME: sink=%s driver=%s busid=%s import=1\n", path, name, busid);
            return descriptor;
        }
        if (close(descriptor))
            return -1;
    }
    errno = ENODEV;
    return -1;
}

static int prime_case(const char *path, unsigned int seed) {
    int owner = -1, sink = -1, shared = -1, reexported = -1;
    uint32_t imported = 0;
    unsigned char *source = MAP_FAILED, *destination = MAP_FAILED;
    struct drm_nouveau_gem_new object = {.info = {
        .size = 32768, .domain = NOUVEAU_GEM_DOMAIN_GART | NOUVEAU_GEM_DOMAIN_MAPPABLE
    }, .align = 4096};
    int result = 1;
    const char *stage = "open devices";
    owner = open(path, O_RDWR | O_CLOEXEC);
    sink = find_sink();
    if (owner < 0 || sink < 0)
        goto cleanup;
    stage = "Nouveau GART allocation";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_NEW, &object))
        goto cleanup;
    if (object.info.size != 32768) {
        errno = EPROTO;
        goto cleanup;
    }
    stage = "exporter mapping";
    source = mmap(NULL, object.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, owner, object.info.map_handle);
    if (source == MAP_FAILED)
        goto cleanup;
    stage = "exporter CPU access";
    if (cpu_access(owner, object.info.handle, 1))
        goto cleanup;
    pattern_write(source, object.info.size, seed);
    if (cpu_access(owner, object.info.handle, 0))
        goto cleanup;
    struct drm_prime_handle export = {.handle = object.info.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    stage = "Nouveau PRIME export";
    if (ioctl(owner, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export))
        goto cleanup;
    shared = export.fd;
    struct drm_prime_handle import = {.fd = shared};
    stage = "VirtIO PRIME import";
    if (ioctl(sink, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import))
        goto cleanup;
    imported = import.handle;
    struct drm_prime_handle repeated = {.fd = shared};
    stage = "repeated import identity";
    if (ioctl(sink, DRM_IOCTL_PRIME_FD_TO_HANDLE, &repeated) || repeated.handle != imported)
        goto cleanup;
    struct drm_mode_map_dumb mapping = {.handle = imported};
    stage = "import mapping token";
    if (ioctl(sink, DRM_IOCTL_MODE_MAP_DUMB, &mapping))
        goto cleanup;
    stage = "import CPU mapping";
    destination = mmap(NULL, object.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, sink, mapping.offset);
    if (destination == MAP_FAILED)
        goto cleanup;
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
    stage = "import CPU sync and bytes";
    if (ioctl(shared, DMA_BUF_IOCTL_SYNC, &sync))
        goto cleanup;
    int mismatch = pattern_check(destination, object.info.size, 0, seed);
    if (!mismatch)
        pattern_write(destination, object.info.size, seed + 1);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    if (ioctl(shared, DMA_BUF_IOCTL_SYNC, &sync) || mismatch)
        goto cleanup;
    stage = "exporter sees importer write";
    if (cpu_access(owner, object.info.handle, 1))
        goto cleanup;
    mismatch = pattern_check(source, object.info.size, 0, seed + 1);
    if (cpu_access(owner, object.info.handle, 0) || mismatch)
        goto cleanup;
    stage = "release exporter references";
    if (munmap(source, object.info.size))
        goto cleanup;
    source = MAP_FAILED;
    if (gem_close(owner, object.info.handle))
        goto cleanup;
    object.info.handle = 0;
    int close_result = close(shared);
    shared = -1;
    if (close_result)
        goto cleanup;
    close_result = close(owner);
    owner = -1;
    if (close_result)
        goto cleanup;
    struct drm_prime_handle reexport = {.handle = imported, .flags = DRM_CLOEXEC | DRM_RDWR};
    stage = "import pins exporter after owner/fd close";
    if (ioctl(sink, DRM_IOCTL_PRIME_HANDLE_TO_FD, &reexport))
        goto cleanup;
    reexported = reexport.fd;
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    if (ioctl(reexported, DMA_BUF_IOCTL_SYNC, &sync))
        goto cleanup;
    mismatch = pattern_check(destination, object.info.size, 0, seed + 1);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(reexported, DMA_BUF_IOCTL_SYNC, &sync) || mismatch)
        goto cleanup;
    result = 0;
cleanup:
    if (result)
        prime_failed(stage);
    if (destination != MAP_FAILED && munmap(destination, object.info.size))
        result = prime_failed("unmap import");
    if (source != MAP_FAILED && munmap(source, object.info.size))
        result = prime_failed("unmap exporter");
    if (imported && gem_close(sink, imported))
        result = prime_failed("close imported GEM");
    if (object.info.handle && gem_close(owner, object.info.handle))
        result = prime_failed("close exported GEM");
    if (reexported >= 0 && close(reexported))
        result = prime_failed("close reexport fd");
    if (shared >= 0 && close(shared))
        result = prime_failed("close export fd");
    if (sink >= 0 && close(sink))
        result = prime_failed("close sink");
    if (owner >= 0 && close(owner))
        result = prime_failed("close exporter");
    if (!result)
        printf("NOUVEAU_C6_PRIME: PASS Nouveau->VirtIO GART bytes=32768 seed=%u alias/reimport/reexport/lifetime; presentation not tested\n", seed);
    return result;
}

#ifndef NOUVEAU_PRIME_ENTRY
#define NOUVEAU_PRIME_ENTRY main
#endif
int NOUVEAU_PRIME_ENTRY(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--software")) {
        if (software_test())
            return 1;
        puts("NOUVEAU_C6_PRIME: SOFTWARE pattern oracle pass; no GPU access");
        return 0;
    }
    if (argc != 1)
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(90);
    puts("NOUVEAU_C6_PRIME: START sharing");
    char path[64];
    if (find_device(path, sizeof(path)) || prime_case(path, 41) || prime_case(path, 42))
        return 1;
    puts("NOUVEAU_C6_PRIME: END sharing status=pass");
    return 0;
}
