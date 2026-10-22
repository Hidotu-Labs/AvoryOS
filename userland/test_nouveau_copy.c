#ifndef NOUVEAU_COPY_SHARED_MEMORY
#define main nouveau_memory_oracle_main
#include "test_nouveau_memory.c"
#undef main
#endif
#include <time.h>
#include <nvif/ioctl.h>
#include <drf.h>
#include <cl906f.h>
#include <clc7b5.h>

_Static_assert(sizeof(struct nvif_ioctl_v0) == 24, "NVIF header ABI");
_Static_assert(sizeof(struct nvif_ioctl_new_v0) == 32, "NVIF NEW ABI");

struct copy_buffer {
    struct drm_nouveau_gem_new object;
    unsigned char *mapping;
    uint64_t address;
    int bound;
};

static uint32_t copy_header(uint32_t method, unsigned int count) {
    return NVDEF(NV906F, DMA_INCR, OPCODE, VALUE) |
           NVVAL(NV906F, DMA_INCR, COUNT, count) |
           NVVAL(NV906F, DMA_INCR, SUBCHANNEL, 4) |
           NVVAL(NV906F, DMA_INCR, ADDRESS, method >> 2);
}

static size_t copy_commands(uint32_t *words, uint64_t source, uint64_t destination, uint32_t size) {
    words[0] = copy_header(NVC7B5_OFFSET_IN_UPPER, 8);
    words[1] = source >> 32;
    words[2] = source;
    words[3] = destination >> 32;
    words[4] = destination;
    words[5] = size;
    words[6] = size;
    words[7] = size;
    words[8] = 1;
    words[9] = copy_header(NVC7B5_LAUNCH_DMA, 1);
    words[10] = NVDEF(NVC7B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                NVDEF(NVC7B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                NVDEF(NVC7B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                NVDEF(NVC7B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
    return 11;
}

static int copy_software(void) {
    uint32_t words[13];
    for (unsigned int index = 0; index < 13; index++)
        words[index] = 0xdeadbeef;
    size_t count = copy_commands(words, 0x123456789ULL, 0x987654321ULL, 8256);
    if (count != 11 || words[11] != 0xdeadbeef || words[12] != 0xdeadbeef ||
        NVVAL_GET(words[0], NV906F, DMA_INCR, COUNT) != 8 ||
        NVVAL_GET(words[0], NV906F, DMA_INCR, SUBCHANNEL) != 4 ||
        NVVAL_GET(words[0], NV906F, DMA_INCR, ADDRESS) != (NVC7B5_OFFSET_IN_UPPER >> 2) ||
        words[1] != 1 || words[2] != 0x23456789 || words[3] != 9 ||
        words[4] != 0x87654321 || words[7] != 8256 || words[8] != 1 || words[10] != 0x186)
        return 1;
    puts("NOUVEAU_C5_COPY: SOFTWARE push encoding and canaries pass; no GPU access");
    return 0;
}

static int nvif_ioctl(int descriptor, void *request, size_t size) {
    return ioctl(descriptor, _IOC(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE,
                                DRM_COMMAND_BASE + DRM_NOUVEAU_NVIF, size), request);
}

static int copy_object(int descriptor, int channel) {
    struct {
        struct nvif_ioctl_v0 header;
        struct nvif_ioctl_sclass_v0 query;
        struct nvif_ioctl_sclass_oclass_v0 classes[32];
    } query = {.header = {.type = NVIF_IOCTL_V0_SCLASS, .route = 0xff, .token = channel},
               .query = {.count = 32}};
    if (nvif_ioctl(descriptor, &query, sizeof(query)))
        return -1;
    if (query.query.count > 32) {
        errno = EOVERFLOW;
        return -1;
    }
    uint32_t selected = 0;
    for (unsigned int index = 0; index < query.query.count; index++) {
        uint32_t candidate = query.classes[index].oclass;
        if (candidate == AMPERE_DMA_COPY_B)
            selected = candidate;
    }
    if (!selected) {
        errno = ENOTSUP;
        return -1;
    }
    struct {
        struct nvif_ioctl_v0 header;
        struct nvif_ioctl_new_v0 object;
    } request = {.header = {.type = NVIF_IOCTL_V0_NEW, .route = 0xff, .token = channel},
                 .object = {.route = NVIF_IOCTL_V0_ROUTE_NVIF, .oclass = selected}};
    request.object.token = request.object.object = 0xc5000001;
    if (nvif_ioctl(descriptor, &request, sizeof(request)))
        return -1;
    printf("NOUVEAU_C5_COPY: channel=%d class=%04x\n", channel, selected);
    return 0;
}

static int create_copy_buffer(int descriptor, struct copy_buffer *buffer, uint32_t domain, uint64_t address) {
    buffer->mapping = MAP_FAILED;
    buffer->address = address;
    buffer->object.info.size = 16384;
    buffer->object.info.domain = domain | NOUVEAU_GEM_DOMAIN_MAPPABLE;
    buffer->object.align = 4096;
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_GEM_NEW, &buffer->object))
        return -1;
    if (buffer->object.info.size != 16384) {
        errno = EINVAL;
        return -1;
    }
    buffer->mapping = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED,
                           descriptor, buffer->object.info.map_handle);
    if (buffer->mapping == MAP_FAILED)
        return -1;
    struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_MAP,
        .handle = buffer->object.info.handle, .addr = address, .range = 16384};
    if (bind_one(descriptor, &operation))
        return -1;
    buffer->bound = 1;
    return 0;
}

