#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef __user
#define __user
#endif
#include <nouveau_drm.h>

_Static_assert(sizeof(struct drm_nouveau_gem_new) == 48, "GEM_NEW ABI");
_Static_assert(sizeof(struct drm_nouveau_vm_init) == 16, "VM_INIT ABI");
_Static_assert(sizeof(struct drm_nouveau_vm_bind) == 40, "VM_BIND ABI");
_Static_assert(sizeof(struct drm_nouveau_vm_bind_op) == 40, "VM_BIND operation ABI");
_Static_assert(sizeof(off_t) == 8, "64-bit mmap offsets required");

static int failed(const char *stage) {
    fprintf(stderr, "NOUVEAU_C5: FAIL %s errno=%d (%s)\n", stage, errno, strerror(errno));
    return 1;
}

static int expect_error(int result, int expected, const char *stage) {
    if (result == -1 && errno == expected)
        return 0;
    fprintf(stderr, "NOUVEAU_C5: FAIL %s result=%d errno=%d expected=%d\n",
            stage, result, errno, expected);
    return 1;
}

static unsigned char pattern_byte(size_t offset, unsigned int seed) {
    return (unsigned char)((offset * 131u) ^ (offset >> 8) ^ (seed * 17u));
}

static void pattern_write(unsigned char *memory, size_t size, unsigned int seed) {
    for (size_t offset = 0; offset < size; offset++)
        memory[offset] = pattern_byte(offset, seed);
}

static int pattern_check(const unsigned char *memory, size_t size,
                         size_t base, unsigned int seed) {
    for (size_t offset = 0; offset < size; offset++) {
        if (memory[offset] != pattern_byte(base + offset, seed)) {
            fprintf(stderr, "NOUVEAU_C5: mismatch offset=%zu actual=%02x expected=%02x\n",
                    base + offset, memory[offset], pattern_byte(base + offset, seed));
            return 1;
        }
    }
    return 0;
}

static int software_test(void) {
    unsigned char memory[3 * 4096 + 137];
    pattern_write(memory, sizeof(memory), 37);
    if (pattern_check(memory, sizeof(memory), 0, 37) ||
        pattern_check(memory + 4096, 4096, 4096, 37))
        return 1;
    memory[4096] ^= 1;
    if (!pattern_check(memory, sizeof(memory), 0, 37))
        return 1;
    puts("NOUVEAU_C5: SOFTWARE pattern and corruption oracle pass; no GPU access");
    return 0;
}

