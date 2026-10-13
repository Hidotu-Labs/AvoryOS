// AvoryOS Phase 6 C7 userland test: unmodified Mesa radeonsi through the
// amdgpu render node.
//
// The uAPI surface is proven by C5's raw-ioctl suite; this one proves the
// real userspace consumer works: Mesa's radeonsi DRI driver loaded by libEGL
// on the amdgpu render node, with a GBM device and GLES2.
//
// Steps:
//   1. discover amdgpu's render node by DRM_IOCTL_VERSION name (the C5/C6
//      layout puts it on renderD129; nothing is hard-coded);
//   2. gbm_create_device on it, EGL platform GBM + GLES2 context;
//   3. assert the renderer string is radeonsi (not llvmpipe/swrast - that
//      would mean the GPU path did not come up);
//   4. render a triangle, read the pixel back (proves execution), then time a
//      frame loop and print fps;
//   5. round-trip a GBM dma-buf through EGL (EGL_EXT_image_dma_buf_import)
//      when the extension is present - the PRIME path Mesa uses for
//      Wayland/GBM clients.
//
// Prints `=== ALL TESTS PASSED ===` on success (same contract as
// bin/test_kpi_amdgpu).  Build: userland/test_kpi_radeonsi.elf, installed as
// /bin/test_kpi_radeonsi; links the Alpine rootfs Mesa/GBM (musl).

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static int failures;

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, long detail) {
    failures++;
    printf("  [FAIL] %s", what);
    if (detail)
        printf(" (%ld: %s)", detail, strerror((int)detail));
    printf("\n");
}

static void skip(const char *what) { printf("  [SKIP] %s\n", what); }
static int drm_name_is(const char *path, const char *want);
static void test_texture_pressure(void);
static void test_fog_uniforms(void);
static void test_amd_export_readback(EGLDisplay dpy, struct gbm_device *gbm);

/* Render into a buffer owned by the visible virtual display, exactly the
 * sharing direction used when DRI3 offloads an application to AMDGPU. */
static void test_prime_offload(EGLDisplay dpy) {
    struct drm_mode_create_dumb dumb = {.width = 64, .height = 64, .bpp = 32};
    struct drm_mode_destroy_dumb destroy = {0};
    struct drm_mode_map_dumb map = {0};
    int card = -1, fd = -1;
    void *pixels = MAP_FAILED;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
    GLint previous_fbo = 0;
    PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    const char *extensions = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!create_image || !destroy_image || !bind_image || !extensions ||
        !strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
        fail("PRIME offload requires EGL dma-buf image import", 0);
        return;
    }
    card = drm_name_is("/dev/dri/card0", "ascentdrm");
    if (card < 0) { fail("PRIME visible card0 is unavailable", errno); goto out; }
    if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
        fail("PRIME create display-owned dumb buffer", errno); goto out;
    }
    destroy.handle = dumb.handle;
    map.handle = dumb.handle;
    if (ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        fail("PRIME map display-owned buffer", errno); goto out;
    }
    pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, map.offset);
    if (pixels == MAP_FAILED || drmPrimeHandleToFD(card, dumb.handle, DRM_CLOEXEC | DRM_RDWR, &fd)) {
        fail("PRIME export display buffer", errno); goto out;
    }
    EGLint attrs[] = {
        EGL_WIDTH, 64, EGL_HEIGHT, 64,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, dumb.pitch, EGL_NONE
    };
    image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        fail("PRIME AMD imports virtual display dma-buf", eglGetError()); goto out;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    bind_image(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail("PRIME imported buffer is renderable", 0); goto out;
    }
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (glGetError() != GL_NO_ERROR) { fail("PRIME AMD rendering", 0); goto out; }
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) {
        fail("PRIME wait for AMD writes", errno); goto out;
    }
    uint32_t pixel = *(volatile uint32_t *)((uint8_t *)pixels + 32 * dumb.pitch + 32 * 4);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) || pixel != 0xff00ff00u)
        fail("PRIME AMD pixels visible in virtual display buffer", pixel);
    else
        pass("PRIME AMD renders into the virtual display's shared buffer");
out:
    if (fbo) { glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo); glDeleteFramebuffers(1, &fbo); }
    if (texture) glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) destroy_image(dpy, image);
    if (fd >= 0) close(fd);
    if (pixels != MAP_FAILED) munmap(pixels, dumb.size);
    if (destroy.handle) ioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    if (card >= 0) close(card);
}

/* ── render-node discovery ──────────────────────────────────────────────── */

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

/* ── libdrm/sysfs diagnostics ───────────────────────────────────────────── */

/* radeonsi is reached only if libdrm can identify the DRM node from the fd:
 * drmGetDevice2() must see it as a char device (fstat st_rdev) and walk
 * /sys/dev/char/<maj>:<min>/device/{drm,subsystem,vendor,...,uevent}.  When
 * the loader falls back to llvmpipe these prints show which link is missing;
 * the renderer string alone cannot. */
static void diag_show(const char *path) {
    char buf[256];
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printf("  [DIAG] %s: open -> %s\n", path, strerror(errno));
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        printf("  [DIAG] %s: read -> %s\n", path, strerror(errno));
        return;
    }
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\n')
            buf[i] = ' ';
    printf("  [DIAG] %s = \"%s\"\n", path, buf);
}

static void diag_symlink(const char *path) {
    char target[512];
    ssize_t n = readlink(path, target, sizeof(target) - 1);

    if (n < 0) {
        printf("  [DIAG] readlink(%s) -> %s\n", path, strerror(errno));
        return;
    }
    target[n] = '\0';
    printf("  [DIAG] readlink(%s) = %s\n", path, target);
}

