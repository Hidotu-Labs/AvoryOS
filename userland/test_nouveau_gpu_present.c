#define NOUVEAU_PRESENT_ENTRY nouveau_cpu_present_main
#include "test_nouveau_present.c"
#define NOUVEAU_COPY_SHARED_MEMORY 1
#define NOUVEAU_COPY_ENTRY nouveau_copy_oracle_main
#include "test_nouveau_copy.c"
#ifdef NOUVEAU_PENDING_FLIPS
#include <poll.h>
#include <sys/wait.h>
#include "kpi_dmabuf.h"

static int64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static int selection_result(const char *expected) {
    FILE *output = popen("/bin/drm-pick.sh --pair", "r");
    if (!output) return 1;
    char line[256] = {0};
    int valid = fgets(line, sizeof(line), output) != NULL;
    int status = pclose(output);
    line[strcspn(line, "\n")] = 0;
    return !valid || status || strcmp(line, expected);
}

static int selection_test(int hardware) {
    FILE *inventory = popen("/bin/drm-identify --verbose", "r");
    if (!inventory) return 1;
    char line[512], native[64] = {0}, nvidia[64] = {0}, render[64] = "-", gpu_bus[64] = {0};
    while (fgets(line, sizeof(line), inventory)) {
        char node[64], driver[64], bus[64], paired[64];
        int connected;
        if (sscanf(line, "%63s %63s %63s %d %63s", node, driver, bus, &connected, paired) != 5) {
            pclose(inventory);
            return 1;
        }
        printf("NOUVEAU_C6_SELECT: inventory %s", line);
        if (!strcmp(driver, "ascentdrm")) memcpy(native, node, sizeof(native));
        if (!strcmp(driver, "nouveau")) {
            memcpy(nvidia, node, sizeof(nvidia));
            memcpy(render, paired, sizeof(render));
            memcpy(gpu_bus, bus, sizeof(gpu_bus));
        }
    }
    if (pclose(inventory) || !native[0] || (hardware && (!nvidia[0] || !strcmp(render, "-") ||
        strncmp(gpu_bus, "pci:", 4))))
        return 1;
    char expected[256];
    snprintf(expected, sizeof(expected), "%s %s", native, render);
    if (selection_result(expected)) return 1;
    setenv("ASCENT_DRM_CARD", "card99", 1);
    if (selection_result(expected)) return 1;
    unsetenv("ASCENT_DRM_CARD");
    setenv("ASCENT_DRM_BDF", "ffff:ff:1f.0", 1);
    if (selection_result(expected)) return 1;
    unsetenv("ASCENT_DRM_BDF");
    setenv("ASCENT_DRM_RENDER", "/dev/dri/renderD999", 1);
    if (selection_result(expected)) return 1;
    unsetenv("ASCENT_DRM_RENDER");
    if (hardware) {
        snprintf(expected, sizeof(expected), "%s %s", nvidia, render);
        setenv("ASCENT_DRM_CARD", nvidia, 1);
        if (selection_result(expected)) return 1;
        unsetenv("ASCENT_DRM_CARD");
        setenv("ASCENT_DRM_BDF", gpu_bus, 1);
        if (selection_result(expected)) return 1;
        unsetenv("ASCENT_DRM_BDF");
        setenv("ASCENT_DRM_DRIVER", "nouveau", 1);
        if (selection_result(expected)) return 1;
        unsetenv("ASCENT_DRM_DRIVER");
    }
    printf("NOUVEAU_C6_SELECT: PASS real inventory, paired roles, overrides and unavailable-device fallback hardware=%d\n", hardware);
    return 0;
}

static int flip_event(int descriptor, uint64_t cookie, uint32_t crtc,
                      uint32_t *sequence, uint64_t *timestamp) {
    struct pollfd pending = {.fd = descriptor, .events = POLLIN};
    if (poll(&pending, 1, 5000) != 1 || !(pending.revents & POLLIN))
        return -1;
    struct drm_event_vblank event;
    if (read(descriptor, &event, sizeof(event)) != sizeof(event) ||
        event.base.type != DRM_EVENT_FLIP_COMPLETE || event.base.length != sizeof(event) ||
        event.user_data != cookie || event.crtc_id != crtc || event.tv_usec >= 1000000) {
        errno = EPROTO;
        return -1;
    }
    uint64_t current = (uint64_t)event.tv_sec * 1000000 + event.tv_usec;
    if (*timestamp && ((int32_t)(event.sequence - *sequence) <= 0 || current <= *timestamp)) {
        errno = EPROTO;
        return -1;
    }
    *sequence = event.sequence;
    *timestamp = current;
    if (expect_error(read(descriptor, &event, sizeof(event)), EAGAIN, "empty nonblocking flip read"))
        return -1;
    return 0;
}
#endif