static int copy_case(const char *path, unsigned int seed, int timeline) {
    struct copy_buffer buffers[4] = {0};
    for (unsigned int index = 0; index < 4; index++)
        buffers[index].mapping = MAP_FAILED;
    int descriptor = open(path, O_RDWR | O_CLOEXEC);
    int result = 1, channel_live = 0;
    const char *stage = "open copy client";
    struct drm_nouveau_channel_alloc channel = {.fb_ctxdma_handle = UINT32_MAX,
                                               .tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_CE};
    struct drm_syncobj_create sync = {0};
    if (descriptor < 0)
        return failed(stage);
    struct drm_nouveau_vm_init init = {.kernel_managed_addr = 1ULL << 38,
                                      .kernel_managed_size = 1ULL << 30};
    stage = "copy VM_INIT";
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_VM_INIT, &init))
        goto cleanup;
    stage = "CE CHANNEL_ALLOC";
    puts("NOUVEAU_C5_COPY: creating CE channel");
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &channel))
        goto cleanup;
    channel_live = 1;
    stage = "query/create AMPERE_DMA_COPY_B object";
    if (copy_object(descriptor, channel.channel))
        goto cleanup;
    stage = "SYNCOBJ_CREATE";
    if (ioctl(descriptor, DRM_IOCTL_SYNCOBJ_CREATE, &sync))
        goto cleanup;
    stage = "copy GEM/map/VM_BIND";
    for (unsigned int index = 0; index < 4; index++) {
        uint32_t domain = index == 1 ? NOUVEAU_GEM_DOMAIN_VRAM : NOUVEAU_GEM_DOMAIN_GART;
        if (create_copy_buffer(descriptor, &buffers[index], domain, 0x1000000ULL * (index + 1)))
            goto cleanup;
        if (cpu_access(descriptor, buffers[index].object.info.handle, 1))
            goto cleanup;
    }
    pattern_write(buffers[0].mapping, 16384, seed);
    memset(buffers[1].mapping, 0xa5, 16384);
    memset(buffers[2].mapping, 0x3c, 16384);
    uint32_t *words = (uint32_t *)buffers[3].mapping;
    size_t count = copy_commands(words, buffers[0].address + 256, buffers[1].address + 512, 8256);
    count += copy_commands(words + count, buffers[1].address + 512, buffers[2].address + 128, 8256);
    for (unsigned int index = 0; index < 4; index++) {
        if (cpu_access(descriptor, buffers[index].object.info.handle, 0))
            goto cleanup;
    }
    struct drm_nouveau_sync bind_signal = {.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ,
        .handle = sync.handle, .timeline_value = 1};
    if (timeline) {
        stage = "asynchronous VM_BIND with timeline signal";
        struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_UNMAP,
            .addr = buffers[3].address, .range = 16384};
        if (bind_one(descriptor, &operation))
            goto cleanup;
        buffers[3].bound = 0;
        operation.op = DRM_NOUVEAU_VM_BIND_OP_MAP;
        operation.handle = buffers[3].object.info.handle;
        struct drm_nouveau_vm_bind bind = {.op_count = 1, .flags = DRM_NOUVEAU_VM_BIND_RUN_ASYNC,
            .op_ptr = (uintptr_t)&operation, .sig_count = 1, .sig_ptr = (uintptr_t)&bind_signal};
        if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_VM_BIND, &bind))
            goto cleanup;
        buffers[3].bound = 1;
    }
    struct drm_nouveau_sync signal = {.flags = timeline ? DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ : DRM_NOUVEAU_SYNC_SYNCOBJ,
        .handle = sync.handle, .timeline_value = timeline ? 2 : 0};
    struct drm_nouveau_exec_push push = {.va = buffers[3].address, .va_len = count * 4};
    struct drm_nouveau_exec execute = {.channel = channel.channel, .push_count = 1,
        .sig_count = 1, .sig_ptr = (uintptr_t)&signal, .push_ptr = (uintptr_t)&push};
    if (timeline) {
        execute.wait_count = 1;
        execute.wait_ptr = (uintptr_t)&bind_signal;
    }
    stage = "EXEC GPU copy";
    puts("NOUVEAU_C5_COPY: submitting GART -> VRAM -> GART");
    if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_EXEC, &execute))
        goto cleanup;
    struct timespec now;
    stage = "bounded GPU fence wait";
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        goto cleanup;
    int64_t deadline = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + 10000000000LL;
    if (timeline) {
        uint64_t point = 2;
        struct drm_syncobj_timeline_wait wait = {.handles = (uintptr_t)&sync.handle,
            .points = (uintptr_t)&point, .count_handles = 1,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
            .timeout_nsec = deadline};
        if (ioctl(descriptor, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait))
            goto cleanup;
        uint64_t completed = 0;
        struct drm_syncobj_timeline_array query = {.handles = (uintptr_t)&sync.handle,
            .points = (uintptr_t)&completed, .count_handles = 1};
        stage = "timeline completed-point query";
        if (ioctl(descriptor, DRM_IOCTL_SYNCOBJ_QUERY, &query))
            goto cleanup;
        if (completed != 2) {
            errno = EIO;
            goto cleanup;
        }
        stage = "unsubmitted timeline point times out";
        point = 3;
        wait.timeout_nsec = 0;
        if (expect_error(ioctl(descriptor, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait), ETIME, stage))
            goto cleanup;
    } else {
        struct drm_syncobj_wait wait = {.handles = (uintptr_t)&sync.handle, .count_handles = 1,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, .timeout_nsec = deadline};
        if (ioctl(descriptor, DRM_IOCTL_SYNCOBJ_WAIT, &wait))
            goto cleanup;
    }
    stage = "GPU copy bytes/canaries";
    for (unsigned int index = 0; index < 3; index++) {
        if (cpu_access(descriptor, buffers[index].object.info.handle, 1))
            goto cleanup;
    }
    if (pattern_check(buffers[0].mapping, 16384, 0, seed) ||
        pattern_check(buffers[1].mapping + 512, 8256, 256, seed) ||
        pattern_check(buffers[2].mapping + 128, 8256, 256, seed))
        goto cleanup;
    for (size_t offset = 0; offset < 16384; offset++) {
        if ((offset < 512 || offset >= 512 + 8256) && buffers[1].mapping[offset] != 0xa5)
            goto cleanup;
        if ((offset < 128 || offset >= 128 + 8256) && buffers[2].mapping[offset] != 0x3c)
            goto cleanup;
    }
    for (unsigned int index = 0; index < 3; index++) {
        if (cpu_access(descriptor, buffers[index].object.info.handle, 0))
            goto cleanup;
    }
    printf("NOUVEAU_C5_COPY: PASS EXEC/fence GART->VRAM->GART bytes=8256 seed=%u canaries intact\n", seed);
    if (timeline)
        puts("NOUVEAU_C5_COPY: PASS async VM_BIND point=1 -> EXEC wait=1 signal=2 timeline wait/query/future timeout");
    result = 0;