static void diag_drm_identification(int node_fd, const char *node_path) {
    static const char *fields[] = {"vendor", "device", "subsystem_vendor",
                                   "subsystem_device", "revision"};
    struct stat st;
    char path[256];
    drmDevicePtr devices[8];
    drmDevicePtr dev = NULL;
    int n, i, ret;
    unsigned maj, min;

    printf("  [DIAG] libdrm device identification:\n");

    {
        DIR *dir = opendir("/dev/dri");
        struct dirent *de;

        if (!dir) {
            printf("  [DIAG] opendir(/dev/dri) -> %s\n", strerror(errno));
        } else {
            while ((de = readdir(dir)) != NULL)
                printf("  [DIAG] /dev/dri/%s type=%d\n", de->d_name,
                       (int)de->d_type);
            closedir(dir);
        }
    }

    memset(&st, 0, sizeof(st));
    if (fstat(node_fd, &st) != 0)
        printf("  [DIAG] fstat(%s) -> %s\n", node_path, strerror(errno));
    else
        printf("  [DIAG] fstat(%s): %s rdev=%u:%u (0x%llx)\n", node_path,
               S_ISCHR(st.st_mode) ? "chardev" : "NOT a chardev",
               major(st.st_rdev), minor(st.st_rdev),
               (unsigned long long)st.st_rdev);
    maj = major(st.st_rdev);
    min = minor(st.st_rdev);

    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u", maj, min);
    diag_symlink(path);

    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/drm", maj, min);
    {
        struct stat dst;

        if (stat(path, &dst) != 0)
            printf("  [DIAG] stat(%s) -> %s\n", path, strerror(errno));
        else
            printf("  [DIAG] stat(%s) ok\n", path);
    }

    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/subsystem", maj,
             min);
    diag_symlink(path);

    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/uevent", maj, min);
    diag_show(path);

    for (i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
        snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/%s", maj, min,
                 fields[i]);
        diag_show(path);
    }

    n = drmGetDevices2(0, devices, 8);
    printf("  [DIAG] drmGetDevices2 -> %d\n", n);
    for (i = 0; i < n && i < 8; i++) {
        bool pci = devices[i]->bustype == DRM_BUS_PCI &&
                   devices[i]->deviceinfo.pci != NULL;

        printf("  [DIAG]   device %d: bustype=%d primary=%s render=%s "
               "ids=%04x:%04x\n",
               i, devices[i]->bustype,
               devices[i]->nodes[DRM_NODE_PRIMARY]
                   ? devices[i]->nodes[DRM_NODE_PRIMARY]
                   : "(none)",
               devices[i]->nodes[DRM_NODE_RENDER]
                   ? devices[i]->nodes[DRM_NODE_RENDER]
                   : "(none)",
               pci ? devices[i]->deviceinfo.pci->vendor_id : 0,
               pci ? devices[i]->deviceinfo.pci->device_id : 0);
        drmFreeDevice(&devices[i]);
    }

    ret = drmGetDevice2(node_fd, 0, &dev);
    printf("  [DIAG] drmGetDevice2(%s) -> %d\n", node_path, ret);
    if (ret == 0 && dev) {
        bool pci = dev->bustype == DRM_BUS_PCI && dev->deviceinfo.pci != NULL;

        printf("  [DIAG]   primary=%s render=%s ids=%04x:%04x\n",
               dev->nodes[DRM_NODE_PRIMARY] ? dev->nodes[DRM_NODE_PRIMARY]
                                            : "(none)",
               dev->nodes[DRM_NODE_RENDER] ? dev->nodes[DRM_NODE_RENDER]
                                           : "(none)",
               pci ? dev->deviceinfo.pci->vendor_id : 0,
               pci ? dev->deviceinfo.pci->device_id : 0);
        drmFreeDevice(&dev);
    }
}

/* ── GLES2 helpers ──────────────────────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("  [FAIL] shader compile: %s\n", log);
        failures++;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static double now_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* EGL_EXT_platform_device: match the render node exactly by its DRM file. */
static EGLDisplay egl_display_from_device(const char *node_path) {
    PFNEGLQUERYDEVICESEXTPROC query_devices;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    EGLDeviceEXT devices[16];
    EGLint n = 0;

    query_devices = (PFNEGLQUERYDEVICESEXTPROC)
        eglGetProcAddress("eglQueryDevicesEXT");
    query_device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)
        eglGetProcAddress("eglQueryDeviceStringEXT");
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!query_devices || !query_device_string || !get_platform_display)
        return EGL_NO_DISPLAY;
    EGLDeviceEXT fallback = NULL;

    if (!query_devices(16, devices, &n) || n < 1)
        return EGL_NO_DISPLAY;
    for (EGLint i = 0; i < n; i++) {
        /* Mesa exposes both the card node (EGL_DRM_DEVICE_FILE_EXT) and the
         * render node (EGL_DRM_RENDER_NODE_FILE_EXT); match either. */
        const char *render = query_device_string(devices[i],
                                                 EGL_DRM_RENDER_NODE_FILE_EXT);
        const char *card = query_device_string(devices[i],
                                               EGL_DRM_DEVICE_FILE_EXT);
        if ((render && strcmp(render, node_path) == 0) ||
            (card && strcmp(card, node_path) == 0))
            return get_platform_display(EGL_PLATFORM_DEVICE_EXT, devices[i],
                                        NULL);
        /* Keep a candidate that is a real DRM device but not vgem
         * (renderD128), which has no DRI driver and would give llvmpipe. */
        if (!fallback && (card || render) &&
            !(render && strcmp(render, "/dev/dri/renderD128") == 0))
            fallback = devices[i];
        if (i < 4)
            printf("  [INFO] EGL device %d: card=%s render=%s\n", i,
                   card ? card : "(none)", render ? render : "(none)");
    }
    if (fallback)
        return get_platform_display(EGL_PLATFORM_DEVICE_EXT, fallback, NULL);
    return EGL_NO_DISPLAY;
}

