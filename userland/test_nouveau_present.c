#define NOUVEAU_PRIME_ENTRY nouveau_prime_sharing_main
#include "test_nouveau_prime.c"

#define PRESENT_WIDTH 1280
#define PRESENT_HEIGHT 800
#define PRESENT_SIZE (PRESENT_WIDTH * PRESENT_HEIGHT * 4)

static uint32_t present_pixel(unsigned int horizontal, unsigned int vertical, unsigned int frame) {
    static const uint32_t colors[] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
        0xff00ff, 0x00ffff, 0xffffff, 0x202020
    };
    if (horizontal < 8 || vertical < 8 || horizontal >= PRESENT_WIDTH - 8 || vertical >= PRESENT_HEIGHT - 8)
        return 0xffffff;
    if (horizontal < 128 && vertical < 64)
        return frame ? 0x00ffff : 0xffff00;
    return colors[(horizontal * 8 / PRESENT_WIDTH + frame) % 8];
}

static int present_software(void) {
    if (present_pixel(0, 100, 0) != 0xffffff ||
        present_pixel(20, 20, 0) != 0xffff00 || present_pixel(20, 20, 1) != 0x00ffff ||
        present_pixel(200, 200, 0) != 0x00ff00 || present_pixel(200, 200, 1) != 0x0000ff)
        return prime_failed("presentation pattern oracle");
    puts("NOUVEAU_C6_PRESENT: SOFTWARE two-frame pattern pass; no GPU access");
    return 0;
}

static int present_mode(int descriptor, uint32_t *connector_id, uint32_t *crtc_id,
                        struct drm_mode_modeinfo *mode) {
    uint32_t connectors[16], crtcs[16];
    struct drm_mode_card_res resources = {
        .connector_id_ptr = (uintptr_t)connectors, .count_connectors = 16,
        .crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = 16
    };
    if (ioctl(descriptor, DRM_IOCTL_MODE_GETRESOURCES, &resources))
        return -1;
    if (resources.count_connectors > 16 || !resources.count_crtcs || resources.count_crtcs > 16) {
        errno = EOVERFLOW;
        return -1;
    }
    for (uint32_t index = 0; index < resources.count_connectors; index++) {
        struct drm_mode_modeinfo modes[64];
        struct drm_mode_get_connector connector = {
            .connector_id = connectors[index], .modes_ptr = (uintptr_t)modes, .count_modes = 64
        };
        if (ioctl(descriptor, DRM_IOCTL_MODE_GETCONNECTOR, &connector))
            return -1;
        if (connector.count_modes > 64) {
            errno = EOVERFLOW;
            return -1;
        }
        if (connector.connection != 1 || !connector.encoder_id)
            continue;
        struct drm_mode_get_encoder encoder = {.encoder_id = connector.encoder_id};
        if (ioctl(descriptor, DRM_IOCTL_MODE_GETENCODER, &encoder))
            return -1;
        for (uint32_t candidate = 0; candidate < connector.count_modes; candidate++) {
            if (modes[candidate].hdisplay != PRESENT_WIDTH || modes[candidate].vdisplay != PRESENT_HEIGHT)
                continue;
            for (uint32_t position = 0; position < resources.count_crtcs; position++) {
                if (!(encoder.possible_crtcs & (1u << position)))
                    continue;
                *mode = modes[candidate];
                *connector_id = connectors[index];
                *crtc_id = crtcs[position];
                return 0;
            }
        }
    }
    errno = ENODEV;
    return -1;
}