static int find_device(char *path, size_t capacity) {
    for (unsigned int minor = 128; minor < 144; minor++) {
        char name[64] = {0};
        struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
        snprintf(path, capacity, "/dev/dri/renderD%u", minor);
        int descriptor = open(path, O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        int result = ioctl(descriptor, DRM_IOCTL_VERSION, &version);
        if (!result && !strcmp(name, "nouveau")) {
            struct drm_nouveau_getparam vendor = {.param = NOUVEAU_GETPARAM_PCI_VENDOR};
            struct drm_nouveau_getparam device = {.param = NOUVEAU_GETPARAM_PCI_DEVICE};
            if (!ioctl(descriptor, DRM_IOCTL_NOUVEAU_GETPARAM, &vendor) &&
                !ioctl(descriptor, DRM_IOCTL_NOUVEAU_GETPARAM, &device) &&
                vendor.value == 0x10de && device.value == 0x2803) {
                printf("NOUVEAU_C5: device=%s driver=nouveau pci=10de:2803\n", path);
                close(descriptor);
                return 0;
            }
        }
        close(descriptor);
    }
    errno = ENODEV;
    return failed("find AD106 Nouveau render node");
}

static int gem_close(int descriptor, uint32_t handle) {
    struct drm_gem_close request = {.handle = handle};
    return ioctl(descriptor, DRM_IOCTL_GEM_CLOSE, &request);
}

static int cpu_access(int descriptor, uint32_t handle, int begin) {
    if (begin) {
        struct drm_nouveau_gem_cpu_prep request = {
            .handle = handle,
            .flags = NOUVEAU_GEM_CPU_PREP_WRITE | NOUVEAU_GEM_CPU_PREP_NOWAIT
        };
        return ioctl(descriptor, DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, &request);
    }
    struct drm_nouveau_gem_cpu_fini request = {.handle = handle};
    return ioctl(descriptor, DRM_IOCTL_NOUVEAU_GEM_CPU_FINI, &request);
}

static int memory_case(const char *path, uint32_t domain, unsigned int seed) {
    struct drm_nouveau_gem_new object = {
        .info = {.size = 3 * 4096 + 137, .domain = domain | NOUVEAU_GEM_DOMAIN_MAPPABLE},
        .align = 4096
    };
    struct drm_nouveau_gem_new peer_object = {0};
    unsigned char *first = MAP_FAILED, *alias = MAP_FAILED, *partial = MAP_FAILED;
    unsigned char *peer_mapping = MAP_FAILED;
    int owner = -1, peer = -1, result = 1;
    int handle_live = 0;
    const char *stage = "open clients";

    owner = open(path, O_RDWR | O_CLOEXEC);
    peer = open(path, O_RDWR | O_CLOEXEC);
    if (owner < 0 || peer < 0)
        goto cleanup;
    stage = "GEM_NEW";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_NEW, &object))
        goto cleanup;
    handle_live = 1;
    if (!object.info.handle || object.info.size < 3 * 4096 + 137 ||
        object.info.size > 1024 * 1024 || (object.info.size & 4095) ||
        !object.info.map_handle || (object.info.map_handle & 4095) ||
        object.info.domain != domain) {
        errno = EINVAL;
        stage = "GEM_NEW geometry/domain";
        goto cleanup;
    }
    struct drm_nouveau_gem_info info = {.handle = object.info.handle};
    stage = "GEM_INFO";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_INFO, &info) ||
        info.size != object.info.size || info.map_handle != object.info.map_handle)
        goto cleanup;
    stage = "foreign GEM handle isolation";
    if (expect_error(ioctl(peer, DRM_IOCTL_NOUVEAU_GEM_INFO, &info), ENOENT, stage))
        goto cleanup;
    void *foreign = mmap(NULL, object.info.size, PROT_READ, MAP_SHARED, peer, object.info.map_handle);
    stage = "foreign mmap isolation";
    if (foreign != MAP_FAILED) {
        munmap(foreign, object.info.size);
        errno = EACCES;
        goto cleanup;
    }
    if (errno != EACCES)
        goto cleanup;
    struct drm_nouveau_vm_init legacy_init = {.kernel_managed_addr = 1ULL << 38,
                                              .kernel_managed_size = 1ULL << 30};
    stage = "legacy GEM forbids later VM_INIT";
    if (expect_error(ioctl(owner, DRM_IOCTL_NOUVEAU_VM_INIT, &legacy_init), ENOSYS, stage))
        goto cleanup;
    stage = "GEM mappings";
    first = mmap(NULL, object.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, owner, object.info.map_handle);
    alias = mmap(NULL, object.info.size, PROT_READ | PROT_WRITE, MAP_SHARED, owner, object.info.map_handle);
    partial = mmap(NULL, 4096, PROT_READ, MAP_SHARED, owner, object.info.map_handle);
    if (first == MAP_FAILED || alias == MAP_FAILED || partial == MAP_FAILED)
        goto cleanup;
    stage = "CPU_PREP";
    if (cpu_access(owner, object.info.handle, 1))
        goto cleanup;
    pattern_write(first, object.info.size, seed);
    stage = "CPU_FINI";
    if (cpu_access(owner, object.info.handle, 0))
        goto cleanup;
    stage = "mapped byte/alias verification";
    if (pattern_check(alias, object.info.size, 0, seed) || pattern_check(partial, 4096, 0, seed))
        goto cleanup;
    stage = "GEM_CLOSE with live mappings";
    if (gem_close(owner, object.info.handle))
        goto cleanup;
    handle_live = 0;
    info.handle = object.info.handle;
    stage = "stale handle rejected";
    if (expect_error(ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_INFO, &info), ENOENT, stage))
        goto cleanup;
    if (close(owner)) {
        stage = "owner fd close";
        owner = -1;
        goto cleanup;
    }
    owner = -1;
    peer_object.info.size = object.info.size;
    peer_object.info.domain = domain | NOUVEAU_GEM_DOMAIN_MAPPABLE;
    peer_object.align = 4096;
    stage = "new client allocation while old mappings survive";
    if (ioctl(peer, DRM_IOCTL_NOUVEAU_GEM_NEW, &peer_object))
        goto cleanup;
    if (peer_object.info.size != object.info.size) {
        errno = EINVAL;
        goto cleanup;
    }
    peer_mapping = mmap(NULL, peer_object.info.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, peer, peer_object.info.map_handle);
    if (peer_mapping == MAP_FAILED || cpu_access(peer, peer_object.info.handle, 1))
        goto cleanup;
    pattern_write(peer_mapping, peer_object.info.size, seed + 73);
    if (cpu_access(peer, peer_object.info.handle, 0))
        goto cleanup;
    stage = "mapping survives handle and fd close without client aliasing";
    if (pattern_check(first, object.info.size, 0, seed) ||
        pattern_check(alias, object.info.size, 0, seed) || pattern_check(partial, 4096, 0, seed))
        goto cleanup;
    result = 0;
    printf("NOUVEAU_C5: PASS %s bytes=%llu seed=%u aliases/partial/ownership/lifetime\n",
           domain == NOUVEAU_GEM_DOMAIN_GART ? "GART" : "VRAM",
           (unsigned long long)object.info.size, seed);