/* GBM first (the standard Mesa path); if the Alpine package cannot load its
 * DRI backend, fall back to EGL_EXT_platform_device on the exact node, then
 * to the surfaceless platform (render node auto-selected by Mesa). */
static EGLDisplay open_egl_display(const char *node_path, struct gbm_device *gbm,
                                   const char **how) {
    EGLDisplay d = EGL_NO_DISPLAY;

    if (gbm) {
        d = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm, NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) {
            *how = "EGL_MESA_platform_gbm";
            return d;
        }
        if (d != EGL_NO_DISPLAY)
            eglTerminate(d);
    }
    d = egl_display_from_device(node_path);
    if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) {
        *how = "EGL_EXT_platform_device (EGL_EXT_device_drm)";
        return d;
    }
    if (d != EGL_NO_DISPLAY)
        eglTerminate(d);
    d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                              NULL);
    if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) {
        *how = "EGL_MESA_platform_surfaceless";
        return d;
    }
    if (d != EGL_NO_DISPLAY)
        eglTerminate(d);
    return EGL_NO_DISPLAY;
}

#define BENCH_W 512
#define BENCH_H 512
#define BENCH_FRAMES 300

/* ── suite ──────────────────────────────────────────────────────────────── */

int main(void) {
    static const char *vs_src =
        "attribute vec2 pos;\n"
        "attribute vec3 col;\n"
        "varying vec3 vcol;\n"
        "void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec3 vcol;\n"
        "void main() { gl_FragColor = vec4(vcol, 1.0); }\n";
    static const GLfloat verts[3][5] = {
        {-0.8f, -0.8f, 1.0f, 0.0f, 0.0f},
        {0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.8f, 0.0f, 0.0f, 1.0f},
    };
    char node_path[64] = {0};
    struct gbm_device *gbm = NULL;
    struct gbm_surface *gsurf = NULL;
    struct gbm_bo *bo = NULL;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLConfig cfg = NULL;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLImage img = EGL_NO_IMAGE;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = NULL;
    GLuint vs = 0, fs = 0, prog = 0, vbo = 0, tex = 0;
    GLint n_cfg = 0, pos_loc, col_loc, readback = 0;
    const char *renderer, *vendor;
    int node_fd = -1;
    int dmabuf_fd = -1;
    int have_gl = 0;
    int gbm_window = 0; /* EGL display came from GBM: window surfaces only */

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("AvoryOS LinuxKPI Phase 6 C7 - Mesa radeonsi on the amdgpu render node\n");

    /* Alpine's Mesa DRI drivers live under /usr/lib/xorg/modules/dri while
     * Mesa's default search path is /usr/lib/dri; without the override the
     * EGL/GBM loader finds no radeonsi and silently falls back to llvmpipe. */
    setenv("LIBGL_DRIVERS_PATH", access("/usr/lib/dri", F_OK) == 0
              ? "/usr/lib/dri" : "/usr/lib/xorg/modules/dri", 0);
    /* Show the loader's own diagnostics when a DRI driver cannot be loaded. */
    setenv("LIBGL_DEBUG", "verbose", 0);

    /* 1. render node */
    node_fd = find_amdgpu_render_node(node_path, sizeof(node_path));
    if (node_fd < 0) {
        skip("radeonsi suite (no amdgpu render node; bind amdgpu first)");
        printf("=== TEST SKIPPED ===\n");
        return 0;
    }
    printf("  [INFO] amdgpu render node: %s\n", node_path);

    /* 1b. why libdrm/Mesa do or do not see the node (device identification is
     * the gate for the radeonsi loader). */
    diag_drm_identification(node_fd, node_path);

    /* 2. GBM + EGL + GLES2 */
    {
        const char *how = NULL;

        gbm = gbm_create_device(node_fd);
        if (gbm) {
            pass("gbm device on the amdgpu render node");
        } else {
            printf("  [SKIP] gbm device unavailable (errno=%d: %s); trying "
                   "EGL device/surfaceless\n", errno, strerror(errno));
        }
        dpy = open_egl_display(node_path, gbm, &how);
        if (dpy == EGL_NO_DISPLAY) {
            fail("no EGL display on the amdgpu render node",
                 (long)eglGetError());
            goto out;
        }
        printf("  [INFO] EGL platform: %s\n", how);
        gbm_window = gbm && strstr(how, "gbm") != NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fail("eglBindAPI(OpenGL ES)", (long)eglGetError());
        goto out;
    }
    {
        /* Mesa's DRM/GBM platform (platform_drm.c) adds EGL_WINDOW_BIT configs
         * only; pbuffer configs exist on the device/surfaceless platforms. */
        EGLint attrs[] = {
            EGL_SURFACE_TYPE,
            gbm_window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_NONE,
        };
        if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n_cfg) || n_cfg < 1) {
            printf("  [INFO] eglChooseConfig(%s) -> n_cfg=%d\n",
                   gbm_window ? "EGL_WINDOW_BIT" : "EGL_PBUFFER_BIT", n_cfg);
            fail("eglChooseConfig", 0);
            goto out;
        }
    }
    if (gbm_window) {
        /* Weston's path: a GBM surface wrapped in an EGL window surface.  The
         * gbm_surface format must match the config's native visual. */
        EGLint native_format = 0;

        if (!eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID,
                                &native_format) ||
            native_format == 0) {
            fail("eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)",
                 (long)eglGetError());
            goto out;
        }
        gsurf = gbm_surface_create(gbm, BENCH_W, BENCH_H,
                                   (uint32_t)native_format,
                                   GBM_BO_USE_RENDERING);
        if (!gsurf) {
            fail("gbm_surface_create", errno);
            goto out;
        }
        surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gsurf,
                                      NULL);
        if (surf == EGL_NO_SURFACE) {
            fail("eglCreateWindowSurface(gbm_surface)", (long)eglGetError());
            goto out;
        }
        pass("EGL window surface on a GBM surface (Weston's path)");
    } else {
        EGLint attrs[] = {EGL_WIDTH, BENCH_W, EGL_HEIGHT, BENCH_H, EGL_NONE};

        surf = eglCreatePbufferSurface(dpy, cfg, attrs);
        if (surf == EGL_NO_SURFACE) {
            fail("eglCreatePbufferSurface", (long)eglGetError());
            goto out;
        }
    }
    {
        EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, attrs);
        if (ctx == EGL_NO_CONTEXT) {
            fail("eglCreateContext", (long)eglGetError());
            goto out;
        }
    }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        fail("eglMakeCurrent", (long)eglGetError());
        goto out;
    }
    have_gl = 1;
    pass("EGL/GLES2 context on the amdgpu render node");

    /* 3. renderer string */
    renderer = (const char *)glGetString(GL_RENDERER);
    vendor = (const char *)glGetString(GL_VENDOR);
    if (!renderer || !vendor) {
        fail("glGetString(GL_RENDERER/VENDOR)", 0);
        goto out;
    }
    printf("  [INFO] Mesa renderer: %s\n", renderer);
    printf("  [INFO] Mesa vendor:   %s\n", vendor);
    if (strstr(renderer, "radeonsi") || strstr(renderer, "Radeon")) {
        pass("renderer string is radeonsi (unmodified Mesa on amdgpu)");
    } else {
        if (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe"))
            printf("  [INFO] hint: radeonsi not loaded from %s\n",
                   getenv("LIBGL_DRIVERS_PATH")
                       ? getenv("LIBGL_DRIVERS_PATH")
                       : "(LIBGL_DRIVERS_PATH unset)");
        fail("renderer is not radeonsi", 0);
        goto out;
    }

    /* 4. draw a triangle and check a pixel */
    vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        goto out;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    {
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            fail("program link", 0);
            goto out;
        }
    }
    glUseProgram(prog);
    pos_loc = glGetAttribLocation(prog, "pos");
    col_loc = glGetAttribLocation(prog, "col");
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray((GLuint)pos_loc);
    glVertexAttribPointer((GLuint)pos_loc, 2, GL_FLOAT, GL_FALSE,
                          5 * sizeof(GLfloat), (const void *)0);
    glEnableVertexAttribArray((GLuint)col_loc);
    glVertexAttribPointer((GLuint)col_loc, 3, GL_FLOAT, GL_FALSE,
                          5 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));

    glViewport(0, 0, BENCH_W, BENCH_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(BENCH_W / 2, BENCH_H / 2, 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, &readback);
    if ((readback & 0x00ffffffu) != 0)
        pass("triangle rendered and read back from the GPU");
    else
        fail("readback pixel is black (draw did not execute?)", 0);

    /* 5. timed frame loop */
    {
        double t0, t1, fps;
        int i;

        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish(); /* warm-up */
        t0 = now_seconds();
        for (i = 0; i < BENCH_FRAMES; i++) {
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glFinish();
        t1 = now_seconds();
        fps = (t1 - t0) > 0.0 ? (double)BENCH_FRAMES / (t1 - t0) : 0.0;
        printf("  [INFO] radeonsi: %d frames %dx%d in %.3f s (%.1f fps)\n",
               BENCH_FRAMES, BENCH_W, BENCH_H, t1 - t0, fps);
        if (fps > 0.0)
            pass("timed render loop completed");
        else
            fail("frame timing produced no frames", 0);
    }

    test_prime_offload(dpy);
    if (getenv("AVORY_GPU_TEXTURE_STRESS")) {
        test_texture_pressure();
        test_fog_uniforms();
        test_amd_export_readback(dpy, gbm);
    }

    /* 6. dma-buf round trip through EGL (PRIME path) */
    {
        const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
        PFNEGLCREATEIMAGEKHRPROC create_image = NULL;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = NULL;

        if (!gbm) {
            skip("dma-buf EGL roundtrip (no gbm device)");
            goto out;
        }
        if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
            skip("EGL_EXT_image_dma_buf_import not advertised");
            goto out;
        }
        /* KHR entry points are not always linkable symbols (glvnd dispatch);
         * resolve them through the loader like Mesa clients do. */
        create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!create_image || !image_target_texture) {
            skip("EGL image entry points unavailable");
            goto out;
        }
        bo = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_ARGB8888,
                           GBM_BO_USE_RENDERING);
        if (!bo) {
            fail("gbm_bo_create (dma-buf)", errno);
            goto out;
        }
        dmabuf_fd = gbm_bo_get_fd(bo);
        if (dmabuf_fd < 0) {
            fail("gbm_bo_get_fd", errno);
            goto out;
        }
        {
            struct stat st;
            off_t size = lseek(dmabuf_fd, 0, SEEK_END);
            uint64_t minimum = (uint64_t)gbm_bo_get_stride(bo) *
                               gbm_bo_get_height(bo);
            if (size <= 0 || (uint64_t)size < minimum ||
                fstat(dmabuf_fd, &st) || st.st_size != size ||
                lseek(dmabuf_fd, 0, SEEK_SET) != 0) {
                fail("AMD dma-buf size discovery for Xorg/Mesa import", errno);
                goto out;
            }
            pass("AMD dma-buf fstat/SEEK_END report its allocation size");
        }
        {
            uint64_t modifier = gbm_bo_get_modifier(bo);
            EGLint attrs[16];
            int a = 0;

            attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
            attrs[a++] = (EGLint)GBM_FORMAT_ARGB8888;
            attrs[a++] = EGL_WIDTH;
            attrs[a++] = 256;
            attrs[a++] = EGL_HEIGHT;
            attrs[a++] = 256;
            attrs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            attrs[a++] = dmabuf_fd;
            attrs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            attrs[a++] = 0;
            attrs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            attrs[a++] = (EGLint)gbm_bo_get_stride(bo);
            /* Tiled allocations need the modifier, but only advertise it when
             * Mesa's modifier import extension is present. */
            if (modifier != 0 && strstr(exts, "import_modifiers")) {
                attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                attrs[a++] = (EGLint)(modifier & 0xffffffffu);
                attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                attrs[a++] = (EGLint)(modifier >> 32);
            }
            attrs[a++] = EGL_NONE;
            img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                               NULL, attrs);
            if (img == EGL_NO_IMAGE) {
                fail("eglCreateImageKHR(dma_buf)", (long)eglGetError());
                goto out;
            }
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        image_target_texture(GL_TEXTURE_2D, img);
        if (glGetError() == GL_NO_ERROR)
            pass("GBM dma-buf imported as an EGL image (PRIME roundtrip)");
        else
            fail("glEGLImageTargetTexture2DOES(dma_buf)", 0);
    }