static int gpu_present_case(const char *path) {
    int owner = -1, sink = -1, shared = -1, result = 1, channel_live = 0;
    uint32_t imported = 0, connector = 0, crtc = 0;
    struct drm_nouveau_gem_new objects[3] = {0};
    uint64_t addresses[] = {0x1000000, 0x2000000, 0x3000000};
    size_t sizes[] = {PRESENT_SIZE, PRESENT_SIZE, 16384};
    int bound[3] = {0};
    void *source = MAP_FAILED, *commands = MAP_FAILED;
    struct drm_mode_modeinfo mode = {0};
    struct drm_mode_fb_cmd framebuffer = {
        .width = PRESENT_WIDTH, .height = PRESENT_HEIGHT,
        .pitch = PRESENT_WIDTH * 4, .bpp = 32, .depth = 24
    };
    struct drm_nouveau_channel_alloc channel = {.fb_ctxdma_handle = UINT32_MAX,
        .tt_ctxdma_handle = NOUVEAU_FIFO_ENGINE_CE};
    struct drm_syncobj_create sync = {0};
#ifdef NOUVEAU_PENDING_FLIPS
    struct drm_syncobj_create gate = {0};
    int controller = -1, control_sync = -1, monitor = -1;
    pid_t signaler = -1;
    uint32_t sequence = 0;
    uint64_t timestamp = 0;
#endif
    const char *stage = "open GPU presentation devices";
    owner = open(path, O_RDWR | O_CLOEXEC);
    sink = find_sink();
    if (owner < 0 || sink < 0)
        goto cleanup;
    stage = "select VirtIO presentation mode";
    if (present_mode(sink, &connector, &crtc, &mode))
        goto cleanup;
    struct drm_nouveau_vm_init init = {.kernel_managed_addr = 1ULL << 38,
        .kernel_managed_size = 1ULL << 30};
    stage = "GPU presentation VM_INIT";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_VM_INIT, &init))
        goto cleanup;
    stage = "GPU presentation CE channel";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, &channel))
        goto cleanup;
    channel_live = 1;
    if (copy_object(owner, channel.channel))
        goto cleanup;
    stage = "GPU presentation timeline creation";
    if (ioctl(owner, DRM_IOCTL_SYNCOBJ_CREATE, &sync))
        goto cleanup;
#ifdef NOUVEAU_PENDING_FLIPS
    if (ioctl(owner, DRM_IOCTL_SYNCOBJ_CREATE, &gate) ||
        fcntl(sink, F_SETFL, O_NONBLOCK) < 0)
        goto cleanup;