static int present_case(const char *path) {
    int owner = -1, sink = -1, shared = -1, result = 1;
    uint32_t imported = 0, connector = 0, crtc = 0;
    uint32_t *mapping = MAP_FAILED;
    struct drm_mode_modeinfo mode = {0};
    struct drm_mode_fb_cmd framebuffer = {
        .width = PRESENT_WIDTH, .height = PRESENT_HEIGHT,
        .pitch = PRESENT_WIDTH * 4, .bpp = 32, .depth = 24
    };
    struct drm_nouveau_gem_new object = {.info = {
        .size = PRESENT_SIZE, .domain = NOUVEAU_GEM_DOMAIN_GART | NOUVEAU_GEM_DOMAIN_MAPPABLE
    }, .align = 4096};
    const char *stage = "open presentation devices";
    owner = open(path, O_RDWR | O_CLOEXEC);
    sink = find_sink();
    if (owner < 0 || sink < 0)
        goto cleanup;
    stage = "select advertised VirtIO mode and compatible CRTC";
    if (present_mode(sink, &connector, &crtc, &mode))
        goto cleanup;
    stage = "allocate NVIDIA presentation buffer";
    if (ioctl(owner, DRM_IOCTL_NOUVEAU_GEM_NEW, &object))
        goto cleanup;
    if (object.info.size != PRESENT_SIZE) {
        errno = EPROTO;
        goto cleanup;
    }
    stage = "map NVIDIA presentation buffer";
    mapping = mmap(NULL, PRESENT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, owner, object.info.map_handle);
    if (mapping == MAP_FAILED)
        goto cleanup;
    struct drm_prime_handle export = {.handle = object.info.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    stage = "export NVIDIA presentation buffer";
    if (ioctl(owner, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export))
        goto cleanup;
    shared = export.fd;
    struct drm_prime_handle import = {.fd = shared};
    stage = "import NVIDIA presentation buffer on VirtIO";
    if (ioctl(sink, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import))
        goto cleanup;
    imported = import.handle;
    framebuffer.handle = imported;
    stage = "ADDFB imported linear XRGB8888";
    if (ioctl(sink, DRM_IOCTL_MODE_ADDFB, &framebuffer))
        goto cleanup;
    for (unsigned int frame = 0; frame < 2; frame++) {
        stage = "presentation CPU_PREP";
        if (cpu_access(owner, object.info.handle, 1))
            goto cleanup;
        for (unsigned int vertical = 0; vertical < PRESENT_HEIGHT; vertical++)
            for (unsigned int horizontal = 0; horizontal < PRESENT_WIDTH; horizontal++)
                mapping[vertical * PRESENT_WIDTH + horizontal] = present_pixel(horizontal, vertical, frame);
        stage = "presentation CPU_FINI";
        if (cpu_access(owner, object.info.handle, 0))
            goto cleanup;
        struct drm_mode_crtc commit = {
            .crtc_id = crtc, .fb_id = framebuffer.fb_id,
            .set_connectors_ptr = (uintptr_t)&connector, .count_connectors = 1,
            .mode_valid = 1, .mode = mode
        };
        stage = "SETCRTC imported framebuffer";
        if (ioctl(sink, DRM_IOCTL_MODE_SETCRTC, &commit))
            goto cleanup;
        printf("NOUVEAU_C6_PRESENT: READY frame=%u width=%u height=%u pitch=%u imported_fb=%u\n",
               frame, PRESENT_WIDTH, PRESENT_HEIGHT, framebuffer.pitch, framebuffer.fb_id);
        sleep(10);
    }
    result = 0;
cleanup:
    if (result)
        prime_failed(stage);
    if (framebuffer.fb_id && ioctl(sink, DRM_IOCTL_MODE_RMFB, &framebuffer.fb_id))
        result = prime_failed("remove presentation framebuffer");
    if (imported && gem_close(sink, imported))
        result = prime_failed("release presentation import");
    if (mapping != MAP_FAILED && munmap(mapping, PRESENT_SIZE))
        result = prime_failed("unmap presentation exporter");
    if (object.info.handle && gem_close(owner, object.info.handle))
        result = prime_failed("release presentation export");
    if (shared >= 0 && close(shared))
        result = prime_failed("close presentation dma-buf");
    if (sink >= 0 && close(sink))
        result = prime_failed("close presentation sink");
    if (owner >= 0 && close(owner))
        result = prime_failed("close presentation owner");
    return result;
}

#ifndef NOUVEAU_PRESENT_ENTRY
#define NOUVEAU_PRESENT_ENTRY main
#endif
int NOUVEAU_PRESENT_ENTRY(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--software"))
        return present_software();
    if (argc != 1)
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    int telemetry = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (telemetry < 0)
        return prime_failed("open serial-safe presentation telemetry");
    if (dup2(telemetry, STDOUT_FILENO) < 0 || dup2(telemetry, STDERR_FILENO) < 0) {
        close(telemetry);
        return prime_failed("redirect presentation telemetry");
    }
    if (close(telemetry))
        return prime_failed("close telemetry source descriptor");
    alarm(90);
    puts("NOUVEAU_C6_PRESENT: START linear PRIME presentation");
    char path[64];
    if (find_device(path, sizeof(path)) || present_case(path))
        return 1;
    puts("NOUVEAU_C6_PRESENT: END presentation status=pass; host pixel verification required");
    return 0;
}