out:
    if (have_gl && tex)
        glDeleteTextures(1, &tex);
    if (img != EGL_NO_IMAGE && destroy_image)
        destroy_image(dpy, img);
    if (dmabuf_fd >= 0)
        close(dmabuf_fd);
    if (bo)
        gbm_bo_destroy(bo);
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext(dpy, ctx);
        if (surf != EGL_NO_SURFACE)
            eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
    }
    if (gsurf)
        gbm_surface_destroy(gsurf);
    if (gbm)
        gbm_device_destroy(gbm);
    if (node_fd >= 0)
        close(node_fd);

    if (failures) {
        printf("=== %d TEST(S) FAILED ===\n", failures);
        return 1;
    }
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}

/* Exercise allocation/eviction and sampled texture data independently of
 * Xorg, KWin and PRIME scanout. 768 MiB exceeds this APU's 512 MiB VRAM;
 * every output pixel has a CPU oracle, including after updating reused BOs.
 * Opt-in so ordinary boot diagnostics remain small. */
static uint32_t texture_pixel(unsigned x, unsigned y, unsigned seed) {
    unsigned r = (x * 13u + y * 7u + seed * 31u) & 255u;
    unsigned g = ((x >> 3) * 19u + y * 11u + seed * 47u) & 255u;
    unsigned b = (x * 3u + (y >> 2) * 23u + seed * 61u) & 255u;
    return r | (g << 8) | (b << 16) | 0xff000000u;
}