#endif
    for (unsigned int index = 0; index < 3; index++) {
        objects[index].info.size = sizes[index];
        objects[index].info.domain = NOUVEAU_GEM_DOMAIN_GART | NOUVEAU_GEM_DOMAIN_MAPPABLE;
        objects[index].align = 4096;
        stage = "allocate GPU presentation BO";
        if (ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_NEW, &objects[index]))
            goto cleanup;
        if (objects[index].info.size != sizes[index]) {
            errno = EPROTO;
            goto cleanup;
        }
        struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_MAP,
            .handle = objects[index].info.handle, .addr = addresses[index], .range = sizes[index]};
        stage = "GPU presentation VM_BIND";
        if (bind_one(owner, &operation))
            goto cleanup;
        bound[index] = 1;
    }
    stage = "map source and commands only";
    source = mmap(NULL, PRESENT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, owner, objects[0].info.map_handle);
    commands = mmap(NULL, sizes[2], PROT_READ | PROT_WRITE, MAP_SHARED, owner, objects[2].info.map_handle);
    if (source == MAP_FAILED || commands == MAP_FAILED)
        goto cleanup;
    struct drm_prime_handle export = {.handle = objects[1].info.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    stage = "export GPU destination";
    if (ioctl(owner, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export))
        goto cleanup;
    shared = export.fd;
    struct drm_prime_handle import = {.fd = shared};
    stage = "import GPU destination on VirtIO";
    if (ioctl(sink, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import))
        goto cleanup;
    imported = import.handle;
    framebuffer.handle = imported;
    stage = "ADDFB GPU-produced destination";
    if (ioctl(sink, DRM_IOCTL_MODE_ADDFB, &framebuffer))
        goto cleanup;
    unsigned int frame_count = 2;
#ifdef NOUVEAU_PENDING_FLIPS
    frame_count = 7;
#endif
    for (unsigned int frame = 0; frame < frame_count; frame++) {
        stage = "prepare CPU upload source";
        if (cpu_access(owner, objects[0].info.handle, 1))
            goto cleanup;
        for (unsigned int vertical = 0; vertical < PRESENT_HEIGHT; vertical++)
            for (unsigned int horizontal = 0; horizontal < PRESENT_WIDTH; horizontal++)
                ((uint32_t *)source)[vertical * PRESENT_WIDTH + horizontal] = present_pixel(horizontal, vertical, frame % 2);
        if (cpu_access(owner, objects[0].info.handle, 0))
            goto cleanup;
        stage = "encode GPU copy to unmapped destination";
        if (cpu_access(owner, objects[2].info.handle, 1))
            goto cleanup;
        size_t count = copy_commands(commands, addresses[0], addresses[1], PRESENT_SIZE);
        if (cpu_access(owner, objects[2].info.handle, 0))
            goto cleanup;
        uint64_t point = frame + 1;
        struct drm_nouveau_sync signal = {.flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ,
            .handle = sync.handle, .timeline_value = point};
        struct drm_nouveau_exec_push push = {.va = addresses[2], .va_len = count * 4};
        struct drm_nouveau_exec execute = {.channel = channel.channel, .push_count = 1,
            .sig_count = 1, .sig_ptr = (uintptr_t)&signal, .push_ptr = (uintptr_t)&push};
#ifdef NOUVEAU_PENDING_FLIPS
        stage = "create controlled unsignaled dependency";
        controller = open("/dev/kpi_dmabuf", O_RDWR | O_CLOEXEC);
        if (controller < 0)
            goto cleanup;
        control_sync = ioctl(controller, KPI_DMABUF_IOC_SYNC_FD, 0);
        if (control_sync < 0)
            goto cleanup;
        struct drm_syncobj_handle import_fence = {.handle = gate.handle, .fd = control_sync,
            .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE};
        if (ioctl(owner, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import_fence))
            goto cleanup;
        struct drm_nouveau_sync dependency = {.flags = DRM_NOUVEAU_SYNC_SYNCOBJ, .handle = gate.handle};
        execute.wait_count = 1;
        execute.wait_ptr = (uintptr_t)&dependency;
#endif
        stage = "EXEC GPU presentation copy";
        if (ioctl(owner, DRM_IOCTL_NOUVEAU_EXEC, &execute))
            goto cleanup;
#ifdef NOUVEAU_PENDING_FLIPS
        struct drm_syncobj_timeline_wait not_ready = {.handles = (uintptr_t)&sync.handle,
            .points = (uintptr_t)&point, .count_handles = 1,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT};
        stage = "real queued EXEC remains pending";
        if (expect_error(ioctl(owner, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &not_ready), ETIME, stage))
            goto cleanup;
        signaler = fork();
        if (signaler < 0)
            goto cleanup;
        if (!signaler) {
            close(sink);
            if (monitor >= 0) close(monitor);
            close(owner);
            close(shared);
            usleep(750000);
            _exit(ioctl(controller, KPI_DMABUF_IOC_SIGNAL, 0) ? 1 : 0);
        }
        struct drm_mode_crtc_page_flip flip = {.crtc_id = crtc, .fb_id = framebuffer.fb_id,
            .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 0xc6000000ULL + frame};
        int64_t before_flip = monotonic_ns();
        stage = "nonblocking page flip with pending GPU producer";
        if (before_flip < 0 || ioctl(sink, DRM_IOCTL_MODE_PAGE_FLIP, &flip))
            goto cleanup;
        int64_t flip_elapsed = monotonic_ns() - before_flip;
        if (flip_elapsed < 0 || flip_elapsed >= 250000000 ||
            expect_error(ioctl(owner, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &not_ready), ETIME, stage))
            goto cleanup;
        struct pollfd empty = {.fd = sink, .events = POLLIN};
        if (poll(&empty, 1, 100) != 0 ||
            expect_error(ioctl(sink, DRM_IOCTL_MODE_PAGE_FLIP, &flip), EBUSY, "second pending flip"))
            goto cleanup;
        printf("NOUVEAU_C6_FLIP: PENDING frame=%u timeline=%llu ioctl_us=%lld event_not_ready=1 second=EBUSY\n",
               frame, (unsigned long long)point, (long long)(flip_elapsed / 1000));
        if (frame == 6) {
            stage = "close and RMFB with pending event";
            monitor = find_sink();
            if (monitor < 0 || fcntl(monitor, F_SETFL, O_NONBLOCK) < 0 ||
                ioctl(sink, DRM_IOCTL_MODE_RMFB, &framebuffer.fb_id))
                goto cleanup;
            framebuffer.fb_id = 0;
            if (gem_close(sink, imported))
                goto cleanup;
            imported = 0;
            int closed = close(sink);
            sink = -1;
            if (closed)
                goto cleanup;
        }
#endif
        struct timespec now;
        stage = "bounded producer timeline fence wait";
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            goto cleanup;
        struct drm_syncobj_timeline_wait wait = {.handles = (uintptr_t)&sync.handle,
            .points = (uintptr_t)&point, .count_handles = 1,
            .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
            .timeout_nsec = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + 10000000000LL};
        if (ioctl(owner, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait))
            goto cleanup;
        uint64_t completed = 0;
        struct drm_syncobj_timeline_array query = {.handles = (uintptr_t)&sync.handle,
            .points = (uintptr_t)&completed, .count_handles = 1};
        stage = "producer timeline query";
        if (ioctl(owner, DRM_IOCTL_SYNCOBJ_QUERY, &query))
            goto cleanup;
        if (completed != point) {
            errno = EIO;
            goto cleanup;
        }
        point++;
        wait.timeout_nsec = 0;
        stage = "future producer timeline timeout";
        if (expect_error(ioctl(owner, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait), ETIME, stage))
            goto cleanup;
#ifdef NOUVEAU_PENDING_FLIPS
        int child_status = 0;
        stage = "controlled producer signal child";
        if (waitpid(signaler, &child_status, 0) != signaler || !WIFEXITED(child_status) || WEXITSTATUS(child_status))
            goto cleanup;
        signaler = -1;
        int closed_sync = close(control_sync), closed_controller = close(controller);
        control_sync = controller = -1;
        if (closed_sync || closed_controller)
            goto cleanup;
        if (frame == 6) {
            struct pollfd no_leak = {.fd = monitor, .events = POLLIN};
            stage = "no closed-file event delivered to another client";
            if (poll(&no_leak, 1, 250) != 0)
                goto cleanup;
            struct drm_prime_handle replacement = {.fd = shared};
            if (ioctl(monitor, DRM_IOCTL_PRIME_FD_TO_HANDLE, &replacement))
                goto cleanup;
            imported = replacement.handle;
            framebuffer.handle = imported;
            if (ioctl(monitor, DRM_IOCTL_MODE_ADDFB, &framebuffer))
                goto cleanup;
            sink = monitor;
            monitor = -1;
            struct drm_mode_crtc_page_flip recovered = {.crtc_id = crtc, .fb_id = framebuffer.fb_id,
                .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 0xc6ffffff};
            if (ioctl(sink, DRM_IOCTL_MODE_PAGE_FLIP, &recovered) ||
                flip_event(sink, recovered.user_data, crtc, &sequence, &timestamp))
                goto cleanup;
            puts("NOUVEAU_C6_FLIP: PASS pending RMFB/handle/fd teardown, isolated event queue and fresh-client recovery");
            continue;
        }
        stage = "flip event cookie/sequence/timestamp/nonblocking read";
        if (flip_event(sink, flip.user_data, crtc, &sequence, &timestamp))
            goto cleanup;
        printf("NOUVEAU_C6_FLIP: PASS event frame=%u sequence=%u timestamp_us=%llu\n",
               frame, sequence, (unsigned long long)timestamp);
#endif
        printf("NOUVEAU_C6_GPU: PASS EXEC frame=%u bytes=%u timeline=%llu future=ETIME destination_cpu_unmapped=1\n",
               frame, PRESENT_SIZE, (unsigned long long)completed);
#ifndef NOUVEAU_PENDING_FLIPS
        struct drm_mode_crtc commit = {.crtc_id = crtc, .fb_id = framebuffer.fb_id,
            .set_connectors_ptr = (uintptr_t)&connector, .count_connectors = 1, .mode_valid = 1, .mode = mode};
        stage = "present completed GPU destination";
        if (ioctl(sink, DRM_IOCTL_MODE_SETCRTC, &commit))
            goto cleanup;
#endif
        printf("NOUVEAU_C6_PRESENT: READY frame=%u width=%u height=%u pitch=%u imported_fb=%u\n",
               frame, PRESENT_WIDTH, PRESENT_HEIGHT, framebuffer.pitch, framebuffer.fb_id);
#ifdef NOUVEAU_PENDING_FLIPS
        sleep(2);
#else
        sleep(10);
#endif
    }
    result = 0;
cleanup:
    if (result)
        prime_failed(stage);
#ifdef NOUVEAU_PENDING_FLIPS
    if (controller >= 0) ioctl(controller, KPI_DMABUF_IOC_SIGNAL, 0);
    if (signaler > 0) waitpid(signaler, NULL, 0);
    if (control_sync >= 0 && close(control_sync)) result = prime_failed("close control fence");
    if (controller >= 0 && close(controller)) result = prime_failed("close fence controller");
    if (monitor >= 0 && close(monitor)) result = prime_failed("close flip monitor");
    if (gate.handle) {
        struct drm_syncobj_destroy destroy = {.handle = gate.handle};
        if (ioctl(owner, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy)) result = prime_failed("destroy dependency gate");
    }
#endif
    if (channel_live) {
        struct drm_nouveau_channel_free release = {.channel = channel.channel};
        if (ioctl(owner, DRM_IOCTL_NOUVEAU_CHANNEL_FREE, &release))
            result = prime_failed("release GPU presentation channel");
    }
    if (framebuffer.fb_id && ioctl(sink, DRM_IOCTL_MODE_RMFB, &framebuffer.fb_id))
        result = prime_failed("remove GPU presentation framebuffer");
    if (imported && gem_close(sink, imported))
        result = prime_failed("release GPU presentation import");
    for (unsigned int index = 0; index < 3; index++) {
        if (bound[index]) {
            struct drm_nouveau_vm_bind_op operation = {.op = DRM_NOUVEAU_VM_BIND_OP_UNMAP,
                .addr = addresses[index], .range = sizes[index]};
            if (bind_one(owner, &operation))
                result = prime_failed("unmap GPU presentation VA");
        }
        if (objects[index].info.handle && gem_close(owner, objects[index].info.handle))
            result = prime_failed("close GPU presentation BO");
    }
    if (source != MAP_FAILED && munmap(source, PRESENT_SIZE))
        result = prime_failed("unmap GPU upload source");
    if (commands != MAP_FAILED && munmap(commands, sizes[2]))
        result = prime_failed("unmap GPU commands");
    if (sync.handle) {
        struct drm_syncobj_destroy destroy = {.handle = sync.handle};
        if (ioctl(owner, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy))
            result = prime_failed("destroy producer timeline");
    }
    if (shared >= 0 && close(shared))
        result = prime_failed("close GPU presentation dma-buf");
    if (sink >= 0 && close(sink))
        result = prime_failed("close GPU presentation sink");
    if (owner >= 0 && close(owner))
        result = prime_failed("close GPU presentation owner");
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--software")) {
        if (present_software() || copy_software())
            return 1;
        puts("NOUVEAU_C6_GPU: SOFTWARE copy encoding and presentation pattern pass; no GPU access");
#ifdef NOUVEAU_PENDING_FLIPS
        if (selection_test(0)) return prime_failed("software desktop selection");
        puts("NOUVEAU_C6_FLIP: SOFTWARE pending-producer/event profile compiled; no GPU access");
#endif
        return 0;
    }
    if (argc != 1)
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    int telemetry = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (telemetry < 0)
        return prime_failed("open GPU presentation telemetry");
    if (dup2(telemetry, STDOUT_FILENO) < 0 || dup2(telemetry, STDERR_FILENO) < 0) {
        close(telemetry);
        return prime_failed("redirect GPU presentation telemetry");
    }
    if (close(telemetry))
        return prime_failed("close GPU telemetry source");
    alarm(90);
    puts("NOUVEAU_C6_PRESENT: START linear PRIME presentation");
    puts("NOUVEAU_C6_GPU: START copy-engine producer with explicit timeline wait");
#ifdef NOUVEAU_PENDING_FLIPS
    puts("NOUVEAU_C6_FLIP: START queued EXEC dependency -> nonblocking implicit-fence flip");
    if (selection_test(1)) return prime_failed("hardware desktop selection");
#endif
    char path[64];
    if (find_device(path, sizeof(path)) || gpu_present_case(path))
        return 1;
#ifdef NOUVEAU_PENDING_FLIPS
    puts("NOUVEAU_C6_FLIP: END six verified flips and pending teardown status=pass");
#endif
    puts("NOUVEAU_C6_GPU: END GPU-produced presentation status=pass");
    puts("NOUVEAU_C6_PRESENT: END presentation status=pass; host pixel verification required");
    return 0;
}
