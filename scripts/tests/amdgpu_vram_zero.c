/* Native AMD allocation regression: no userspace VRAM_CLEARED flag.
 * Run on an amdgpu render node; churn buffers after filling them with 0xa5.
 * Wait for the kernel's initialization fence before inspecting contents. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <drm.h>
#include <amdgpu_drm.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(int argc, char **argv) {
    const size_t size = 2 * 1024 * 1024;
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/renderD129", O_RDWR);
    CHECK(fd >= 0);
    for (int pass = 0; pass < 32; ++pass) {
        union drm_amdgpu_gem_create create = {0};
        create.in.bo_size = size;
        create.in.alignment = 4096;
        create.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
        create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
        CHECK(ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create) == 0);
        uint32_t handle = create.out.handle;
        union drm_amdgpu_gem_wait_idle idle = {0};
        idle.in.handle = handle;
        struct timespec now;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        idle.in.timeout = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec + 5000000000ULL;
        CHECK(ioctl(fd, DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE, &idle) == 0);
        CHECK(idle.out.status == 0);
        union drm_amdgpu_gem_mmap map = {0};
        map.in.handle = handle;
        CHECK(ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &map) == 0);
        unsigned char *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, map.out.addr_ptr);
        CHECK(pixels != MAP_FAILED);
        for (size_t i = 0; i < size; ++i) {
            if (pixels[i]) {
                fprintf(stderr, "FAIL: pass %d offset %zu contains 0x%02x\n",
                        pass, i, pixels[i]);
                return 1;
            }
        }
        memset(pixels, 0xa5, size);
        CHECK(munmap(pixels, size) == 0);
        struct drm_gem_close close_bo = {.handle = handle};
        CHECK(ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo) == 0);
    }
    close(fd);
    puts("PASS: 32 recycled 2 MiB VRAM allocations cleared by kernel without userspace clear flag");
    return 0;
}