static void test_texture_pressure(void) {
    enum { SIDE = 2048, OUT = 512, COUNT = 48, ROUNDS = 4 };
    GLuint textures[COUNT] = {0}, output = 0, fbo = 0, vbo = 0;
    GLuint snapshots[COUNT] = {0}, snapshot_fbos[COUNT] = {0};
    GLuint vs = 0, fs = 0, program = 0;
    GLint saved_fb = 0, saved_program = 0, saved_buffer = 0;
    uint32_t *source = malloc((size_t)SIDE * SIDE * 4);
    uint32_t *result = malloc((size_t)OUT * OUT * 4);
    static const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    unsigned checked = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_buffer);
    if (!source || !result) { fail("texture pressure CPU allocation", ENOMEM); goto out; }
    vs = compile_shader(GL_VERTEX_SHADER,
        "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}");
    fs = compile_shader(GL_FRAGMENT_SHADER,
        "precision highp float; uniform sampler2D tex;"
        "void main(){gl_FragColor=texture2D(tex,gl_FragCoord.xy/512.0);}");
    if (!vs || !fs) goto out;
    program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "pos"); glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { fail("texture pressure program link", 0); goto out; }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OUT, OUT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail("texture pressure framebuffer", 0); goto out;
    }
    glViewport(0, 0, OUT, OUT);
    glGenTextures(COUNT, textures);
    printf("  [INFO] texture pressure: %u MiB, %u textures, %u rounds\n",
           COUNT * SIDE * SIDE * 4u / (1024u * 1024u), COUNT, ROUNDS);
    fflush(stdout);
    for (unsigned i = 0; i < COUNT; i++) {
        for (unsigned y = 0; y < SIDE; y++)
            for (unsigned x = 0; x < SIDE; x++) source[y * SIDE + x] = texture_pixel(x, y, i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
        GLenum err = glGetError();
        if (err) { printf("  [DIAG] upload texture %u GL error=0x%x\n", i, err); fail("texture allocation/upload", 0); goto out; }
    }
    for (unsigned round = 0; round < ROUNDS; round++) {
        for (unsigned j = 0; j < COUNT; j++) {
            unsigned i = (31u * j + round) % COUNT;
            unsigned seed = i + round * COUNT;
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            if (round) {
                for (unsigned y = 0; y < SIDE; y++)
                    for (unsigned x = 0; x < SIDE; x++) source[y * SIDE + x] = texture_pixel(x, y, seed);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, source);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, OUT, OUT, GL_RGBA, GL_UNSIGNED_BYTE, result);
            GLenum err = glGetError();
            if (err) { printf("  [DIAG] round %u texture %u GL error=0x%x\n", round, i, err); fail("texture render/readback", 0); goto out; }
            for (unsigned y = 0; y < OUT; y++) {
                for (unsigned x = 0; x < OUT; x++) {
                    uint32_t expected = texture_pixel(4u*x+2u, 4u*y+2u, seed);
                    uint32_t actual = result[y * OUT + x];
                    if (actual != expected) {
                        printf("  [DIAG] GPU corruption: round=%u texture=%u pixel=(%u,%u) expected=%08x actual=%08x\n",
                               round, i, x, y, expected, actual);
                        fail("GPU texture pixels differ before PRIME presentation", 0); goto out;
                    }
                }
            }
            checked++;
        }
        printf("  [INFO] texture pressure round %u: all pixels match\n", round);
        fflush(stdout);
    }
    /* Queue a complete draw/update burst before reading anything back.
     * Replacing each source after its draw stresses GPU/CPU reuse ordering;
     * snapshots must retain the old image even after all sources changed. */
    glGenTextures(COUNT, snapshots);
    glGenFramebuffers(COUNT, snapshot_fbos);
    for (unsigned i = 0; i < COUNT; i++) {
        glBindTexture(GL_TEXTURE_2D, snapshots[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OUT, OUT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, snapshot_fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapshots[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fail("queued texture snapshot framebuffer", 0); goto out;
        }
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        for (unsigned y = 0; y < SIDE; y++)
            for (unsigned x = 0; x < SIDE; x++) source[y * SIDE + x] = texture_pixel(x, y, i + ROUNDS * COUNT);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, source);
    }
    for (unsigned i = 0; i < COUNT; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, snapshot_fbos[i]);
        glReadPixels(0, 0, OUT, OUT, GL_RGBA, GL_UNSIGNED_BYTE, result);
        if (glGetError()) { fail("queued texture snapshot readback", 0); goto out; }
        for (unsigned y = 0; y < OUT; y++) {
            for (unsigned x = 0; x < OUT; x++) {
                uint32_t expected = texture_pixel(4u*x+2u, 4u*y+2u, i + (ROUNDS - 1) * COUNT);
                uint32_t actual = result[y * OUT + x];
                if (actual != expected) {
                    printf("  [DIAG] queued GPU corruption: texture=%u pixel=(%u,%u) expected=%08x actual=%08x\n", i, x, y, expected, actual);
                    fail("queued draw/source reuse differs before PRIME presentation", 0); goto out;
                }
            }
        }
        checked++;
    }
    printf("  [INFO] checked %u frames, %u pixels (including queued draw/reuse)\n", checked, checked * OUT * OUT);
    pass("texture pressure allocation/reuse/render/readback integrity");