cleanup:
    if (result)
        failed(stage);
    if (peer_mapping != MAP_FAILED && munmap(peer_mapping, peer_object.info.size))
        result = failed("peer munmap");
    if (peer_object.info.handle && gem_close(peer, peer_object.info.handle))
        result = failed("peer GEM_CLOSE");
    if (partial != MAP_FAILED && munmap(partial, 4096))
        result = failed("partial munmap");
    if (alias != MAP_FAILED && munmap(alias, object.info.size))
        result = failed("alias munmap");
    if (first != MAP_FAILED && munmap(first, object.info.size))
        result = failed("first munmap");
    if (handle_live && gem_close(owner, object.info.handle))
        result = failed("owner GEM_CLOSE");
    if (owner >= 0 && close(owner))
        result = failed("owner close");
    if (peer >= 0 && close(peer))
        result = failed("peer close");
    return result;
}

static int bind_one(int descriptor, struct drm_nouveau_vm_bind_op *operation) {
    struct drm_nouveau_vm_bind request = {.op_count = 1, .op_ptr = (uintptr_t)operation};
    return ioctl(descriptor, DRM_IOCTL_NOUVEAU_VM_BIND, &request);
}

static int vm_case(const char *path) {
    int descriptor = open(path, O_RDWR | O_CLOEXEC);
    int result = 1;
    const char *stage = "open explicit VM client";
    struct drm_nouveau_gem_new object = {0};
    if (descriptor < 0)
        return failed(stage);
    struct drm_nouveau_vm_init invalid = {.kernel_managed_addr = UINT64_MAX - 4095,
                                         .kernel_managed_size = 8192};
    stage = "VM_INIT overflow rejected";
    if (expect_error(ioctl(descriptor, DRM_IOCTL_NOUVEAU_VM_INIT, &invalid), EINVAL, stage))
        goto cleanup;
    struct drm_nouveau_vm_init init = {.kernel_managed_addr = 1ULL << 38,
                                      .kernel_managed_size = 1ULL << 30};
    stage = "VM_INIT";
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_VM_INIT, &init))
        goto cleanup;
    object.info.size = 8192;
    object.info.domain = NOUVEAU_GEM_DOMAIN_GART;
    object.align = 4096;
    stage = "explicit VM GEM_NEW";
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_GEM_NEW, &object))
        goto cleanup;
    struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_MAP,
        .handle = UINT32_MAX, .addr = 0x2000000, .range = 8192};
    stage = "VM_BIND invalid handle rejected";
    if (expect_error(bind_one(descriptor, &operation), ENOENT, stage))
        goto cleanup;
    operation.handle = object.info.handle;
    operation.addr++;
    stage = "VM_BIND unaligned address rejected";
    if (expect_error(bind_one(descriptor, &operation), EINVAL, stage))
        goto cleanup;
    operation.addr--;
    stage = "synchronous VM_BIND MAP";
    if (bind_one(descriptor, &operation))
        goto cleanup;
    stage = "mapped GEM handle close";
    if (gem_close(descriptor, object.info.handle))
        goto cleanup;
    object.info.handle = 0;
    operation.op = DRM_NOUVEAU_VM_BIND_OP_UNMAP;
    operation.handle = 0;
    stage = "synchronous VM_BIND UNMAP after handle close";
    if (bind_one(descriptor, &operation))
        goto cleanup;
    puts("NOUVEAU_C5: PASS VM_INIT/synchronous MAP/UNMAP/negative ranges/handle lifetime");
    result = 0;
cleanup:
    if (result)
        failed(stage);
    if (object.info.handle && gem_close(descriptor, object.info.handle))
        result = failed("VM GEM_CLOSE");
    if (close(descriptor))
        result = failed("VM fd close");
    return result;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--software"))
        return software_test();
    if (argc != 1)
        return 2;
    char path[64];
    puts("NOUVEAU_C5: START memory-vm");
    alarm(90);
    if (find_device(path, sizeof(path)))
        return 1;
    for (unsigned int iteration = 0; iteration < 2; iteration++) {
        if (memory_case(path, NOUVEAU_GEM_DOMAIN_GART, 11 + iteration) ||
            memory_case(path, NOUVEAU_GEM_DOMAIN_VRAM, 41 + iteration))
            return 1;
    }
    if (vm_case(path))
        return 1;
    alarm(0);
    puts("NOUVEAU_C5: END memory-vm status=pass");
    return 0;
}
