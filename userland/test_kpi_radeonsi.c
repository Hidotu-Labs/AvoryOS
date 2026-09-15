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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
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
    setenv("LIBGL_DRIVERS_PATH", "/usr/lib/xorg/modules/dri", 0);
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