out:
    glBindFramebuffer(GL_FRAMEBUFFER, saved_fb);
    glUseProgram(saved_program); glBindBuffer(GL_ARRAY_BUFFER, saved_buffer);
    glDeleteTextures(COUNT, textures);
    glDeleteTextures(COUNT, snapshots);
    glDeleteFramebuffers(COUNT, snapshot_fbos);
    if (output) glDeleteTextures(1, &output);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    free(source); free(result);
    fflush(stdout);
}

/* Fog-toggle readback oracle (tunnel minimal repro).
 * Same geometry/texture with fog OFF vs EXP2 fog ON; per-draw uniform churn
 * like tunnel's per-block state changes. glFinish + glReadPixels on the same
 * frame decides render/GPU-input fault (readback corrupt, Outcome A) vs
 * display-only (readback clean, Outcome B). Tolerance covers shader
 * precision; large deviations print DIAG like the pressure test. */
static void test_fog_uniforms(void) {
    enum { SIDE = 256, OUT = 256, DRAWS = 64 };
    GLuint texture = 0, output = 0, fbo = 0, vbo = 0;
    GLuint vs = 0, fs_plain = 0, fs_fog = 0, prog_plain = 0, prog_fog = 0;
    GLint saved_fb = 0, saved_program = 0, saved_buffer = 0;
    uint32_t *source = malloc((size_t)SIDE * SIDE * 4);
    uint32_t *result = malloc((size_t)OUT * OUT * 4);
    static const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    unsigned fog_bad = 0, plain_bad = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_buffer);
    if (!source || !result) { fail("fog diagnostic CPU allocation", ENOMEM); goto out; }
    for (unsigned y = 0; y < SIDE; y++)
        for (unsigned x = 0; x < SIDE; x++) source[y * SIDE + x] = texture_pixel(x, y, 7);
    vs = compile_shader(GL_VERTEX_SHADER,
        "attribute vec2 pos; varying vec2 uv; varying float depth;"
        "uniform float uDepthOff;"
        "void main(){uv=pos*0.5+0.5; depth=(pos.x+pos.y)*0.25+0.5+uDepthOff;"
        "gl_Position=vec4(pos,0.0,1.0);}");
    fs_plain = compile_shader(GL_FRAGMENT_SHADER,
        "precision highp float; varying vec2 uv; varying float depth; uniform sampler2D tex;"
        "void main(){gl_FragColor=texture2D(tex,uv);}");
    fs_fog = compile_shader(GL_FRAGMENT_SHADER,
        "precision highp float; varying vec2 uv; varying float depth; uniform sampler2D tex;"
        "uniform vec3 fogColor; uniform float fogDensity;"
        "void main(){vec4 t=texture2D(tex,uv); float d=depth*49.0+1.0;"
        "float f=exp(-pow(fogDensity*d,2.0)); gl_FragColor=vec4(mix(fogColor,t.rgb,clamp(f,0.0,1.0)),1.0);}");
    if (!vs || !fs_plain || !fs_fog) goto out;
    prog_plain = glCreateProgram();
    glAttachShader(prog_plain, vs); glAttachShader(prog_plain, fs_plain);
    glBindAttribLocation(prog_plain, 0, "pos"); glLinkProgram(prog_plain);
    prog_fog = glCreateProgram();
    glAttachShader(prog_fog, vs); glAttachShader(prog_fog, fs_fog);
    glBindAttribLocation(prog_fog, 0, "pos"); glLinkProgram(prog_fog);
    GLint ok = 0;
    glGetProgramiv(prog_plain, GL_LINK_STATUS, &ok);
    if (!ok) { fail("fog diagnostic plain link", 0); goto out; }
    glGetProgramiv(prog_fog, GL_LINK_STATUS, &ok);
    if (!ok) { fail("fog diagnostic fog link", 0); goto out; }
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError()) { fail("fog diagnostic texture upload", 0); goto out; }
    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OUT, OUT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail("fog diagnostic framebuffer", 0); goto out;
    }
    glViewport(0, 0, OUT, OUT);
    glActiveTexture(GL_TEXTURE0);
    printf("  [INFO] fog toggle: %u draws each, %ux%u, EXP2 density 0.06 gray 0.7\n", DRAWS, OUT, OUT);
    fflush(stdout);
    for (unsigned round = 0; round < 2; round++) {
        bool fog = round == 1;
        GLuint prog = fog ? prog_fog : prog_plain;
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        GLint depth_loc = glGetUniformLocation(prog, "uDepthOff");
        GLint color_loc = fog ? glGetUniformLocation(prog, "fogColor") : -1;
        GLint density_loc = fog ? glGetUniformLocation(prog, "fogDensity") : -1;
        for (unsigned draw = 0; draw < DRAWS; draw++) {
            float off = (draw % 8) * 0.002f;
            if (depth_loc >= 0) glUniform1f(depth_loc, off);
            if (fog) {
                if (color_loc >= 0) glUniform3f(color_loc, 0.7f, 0.7f, 0.7f);
                if (density_loc >= 0) glUniform1f(density_loc, 0.06f);
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
            glReadPixels(0, 0, OUT, OUT, GL_RGBA, GL_UNSIGNED_BYTE, result);
            if (glGetError()) {
                printf("  [DIAG] fog=%d draw %u GL error=0x%x\n", fog, draw, glGetError());
                fail("fog diagnostic render/readback", 0); goto out;
            }
            for (unsigned y = 0; y < OUT; y++) {
                for (unsigned x = 0; x < OUT; x++) {
                    float u = ((float)x + 0.5f) / OUT, v = ((float)y + 0.5f) / OUT;
                    unsigned tx = (unsigned)(u * SIDE), ty = (unsigned)(v * SIDE);
                    if (tx >= SIDE) tx = SIDE - 1;
                    if (ty >= SIDE) ty = SIDE - 1;
                    uint32_t base = texture_pixel(tx, ty, 7);
                    uint32_t actual = result[y * OUT + x];
                    if (!fog) {
                        if (actual != base) {
                            if (plain_bad < 5)
                                printf("  [DIAG] fog=0 draw=%u pixel=(%u,%u) expected=%08x actual=%08x\n",
                                       draw, x, y, base, actual);
                            plain_bad++;
                        }
                    } else {
                        float depth = ((float)(2 * x + 1) / (2 * OUT) * 0.5f +
                                       (float)(2 * y + 1) / (2 * OUT) * 0.5f) + off;
                        float d = depth * 49.0f + 1.0f;
                        float f = expf(-powf(0.06f * d, 2.0f));
                        if (f < 0) f = 0; if (f > 1) f = 1;
                        float br = (base & 255) / 255.0f, bg = ((base >> 8) & 255) / 255.0f,
                              bb = ((base >> 16) & 255) / 255.0f;
                        unsigned er = (unsigned)(179 + (br * 255 - 179) * f + 0.5f);
                        unsigned eg = (unsigned)(179 + (bg * 255 - 179) * f + 0.5f);
                        unsigned eb = (unsigned)(179 + (bb * 255 - 179) * f + 0.5f);
                        unsigned ar = actual & 255, ag = (actual >> 8) & 255, ab = (actual >> 16) & 255;
                        unsigned err = (ar > er ? ar - er : er - ar) +
                                       (ag > eg ? ag - eg : eg - ag) +
                                       (ab > eb ? ab - eb : eb - ab);
                        if (err > 12) {
                            if (fog_bad < 8)
                                printf("  [DIAG] fog=1 draw=%u pixel=(%u,%u) expected~%02x%02x%02x actual=%08x err=%u\n",
                                       draw, x, y, er, eg, eb, actual, err);
                            fog_bad++;
                        }
                    }
                }
            }
        }
        printf("  [INFO] fog=%d: %u bad pixels over %u draws\n", fog, fog ? fog_bad : plain_bad, DRAWS);
        fflush(stdout);
    }
    if (plain_bad == 0 && fog_bad == 0)
        pass("fog toggle readback matches oracle (render path clean)");
    else if (plain_bad == 0 && fog_bad > 0)
        fail("fog-ON readback differs; fog-OFF clean (fog uniform/shader-variant path)", fog_bad);
    else
        fail("readback differs even without fog (base texture path)", plain_bad);
out:
    glBindFramebuffer(GL_FRAMEBUFFER, saved_fb);
    glUseProgram(saved_program); glBindBuffer(GL_ARRAY_BUFFER, saved_buffer);
    if (texture) glDeleteTextures(1, &texture);
    if (output) glDeleteTextures(1, &output);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (prog_plain) glDeleteProgram(prog_plain);
    if (prog_fog) glDeleteProgram(prog_fog);
    if (vs) glDeleteShader(vs);
    if (fs_plain) glDeleteShader(fs_plain);
    if (fs_fog) glDeleteShader(fs_fog);
    free(source); free(result);
    fflush(stdout);
}