cleanup:
    if (result)
        failed(stage);
    if (sync.handle) {
        struct drm_syncobj_destroy destroy = {.handle = sync.handle};
        if (ioctl(descriptor, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
            result = failed("SYNCOBJ_DESTROY");
    }
    if (channel_live) {
        struct drm_nouveau_channel_free release = {.channel = channel.channel};
        if (ioctl(descriptor, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &release))
            result = failed("CHANNEL_FREE");
    }
    for (unsigned int index = 0; index < 4; index++) {
        if (buffers[index].bound) {
            struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_UNMAP,
                .addr = buffers[index].address, .range = 16384};
            if (bind_one(descriptor, &operation))
                result = failed("copy VM UNMAP");
        }
        if (buffers[index].mapping != MAP_FAILED && munmap(buffers[index].mapping, 16384))
            result = failed("copy munmap");
        if (buffers[index].object.info.handle && gem_close(descriptor, buffers[index].object.info.handle))
            result = failed("copy GEM_CLOSE");
    }
    if (close(descriptor))
        result = failed("copy fd close");
    return result;
}

#ifndef NOUVEAU_COPY_ENTRY
#define NOUVEAU_COPY_ENTRY main
#endif
int NOUVEAU_COPY_ENTRY(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--software"))
        return copy_software();
    if (argc != 1)
        return 2;
    puts("NOUVEAU_C5_COPY: START execution");
    alarm(90);
    char path[64];
    if (find_device(path, sizeof(path)))
        return 1;
    for (unsigned int iteration = 0; iteration < 2; iteration++) {
        if (copy_case(path, 17 + iteration, iteration == 1))
            return 1;
    }
    alarm(0);
    puts("NOUVEAU_C5_COPY: END execution status=pass");
    return 0;
}