/* Reverse sharing direction: AMD allocates/renders; native DRM imports and
 * CPU-maps exactly that dma-buf. Compare GPU readback and native CPU aliases
 * against an independent tiled pattern before any host display transfer. */
static void test_amd_export_readback(EGLDisplay dpy, struct gbm_device *gbm) {
    enum { SIDE = 512, TILE = 128, FRAMES = 64 };
    int card = -1, fd = -1;
    uint32_t handle = 0, pitch = 0;
    struct gbm_bo *bo = NULL;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
    GLint old_fb = 0;
    void *cpu = MAP_FAILED;
    size_t bytes = 0;
    uint32_t *readback = malloc(SIDE * SIDE * 4);
    PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
    if (!readback || !gbm || !create_image || !destroy_image || !bind_image) {
        fail("AMD export diagnostic setup", 0); goto out;
    }
    card = drm_name_is("/dev/dri/card0", "ascentdrm");
    if (card < 0) {
        fail("AMD export diagnostic native display node", errno); goto out;
    }
    const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    bo = gbm_bo_create_with_modifiers2(gbm, SIDE, SIDE, GBM_FORMAT_ABGR8888,
                       &linear, 1, GBM_BO_USE_RENDERING);
    if (!bo) {
        fail("AMD export diagnostic linear buffer", errno); goto out;
    }
    printf("  [INFO] AMD export buffer modifier: 0x%llx\n",
           (unsigned long long)gbm_bo_get_modifier(bo));
    if (gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_LINEAR) {
        fail("AMD export diagnostic unexpected modifier", 0); goto out;
    }
    pitch = gbm_bo_get_stride(bo);
    bytes = (size_t)pitch * SIDE;
    fd = gbm_bo_get_fd(bo);
    if (fd < 0 || drmPrimeFDToHandle(card, fd, &handle)) {
        fail("native display imports AMD-produced dma-buf", errno); goto out;
    }
    struct drm_mode_map_dumb map = {.handle = handle};
    if (ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        fail("native display AMD import mmap token", errno); goto out;
    }
    cpu = mmap(NULL, bytes, PROT_READ, MAP_SHARED, card, map.offset);
    if (cpu == MAP_FAILED) { fail("native display AMD import CPU mapping", errno); goto out; }
    EGLint attrs[] = { EGL_WIDTH, SIDE, EGL_HEIGHT, SIDE,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch, EGL_NONE };
    image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) { fail("AMD export render image", eglGetError()); goto out; }
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    bind_image(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail("AMD export render framebuffer", 0); goto out;
    }
    glViewport(0, 0, SIDE, SIDE);
    glDisable(GL_DITHER); glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        uint32_t colors[16];
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                unsigned i = y * 4 + x;
                colors[i] = texture_pixel(x, y, frame);
                uint32_t c = colors[i];
                glScissor(x*TILE, y*TILE, TILE, TILE);
                glClearColor((c&255)/255.0f, ((c>>8)&255)/255.0f,
                             ((c>>16)&255)/255.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, readback);
        if (glGetError()) { fail("AMD export GPU readback", 0); goto out; }
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) { fail("AMD export CPU access", errno); goto out; }
        bool mismatch = false;
        for (unsigned y = 0; y < SIDE && !mismatch; y++) {
            for (unsigned x = 0; x < SIDE; x++) {
                uint32_t expected = colors[(y/TILE)*4+x/TILE];
                uint32_t gpu_pixel = readback[y*SIDE+x];
                uint32_t cpu_pixel = *(volatile uint32_t *)((uint8_t *)cpu + y*pitch + x*4);
                if (expected != gpu_pixel || expected != cpu_pixel) {
                    printf("  [DIAG] AMD export frame=%u pixel=(%u,%u) expected=%08x gpu=%08x native_cpu=%08x\n",
                           frame,x,y,expected,gpu_pixel,cpu_pixel);
                    mismatch = true; break;
                }
            }
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) { fail("AMD export end CPU access", errno); goto out; }
        if (mismatch) { fail("AMD rendering/native import alias integrity", 0); goto out; }
    }
    pass("AMD-produced PRIME buffer: 64 frames, GPU/native CPU pixels match");
out:
    glDisable(GL_SCISSOR_TEST); glBindFramebuffer(GL_FRAMEBUFFER, old_fb);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) destroy_image(dpy,image);
    if (cpu != MAP_FAILED) munmap(cpu,bytes);
    if (handle) { struct drm_gem_close close_arg={.handle=handle}; ioctl(card,DRM_IOCTL_GEM_CLOSE,&close_arg); }
    if (fd >= 0) close(fd);
    if (bo) gbm_bo_destroy(bo);
    if (card >= 0) close(card);
    free(readback); fflush(stdout);
}
